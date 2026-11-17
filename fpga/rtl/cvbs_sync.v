// SPDX-License-Identifier: GPL-3.0-only
// Ported from the p4vrx P4 decoder (firmware/p4/main/decode.c), itself adapted from C5VRX p4usb, by Twotoz and the
// C5VRX contributors.
//
// Stage 1 of CVBS decode: the P4 decoder's sync slicer, line clock, vertical sync and level tracking, bit for bit
// as models/cvbs_sync.py. Takes one int8 phase step a sample at 13.33 MS/s (`take`), and for each line the clock
// renders gives its start (sample and 16-bit fraction), whether an hsync steered it, and its field line and parity.
// A sequential engine: it buffers samples in a ring and catches up between lines.
module cvbs_sync (
    input  wire              clk,
    input  wire              take,
    input  wire signed [7:0] step,
    output reg               line_valid = 0,
    output reg        [31:0] line_start = 0,
    output reg        [15:0] line_frac = 0,
    output reg               line_hit = 0,
    output reg         [8:0] line_vline = 0,
    output reg               line_parity = 0,
    output reg               field_valid = 0,
    output reg         [8:0] field_hits = 0,
    output reg               field_vsync = 0,
    output reg        [31:0] period_out = 0,
    output wire              overrun
);
    // Timing at 13.33 MS/s, as decode.c's US().
    localparam integer LINE_Q16 = 55535691;  // 80e6 * 286 * 65536 / 6 / 4.5e6, truncated at each step
    localparam integer LINE = LINE_Q16 >>> 16, HALF = LINE / 2;
    localparam integer HSYNC_MIN = 45, HSYNC_MAX = 85, BROAD_MIN = 240;
    localparam integer RENDER_AT = 125 + 702 + 32;
    localparam integer LOCK_WINDOW = 39, LOST = 30, SYNC_BLOCKS = 16, SYNC_DELAY = 32, HSYNC_WIDTH = 62;
    localparam integer FIELD_LINES = 262, LAST_ACTIVE = 259;
    localparam integer VSYNC_LINE = 3, VSYNC_WINDOW = 6, VSYNC_REACQUIRE = 3, VSYNC_SLOP = 39;
    localparam integer BOX = 4, TIP_START = 8, TIP_END = 56, PORCH_START = 72, PORCH_END = 112, LEVEL_LINES_MIN = 60;
    localparam integer SLACK = LINE_Q16 / 333;

    // C's division truncates toward zero; an arithmetic shift floors.
    function signed [31:0] tdiv2(input signed [31:0] x, input integer k);
        tdiv2 = x < 0 ? -((-x) >>> k) : x >>> k;
    endfunction

    // Samples and their sums of four, in rings the engine reads behind the input.
    reg [31:0] wr = 0;
    reg signed [10:0] acc4 = 0;
    wire [31:0] wr_next = wr + 1'd1;
    reg s_we = 0, b_we = 0;
    reg [10:0] s_waddr = 0;
    reg [8:0] b_waddr = 0;
    reg [7:0] s_wdata = 0;
    reg [10:0] b_wdata = 0;
    always @(posedge clk) begin
        s_we <= 0;
        b_we <= 0;
        if (take) begin
            s_we <= 1;
            s_waddr <= wr[10:0];
            s_wdata <= step;
            if (wr[1:0] == 3) begin
                b_we <= 1;
                b_waddr <= wr[10:2];
                b_wdata <= acc4 + step;
                acc4 <= 0;
            end else begin
                acc4 <= acc4 + step;
            end
            wr <= wr_next;
        end
    end
    reg [10:0] s_raddr = 0;
    reg [8:0] b_raddr = 0;
    wire [7:0] s_rdata;
    wire [10:0] b_rdata;
    sdp_ram #(.WIDTH(8), .DEPTH(2048), .ABITS(11)) samples (
        .clk(clk), .we(s_we), .waddr(s_waddr), .wdata(s_wdata), .raddr(s_raddr), .rdata(s_rdata));
    sdp_ram #(.WIDTH(11), .DEPTH(512), .ABITS(9)) blocks (
        .clk(clk), .we(b_we), .waddr(b_waddr), .wdata(b_wdata), .raddr(b_raddr), .rdata(b_rdata));
    wire signed [7:0] hs = s_rdata;
    wire signed [10:0] hb = b_rdata;

    // The level histogram, read-modify-written by the engine.
    reg h_we = 0;
    reg [7:0] h_waddr = 0, h_raddr = 0;
    reg [15:0] h_wdata = 0;
    wire [15:0] h_rdata;
    sdp_ram #(.WIDTH(16), .DEPTH(256), .ABITS(8)) histogram (
        .clk(clk), .we(h_we), .waddr(h_waddr), .wdata(h_wdata), .raddr(h_raddr), .rdata(h_rdata));

    // The decoder's state.
    reg signed [31:0] thr_lo = 0, thr_hi = 0, clamp = 0, thr_lo_mean = 0, thr_hi_mean = 0, clamp_mean = 0, tilt = 0;
    reg [31:0] line = 0, render_at = RENDER_AT, period = LINE_Q16, low_start = 0, last_broad = 0, line_count = 0;
    reg [15:0] frac = 0;
    reg hit = 0, low = 0, parity = 0, vline_guessed = 0, holding = 0;
    reg [7:0] missed = LOST;
    reg [8:0] vline = 0, hits = 0;
    reg [31:0] vs_cand = 0, vs_cand_lines = 0, hold_until = 0;
    reg signed [31:0] vs_cand_into = 0;
    reg signed [31:0] tip = -57 * 16, blank = -17 * 16;
    reg signed [31:0] tip_sum = 0, porch_sum = 0;
    reg [15:0] level_lines = 0;
    reg [7:0] unlocked = 0;
    reg [1:0] vsync_missed = VSYNC_REACQUIRE;
    reg vsyncs = 0;
    reg signed [31:0] box = 0;
    reg [31:0] nb = 0;

    // A shared divider for update_levels and the tilt: quotient truncated toward zero.
    reg div_go = 0, div_busy = 0;
    reg signed [31:0] div_n = 0, div_d = 0;
    reg [31:0] div_rem = 0, div_quo = 0, div_den = 0;
    reg [5:0] div_i = 0;
    reg div_neg = 0;
    wire signed [31:0] div_q = div_neg ? -$signed(div_quo) : $signed(div_quo);
    always @(posedge clk) begin
        if (div_go) begin
            div_busy <= 1;
            div_neg <= (div_n < 0) != (div_d < 0);
            div_rem <= 0;
            div_quo <= div_n < 0 ? -div_n : div_n;
            div_den <= div_d < 0 ? -div_d : div_d;
            div_i <= 32;
        end else if (div_busy) begin
            if ({div_rem[30:0], div_quo[31]} >= div_den) begin
                div_rem <= {div_rem[30:0], div_quo[31]} - div_den;
                div_quo <= {div_quo[30:0], 1'b1};
            end else begin
                div_rem <= {div_rem[30:0], div_quo[31]};
                div_quo <= {div_quo[30:0], 1'b0};
            end
            div_i <= div_i - 1'd1;
            if (div_i == 1) div_busy <= 0;
        end
    end

    // The engine. Block RAM reads land two clocks after their address is set, so each loop issues one address a
    // clock and uses the data two clocks behind it.
    localparam
        WAIT = 0, BOX_A = 1, BOX_B = 2, BOX_C = 3, SLICE = 4, EDGE = 5, PULSE = 6, VSYNC_A = 7, VSYNC_B = 8,
        HOLD = 9, RENDER = 10, FIELD = 11, HIST = 12, HIST_R = 13, HIST_W = 14, TIP_A = 15, TIP_B = 16, TILT_A = 17,
        TILT_B = 18, PORCH = 19, ADVANCE = 20, THRESH = 21, BOXEND = 22, NEXT = 23, LEV_SUM = 24, LEV_DIV_50 = 25,
        LEV_DIV_50W = 26, LEV_TIP = 27, LEV_BAND = 28, LEV_BLANK = 29, LEV_CLEAR = 30, LEV_DIV_T = 31,
        LEV_DIV_TW = 32, LEV_DIV_BW = 33, LEV_SET = 34, FIELD_DONE = 35, HIST_R2 = 36, PORCH_A = 37;
    reg [5:0] state = WAIT, ret = WAIT, bx_ret = WAIT;
    reg signed [31:0] in_b = 0, t_sum = 0, p_sum = 0, edge_before = 0, hist_s = 0;
    reg [31:0] edge_near = 0, edge_best = 0, pulse_start = 0, pulse_width = 0, ptr = 0;
    reg [4:0] edge_reach = 0;
    reg signed [7:0] edge_h [0:3];
    reg signed [10:0] hist_q [0:15];
    reg [9:0] k = 0;
    reg [15:0] lev_total = 0, lev_acc = 0, lev_band = 0, lev_target = 0;
    reg [8:0] lev_tip = 0, lev_blank = 0, lev_lo = 0, lev_hi = 0;
    reg signed [31:0] new_tip = 0, new_blank = 0, vs_into = 0, vs_since = 0;
    reg lev_locked = 0, lev_found = 0;
    integer m;
    initial for (m = 0; m < 4; m = m + 1) edge_h[m] = 0;
    initial for (m = 0; m < 16; m = m + 1) hist_q[m] = 0;

    // Thresholds from tip and blank, then tilted: set_thresholds and tilt_thresholds.
    wire signed [31:0] span = blank - tip < 128 ? 128 : blank - tip;
    wire signed [31:0] thr_mid = tdiv2(tip + blank, 5), hyst = span >>> 7;
    wire signed [31:0] tilt4 = tdiv2(BOX * tilt, 4);
    wire signed [31:0] clamp_m = tdiv2(BOX * (blank + tdiv2(span, 1)), 4);

    // The sliced box with this block in, and the block 16 back out.
    wire signed [31:0] out_b = nb < 64 ? 0 : (hb > clamp ? clamp : hb);
    wire signed [31:0] box_new = box + in_b - out_b;
    wire [31:0] width_now = nb - SYNC_DELAY - low_start;
    // on_pulse's line clock step.
    wire signed [31:0] err = pulse_start - line;
    wire signed [31:0] e16 = err * 65536 - $signed({16'd0, frac});
    wire signed [31:0] pos = $signed({16'd0, frac}) + tdiv2(e16, 3);
    wire signed [31:0] new_period = $signed(period) + tdiv2(e16, 8);
    // Whole samples the step moves the line: shifted on its own, since in a sum with the unsigned line Verilog
    // would shift it unsigned.
    wire signed [31:0] pos_lines = pos >>> 16;
    wire [31:0] since_broad = pulse_start - last_broad;
    wire [31:0] hold_at = (line + (period >> 16) - LOCK_WINDOW - 8) & ~32'd3;
    // leading_edge's step d = k - 18, with h(near + d) arriving now.
    wire signed [6:0] edge_d = $signed({2'b0, k[4:0]}) - 18;
    wire [6:0] edge_abs = edge_d < 0 ? -edge_d : edge_d;
    // At d = -12 the window before is box_at(near - 13), the four samples already in.
    wire signed [31:0] edge_prev = k == 6 ? edge_h[0] + edge_h[1] + edge_h[2] + edge_h[3] : edge_before;
    wire signed [31:0] edge_box = edge_h[1] + edge_h[2] + edge_h[3] + hs;

    assign overrun = wr - nb > 1400;

    always @(posedge clk) begin
        line_valid <= 0;
        field_valid <= 0;
        h_we <= 0;
        div_go <= 0;
        case (state)
            WAIT:
                if (wr - nb >= 4 && wr - nb < 32'h80000000) state <= holding && nb != hold_until ? RENDER : BOX_A;
            BOX_A: begin
                holding <= 0;
                b_raddr <= nb[10:2];
                state <= BOX_B;
            end
            BOX_B: begin
                b_raddr <= nb[10:2] - 9'd16;
                state <= BOX_C;
            end
            BOX_C: begin
                in_b <= hb > clamp ? clamp : hb;
                state <= SLICE;
            end
            SLICE: begin
                box <= box_new;
                state <= RENDER;
                if (low) begin
                    if (box_new > SYNC_BLOCKS * thr_hi) begin
                        low <= 0;
                        pulse_width <= width_now;
                        if (width_now >= HSYNC_MIN && width_now <= HSYNC_MAX) begin
                            edge_near <= nb - SYNC_DELAY - HSYNC_WIDTH;
                            edge_best <= nb - SYNC_DELAY - HSYNC_WIDTH;
                            edge_reach <= 13;
                            k <= 0;
                            state <= EDGE;
                        end else begin
                            pulse_start <= low_start;
                            state <= PULSE;
                        end
                    end
                end else if (box_new < SYNC_BLOCKS * thr_lo) begin
                    low <= 1;
                    low_start <= nb - SYNC_DELAY;
                end
            end
            // leading_edge(near, thr_lo): addresses near-16 .. near+12, one a clock; h(near-16+j) lands at k = j+2.
            EDGE: begin
                if (k < 29) s_raddr <= edge_near - 16 + k;
                if (k >= 2) begin
                    edge_h[0] <= edge_h[1];
                    edge_h[1] <= edge_h[2];
                    edge_h[2] <= edge_h[3];
                    edge_h[3] <= hs;
                end
                // From k = 6, h(near + d) arrives for d = -12 .. 12.
                if (k >= 6 && k <= 30) begin
                    if (edge_prev >= thr_lo && edge_box < thr_lo && edge_abs < edge_reach) begin
                        edge_best <= edge_near + {{25{edge_d[6]}}, edge_d};
                        edge_reach <= edge_abs;
                    end
                    edge_before <= edge_box;
                end
                k <= k + 1'd1;
                if (k == 31) begin
                    pulse_start <= edge_best;
                    state <= PULSE;
                end
            end
            PULSE: begin
                state <= RENDER;
                if (pulse_width >= HSYNC_MIN && pulse_width <= HSYNC_MAX) begin
                    state <= HOLD;
                    if (missed >= LOST) begin
                        line <= pulse_start;
                        frac <= 0;
                        period <= LINE_Q16;
                        render_at <= pulse_start + RENDER_AT;
                        hit <= 1;
                    end else if (err >= -LOCK_WINDOW && err <= LOCK_WINDOW) begin
                        line <= line + pos_lines;
                        frac <= pos[15:0];
                        render_at <= line + pos_lines + RENDER_AT;
                        period <= new_period < LINE_Q16 - SLACK ? LINE_Q16 - SLACK :
                                  new_period > LINE_Q16 + SLACK ? LINE_Q16 + SLACK : new_period;
                        hit <= 1;
                    end
                end else if (pulse_width >= BROAD_MIN) begin
                    last_broad <= pulse_start;
                    if (since_broad > 4 * LINE) begin
                        vs_cand <= pulse_start;
                        vs_cand_lines <= line_count;
                        vs_cand_into <= err;
                    end else if (vs_cand == last_broad &&
                                 ($signed(since_broad - HALF) > -(HALF / 8) && $signed(since_broad - HALF) < HALF / 8 ||
                                  $signed(since_broad - LINE) > -(HALF / 8) && $signed(since_broad - LINE) < HALF / 8)) begin
                        vs_into <= vs_cand_into;
                        vs_since <= line_count - vs_cand_lines;
                        vs_cand <= pulse_start - 1;
                        state <= VSYNC_A;
                    end
                end
            end
            VSYNC_A:
                if (vs_into < -(HALF / 2)) begin
                    vs_into <= vs_into + LINE;
                    vs_since <= vs_since + 1;
                end else begin
                    state <= VSYNC_B;
                end
            VSYNC_B: begin : vs
                reg signed [31:0] vl;
                reg expected, par;
                state <= RENDER;
                vl = $signed({23'd0, vline}) - vs_since;
                if (vl < 0) vl = vl + FIELD_LINES + (parity ? 0 : 1);
                expected = vl >= FIELD_LINES - VSYNC_WINDOW || vl <= VSYNC_LINE + VSYNC_WINDOW;
                par = vs_into > HALF / 2 && vs_into < (HALF * 3) / 2;
                if (!((vs_into < 0 ? -vs_into : vs_into) > VSYNC_SLOP && (vs_into < HALF ? HALF - vs_into : vs_into - HALF) > VSYNC_SLOP) &&
                    (expected || vsync_missed >= VSYNC_REACQUIRE || vline_guessed)) begin
                    vsync_missed <= 0;
                    parity <= par;
                    vline_guessed <= 0;
                    vline <= (par ? VSYNC_LINE : VSYNC_LINE + 1) + vs_since;
                    vsyncs <= 1;
                end
            end
            HOLD: begin
                state <= RENDER;
                if (hit && vline > VSYNC_LINE + VSYNC_WINDOW && vline < FIELD_LINES - VSYNC_WINDOW - 1 &&
                    $signed(hold_at - nb) > 4) begin
                    hold_until <= hold_at;
                    holding <= 1;
                end
            end
            RENDER:
                if ($signed(nb + 4 - render_at) >= 0) begin
                    line_valid <= 1;
                    line_start <= line;
                    line_frac <= frac;
                    line_hit <= hit;
                    line_vline <= vline;
                    line_parity <= parity;
                    k <= 0;
                    hist_s <= 0;
                    state <= vline == LAST_ACTIVE ? FIELD : vline[1:0] == 0 ? HIST : TIP_A;
                end else begin
                    state <= NEXT;
                end
            FIELD: begin
                if (!vsyncs && vsync_missed < VSYNC_REACQUIRE) vsync_missed <= vsync_missed + 1'd1;
                field_valid <= 1;
                field_hits <= hits;
                field_vsync <= vsyncs;
                period_out <= period;
                lev_total <= 0;
                k <= 0;
                state <= LEV_SUM;
            end
            // The histogram: blocks line/4 + j, j = 0 .. 211, one at a time; a slot every 4 blocks from j = 16.
            HIST: begin
                b_raddr <= line[10:2] + k[8:0];
                state <= HIST_R;
            end
            HIST_R: state <= HIST_R2;
            HIST_R2: begin
                for (m = 15; m > 0; m = m - 1) hist_q[m] <= hist_q[m - 1];
                hist_q[0] <= hb;
                hist_s <= k >= 16 ? hist_s + hb - hist_q[15] : hist_s + hb;
                k <= k + 1'd1;
                if (k >= 16 && k[1:0] == 0) begin
                    h_raddr <= tdiv2(hist_s + hb - hist_q[15], 6) + 128;
                    state <= HIST_W;
                end else begin
                    state <= (k + 1) * 4 < LINE ? HIST : TIP_A;
                end
            end
            HIST_W: state <= HIST_W + 40;
            HIST_W + 40: begin
                h_we <= 1;
                h_waddr <= h_raddr;
                h_wdata <= h_rdata + 1'd1;
                state <= k * 4 < LINE ? HIST : TIP_A;
            end
            TIP_A: begin
                hits <= hits + hit;
                if (missed >= LOST && tilt != 0) tilt <= 0;
                k <= 0;
                t_sum <= 0;
                state <= vline[0] && missed < LOST ? TIP_B : ADVANCE;
            end
            // h(line + 8 .. line + 55)
            TIP_B: begin
                if (k < TIP_END - TIP_START) s_raddr <= line + TIP_START + k;
                if (k >= 2) t_sum <= t_sum + hs;
                k <= k + 1'd1;
                if (k == TIP_END - TIP_START + 1)
                    state <= vline > VSYNC_LINE + VSYNC_WINDOW && vline < FIELD_LINES - VSYNC_WINDOW - 1 ? TILT_A : PORCH_A;
            end
            TILT_A: begin
                div_n <= t_sum * 16;
                div_d <= TIP_END - TIP_START;
                div_go <= 1;
                state <= TILT_B;
            end
            TILT_B:
                if (!div_go && !div_busy) begin : tl
                    reg signed [31:0] t, most;
                    most = tdiv2(blank - tip, 1);
                    t = div_q - tip;
                    t = t > most ? most : t < -most ? -most : t;
                    tilt <= tilt + tdiv2(t - tilt, 1);
                    state <= PORCH_A;
                end
            PORCH_A: begin
                k <= 0;
                p_sum <= 0;
                state <= hit ? PORCH : ADVANCE;
            end
            PORCH: begin
                if (k == 0) begin
                    tip_sum <= tip_sum + t_sum;
                    level_lines <= level_lines + 1'd1;
                end
                if (k < PORCH_END - PORCH_START) s_raddr <= line + PORCH_START + k;
                if (k >= 2 && k <= PORCH_END - PORCH_START + 1) p_sum <= p_sum + hs;
                k <= k + 1'd1;
                if (k == PORCH_END - PORCH_START + 2) begin
                    porch_sum <= porch_sum + p_sum;
                    state <= ADVANCE;
                end
            end
            ADVANCE: begin
                missed <= hit ? 0 : missed == 255 ? 255 : missed + 1'd1;
                hit <= 0;
                line_count <= line_count + 1'd1;
                if (vline + 1 >= FIELD_LINES + (parity ? 0 : 1)) begin
                    vline <= 0;
                    parity <= ~parity;
                end else begin
                    vline <= vline + 1'd1;
                end
                {line, frac} <= {line, frac} + period;
                render_at <= line + (({16'd0, frac} + period) >> 16) + RENDER_AT;
                ret <= BOXEND;
                bx_ret <= NEXT;
                state <= THRESH;
            end
            THRESH: begin
                thr_lo <= BOX * (thr_mid - hyst) + tilt4;
                thr_hi <= BOX * (thr_mid + hyst) + tilt4;
                clamp <= clamp_m + tilt4;
                k <= 0;
                ptr <= nb;
                box <= 0;
                hist_s <= 0;
                state <= ret;
            end
            // box_ending(ptr): blocks ptr/4, ptr/4 - 1, .. ptr/4 - 15, clamped; one in two clocks behind.
            BOXEND: begin
                if (k < 16) b_raddr <= ptr[10:2] - k[8:0];
                if (k >= 2)
                    box <= box + ($signed(ptr) - 4 * ($signed({22'd0, k}) - 2) < 0 ? 0 : hb > clamp ? clamp : hb);
                k <= k + 1'd1;
                if (k == 17) state <= bx_ret;
            end
            NEXT: begin
                nb <= nb + 4;
                state <= WAIT;
                if (holding && nb + 4 == hold_until) begin
                    ptr <= nb;
                    k <= 0;
                    box <= 0;
                    state <= BOXEND;
                    bx_ret <= WAIT;
                end
            end
            // update_levels: 2nd percentile = tip, median 1-3 MHz above it = blanking.
            LEV_SUM: begin
                if (k < 256) h_raddr <= k[7:0];
                if (k >= 2) lev_total <= lev_total + h_rdata;
                k <= k + 1'd1;
                if (k == 257) state <= LEV_DIV_50;
            end
            LEV_DIV_50:
                if (lev_total < 1000) begin
                    state <= FIELD_DONE;
                end else begin
                    div_n <= lev_total;
                    div_d <= 50;
                    div_go <= 1;
                    state <= LEV_DIV_50W;
                end
            LEV_DIV_50W:
                if (!div_go && !div_busy) begin
                    lev_target <= div_q;
                    lev_acc <= 0;
                    lev_found <= 0;
                    k <= 0;
                    state <= LEV_TIP;
                end
            // The first slot where the running total reaches total/50, or 256.
            LEV_TIP: begin
                if (k < 256) h_raddr <= k[7:0];
                k <= k + 1'd1;
                if (k >= 2 && !lev_found) begin
                    if (lev_acc + h_rdata >= lev_target) begin
                        lev_found <= 1;
                        lev_tip <= k - 2;
                    end
                    lev_acc <= lev_acc + h_rdata;
                end
                if (k == 258) begin
                    if (!lev_found) lev_tip <= 256;
                    k <= 0;
                    lev_band <= 0;
                    state <= LEV_BAND;
                end
            end
            // band = the sum of slots lo .. hi.
            LEV_BAND: begin
                lev_lo <= lev_tip + 19;
                lev_hi <= lev_tip + 57 > 255 ? 255 : lev_tip + 57;
                if (k < 256) h_raddr <= k[7:0];
                if (k >= 2 && k - 2 >= lev_tip + 19 && k - 2 <= (lev_tip + 57 > 255 ? 255 : lev_tip + 57))
                    lev_band <= lev_band + h_rdata;
                k <= k + 1'd1;
                if (k == 257) begin
                    k <= 0;
                    lev_acc <= 0;
                    lev_found <= 0;
                    state <= LEV_BLANK;
                end
            end
            // blank: from lo, the first slot below hi where the running total reaches band/2, else hi.
            LEV_BLANK: begin
                if (k < 256) h_raddr <= k[7:0];
                k <= k + 1'd1;
                if (k >= 2 && !lev_found && k - 2 >= lev_lo && k - 2 < lev_hi) begin
                    if (lev_acc + h_rdata >= (lev_band >> 1)) begin
                        lev_found <= 1;
                        lev_blank <= k - 2;
                    end
                    lev_acc <= lev_acc + h_rdata;
                end
                if (k == 257) begin
                    if (!lev_found) lev_blank <= lev_hi > lev_lo ? lev_hi : lev_lo;
                    k <= 0;
                    state <= LEV_CLEAR;
                end
            end
            LEV_CLEAR: begin
                h_we <= 1;
                h_waddr <= k[7:0];
                h_wdata <= 0;
                k <= k + 1'd1;
                if (k == 255) begin
                    lev_locked <= level_lines >= LEVEL_LINES_MIN;
                    new_tip <= ($signed({23'd0, lev_tip}) - 128) * 16;
                    new_blank <= ($signed({23'd0, lev_blank}) - 128) * 16;
                    state <= level_lines >= LEVEL_LINES_MIN ? LEV_DIV_T : LEV_SET;
                end
            end
            LEV_DIV_T: begin
                div_n <= tip_sum * 16;
                div_d <= level_lines * (TIP_END - TIP_START);
                div_go <= 1;
                state <= LEV_DIV_TW;
            end
            LEV_DIV_TW:
                if (!div_go && !div_busy) begin
                    new_tip <= div_q;
                    div_n <= porch_sum * 16;
                    div_d <= level_lines * (PORCH_END - PORCH_START);
                    div_go <= 1;
                    state <= LEV_DIV_BW;
                end
            LEV_DIV_BW:
                if (!div_go && !div_busy) begin
                    new_blank <= div_q;
                    state <= LEV_SET;
                end
            LEV_SET: begin
                if (lev_locked) begin
                    unlocked <= 0;
                    tip <= tdiv2(3 * tip + new_tip, 2);
                    blank <= tdiv2(3 * blank + new_blank, 2);
                end else begin
                    unlocked <= unlocked + 1'd1;
                    tip <= new_tip;
                    // Fields that fail to lock try sync depths of 1200, 1700 and 800 kHz in turn.
                    case ((unlocked / 3) % 4)
                        1: blank <= new_tip + 369;
                        2: blank <= new_tip + 523;
                        3: blank <= new_tip + 246;
                        default: blank <= new_blank;
                    endcase
                end
                tip_sum <= 0;
                porch_sum <= 0;
                level_lines <= 0;
                state <= FIELD_DONE;
            end
            FIELD_DONE: begin
                hits <= 0;
                vsyncs <= 0;
                ret <= vline[1:0] == 0 ? HIST : TIP_A;
                state <= THRESH;
            end
            default: state <= WAIT;
        endcase
    end
endmodule
