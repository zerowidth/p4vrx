// 1,024-point FFT, radix-2^2 single-path delay feedback: five stage pairs, natural order in, bit-reversed out,
// each butterfly halving. One sample per ce; a symbol's bins come out while the next symbol goes in. Bit for bit
// with fft_r22 in models/hdz_front.py.
module hdz_fft (
    input  wire               clk,
    input  wire               ce,
    input  wire        [9:0]  in_pos,
    input  wire signed [17:0] in_re,
    input  wire signed [17:0] in_im,
    output wire        [9:0]  out_pos,
    output wire signed [17:0] out_re,
    output wire signed [17:0] out_im
);
    wire [9:0] p [0:4];
    wire signed [17:0] r [0:4], i [0:4];
    hdz_r22 #(.L(1024), .LB(10), .TWIDDLE(1), .TWFILE("fpga/rtl/hdz_tw1024.hex")) s0 (clk, ce, in_pos, in_re, in_im, p[0], r[0], i[0]);
    hdz_r22 #(.L(256), .LB(8), .TWIDDLE(1), .TWFILE("fpga/rtl/hdz_tw256.hex")) s1 (clk, ce, p[0], r[0], i[0], p[1], r[1], i[1]);
    hdz_r22 #(.L(64), .LB(6), .TWIDDLE(1), .TWFILE("fpga/rtl/hdz_tw64.hex")) s2 (clk, ce, p[1], r[1], i[1], p[2], r[2], i[2]);
    hdz_r22 #(.L(16), .LB(4), .TWIDDLE(1), .TWFILE("fpga/rtl/hdz_tw16.hex")) s3 (clk, ce, p[2], r[2], i[2], p[3], r[3], i[3]);
    hdz_r22 #(.L(4), .LB(2), .TWIDDLE(0)) s4 (clk, ce, p[3], r[3], i[3], p[4], r[4], i[4]);
    assign out_pos = p[4];
    assign out_re = r[4];
    assign out_im = i[4];
endmodule
