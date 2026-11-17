// Feeds a capture's words (hex, I low byte and Q high, each 2v+1) through fm_demod at one a clock, keeping
// every third output, and writes them as decimal for models/fm_demod_check.py to compare with the model.
`timescale 1ns / 1ps
module fm_demod_tb;
    reg clk = 0;
    always #12.5 clk = ~clk;

    reg [15:0] words [0:1048575];
    integer n, k, fd, phase;
    reg signed [7:0] dc_i, dc_q;
    reg [15:0] w = 0;
    reg take = 0;
    wire signed [13:0] out;
    wire [8:0] mag;

    fm_demod dut (.clk(clk), .in_i(w[7:1]), .in_q(w[15:9]), .dc_i(dc_i), .dc_q(dc_q), .take(take), .out(out), .magnitude(mag));

    initial begin
        if (!$value$plusargs("n=%d", n)) n = 1000;
        if (!$value$plusargs("dci=%d", k)) k = 0;
        dc_i = k;
        if (!$value$plusargs("dcq=%d", k)) k = 0;
        dc_q = k;
        $readmemh("/tmp/fm_in.hex", words);
        fd = $fopen("/tmp/fm_out.txt", "w");
        for (k = 0; k < n + 40; k = k + 1) begin
            @(negedge clk);
            w = k < n ? words[k] : 16'h0101;
            take = k % 3 == 0;
            @(posedge clk);
            #1 if (take) $fdisplay(fd, "%0d", out);
        end
        $fclose(fd);
        $finish;
    end
endmodule
