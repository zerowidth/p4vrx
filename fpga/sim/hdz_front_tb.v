// Feeds hdz_front the raw words in hdz_front_in.txt (hex, one 40 MS/s sample a clock) and writes every non-idle
// bus word to hdz_front_out.txt as the P4 would capture it (I byte 2v+1 low, Q byte high), for
// models/hdz_front_check.py.
`timescale 1ns/1ps
module hdz_front_tb;
    reg clk = 0;
    always #12.5 clk = ~clk;
    reg in_v = 0;
    reg [6:0] in_i = 0, in_q = 0;
    wire [6:0] o_i, o_q;
    wire [10:0] peak;
    wire [15:0] theta;
    wire [7:0] agc;
    wire [11:0] symbols;
    hdz_front dut (.clk(clk), .in_v(in_v), .in_i(in_i), .in_q(in_q), .out_i(o_i), .out_q(o_q), .peak_bin(peak),
                   .theta(theta), .agc(agc), .symbols(symbols));
    integer fin, fout, n, w, count;
    initial begin
        fin = $fopen("hdz_front_in.txt", "r");
        fout = $fopen("hdz_front_out.txt", "w");
        count = 0;
        while (!$feof(fin)) begin
            n = $fscanf(fin, "%h\n", w);
            @(negedge clk);
            in_i = w[7:1];
            in_q = w[15:9];
            in_v = 1;
            count = count + 1;
        end
        @(negedge clk) in_v = 0;
        repeat (4000) @(negedge clk);
        $display("%0d samples, %0d symbols, peak %0d, theta %0d, agc %0d", count, symbols, peak, theta, agc);
        $fclose(fout);
        $finish;
    end
    always @(posedge clk) if (!(o_i == 7'h40 && o_q == 7'h00))
        $fwrite(fout, "%04x\n", {o_q, 1'b1, o_i, 1'b1});
endmodule
