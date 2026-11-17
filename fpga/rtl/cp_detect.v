// HDZero's cyclic prefix: each sample against the one LAG earlier, summed over WINDOW, |c| folded by the
// symbol period into BINS bins over SPAN samples. Each decision gives the fold's peak, its total and the
// peak's bin; peak x BINS / total near 1 is anything, 3-5 is HDZero. Bit for bit as nano_fold in
// models/cp_detect.py. One sample a clock while `valid`.
module cp_detect #(
    parameter LAG = 1517,
    parameter WINDOW = 48,
    parameter BINS = 1565,
    parameter SPAN = 100120,
    // round(2^32 / 1564.39)
    parameter [31:0] STEP = 32'd2745458
) (
    input  wire              clk,
    input  wire              valid,
    input  wire        [6:0] in_i,
    input  wire        [6:0] in_q,
    input  wire signed [7:0] dc_i,
    input  wire signed [7:0] dc_q,
    output reg        [31:0] peak = 0,
    output reg        [31:0] total = 0,
    output reg        [10:0] peak_bin = 0,
    output reg        [15:0] decisions = 0
);
    // Stage 1: the lanes as 2v+1, less the offsets, halved.
    reg v1 = 0;
    reg signed [7:0] x_i = 0, x_q = 0;
    wire signed [8:0] d_i = $signed({in_i, 1'b1}) - dc_i, d_q = $signed({in_q, 1'b1}) - dc_q;
    always @(posedge clk) begin
        v1 <= valid;
        x_i <= d_i >>> 1;
        x_q <= d_q >>> 1;
    end

    // Stage 2: the sample LAG earlier, from a ring read before it is overwritten.
    reg [15:0] ring [0:LAG - 1];
    integer r;
    initial for (r = 0; r < LAG; r = r + 1) ring[r] = 0;
    reg [10:0] at = 0;
    reg [15:0] old = 0;
    reg v2 = 0;
    reg signed [7:0] a_i = 0, a_q = 0;
    always @(posedge clk) begin
        v2 <= v1;
        if (v1) begin
            old <= ring[at];
            ring[at] <= {x_i, x_q};
            at <= at == LAG - 1 ? 11'd0 : at + 1'd1;
            a_i <= x_i;
            a_q <= x_q;
        end
    end
    wire signed [7:0] b_i = old[15:8], b_q = old[7:0];

    // Stage 3: the product, and the one WINDOW earlier, for the moving sum.
    reg v3 = 0;
    reg signed [16:0] p_re = 0, p_im = 0;
    always @(posedge clk) begin
        v3 <= v2;
        p_re <= a_i * b_i + a_q * b_q;
        p_im <= a_q * b_i - a_i * b_q;
    end
    // A shift chain in flops: as block RAM its address missed hold by 0.12 ns.
    reg [33:0] past [0:WINDOW - 1];
    initial for (r = 0; r < WINDOW; r = r + 1) past[r] = 0;
    reg signed [16:0] q_re = 0, q_im = 0, n_re = 0, n_im = 0;
    reg v4 = 0;
    always @(posedge clk) begin
        v4 <= v3;
        if (v3) begin
            {q_re, q_im} <= past[WINDOW - 1];
            n_re <= p_re;
            n_im <= p_im;
        end
    end
    genvar g;
    generate
        for (g = 0; g < WINDOW; g = g + 1) begin : chain
            always @(posedge clk)
                if (v3) past[g] <= g == 0 ? {p_re, p_im} : past[g - 1];
        end
    endgenerate

    // Stage 5: the moving sums, then |c| as max + min/2.
    reg signed [23:0] c_re = 0, c_im = 0;
    reg v5 = 0, v6 = 0;
    always @(posedge clk) begin
        v5 <= v4;
        if (v4) begin
            c_re <= c_re + n_re - q_re;
            c_im <= c_im + n_im - q_im;
        end
    end
    wire [23:0] m_re = c_re[23] ? -c_re : c_re, m_im = c_im[23] ? -c_im : c_im;
    reg [24:0] mag = 0;
    always @(posedge clk) begin
        v6 <= v5;
        mag <= m_re > m_im ? m_re + (m_im >> 1) : m_im + (m_re >> 1);
    end

    // The bin of each sample, from a phase counter that starts at 0 with the first one.
    reg [31:0] phase = 0;
    reg [10:0] bin1 = 0, bin2 = 0, bin3 = 0, bin4 = 0, bin5 = 0;
    wire [42:0] scaled = phase * BINS;
    always @(posedge clk) begin
        if (v1) phase <= phase + STEP;
        if (v1) bin1 <= scaled[42:32];
        if (v2) bin2 <= bin1;
        if (v3) bin3 <= bin2;
        if (v4) bin4 <= bin3;
        if (v5) bin5 <= bin4;
    end

    // Two folds: one accumulates SPAN samples while the other is read for its peak and total and cleared.
    // Accumulating, a bank is read at the bin and written back the clock after; scanning, it is read in order
    // and each word cleared the clock after.
    reg bank = 0;
    reg [16:0] count = 0;
    reg [24:0] mag7 = 0;
    reg [10:0] bin7 = 0;
    reg v7 = 0, bank7 = 0;
    reg scanning = 0, scan_v = 0;
    reg [10:0] scan_at = 0, scan_bin = 0, best_bin = 0;
    reg [31:0] best = 0, sum = 0;
    wire scan_bank = ~bank;
    // The last sample into a bank is written the clock after the swap; its scan waits for that.
    wire scan_go = scanning && !(v7 && bank7 == scan_bank);
    wire [31:0] rd0, rd1;
    wire [31:0] acc_old = bank7 ? rd1 : rd0;
    wire [31:0] scan_word = scan_bank ? rd1 : rd0;
    sdp_ram fold0 (
        .clk(clk),
        .we((v7 && !bank7) || (scan_v && !scan_bank)),
        .waddr(v7 && !bank7 ? bin7 : scan_bin),
        .wdata(v7 && !bank7 ? acc_old + mag7 : 32'd0),
        .raddr(bank ? scan_at : bin5),
        .rdata(rd0)
    );
    sdp_ram fold1 (
        .clk(clk),
        .we((v7 && bank7) || (scan_v && scan_bank)),
        .waddr(v7 && bank7 ? bin7 : scan_bin),
        .wdata(v7 && bank7 ? acc_old + mag7 : 32'd0),
        .raddr(bank ? bin5 : scan_at),
        .rdata(rd1)
    );
    always @(posedge clk) begin
        v7 <= v6;
        bank7 <= bank;
        bin7 <= bin5;
        mag7 <= mag;
        if (v6) begin
            if (count == SPAN - 1) begin
                count <= 0;
                bank <= ~bank;
                scanning <= 1;
                scan_at <= 0;
            end else begin
                count <= count + 1'd1;
            end
        end
        scan_v <= 0;
        if (scan_go) begin
            scan_bin <= scan_at;
            scan_v <= 1;
            if (scan_at == BINS - 1) scanning <= 0;
            scan_at <= scan_at + 1'd1;
        end
        if (scan_v) begin
            if (scan_bin == 0) begin
                best <= scan_word;
                best_bin <= 0;
                sum <= scan_word;
            end else begin
                if (scan_word > best) begin
                    best <= scan_word;
                    best_bin <= scan_bin;
                end
                sum <= sum + scan_word;
            end
            if (scan_bin == BINS - 1) begin
                peak <= scan_word > best ? scan_word : best;
                peak_bin <= scan_word > best ? scan_bin : best_bin;
                total <= sum + scan_word;
                decisions <= decisions + 1'd1;
            end
        end
    end
endmodule
