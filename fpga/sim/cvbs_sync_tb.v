// Feeds /tmp/sync_in.hex (int8 phase steps, two hex digits each) through cvbs_sync at one every 3 clocks, and
// writes each line (start frac hit vline parity) and field (hits vsync period) to /tmp/sync_out.txt.
`timescale 1ns / 1ps
module cvbs_sync_tb;
    reg clk = 0;
    always #12.5 clk = ~clk;
    reg [7:0] steps [0:2999999];
    integer n, k, fd;
    reg take = 0;
    reg [7:0] step = 0;
    wire lv, lh, lp, fv, fvs, over;
    wire [31:0] ls, period;
    wire [15:0] lf;
    wire [8:0] lvl, fh;
    reg overran = 0;
    cvbs_sync dut (.clk(clk), .take(take), .step(step), .line_valid(lv), .line_start(ls), .line_frac(lf), .line_hit(lh),
                   .line_vline(lvl), .line_parity(lp), .field_valid(fv), .field_hits(fh), .field_vsync(fvs),
                   .period_out(period), .overrun(over));
    always @(posedge clk) begin
        if (lv) $fdisplay(fd, "L %0d %0d %0d %0d %0d", ls, lf, lh, lvl, lp);
        if (fv) $fdisplay(fd, "F %0d %0d %0d", fh, fvs, period);
        if (over && !overran) begin overran = 1; $display("overrun at sample %0d", k); end
    end
    initial begin
        if (!$value$plusargs("n=%d", n)) n = 1000;
        $readmemh("/tmp/sync_in.hex", steps);
        fd = $fopen("/tmp/sync_out.txt", "w");
        for (k = 0; k < n; k = k + 1) begin
            @(negedge clk);
            take = 1;
            step = steps[k];
            @(negedge clk);
            take = 0;
            @(negedge clk);
        end
        repeat (20000) @(negedge clk);
        $fclose(fd);
        $finish;
    end
endmodule
