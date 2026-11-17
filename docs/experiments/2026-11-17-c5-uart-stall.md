# 2026-11-17: C5 replies held back on a dead channel

## Question

Why do C5 commands take seconds, or get no reply, while I/Q streams noise from a dead channel at high gain? That delay made switching from a dead channel to a live one slow.

## Setup

- Commit: uncommitted working copy on top of the UI work (paused scan, touch).
- Bitstream: the current safe bitstream.
- Boards: the pinned p4vrx P4, C5 and Nano, external antenna; a VTX on R4, nothing on R2.
- Wiring changes from `../wiring.md`: none.

## Method

`c5 send ping` latency (10 tries, 3 s wait) on live R4, on dead R2 at gain 77, and on dead R2 with I/Q stopped. Raw bytes from the C5 with `c5 log on`. C5 counters through a `uart` command: lines, junk bytes, pings, spare CPU, slowest line-to-reply and, temporarily, the fewest free TX FIFO bytes at the start of a reply. Then two switch tests, each 20 rounds of `ui pick R2`, 1 s, then R4: the worker's log (300 ms lock window) and `decode sweep pick R4` (3 s lock window).

## Results

- Before: dead R2 gave 1 reply in 10 within 3 s; live R4 and stopped I/Q answered in 2-3 ms. Raw log: replies to 7 `iq off` tries arrived in one burst once one had taken effect. The C5's free TX FIFO fell to 6 of 128 bytes.
- The C5 TX FIFO filling was a symptom. With the P4's RX threshold at 1 nothing changed; with sw/hw flow control explicitly off nothing changed; a reply that waits for TX idle drained in 1.5 ms every time, with replies still late. The C5's RX was holding commands: lane noise kept its line from idling, so the 10-symbol idle timeout never fired and commands waited for 120 bytes in the RX FIFO, then ran in a burst whose replies filled the TX FIFO.
- C5 RX full threshold 1: dead R2 pings 1-2 ms (9 of 10; one `err`).
- Prompt delivery exposed corrupted commands. A C5 counted 10,550 junk bytes in a few minutes, rebooted once, and ended at gain 0 with I/Q RMS 0, which fits a mangled `gain 77` parsed as 0. `clk slip N` with a mangled `slip` falls into `clk <sel> <div>`.
- Check byte (` #hh`, XOR) on every command: 11 lines refused as `err check` in the tests below, and no wrong gain or lost bus since.
- Worker switch test, dead R2 → R4: median 82 ms, worst 394 ms, 1 of 20 not locked in 300 ms. Before: median 901 ms, worst 2307 ms, 7 not locked (captures/ui/switch20.txt).
- `decode sweep pick` test: 20 of 20 locked, median 65 ms, worst 119 ms.
- Ping after the fix: live R4 1-2 ms, dead R2 at gain 77 1-2 ms (9 of 10), dead R2 with I/Q stopped 1 ms.

## Verdict

Adopted: RX full threshold 1 on both UARTs and a check byte on every C5 command. The fallback of pausing I/Q while replying wasn't needed and was removed.

## Next

Replies have no check; a mangled reply only fails its request today. The C5's one reboot during testing wasn't explained.
