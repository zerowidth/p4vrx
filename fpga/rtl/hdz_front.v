// HDZero front end: the C5's 40 MS/s I/Q in, per-symbol 1,024-point spectra out as 14-bit bus words. Bit for bit
// with models/hdz_front.py, which describes each block; the bus format is in docs/hdzero-fpga.md.
module hdz_front (
    input  wire       clk,
    input  wire       in_v,
    input  wire [6:0] in_i,
    input  wire [6:0] in_q,
    // a bus word every clock: (-64, 0) idle, (-64, -64) a symbol's mark, then its header words and bins
    output reg  [6:0] out_i = 7'h40,
    output reg  [6:0] out_q = 7'h00,
    output reg  [10:0] peak_bin = 0,
    output reg  [15:0] theta = 0,
    output reg  [7:0]  agc = 32,
    output reg  [11:0] symbols = 0
);
    localparam SYM = 1056, N = 1024, BLOCK = 64, FIRST = 1055, DELAY = 64, NEAR = 16, START = 24;
    // used before the blocks that make them
    wire ph_take, st_take;
    reg dv_done = 0;
    wire [39:0] dv_q;

    // ---- DC: 2048 x[n] less the sum of x over [n-1024, n+1023], >> 11, to 9 bits
    wire signed [7:0] x_i = {in_i, 1'b1}, x_q = {in_q, 1'b1};
    wire [15:0] xa, xb;
    ce_ring #(.WIDTH(16), .DEPTH(1023), .ABITS(10)) dly_a (.clk(clk), .ce(in_v), .wdata({x_i, x_q}), .rdata(xa));
    ce_ring #(.WIDTH(16), .DEPTH(1025), .ABITS(11)) dly_b (.clk(clk), .ce(in_v), .wdata(xa), .rdata(xb));
    wire signed [7:0] xa_i = xa[15:8], xa_q = xa[7:0], xb_i = xb[15:8], xb_q = xb[7:0];
    reg signed [19:0] sum_i = 0, sum_q = 0;
    wire signed [19:0] sum_i_n = sum_i + x_i - xb_i, sum_q_n = sum_q + x_q - xb_q;
    wire signed [21:0] dd_i = ($signed({xa_i, 11'b0}) - sum_i_n) >>> 11, dd_q = ($signed({xa_q, 11'b0}) - sum_q_n) >>> 11;
    function signed [8:0] sat9(input signed [21:0] v);
        sat9 = v > 255 ? 9'sd255 : v < -256 ? -9'sd256 : v[8:0];
    endfunction
    reg [10:0] dc_fill = 0;
    reg xd_v = 0;
    reg signed [8:0] xd_i = 0, xd_q = 0;
    always @(posedge clk) begin
        xd_v <= 0;
        if (in_v) begin
            sum_i <= sum_i_n;
            sum_q <= sum_q_n;
            if (dc_fill != 1023) dc_fill <= dc_fill + 1'd1;
            else begin
                xd_v <= 1;
                xd_i <= sat9(dd_i);
                xd_q <= sat9(dd_q);
            end
        end
    end

    // ---- Resample 40 -> 27 MS/s: output m from xd[i0-3 .. i0+4], i0 = floor(40m/27), phase (40m) mod 27
    reg [71:0] coef [0:26];
    initial $readmemh("hdz_resample.hex", coef);
    reg signed [8:0] sr_i [0:7], sr_q [0:7];
    integer k;
    integer ki;
    initial for (ki = 0; ki < 8; ki = ki + 1) begin sr_i[ki] = 0; sr_q[ki] = 0; end
    integer ks;
    reg [31:0] xi_n = 0;      // index of the next xd sample
    reg [31:0] next_i0 = 0;
    reg [4:0] ph = 0, emit_ph = 0;
    reg emit = 0;
    always @(posedge clk) begin
        emit <= 0;
        if (xd_v) begin
            for (ks = 7; ks > 0; ks = ks - 1) begin sr_i[ks] <= sr_i[ks - 1]; sr_q[ks] <= sr_q[ks - 1]; end
            sr_i[0] <= xd_i;
            sr_q[0] <= xd_q;
            xi_n <= xi_n + 1;
            if (xi_n == next_i0 + 4) begin
                emit <= 1;
                emit_ph <= ph;
                if (ph + 13 < 27) begin next_i0 <= next_i0 + 1; ph <= ph + 13; end
                else begin next_i0 <= next_i0 + 2; ph <= ph - 14; end
            end
        end
    end
    // tap t multiplies xd[i - 7 + t], which after the shift is sr[7 - t]
    reg signed [20:0] acc_i, acc_q;
    always @(*) begin
        acc_i = 0;
        acc_q = 0;
        for (k = 0; k < 8; k = k + 1) begin
            acc_i = acc_i + $signed(coef[emit_ph][9 * k +: 9]) * sr_i[7 - k];
            acc_q = acc_q + $signed(coef[emit_ph][9 * k +: 9]) * sr_q[7 - k];
        end
    end
    reg y_v = 0;
    reg signed [16:0] y_i = 0, y_q = 0;
    reg [31:0] ny = 0;        // index of the next y sample
    always @(posedge clk) begin
        y_v <= emit;
        if (emit) begin
            y_i <= acc_i >>> 4;
            y_q <= acc_q >>> 4;
        end
    end
    reg [31:0] yn = 0;        // index of the y sample in y_i/y_q while y_v
    reg [10:0] ymod = 0;      // yn mod 1056
    always @(posedge clk) if (y_v) begin
        yn <= yn + 1;
        ymod <= ymod == SYM - 1 ? 11'd0 : ymod + 1'd1;
    end

    // ---- CP correlation: y8[n-1024] conj(y8[n]), summed over 32, magnitude max + min/2
    function signed [7:0] sat8(input signed [16:0] v);
        sat8 = v > 127 ? 8'sd127 : v < -128 ? -8'sd128 : v[7:0];
    endfunction
    wire signed [7:0] b_i = sat8(y_i >>> 4), b_q = sat8(y_q >>> 4);
    wire [15:0] ya;
    ce_ring #(.WIDTH(16), .DEPTH(1024), .ABITS(10)) dly_y (.clk(clk), .ce(y_v), .wdata({b_i, b_q}), .rdata(ya));
    wire signed [7:0] a_i = ya[15:8], a_q = ya[7:0];
    reg c1_v = 0;
    reg [31:0] c1_n = 0;
    reg signed [16:0] pr_i = 0, pr_q = 0;
    always @(posedge clk) begin
        c1_v <= y_v;
        if (y_v) begin
            pr_i <= a_i * b_i + a_q * b_q;
            pr_q <= a_q * b_i - a_i * b_q;
            c1_n <= yn;
        end
    end
    wire [33:0] pr_old;
    ce_ring #(.WIDTH(34), .DEPTH(32), .ABITS(5)) dly_p (.clk(clk), .ce(c1_v), .wdata({pr_i, pr_q}), .rdata(pr_old));
    reg c2_v = 0;
    reg [31:0] c2_n = 0;
    reg signed [22:0] s_i = 0, s_q = 0;
    wire signed [22:0] s_i_n = s_i + pr_i - $signed(pr_old[33:17]), s_q_n = s_q + pr_q - $signed(pr_old[16:0]);
    always @(posedge clk) begin
        c2_v <= c1_v;
        if (c1_v) begin
            s_i <= s_i_n;
            s_q <= s_q_n;
            c2_n <= c1_n;
        end
    end
    wire [22:0] abs_i = s_i[22] ? -s_i : s_i, abs_q = s_q[22] ? -s_q : s_q;
    wire [23:0] mag = abs_i > abs_q ? abs_i + (abs_q >> 1) : abs_q + (abs_i >> 1);

    // ---- Fold over 1056 positions for 64 symbols; the last symbol finds the first maximum
    wire fold_ce = c2_v && c2_n >= FIRST;
    reg [10:0] fj = 0;
    reg [5:0] fsym = 0;
    wire [10:0] fj_next = fj == SYM - 1 ? 11'd0 : fj + 1'd1;
    reg [31:0] fold [0:SYM - 1];
    integer kf;
    initial for (kf = 0; kf < SYM; kf = kf + 1) fold[kf] = 0;
    reg [31:0] fold_q = 0;
    always @(posedge clk) fold_q <= fold[fold_ce ? fj_next : fj];
    wire [31:0] fold_new = (fsym == 0 ? 32'd0 : fold_q) + mag;
    reg [31:0] best = 0;
    reg [10:0] best_j = 0;
    wire take = fsym == BLOCK - 1 && (fj == 0 || fold_new > best);
    wire [10:0] best_j_n = take ? fj : best_j;
    wire block_end = fold_ce && fj == SYM - 1 && fsym == BLOCK - 1;
    // the correlation at the previous block's peak, summed over this block, for the CFO
    reg have_pj = 0;
    reg [10:0] pj = 0;
    reg signed [35:0] cs_i = 0, cs_q = 0;
    wire hit = have_pj && fj == pj;
    wire signed [35:0] cs_i_n = cs_i + (hit ? s_i : 23'sd0), cs_q_n = cs_q + (hit ? s_q : 23'sd0);
    reg ph_pend = 0, st_go = 0;
    reg signed [35:0] cfo_i = 0, cfo_q = 0;
    reg [31:0] ph_at = 0, st_at = 0;
    reg [10:0] ph_val = 0;
    always @(posedge clk) begin
        st_go <= 0;
        if (fold_ce) begin
            fold[fj] <= fold_new;
            if (take) begin best <= fold_new; best_j <= fj; end
            cs_i <= cs_i_n;
            cs_q <= cs_q_n;
            fj <= fj_next;
            if (fj == SYM - 1) fsym <= fsym + 1'd1;
            if (block_end) begin
                peak_bin <= best_j_n;
                ph_pend <= 1;
                ph_val <= best_j_n;
                ph_at <= c2_n + 1 + DELAY;
                if (have_pj) begin
                    st_go <= 1;
                    st_at <= c2_n + 1 + DELAY;
                    cfo_i <= cs_i_n;
                    cfo_q <= cs_q_n;
                end
                have_pj <= 1;
                pj <= best_j_n;
                cs_i <= 0;
                cs_q <= 0;
            end
        end
        if (ph_take) ph_pend <= 0;
    end

    // ---- CORDIC on the summed correlation: its angle in 2^-16 turns
    localparam [16 * 14 - 1:0] ATAN = {16'd1, 16'd3, 16'd5, 16'd10, 16'd20, 16'd41, 16'd81, 16'd163, 16'd326,
                                      16'd651, 16'd1297, 16'd2555, 16'd4836, 16'd8192};
    reg [1:0] cst = 0;
    reg [3:0] cit = 0;
    reg signed [39:0] cx = 0, cy = 0;
    reg [15:0] cz = 0;
    reg st_pend = 0;
    reg [31:0] st_new = 0;
    wire [39:0] ax = cx[39] ? -cx : cx, ay = cy[39] ? -cy : cy;
    always @(posedge clk) begin
        case (cst)
            0: if (st_go) begin
                cx <= cfo_i;
                cy <= cfo_q;
                cst <= 1;
            end
            1: if (ax >= (40'd1 << 23) || ay >= (40'd1 << 23)) begin
                cx <= cx >>> 1;
                cy <= cy >>> 1;
            end else begin
                if (cx < 0) begin cx <= -cx; cy <= -cy; cz <= 16'h8000; end
                else cz <= 0;
                cit <= 0;
                cst <= 2;
            end
            2: begin
                if (cy >= 0) begin
                    cx <= cx + (cy >>> cit);
                    cy <= cy - (cx >>> cit);
                    cz <= cz + ATAN[16 * cit +: 16];
                end else begin
                    cx <= cx - (cy >>> cit);
                    cy <= cy + (cx >>> cit);
                    cz <= cz - ATAN[16 * cit +: 16];
                end
                if (cit == 13) cst <= 3;
                cit <= cit + 1'd1;
            end
            3: begin
                theta <= cz;
                st_new <= {{10{cz[15]}}, cz, 6'b0};
                st_pend <= 1;
                cst <= 0;
            end
        endcase
        if (st_take) st_pend <= 0;
    end

    // ---- NCO, and the symbol schedule, both on y samples
    reg [31:0] nco = 0, step = 0;
    reg cfo_ok = 0;
    assign st_take = y_v && st_pend && yn >= st_at;
    wire [31:0] step_eff = st_take ? st_new : step;
    wire cfo_eff = cfo_ok || st_take;
    assign ph_take = y_v && ph_pend && yn >= ph_at;
    reg [10:0] phase = 0;
    reg ph_ok = 0;
    wire [10:0] phase_eff = ph_take ? ph_val : phase;
    reg [23:0] lut [0:1023];
    initial $readmemh("hdz_nco.hex", lut);
    reg [23:0] lut_q = 0;
    always @(posedge clk) lut_q <= lut[nco[31:22]];
    reg z1_v = 0;
    reg [31:0] z1_n = 0;
    reg signed [16:0] z1_i = 0, z1_q = 0;
    always @(posedge clk) begin
        z1_v <= y_v;
        if (y_v) begin
            nco <= nco + step_eff;
            step <= step_eff;
            if (st_take) cfo_ok <= 1;
            if (ph_take) begin phase <= ph_val; ph_ok <= 1; end
            z1_i <= y_i;
            z1_q <= y_q;
            z1_n <= yn;
        end
    end
    wire signed [11:0] lc = lut_q[11:0], ls = lut_q[23:12];
    wire signed [29:0] zr = z1_i * lc - z1_q * ls + 30'sd1024, zi = z1_i * ls + z1_q * lc + 30'sd1024;
    reg z_v = 0;
    reg [31:0] z_n = 0;
    reg signed [18:0] z_i = 0, z_q = 0;
    always @(posedge clk) begin
        z_v <= z1_v;
        if (z1_v) begin
            z_i <= zr >>> 11;
            z_q <= zi >>> 11;
            z_n <= z1_n;
        end
    end

    // Schedule: at the tentative start t, the newest phase moves it by d (|d| <= 16) or resyncs to it.
    reg running = 0;
    reg [31:0] t_tent = 0, w_start = 0;
    reg w_set = 0;
    reg [11:0] count = 0;
    reg [13:0] off = 0;
    wire [10:0] dm = phase_eff >= ymod ? phase_eff - ymod : phase_eff + SYM - ymod;   // (phase - t) mod 1056
    wire signed [11:0] dsg = dm >= SYM / 2 ? $signed({1'b0, dm}) - SYM : $signed({1'b0, dm});
    wire near = dsg <= NEAR && dsg >= -NEAR;
    wire [31:0] t_fin = near ? t_tent + {{20{dsg[11]}}, dsg} : t_tent + dm;
    reg [27:0] hq [0:3];      // {cfo_ok, resync, count, off}
    reg [1:0] hq_w = 0, hq_r = 0;
    always @(posedge clk) begin
        if (y_v) begin
            if (!running && ph_take) begin
                running <= 1;
                t_tent <= yn + dm;
            end else if (running && yn == t_tent) begin
                hq[hq_w] <= {cfo_eff, !near, count, near ? off + {{2{dsg[11]}}, dsg} : 14'd0};
                hq_w <= hq_w + 1'd1;
                off <= near ? off + {{2{dsg[11]}}, dsg} : 14'd0;
                count <= count + 1'd1;
                w_start <= t_fin + START;
                w_set <= 1;
                t_tent <= t_fin + SYM;
            end
        end
    end

    // ---- FFT on the window's 1,024 samples
    wire in_win = w_set && z_n >= w_start && z_n < w_start + N;
    wire fft_ce = z_v && in_win;
    wire [9:0] fft_pos = z_n - w_start;
    function signed [17:0] sat18(input signed [20:0] v);
        sat18 = v > 131071 ? 18'sd131071 : v < -131072 ? -18'sd131072 : v[17:0];
    endfunction
    wire [9:0] f_pos;
    wire signed [17:0] f_re, f_im;
    hdz_fft fft (.clk(clk), .ce(fft_ce), .in_pos(fft_pos), .in_re(sat18({z_i, 2'b0})), .in_im(sat18({z_q, 2'b0})),
                 .out_pos(f_pos), .out_re(f_re), .out_im(f_im));

    // ---- Output scale: bin x G[k] A >> 13 (G per bin, Q4; A the AGC, Q5), saturated to +-63
    reg [7:0] gain [0:1023];
    initial $readmemh("hdz_gain_c5narrow.hex", gain);
    function [9:0] bitrev(input [9:0] p);
        integer b;
        for (b = 0; b < 10; b = b + 1) bitrev[b] = p[9 - b];
    endfunction
    reg [10:0] fev = 0;       // FFT events, to 1,038 (the pipeline's depth)
    reg sc_v = 0;
    reg [9:0] sc_pos = 0;
    reg signed [17:0] sc_re = 0, sc_im = 0;
    reg [7:0] g_q = 0;
    always @(posedge clk) begin
        sc_v <= fft_ce && fev == 1038;
        if (fft_ce) begin
            if (fev != 1038) fev <= fev + 1'd1;
            sc_pos <= f_pos;
            sc_re <= f_re;
            sc_im <= f_im;
            g_q <= gain[bitrev(f_pos)];
        end
    end
    // The AGC (models/hdz_front.py): each symbol's bins use one A; the sum of |re| + |im| over symbols 64j..64j+63
    // sets the A of symbols from 64j+65 on.
    reg started = 0;
    reg [11:0] kcur = 0;      // the symbol whose bins are passing
    reg ag_pend = 0;
    reg [11:0] ag_at = 0, dv_k = 0;
    reg [7:0] ag_new = 0;
    wire first_bin = sc_v && sc_pos == 0;
    wire [11:0] k_eff = first_bin ? (started ? kcur + 1'd1 : 12'd0) : kcur;
    wire ag_take = first_bin && ag_pend && ag_at == k_eff;
    wire [7:0] agc_eff = ag_take ? ag_new : agc;
    wire [15:0] gp = g_q * agc_eff;
    wire signed [34:0] pm_re = sc_re * $signed({1'b0, gp}), pm_im = sc_im * $signed({1'b0, gp});
    wire signed [21:0] ar = pm_re >>> 13, ai = pm_im >>> 13;
    wire [21:0] aar = ar[21] ? -ar : ar, aai = ai[21] ? -ai : ai;
    function [6:0] sat7(input signed [21:0] v);
        sat7 = v > 63 ? 7'd63 : v < -63 ? 7'h41 : v[6:0];
    endfunction
    reg [39:0] ag_acc = 0;
    reg dv_go = 0;
    reg [39:0] dv_num = 0, dv_den = 0;
    reg bf_v = 0;
    reg [22:0] bf_d = 0;      // {first, agc, v_i, v_q}
    always @(posedge clk) begin
        bf_v <= sc_v;
        dv_go <= 0;
        if (sc_v) begin
            if (first_bin) begin
                started <= 1;
                kcur <= k_eff;
                agc <= agc_eff;
                if (ag_take) ag_pend <= 0;
            end
            bf_d <= {first_bin, agc_eff, sat7(ar), sat7(ai)};
            if (sc_pos == N - 1 && k_eff[5:0] == BLOCK - 1) begin
                dv_go <= 1;
                dv_num <= agc_eff * 14 * 131072;
                dv_den <= ag_acc + aar + aai;
                dv_k <= k_eff;
                ag_acc <= 0;
            end else begin
                ag_acc <= ag_acc + aar + aai;
            end
        end
        if (dv_done) begin
            ag_pend <= 1;
            ag_at <= dv_k + 12'd2;
            ag_new <= dv_q == 0 ? 8'd1 : dv_q > 255 ? 8'd255 : dv_q[7:0];
        end
    end

    // Restoring division for the AGC: dv_num / dv_den, 40 bits.
    reg [5:0] dv_it = 0;
    reg dv_run = 0;
    reg [39:0] dv_r = 0, dv_qr = 0;
    wire [40:0] dv_t = {dv_r[38:0], dv_qr[39]} - dv_den;
    assign dv_q = dv_qr;
    always @(posedge clk) begin
        dv_done <= 0;
        if (dv_go) begin
            dv_run <= 1;
            dv_it <= 0;
            dv_r <= 0;
            dv_qr <= dv_num;
        end else if (dv_run) begin
            if (!dv_t[40]) begin
                dv_r <= dv_t[39:0];
                dv_qr <= {dv_qr[38:0], 1'b1};
            end else begin
                dv_r <= {dv_r[38:0], dv_qr[39]};
                dv_qr <= {dv_qr[38:0], 1'b0};
            end
            dv_it <= dv_it + 1'd1;
            if (dv_it == 39) begin
                dv_run <= 0;
                dv_done <= 1;
            end
        end
    end

    // ---- Bins into a FIFO; the bus takes a mark and three header words before each symbol's first bin
    reg [22:0] bf [0:63];
    reg [5:0] bf_w = 0, bf_r = 0;
    wire bf_empty = bf_w == bf_r;
    wire [22:0] bf_head = bf[bf_r];
    always @(posedge clk) if (bf_v) begin
        bf[bf_w] <= bf_d;
        bf_w <= bf_w + 1'd1;
    end
    reg [2:0] hs = 0;         // 0 bins; 1..5 the header words of the head symbol, then its first bin
    reg [27:0] hcur = 0;
    always @(posedge clk) begin
        out_i <= 7'h40;
        out_q <= 7'h00;
        case (hs)
            0: if (!bf_empty) begin
                if (bf_head[22]) begin
                    out_i <= 7'h40;
                    out_q <= 7'h40;
                    hcur <= hq[hq_r];
                    hq_r <= hq_r + 1'd1;
                    hs <= 1;
                end else begin
                    out_i <= bf_head[13:7];
                    out_q <= bf_head[6:0];
                    bf_r <= bf_r + 1'd1;
                end
            end
            // header words: 12 bits each, 6 a lane, so none reads as a mark
            1: begin
                out_i <= {1'b0, hcur[19:14]};
                out_q <= {1'b0, hcur[25:20]};
                symbols <= hcur[25:14];
                hs <= 2;
            end
            2: begin
                out_i <= {5'b0, hcur[26], hcur[27]};
                out_q <= 7'd0;
                hs <= 3;
            end
            3: begin
                out_i <= {1'b0, hcur[5:0]};
                out_q <= {1'b0, hcur[11:6]};
                hs <= 4;
            end
            4: begin
                out_i <= {1'b0, bf_head[19:14]};
                out_q <= {5'b0, bf_head[21:20]};
                hs <= 5;
            end
            5: begin
                out_i <= bf_head[13:7];
                out_q <= bf_head[6:0];
                bf_r <= bf_r + 1'd1;
                hs <= 0;
            end
        endcase
    end
endmodule
