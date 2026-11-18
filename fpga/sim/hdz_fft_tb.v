// Feeds hdz_fft the symbols in hdz_fft_in.txt (re im per line, 1,024 a symbol) with random gaps in ce, and writes
// each complete output symbol to hdz_fft_out.txt in output order, for models/hdz_fft_check.py.
`timescale 1ns/1ps
module hdz_fft_tb;
    reg clk = 0;
    always #12.5 clk = ~clk;
    reg ce = 0;
    reg [9:0] pos = 0;
    reg signed [17:0] re = 0, im = 0;
    wire [9:0] opos;
    wire signed [17:0] ore, oim;
    hdz_fft dut (.clk(clk), .ce(ce), .in_pos(pos), .in_re(re), .in_im(im), .out_pos(opos), .out_re(ore), .out_im(oim));
    integer fin, fout, n, a, b, nsym, total, started, seed;
    initial begin
        seed = 7;
        fin = $fopen("hdz_fft_in.txt", "r");
        fout = $fopen("hdz_fft_out.txt", "w");
        n = $fscanf(fin, "%d\n", nsym);
        total = 0;
        started = 0;
        // the symbols, then two of zeros to flush the last out
        while (total < (nsym + 2) * 1024) begin
            @(negedge clk);
            if ($random(seed) % 4 == 0) begin
                ce = 0;
            end else begin
                if (total < nsym * 1024) n = $fscanf(fin, "%d %d\n", a, b);
                else begin a = 0; b = 0; end
                re = a;
                im = b;
                pos = total % 1024;
                ce = 1;
                total = total + 1;
            end
        end
        @(negedge clk) ce = 0;
        $fclose(fout);
        $finish;
    end
    // Outputs are the registered values seen at each ce; the pipeline is 3L/4 + 3 events a stage pair, 1,038 in all.
    integer events = 0;
    always @(posedge clk) if (ce) begin
        if (events >= 1038 && events < 1038 + nsym * 1024) begin
            if ((events - 1038) % 1024 == 0 && opos != 0) $display("symbol start at pos %0d", opos);
            $fwrite(fout, "%0d %0d\n", ore, oim);
        end
        events = events + 1;
    end
endmodule
