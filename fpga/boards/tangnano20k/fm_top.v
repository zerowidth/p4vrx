// lockout_regs_top with a second PLL that multiplies the lockout's clean 13.33 MHz clock by 3, for FM demod
// at 40 MS/s. Registers as lockout_regs_top, and:
//   0x03 bit 4: the 40 MHz PLL locked
//   0x0b 40 MHz PLL phase step, 0-15 (dynamic phase, 22.5 degrees a step)
//   0x0c-0x0d 40 MHz PLL edges per ms (low byte first, holding the high byte)
//   0x0e times the 40 MHz PLL lost lock, saturating at 255
//   0x08 control bit 2: the lanes carry FM demod with PCLK from the 40 MHz clock divided by 3, as a 14-bit phase
//        that sums 3 x each 14-bit frequency (2.44 kHz an LSB): D13..D6 are the P4 decoder's 8-bit phase,
//        1/256 turn at 13.33 MS/s; D4..D0 the sample's radius in steps of 4.86 lane units, to 31, and D5 set
//        from a radius of 121, where the lanes clip; bit 3: track the I/Q offsets rather than use 0x10-0x11
//   0x10-0x11 I and Q offsets, signed, as the lanes read 2v+1; reads give the ones in use
//   0x12-0x13 mean CORDIC magnitude over the last 32768 samples, low byte first (holding the high byte)
//   0x08 bit 4: raw 40 MS/s, the lanes as sampled on the 40 MHz clock with PCLK from it, for I/Q captures
//   0x08 bit 5: CVBS line markers (fpga/rtl/cvbs_sync.v), only with WITH_CVBS (off: it doesn't fit beside HDZero). The fm stream goes out 4096 samples late, the radius
//        code stops at 30, and a line's first sample carries code 31 with the hit flag as D5; the next two carry
//        {half, parity, vline[8:5]} and {0, vline[4:0]}
//   0x20 last field's hsyncs (low 8 bits), 0x21 status: bit 0 vsync in the last field, 1 the engine fell behind
//   In raw 40 MS/s and spectra modes every other word goes out with each lane's top bit flipped, so the P4 can find
//   doubled ones.
//   0x08 bit 7: with bit 4 or 6, the lanes carry n x 0x2d5b mod 2^14 for clock n, to test the link (tools/linktest.py)
//   0x29 features: bit 0 FM demod, 1 HDZero front end, 2 CVBS sync (build 5 on; earlier builds have FM from 2)
//   0x08 bit 6: HDZero spectra (fpga/rtl/hdz_front.v, docs/hdzero-fpga.md) on the lanes, PCLK from the 40 MHz clock
//        as raw 40 MS/s; 0x22-0x23 the front end's timing peak (11 bits), 0x24-0x25 its CFO angle (2^-16 turns),
//        0x26 its AGC (Q5), 0x27-0x28 the last symbol count put out (12 bits), low bytes first
//   0x14-0x1f HDZero detector (fpga/rtl/cp_detect.v): fold peak, fold total (32 bits each), peak bin and
//             decisions (16 bits each), low bytes first; reading 0x14 holds them all for the reads after
// lockout_regs_top's description:
// passthru_lockout_top with an I2C register file at 0x42 on the P4's touch bus, for the P4's self-check:
//   0x00-0x01 "PV", 0x02 build
//   0x03 status: bit 0 PLL lock, 1 bus enable (IRQ wire), 2 C5 clock taken in the last ms, 3 lanes from the pattern
//   0x04-0x05 C5 clock edges taken per ms, 0x06-0x07 C5 clock edges seen per ms (low byte first; reading the
//             low byte holds the high byte for the next read)
//   0x08 control: bit 0 lanes from the pattern, bit 1 PCLK from the 27 MHz clock halved rather than the C5's
//   0x09-0x0a lane pattern, D7..D0 then D13..D8
// The bus is still driven only while the P4 holds IRQ high.
module fm_top #(
    parameter [7:0] BUILD = 8'h06,
    // Analog and HDZero don't fit together (docs/hdzero-fpga.md): this is the analog image, and hdz_top the HDZero
    // one, the same with FM demod and CVBS sync left out and the HDZero front end in.
    parameter WITH_FM = 1,
    parameter WITH_CVBS = 1,
    parameter WITH_HDZ = 0
) (
    input  wire        clk_27m,
    output wire        spk_en,
    input  wire        c5_clk,
    input  wire [6:0]  c5_q,
    input  wire [6:0]  c5_i,
    output wire        p4_pclk,
    output wire [13:0] p4_d,
    input  wire        p4_en,
    input  wire        i2c_scl,
    inout  wire        i2c_sda
);
    assign spk_en = 1'b0;

    wire fast, lock;
    rPLL #(
        .FCLKIN("27"), .IDIV_SEL(0), .FBDIV_SEL(4), .ODIV_SEL(8), .DEVICE("GW2A-18C")
    ) pll (
        .CLKIN(clk_27m), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
        .PSDA(4'd0), .DUTYDA(4'd0), .FDLY(4'd0), .CLKOUT(fast), .CLKOUTP(), .LOCK(lock), .CLKOUTD(), .CLKOUTD3()
    );

    // The lockout, as passthru_lockout_top.
    reg armed = 1, clr = 0, q = 0;
    always @(posedge c5_clk or posedge clr)
        if (clr) q <= 1'b0;
        else q <= q | armed;

    reg [2:0] q_s = 0;
    reg [3:0] since = 4'd15;
    always @(posedge fast) begin
        q_s <= {q_s[1:0], q};
        if (q_s[1] && !q_s[2]) since <= 4'd1;
        else if (since != 4'd15) since <= since + 4'd1;
        clr <= since >= 4'd3 && since < 4'd5;
        armed <= since >= 4'd6;
    end

    reg [13:0] lanes = 0;
    always @(posedge q) lanes <= {c5_q, c5_i};

    reg [1:0] en = 0;
    always @(posedge q) en <= {en[0], p4_en};

    // The C5's bus, 40 MS/s with 802.11p, three samples to each clean clock period.
    wire clk40, lock40;
    reg [3:0] phase40 = 0;
    rPLL #(
        .FCLKIN("13.333"), .IDIV_SEL(0), .FBDIV_SEL(2), .ODIV_SEL(16), .DYN_DA_EN("true"), .DEVICE("GW2A-18C")
    ) pll40 (
        .CLKIN(q), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
        .PSDA(phase40), .DUTYDA(4'd0), .FDLY(4'd0), .CLKOUT(clk40), .CLKOUTP(), .LOCK(lock40), .CLKOUTD(), .CLKOUTD3()
    );
    reg [15:0] count40 = 0;
    always @(posedge clk40) count40 <= count40 + 1'd1;
    wire [15:0] count40_g = count40 ^ (count40 >> 1);
    reg [15:0] count40_g1 = 0, count40_g2 = 0, count40_last = 0, count40_per_ms = 0;
    reg [2:0] lock40_s = 0;
    reg [7:0] losses40 = 0;

    // FM demod at the bus rate. The Nano makes PCLK as clk40 / 3, and the data changes 25 ns before PCLK
    // rises and 12.5 ns after it falls, whichever edge the P4 samples on and whatever the PLL's phase.
    reg [13:0] lanes40 = 0;
    always @(posedge clk40) lanes40 <= {c5_q, c5_i};
    // PCLK high for 1.5 periods of clk40 from the period after the data changes: a 25 ns low pulse lost the
    // P4 about 600 samples per million on the ribbon.
    reg [1:0] third = 0;
    reg pclk_a = 0, pclk_b = 0;
    always @(posedge clk40) begin
        third <= third == 2 ? 2'd0 : third + 1'd1;
        pclk_a <= third == 0;
    end
    always @(negedge clk40) pclk_b <= pclk_a;
    wire pclk_fm = pclk_a | pclk_b;
    reg signed [7:0] dc_i_set = 0, dc_q_set = 0, dc_i_auto = 0, dc_q_auto = 0;
    reg [7:0] control40 = 0;
    always @(posedge clk40) control40 <= control;
    wire signed [7:0] dc_i = control40[3] ? dc_i_auto : dc_i_set;
    wire signed [7:0] dc_q = control40[3] ? dc_q_auto : dc_q_set;
    wire signed [13:0] fm_out;
    wire [8:0] magnitude;
    reg [13:0] fm_phase = 0;
    reg [5:0] fm_radius = 0;
    always @(posedge clk40)
        if (third == 2) begin
            fm_phase <= fm_phase + fm_out * 3;
            fm_radius <= {magnitude >= 9'd25, magnitude > 9'd31 ? 5'd31 : magnitude[4:0]};
        end
    generate if (WITH_FM) begin : g_fm
    fm_demod demod (
        .clk(clk40), .in_i(lanes40[6:0]), .in_q(lanes40[13:7]), .dc_i(dc_i), .dc_q(dc_q),
        .take(third == 1), .out(fm_out), .magnitude(magnitude)
    );
    end else begin : g_no_fm
        assign fm_out = 0;
        assign magnitude = 0;
    end endgenerate
    wire [31:0] cp_peak, cp_total;
    wire [10:0] cp_bin;
    wire [15:0] cp_decisions;
    cp_detect hdz (
        .clk(clk40), .valid(1'b1), .in_i(lanes40[6:0]), .in_q(lanes40[13:7]), .dc_i(dc_i), .dc_q(dc_q),
        .peak(cp_peak), .total(cp_total), .peak_bin(cp_bin), .decisions(cp_decisions)
    );
    // HDZero front end: per-symbol spectra as bus words, every clk40.
    wire [6:0] hz_i, hz_q;
    wire [10:0] hz_peak;
    wire [15:0] hz_theta;
    wire [7:0] hz_agc;
    wire [11:0] hz_symbols;
    generate if (WITH_HDZ) begin : g_hdz
        hdz_front hz (
            .clk(clk40), .in_v(1'b1), .in_i(lanes40[6:0]), .in_q(lanes40[13:7]), .out_i(hz_i), .out_q(hz_q),
            .peak_bin(hz_peak), .theta(hz_theta), .agc(hz_agc), .symbols(hz_symbols)
        );
    end else begin : g_no_hdz
        assign {hz_i, hz_q, hz_peak, hz_theta, hz_agc, hz_symbols} = 0;
    end endgenerate
    // The decoder's 8-bit phase steps at 13.33 MS/s, into the sync engine, the sample after the phase updates.
    reg sync_take = 0;
    reg signed [7:0] sync_step = 0;
    reg [7:0] p8_prev = 0;
    always @(posedge clk40) begin
        sync_take <= 0;
        if (third == 0) begin
            sync_take <= 1;
            sync_step <= fm_phase[13:6] - p8_prev;
            p8_prev <= fm_phase[13:6];
        end
    end
    wire line_valid, line_hit, line_parity, field_valid, field_vsync, sync_overrun;
    wire [31:0] line_start, sync_period;
    wire [15:0] line_frac;
    wire [8:0] line_vline, field_hits;
    generate
        if (WITH_CVBS) begin : g_cvbs
            cvbs_sync sync (
                .clk(clk40), .take(sync_take), .step(sync_step), .line_valid(line_valid), .line_start(line_start),
                .line_frac(line_frac), .line_hit(line_hit), .line_vline(line_vline), .line_parity(line_parity),
                .field_valid(field_valid), .field_hits(field_hits), .field_vsync(field_vsync), .period_out(sync_period),
                .overrun(sync_overrun)
            );
        end else begin : g_no_cvbs
            assign {line_valid, line_hit, line_parity, field_valid, field_vsync, sync_overrun} = 6'd0;
            assign {line_start, sync_period, line_frac, line_vline, field_hits} = 0;
        end
    endgenerate
    reg [8:0] last_hits = 0;
    reg last_vsync = 0, fell_behind = 0;
    always @(posedge clk40) begin
        if (field_valid) begin
            last_hits <= field_hits;
            last_vsync <= field_vsync;
        end
        if (sync_overrun) fell_behind <= 1;
    end
    // The stream goes out DELAY samples late, so each line's verdict, given some 860-1500 samples after its
    // start, reaches the line's first sample before it leaves. Sample n of the engine is phase word n.
    localparam DELAY = 4095;
    reg [31:0] word_n = 0;
    // {hit, half, parity, vline}; the delay holds about 4.8 lines, so 8 are queued.
    reg [11:0] mark_fifo [0:7];
    reg [31:0] mark_at [0:7];
    reg [2:0] mark_head = 0, mark_tail = 0;
    wire marks_waiting = mark_head != mark_tail;
    wire [16:0] halves = {1'b0, line_frac} + 17'h4000;
    always @(posedge clk40) begin
        if (line_valid) begin
            mark_at[mark_tail] <= line_start + halves[16];
            mark_fifo[mark_tail] <= {line_hit, halves[15], line_parity, line_vline};
            mark_tail <= mark_tail + 1'd1;
        end
    end
    wire [13:0] word_in = {fm_phase[13:6], fm_radius[5], fm_radius[4:0] == 5'd31 ? 5'd30 : fm_radius[4:0]};
    wire [13:0] word_late;
    reg [11:0] ring_at = 0;
    sdp_ram #(.WIDTH(14), .DEPTH(4096), .ABITS(12), .ZERO(0)) delay (
        .clk(clk40), .we(third == 0), .waddr(ring_at), .wdata(word_in), .raddr(ring_at + 12'd1), .rdata(word_late));
    reg [13:0] fm_word = 0;
    reg [1:0] mark_phase = 0;
    reg [11:0] mark_now = 0;
    wire [31:0] out_n = word_n - DELAY;
    always @(posedge clk40) begin
        if (third == 0) begin
            ring_at <= ring_at + 1'd1;
            word_n <= word_n + 1'd1;
        end
        // The delayed word of sample out_n has landed by third 2; it goes out as the phase updates.
        if (third == 2) begin
            fm_word <= word_late;
            if (mark_phase == 1) begin
                fm_word[5:0] <= {mark_now[10], mark_now[9], mark_now[8:5]};
                mark_phase <= 2;
            end else if (mark_phase == 2) begin
                fm_word[5:0] <= {1'b0, mark_now[4:0]};
                mark_phase <= 0;
            end else if (marks_waiting && mark_at[mark_head] == out_n) begin
                fm_word[5:0] <= {mark_fifo[mark_head][11], 5'd31};
                mark_now <= mark_fifo[mark_head];
                mark_head <= mark_head + 1'd1;
                mark_phase <= 1;
            end else if (marks_waiting && $signed(mark_at[mark_head] - out_n) < 0) begin
                mark_head <= mark_head + 1'd1;
            end
        end
    end

    // Offsets: the mean of each lane, as 2v+1, over 65536 samples. Magnitude: the mean over 32768.
    reg [15:0] tally = 0;
    reg signed [23:0] sum_i = 0, sum_q = 0;
    reg [23:0] sum_mag = 0;
    reg [8:0] mean_mag = 0;
    always @(posedge clk40) begin
        tally <= tally + 1'd1;
        if (tally == 16'hffff) begin
            dc_i_auto <= (sum_i + $signed({1'b0, lanes40[6:0], 1'b1}) + 24'sd32768) >>> 16;
            dc_q_auto <= (sum_q + $signed({1'b0, lanes40[13:7], 1'b1}) + 24'sd32768) >>> 16;
            sum_i <= 0;
            sum_q <= 0;
        end else begin
            sum_i <= sum_i + $signed({lanes40[6:0], 1'b1});
            sum_q <= sum_q + $signed({lanes40[13:7], 1'b1});
        end
        if (tally[14:0] == 15'h7fff) begin
            mean_mag <= (sum_mag + magnitude) >> 15;
            sum_mag <= 0;
        end else begin
            sum_mag <= sum_mag + magnitude;
        end
    end

    // Edges counted in their own clocks as Gray codes, which cross into clk_27m one bit change at a time.
    reg [15:0] taken = 0, seen = 0;
    always @(posedge q) taken <= taken + 1'd1;
    always @(posedge c5_clk) seen <= seen + 1'd1;
    wire [15:0] taken_g = taken ^ (taken >> 1), seen_g = seen ^ (seen >> 1);
    reg [15:0] taken_g1 = 0, taken_g2 = 0, seen_g1 = 0, seen_g2 = 0;

    function [15:0] from_gray(input [15:0] g);
        integer i;
        begin
            from_gray[15] = g[15];
            for (i = 14; i >= 0; i = i - 1) from_gray[i] = from_gray[i + 1] ^ g[i];
        end
    endfunction

    reg [14:0] ms_count = 0;
    reg [15:0] taken_last = 0, seen_last = 0, taken_per_ms = 0, seen_per_ms = 0;
    reg [1:0] en27 = 0;
    reg half = 0;
    always @(posedge clk_27m) begin
        taken_g1 <= taken_g;
        taken_g2 <= taken_g1;
        seen_g1 <= seen_g;
        seen_g2 <= seen_g1;
        en27 <= {en27[0], p4_en};
        count40_g1 <= count40_g;
        count40_g2 <= count40_g1;
        lock40_s <= {lock40_s[1:0], lock40};
        if (lock40_s[2] && !lock40_s[1] && losses40 != 8'hff) losses40 <= losses40 + 1'd1;
        half <= ~half;
        if (ms_count == 15'd26999) begin
            ms_count <= 0;
            taken_per_ms <= from_gray(taken_g2) - taken_last;
            seen_per_ms <= from_gray(seen_g2) - seen_last;
            taken_last <= from_gray(taken_g2);
            seen_last <= from_gray(seen_g2);
            count40_per_ms <= from_gray(count40_g2) - count40_last;
            count40_last <= from_gray(count40_g2);
        end else begin
            ms_count <= ms_count + 1'd1;
        end
    end

    wire sda_low;
    wire [7:0] reg_addr, wr_data;
    wire wr, rd;
    reg [7:0] rd_data;
    reg [7:0] control = 0;
    reg [87:0] cp_held = 0;
    reg [7:0] cp_decisions_hi = 0;
    reg [13:0] pattern = 0;
    reg [7:0] held = 0;

    i2c_target #(.ADDR(7'h42)) i2c (
        .clk(clk_27m), .scl_in(i2c_scl), .sda_in(i2c_sda), .sda_low(sda_low),
        .reg_addr(reg_addr), .wr(wr), .wr_data(wr_data), .rd(rd), .rd_data(rd_data)
    );
    assign i2c_sda = sda_low ? 1'b0 : 1'bz;

    always @(*) begin
        case (reg_addr)
            8'h00: rd_data = "P";
            8'h01: rd_data = "V";
            8'h02: rd_data = BUILD;
            8'h03: rd_data = {3'd0, lock40_s[2], control[0], taken_per_ms != 0, en27[1], lock};
            8'h04: rd_data = taken_per_ms[7:0];
            8'h05: rd_data = held;
            8'h06: rd_data = seen_per_ms[7:0];
            8'h07: rd_data = held;
            8'h08: rd_data = control;
            8'h09: rd_data = pattern[7:0];
            8'h0a: rd_data = {2'd0, pattern[13:8]};
            8'h0b: rd_data = {4'd0, phase40};
            8'h0c: rd_data = count40_per_ms[7:0];
            8'h0d: rd_data = held;
            8'h0e: rd_data = losses40;
            8'h10: rd_data = dc_i;
            8'h11: rd_data = dc_q;
            8'h12: rd_data = mean_mag[7:0];
            8'h13: rd_data = held;
            8'h14: rd_data = cp_peak[7:0];
            8'h15: rd_data = cp_held[15:8];
            8'h16: rd_data = cp_held[23:16];
            8'h17: rd_data = cp_held[31:24];
            8'h18: rd_data = cp_held[39:32];
            8'h19: rd_data = cp_held[47:40];
            8'h1a: rd_data = cp_held[55:48];
            8'h1b: rd_data = cp_held[63:56];
            8'h1c: rd_data = cp_held[71:64];
            8'h1d: rd_data = cp_held[79:72];
            8'h1e: rd_data = cp_held[87:80];
            8'h1f: rd_data = cp_decisions_hi;
            8'h20: rd_data = last_hits[7:0];
            8'h21: rd_data = {6'd0, fell_behind, last_vsync};
            8'h22: rd_data = hz_peak[7:0];
            8'h23: rd_data = {5'd0, hz_peak[10:8]};
            8'h24: rd_data = hz_theta[7:0];
            8'h25: rd_data = hz_theta[15:8];
            8'h26: rd_data = hz_agc;
            8'h27: rd_data = hz_symbols[7:0];
            8'h28: rd_data = {4'd0, hz_symbols[11:8]};
            8'h29: rd_data = {5'd0, WITH_CVBS != 0, WITH_HDZ != 0, WITH_FM != 0};
            default: rd_data = 8'h00;
        endcase
    end

    always @(posedge clk_27m) begin
        if (rd && reg_addr == 8'h04) held <= taken_per_ms[15:8];
        if (rd && reg_addr == 8'h06) held <= seen_per_ms[15:8];
        if (rd && reg_addr == 8'h0c) held <= count40_per_ms[15:8];
        if (wr && reg_addr == 8'h0b) phase40 <= wr_data[3:0];
        if (wr && reg_addr == 8'h08) control <= wr_data;
        if (rd && reg_addr == 8'h14) begin
            cp_held <= {cp_decisions[7:0], 5'd0, cp_bin, cp_total, cp_peak};
            cp_decisions_hi <= cp_decisions[15:8];
        end
        if (wr && reg_addr == 8'h10) dc_i_set <= wr_data;
        if (wr && reg_addr == 8'h11) dc_q_set <= wr_data;
        if (rd && reg_addr == 8'h12) held <= {7'd0, mean_mag[8]};
        if (wr && reg_addr == 8'h09) pattern[7:0] <= wr_data;
        if (wr && reg_addr == 8'h0a) pattern[13:8] <= wr_data[5:0];
    end

    // The P4 samples on PCLK's rising edge, the clean clock's falling one, half a period on.
    // Pattern mode wins over FM, which wins over the raw lanes.
    wire own_clock = control[1];
    wire hzs = WITH_HDZ && control[6] && !control[0];
    wire raw40 = control[4] && !control[0] && !hzs;
    wire fm = WITH_FM && control[2] && !control[0] && !raw40 && !hzs;
    wire drive = own_clock || fm || raw40 || hzs ? en27[1] : en[1];
    // Raw 40 MS/s: the lanes change on clk40's rise, and the P4 samples on PCLK's, clk40's fall.
    assign p4_pclk = drive ? (own_clock ? ~half : raw40 || hzs ? ~clk40 : fm ? pclk_fm : ~q) : 1'bz;
    // The 40 MHz words leave from a register, not through the mode mux: the P4 samples half a clk40 later, and
    // the mux's delay cost spectra grabs repeated words where idle ones belonged.
    // Control bit 7 sends count x 0x2d5b (mod 2^14), one a clk40, in place of either stream, to measure the link:
    // the odd factor keeps it invertible and toggles about half the lanes every clock, as spectra do.
    // Raw and spectra words have each lane's top bit flipped on alternate clocks: switching noise gives the P4 an
    // extra PCLK edge now and then (0.4% of words doubled, none lost: tools/linktest.py), and with every word
    // differing from the last the P4 can drop the doubles (iq.c). Flipping only the top bits (+-64) keeps a real
    // pair of samples from looking like a double, where flipping every bit (x to -x) did 176 times a million.
    reg [13:0] bus40 = 0, count14 = 0;
    reg alt = 0;
    always @(posedge clk40) begin
        count14 <= count14 + 14'h2d5b;
        alt <= ~alt;
        bus40 <= control40[7] ? count14 : (hzs ? {hz_q, hz_i} : lanes40) ^ (alt ? 14'h2040 : 14'h0);
    end
    assign p4_d    = drive ? (control[0] ? pattern : hzs || raw40 ? bus40 : fm ? (WITH_CVBS && control[5] ? fm_word : {fm_phase[13:6], fm_radius}) : lanes) : 14'bz;

endmodule
