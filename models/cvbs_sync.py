# SPDX-License-Identifier: GPL-3.0-only
# Ported from the p4vrx P4 decoder (firmware/p4/main/decode.c), itself adapted from C5VRX p4usb, by Twotoz and
# the C5VRX contributors.
"""Stage 1 of CVBS decode in the Nano: the P4 decoder's sync slicer, line clock and vertical sync, in integers as
the RTL will run them.

Input is what the decoder gets in fm mode: one int8 phase step a sample at 13.33 MS/s (52.08 kHz a unit), and the
sum of each 4. From a 40 MS/s capture the steps come bit for bit through models/fm_demod.py and the Nano's
integrated phase. The detector, line clock and field logic follow decode.c's detect, on_pulse, vsync,
render_line's bookkeeping and update_levels; the picture itself (chroma, luma, clicks, noise) is left out.

    uv run --with numpy python models/cvbs_sync.py captures/grab8M-R4.u16
"""

import sys

import numpy as np

import fm_demod

FS_HZ = 80_000_000 // 6
LINE_Q16 = 80_000_000 * 286 * 65536 // 6 // 4_500_000
KHZ_PER_UNIT = 52


def us(x):
    return int(x * FS_HZ / 1_000_000)


HSYNC_MIN, HSYNC_MAX, BROAD_MIN = us(3.4), us(6.4), us(18)
ACTIVE_START = us(9.4)
RENDER_AT = ACTIVE_START + us(52.66) + 32
LOCK_WINDOW = us(3)
LOST_AFTER_LINES = 30
SYNC_BLOCKS = 16
SYNC_DELAY = SYNC_BLOCKS * 4 // 2
HSYNC_WIDTH = us(4.7)
FIELD_LINES = 262
FIRST_ACTIVE, ACTIVE_LINES = 20, 240
VSYNC_LINE, VSYNC_WINDOW, VSYNC_REACQUIRE = 3, 6, 3
VSYNC_SLOP = us(3)
BOX = 4
TIP_START, TIP_END, PORCH_START, PORCH_END = 8, 56, 72, 112
LEVEL_LINES_MIN = 60
DEPTH_FIELDS = 3
DEPTH_KHZ = [0, 1200, 1700, 800]


