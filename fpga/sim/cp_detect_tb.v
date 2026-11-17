// Feeds /tmp/cp_in.hex (capture words, I low byte and Q high, each 2v+1) through cp_detect and writes each
// decision's peak, total and bin to /tmp/cp_out.txt for models/cp_detect_check.py.
`timescale 1ns / 1ps
module cp_detect_tb;
    reg clk = 0;
    always #12.5 clk = ~clk;
    reg [15:0] words [0:524287];
    integer n, k, fd;
    reg signed [7:0] dc_i, dc_q;
    reg [15:0] w = 0;
    reg valid = 0;
    wire [31:0] peak, total;
    wire [10:0] bin;
    wire [15:0] decisions;
    reg [15:0] seen = 0;
    cp_detect dut (.clk(clk), .valid(valid), .in_i(w[7:1]), .in_q(w[15:9]), .dc_i(dc_i), .dc_q(dc_q),
                   .peak(peak), .total(total), .peak_bin(bin), .decisions(decisions));
    initial begin
        if (!$value$plusargs("n=%d", n)) n = 1000;
        if (!$value$plusargs("dci=%d", k)) k = 0;
        dc_i = k;
        if (!$value$plusargs("dcq=%d", k)) k = 0;
        dc_q = k;
        $readmemh("/tmp/cp_in.hex", words);
        fd = $fopen("/tmp/cp_out.txt", "w");
        for (k = 0; k < n + 3000; k = k + 1) begin
            @(negedge clk);
            valid = k < n;
            w = k < n ? words[k] : 16'h0101;
            @(posedge clk);
            #1 if (decisions != seen) begin
                seen = decisions;
                $fdisplay(fd, "%0d %0d %0d", peak, total, bin);
            end
        end
        $fclose(fd);
        $finish;
    end
endmodule
