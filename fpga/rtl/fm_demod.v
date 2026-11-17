// FM demod of the C5's 7+7-bit I/Q at the bus rate: offsets off, phase by a 10-stage vectoring CORDIC,
// phase difference, a 15-tap low-pass, and one output in three as a 14-bit frequency (2.44 kHz an LSB at
// 40 MS/s). Bit for bit as models/fm_demod.py. One sample a clock; `take` marks the clocks whose filter
// output is kept, and `out` changes the clock after.
module fm_demod (
    input  wire               clk,
    input  wire        [6:0]  in_i,
    input  wire        [6:0]  in_q,
    input  wire signed [7:0]  dc_i,
    input  wire signed [7:0]  dc_q,
    input  wire               take,
    output reg  signed [13:0] out = 0,
    output reg         [8:0]  magnitude = 0
);
    localparam STAGES = 10;

    function [13:0] atan(input integer k);
        case (k)
            0: atan = 2048; 1: atan = 1209; 2: atan = 639; 3: atan = 324; 4: atan = 163;
            5: atan = 81;   6: atan = 41;   7: atan = 20;  8: atan = 10;  9: atan = 5;
            default: atan = 0;
        endcase
    endfunction

    // The lanes as the P4 reads them, 2v+1, less the offsets: 9 bits signed.
    reg signed [8:0] i0 = 0, q0 = 0;
    always @(posedge clk) begin
        i0 <= $signed({in_i, 1'b1}) - dc_i;
        q0 <= $signed({in_q, 1'b1}) - dc_q;
    end

    // Into the right half plane, then CORDIC. x and y carry 2 fraction bits and grow by 1.65.
    reg signed [13:0] x [0:STAGES];
    reg signed [13:0] y [0:STAGES];
    reg        [13:0] z [0:STAGES];
    always @(posedge clk) begin
        x[0] <= i0[8] ? -($signed({i0, 2'b0})) : $signed({i0, 2'b0});
        y[0] <= i0[8] ? -($signed({q0, 2'b0})) : $signed({q0, 2'b0});
        z[0] <= i0[8] ? 14'd8192 : 14'd0;
    end
    genvar k;
    generate
        for (k = 0; k < STAGES; k = k + 1) begin : stage
            always @(posedge clk) begin
                if (!y[k][13]) begin
                    x[k + 1] <= x[k] + (y[k] >>> k);
                    y[k + 1] <= y[k] - (x[k] >>> k);
                    z[k + 1] <= z[k] + atan(k);
                end else begin
                    x[k + 1] <= x[k] - (y[k] >>> k);
                    y[k + 1] <= y[k] + (x[k] >>> k);
                    z[k + 1] <= z[k] - atan(k);
                end
            end
        end
    endgenerate

    // 12-bit phase, and its step since the last sample, signed.
    wire [11:0] phase = z[STAGES][13:2];
    reg  [11:0] last = 0;
    reg  signed [11:0] step = 0;
    always @(posedge clk) begin
        last <= phase;
        step <= phase - last;
        magnitude <= x[STAGES][13:5];
    end

    // The last 15 steps, newest first, and the taps, which sum to 1024.
    reg signed [11:0] d [0:14];
    always @(posedge clk) d[0] <= step;
    generate
        for (k = 1; k < 15; k = k + 1) begin : delay
            always @(posedge clk) d[k] <= d[k - 1];
        end
    endgenerate
    function signed [9:0] tap(input integer n);
        case (n)
            0, 14: tap = -1;  1, 13: tap = -6;  2, 12: tap = -15; 3, 11: tap = -11;
            4, 10: tap = 36;  5, 9: tap = 133;  6, 8: tap = 236;  7: tap = 280;
            default: tap = 0;
        endcase
    endfunction

    // Symmetric: each pair of steps shares a tap.
    reg signed [22:0] sum;
    integer j;
    always @(*) begin
        sum = $signed(d[7]) * tap(7);
        for (j = 0; j < 7; j = j + 1) sum = sum + ($signed(d[j]) + $signed(d[14 - j])) * tap(j);
    end
    wire signed [14:0] scaled = sum >>> 8;
    always @(posedge clk)
        if (take) out <= scaled > 15'sd8191 ? 14'sd8191 : scaled < -15'sd8192 ? -14'sd8192 : scaled[13:0];
endmodule