def cdiv(a, b):
    """C's integer division, which truncates toward zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def steps_from_capture(path, skip=0):
    """The P4's int8 phase steps in fm mode, from a 40 MS/s capture: the Nano's frequency words, integrated as
    3 x each into a 14-bit phase whose top 8 bits the P4 differences."""
    i, q = fm_demod.lanes(path)
    i, q = i[skip:], q[skip:]
    words = fm_demod.nano(i, q, int(round(i.mean())), int(round(q.mean())), fm_demod.fir_taps())
    phase = np.cumsum(words.astype(np.int64) * 3) & 0x3FFF
    p8 = phase >> 6
    d = np.diff(p8)
    return (((d + 128) & 0xFF) - 128).astype(np.int64)


class Sync:
    def __init__(self, hist):
        self.hist = hist
        n = len(hist) // 4 * 4
        self.blocks = hist[:n].reshape(-1, 4).sum(axis=1)
        self.thr_lo = self.thr_hi = self.clamp = 0
        self.thr_lo_mean = self.thr_hi_mean = self.clamp_mean = self.tilt = 0
        self.histogram = [0] * 256
        self.line = self.line_frac = 0
        self.period = LINE_Q16
        # decode.c starts at 0 and renders a line from samples not yet in; here the first line waits for its samples.
        self.render_at = RENDER_AT
        self.hit = False
        self.missed = LOST_AFTER_LINES
        self.vline = self.parity = 0
        self.last_broad = 0
        self.line_count = 0
        self.vsync_candidate = self.vsync_candidate_lines = self.vsync_candidate_into = 0
        self.tip, self.blank = -57 * 16, -17 * 16
        self.tip_sum = self.porch_sum = self.level_lines = 0
        self.unlocked_fields = 0
        self.vsync_missed = VSYNC_REACQUIRE
        self.vline_guessed = False
        self.fields = self.field_hits = self.field_vsync = 0
        self.low = False
        self.low_start = 0
        # What the RTL reports: each line's start, hit or not; each field's hits and vsync.
        self.lines, self.field_log = [], []
        self.set_thresholds()

    # Inputs, as the decoder reads its rings.
    def h(self, m):
        return int(self.hist[m]) if 0 <= m < len(self.hist) else 0

    def block(self, nb):
        k = nb // 4
        return int(self.blocks[k]) if 0 <= k < len(self.blocks) else 0

    def clamped_block(self, nb):
        b = self.block(nb)
        return min(b, self.clamp)

    def box_ending(self, nb):
        return sum(self.clamped_block(nb - 4 * k) for k in range(SYNC_BLOCKS))

    def box_at(self, m):
        return self.h(m) + self.h(m - 1) + self.h(m - 2) + self.h(m - 3)

    def leading_edge(self, near, thr):
        best, reach = near, 13
        before = self.box_at(near - reach)
        for d in range(1 - reach, reach):
            b = self.box_at(near + d)
            if before >= thr and b < thr and abs(d) < reach:
                best, reach = near + d, abs(d)
            before = b
        return best

    # Levels and thresholds.
    def tilt_thresholds(self):
        t = cdiv(BOX * self.tilt, 16)
        self.thr_lo, self.thr_hi, self.clamp = self.thr_lo_mean + t, self.thr_hi_mean + t, self.clamp_mean + t

    def set_thresholds(self):
        span = max(self.blank - self.tip, 8 * 16)
        thr, hyst = cdiv(self.tip + self.blank, 32), cdiv(span, 128)
        self.thr_lo_mean = BOX * (thr - hyst)
        self.clamp_mean = cdiv(BOX * (self.blank + cdiv(span, 2)), 16)
        self.thr_hi_mean = BOX * (thr + hyst)
        self.tilt_thresholds()

    def update_levels(self):
        total = sum(self.histogram)
        if total < 1000:
            return
        tip, acc = 0, 0
        while tip < 256:
            acc += self.histogram[tip]
            if acc >= cdiv(total, 50):
                break
            tip += 1
        lo, hi = tip + cdiv(1000, KHZ_PER_UNIT), min(tip + cdiv(3000, KHZ_PER_UNIT), 255)
        band = sum(self.histogram[lo:hi + 1])
        blank, acc = lo, 0
        while blank < hi:
            acc += self.histogram[blank]
            if acc >= cdiv(band, 2):
                break
            blank += 1
        self.histogram = [0] * 256
        tip, blank = (tip - 128) * 16, (blank - 128) * 16
        locked = self.level_lines >= LEVEL_LINES_MIN
        if locked:
            tip = cdiv(self.tip_sum * 16, self.level_lines * (TIP_END - TIP_START))
            blank = cdiv(self.porch_sum * 16, self.level_lines * (PORCH_END - PORCH_START))
            self.unlocked_fields = 0
        else:
            depth = DEPTH_KHZ[(self.unlocked_fields // DEPTH_FIELDS) % len(DEPTH_KHZ)]
            self.unlocked_fields += 1
            if depth:
                blank = tip + cdiv(depth * 16, KHZ_PER_UNIT)
        self.tip_sum = self.porch_sum = self.level_lines = 0
        self.tip = cdiv(3 * self.tip + tip, 4) if locked else tip
        self.blank = cdiv(3 * self.blank + blank, 4) if locked else blank
        self.set_thresholds()

    def emit_field(self):
        if not self.field_vsync and self.vsync_missed < VSYNC_REACQUIRE:
            self.vsync_missed += 1
        self.update_levels()
        self.fields += 1
        self.field_log.append((self.field_hits, self.field_vsync, self.parity, self.period))
        self.field_hits = self.field_vsync = 0

    # Pulses.
    def vsync(self, into, since):
        line = LINE_Q16 >> 16
        half = line // 2
        while into < cdiv(-half, 2):
            into += line
            since += 1
        if abs(into) > VSYNC_SLOP and abs(into - half) > VSYNC_SLOP:
            return
        vline = self.vline - since
        if vline < 0:
            vline += FIELD_LINES + (not self.parity)
        expected = vline >= FIELD_LINES - VSYNC_WINDOW or vline <= VSYNC_LINE + VSYNC_WINDOW
        if not expected and self.vsync_missed < VSYNC_REACQUIRE and not self.vline_guessed:
            return
        self.vsync_missed = 0
        parity = int(cdiv(half, 2) < into < cdiv(half * 3, 2))
        self.parity = parity
        self.vline_guessed = False
        self.vline = (VSYNC_LINE if parity else VSYNC_LINE + 1) + since
        self.field_vsync += 1

    def on_pulse(self, start, width):
        if HSYNC_MIN <= width <= HSYNC_MAX:
            err = start - self.line
            if self.missed >= LOST_AFTER_LINES:
                self.line, self.line_frac, self.period = start, 0, LINE_Q16
                self.render_at = self.line + RENDER_AT
                self.hit = True
            elif -LOCK_WINDOW <= err <= LOCK_WINDOW:
                e16 = err * 65536 - self.line_frac
                pos = self.line_frac + cdiv(e16, 8)
                self.line += pos >> 16
                self.line_frac = pos & 0xFFFF
                self.render_at = self.line + RENDER_AT
                period = self.period + cdiv(e16, 256)
                slack = cdiv(LINE_Q16, 333)
                self.period = min(max(period, LINE_Q16 - slack), LINE_Q16 + slack)
                self.hit = True
        elif width >= BROAD_MIN:
            line = LINE_Q16 >> 16
            half = line // 2
            since = start - self.last_broad
            if since > 4 * line:
                self.vsync_candidate = start
                self.vsync_candidate_lines = self.line_count
                self.vsync_candidate_into = start - self.line
            elif self.vsync_candidate == self.last_broad and (abs(since - half) < half // 8 or abs(since - line) < half // 8):
                self.vsync(self.vsync_candidate_into, self.line_count - self.vsync_candidate_lines)
                self.vsync_candidate = start - 1
            self.last_broad = start

    # Lines.
    def render_line(self):
        self.lines.append((self.line, self.line_frac, self.hit, self.vline, self.parity))
        if self.vline == FIRST_ACTIVE + ACTIVE_LINES - 1:
            self.emit_field()
        if (self.vline & 3) == 0:
            s, at = 0, self.line & ~3
            for i in range(0, LINE_Q16 >> 16, 4):
                s += self.block(at + i)
                if i < 4 * SYNC_BLOCKS:
                    continue
                s -= self.block(at + i - 4 * SYNC_BLOCKS)
                if not (i & 12):
                    self.histogram[(cdiv(s, 4 * SYNC_BLOCKS) + 128) & 0xFF] += 1
        self.field_hits += self.hit
        if self.missed >= LOST_AFTER_LINES and self.tilt:
            self.tilt = 0
            self.tilt_thresholds()
        if (self.vline & 1) and self.missed < LOST_AFTER_LINES:
            t = sum(self.h(self.line + i) for i in range(TIP_START, TIP_END))
            if VSYNC_LINE + VSYNC_WINDOW < self.vline < FIELD_LINES - VSYNC_WINDOW - 1:
                tilt, most = cdiv(t * 16, TIP_END - TIP_START) - self.tip, cdiv(self.blank - self.tip, 2)
                tilt = max(min(tilt, most), -most)
                self.tilt += cdiv(tilt - self.tilt, 2)
                self.tilt_thresholds()
            if self.hit:
                self.tip_sum += t
                self.porch_sum += sum(self.h(self.line + i) for i in range(PORCH_START, PORCH_END))
                self.level_lines += 1
        self.missed = 0 if self.hit else self.missed + 1
        self.hit = False
        self.line_count += 1
        self.vline += 1
        if self.vline >= FIELD_LINES + (not self.parity):
            self.vline = 0
            self.parity ^= 1
        nxt = self.line_frac + self.period
        self.line += nxt >> 16
        self.line_frac = nxt & 0xFFFF
        self.render_at = self.line + RENDER_AT

    def run(self):
        until = len(self.blocks) * 4
        nb, box = 0, self.box_ending(0)
        # The decoder skips ahead once locked in the picture; here the detector holds off until the same point.
        hold_until = -1
        while until - nb >= 4:
            if nb >= hold_until:
                inn, out = self.block(nb), self.block(nb - 4 * SYNC_BLOCKS)
                box += min(inn, self.clamp) - min(out, self.clamp)
                thr_lo, thr_hi = SYNC_BLOCKS * self.thr_lo, SYNC_BLOCKS * self.thr_hi
                if self.low:
                    if box > thr_hi:
                        self.low = False
                        end = nb - SYNC_DELAY
                        width = end - self.low_start
                        hsync = HSYNC_MIN <= width <= HSYNC_MAX
                        self.on_pulse(self.leading_edge(end - HSYNC_WIDTH, self.thr_lo) if hsync else self.low_start, width)
                        if self.hit and VSYNC_LINE + VSYNC_WINDOW < self.vline < FIELD_LINES - VSYNC_WINDOW - 1:
                            skip = (self.line + (self.period >> 16) - LOCK_WINDOW - 8) & ~3
                            if skip - nb > 4:
                                hold_until = skip
                elif box < thr_lo:
                    self.low = True
                    self.low_start = nb - SYNC_DELAY
            if nb + 4 - self.render_at >= 0:
                self.render_line()
                box = self.box_ending(nb)
            nb += 4
            if nb == hold_until:
                box = self.box_ending(nb - 4)
        return self


def main(path):
    hist = steps_from_capture(path)
    s = Sync(hist).run()
    print(f"{path}: {len(hist)} samples at 13.33 MS/s ({len(hist) / FS_HZ * 1000:.0f} ms), {len(s.lines)} lines, {s.fields} fields")
    for k, (hits, vs, parity, period) in enumerate(s.field_log):
        print(f"  field {k}: {hits}/262 hsyncs, vsync {vs}, parity {parity}, line {period / 65536:.3f} samples")


if __name__ == "__main__":
    main(sys.argv[1])
