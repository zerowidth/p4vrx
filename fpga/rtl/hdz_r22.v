// One radix-2^2 stage pair of the R2^2SDF FFT, for blocks of L samples: BF2I (span L/2), BF2II (span L/4, with -j
// on the second operand for the k1 = 1 quarter pass), then the twiddle W_L^(n3 (k1 + 2 k2)) in Q15. Each butterfly
// halves. Bit for bit with fft_r22 in models/hdz_front.py. Everything moves on ce; pos is the element's index in
// its 1,024-sample symbol, carried along with the data.
module hdz_r22 #(
    parameter L = 1024,
    parameter LB = 10,
    parameter TWIDDLE = 1,
    parameter TWFILE = "hdz_tw1024.hex"
) (
    input  wire               clk,
    input  wire               ce,
    input  wire        [9:0]  in_pos,
    input  wire signed [17:0] in_re,
    input  wire signed [17:0] in_im,
    output reg         [9:0]  out_pos = 0,
    output reg  signed [17:0] out_re = 0,
    output reg  signed [17:0] out_im = 0
);
    localparam H = L / 2, Q = L / 4;

    // BF2I: the first half of each block waits in the ring; the second half adds and subtracts against it, the
    // difference going back into the ring to come out during the next block's first half.
    wire [LB-1:0] c1 = in_pos[LB-1:0];
    wire first1 = c1 < H;
    wire [35:0] r1;
    wire signed [17:0] a1_re = r1[35:18], a1_im = r1[17:0];
    wire signed [18:0] s1_re = a1_re + in_re, s1_im = a1_im + in_im;
    wire signed [18:0] d1_re = a1_re - in_re, d1_im = a1_im - in_im;
    wire [35:0] w1 = first1 ? {in_re, in_im} : {d1_re[18:1], d1_im[18:1]};
    generate
        if (H > 1) begin : g1
            ce_ring #(.WIDTH(36), .DEPTH(H), .ABITS(LB - 1)) ring1 (.clk(clk), .ce(ce), .wdata(w1), .rdata(r1));
        end else begin : g1r
            reg [35:0] q1 = 0;
            always @(posedge clk) if (ce) q1 <= w1;
            assign r1 = q1;
        end
    endgenerate
    reg [9:0] p1 = 0;
    reg signed [17:0] o1_re = 0, o1_im = 0;
    always @(posedge clk) if (ce) begin
        o1_re <= first1 ? a1_re : s1_re[18:1];
        o1_im <= first1 ? a1_im : s1_im[18:1];
        p1 <= in_pos - H;
    end

    // BF2II on BF2I's stream: span Q, and the k1 = 1 pass (the second half of each L) takes -j b.
    wire [LB-1:0] c2 = p1[LB-1:0];
    wire k1 = c2 >= H;
    wire first2 = (c2 % H) < Q;
    wire signed [17:0] b_re = k1 ? o1_im : o1_re;
    wire signed [17:0] b_im = k1 ? -o1_re : o1_im;
    wire [35:0] r2;
    wire signed [17:0] a2_re = r2[35:18], a2_im = r2[17:0];
    wire signed [18:0] s2_re = a2_re + b_re, s2_im = a2_im + b_im;
    wire signed [18:0] d2_re = a2_re - b_re, d2_im = a2_im - b_im;
    // the first half stores the plain sample; -j applies to the operand of the second half only
    wire [35:0] w2 = first2 ? {o1_re, o1_im} : {d2_re[18:1], d2_im[18:1]};
    generate
        if (Q > 1) begin : g2
            ce_ring #(.WIDTH(36), .DEPTH(Q), .ABITS(LB - 2)) ring2 (.clk(clk), .ce(ce), .wdata(w2), .rdata(r2));
        end else begin : g2r
            reg [35:0] q2 = 0;
            always @(posedge clk) if (ce) q2 <= w2;
            assign r2 = q2;
        end
    endgenerate
    reg [9:0] p2 = 0;
    reg signed [17:0] o2_re = 0, o2_im = 0;
    always @(posedge clk) if (ce) begin
        o2_re <= first2 ? a2_re : s2_re[18:1];
        o2_im <= first2 ? a2_im : s2_im[18:1];
        p2 <= p1 - Q;
    end

    generate
        if (TWIDDLE) begin : gt
            // Element p2 of the block: quarter 2 k1 + k2, n3 within it; exponent n3 (k1 + 2 k2). The table is
            // read for the element the next event brings, as ce_ring reads.
            localparam EB = LB;
            reg [31:0] rom [0:3 * (Q - 1)];
            initial $readmemh(TWFILE, rom);
            wire [9:0] p2n = p1 - Q;
            wire [LB-1:0] e_now = p2[LB-1:0], e_next = p2n[LB-1:0];
            // n3 (k1 + 2 k2) by shift and add: no multiplier for a factor of 0 to 3
            function [EB-1:0] expo(input [LB-1:0] e);
                reg [1:0] quarter;
                reg [EB-1:0] n3;
                begin
                    quarter = e / Q;
                    n3 = e % Q;
                    expo = (quarter[1] ? n3 : {EB{1'b0}}) + (quarter[0] ? n3 << 1 : {EB{1'b0}});
                end
            endfunction
            reg [31:0] w = 0;
            always @(posedge clk) w <= rom[ce ? expo(e_next) : expo(e_now)];
            wire signed [15:0] w_re = w[15:0], w_im = w[31:16];
            wire signed [34:0] m_re = o2_re * w_re - o2_im * w_im + (35'sd1 <<< 14);
            wire signed [34:0] m_im = o2_re * w_im + o2_im * w_re + (35'sd1 <<< 14);
            always @(posedge clk) if (ce) begin
                out_re <= m_re >>> 15;
                out_im <= m_im >>> 15;
                out_pos <= p2;
            end
        end else begin : gn
            always @(posedge clk) if (ce) begin
                out_re <= o2_re;
                out_im <= o2_im;
                out_pos <= p2;
            end
        end
    endgenerate
endmodule
