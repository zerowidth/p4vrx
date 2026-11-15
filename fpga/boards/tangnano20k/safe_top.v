// Breadboard-safe bitstream: every pin wired to the C5 or P4 is a high-Z input,
// pin 51 (speaker amp enable) is driven low, nothing else is driven.
module safe_top (
    input  wire        clk_27m,
    output wire        spk_en,
    input  wire        c5_clk,
    input  wire [6:0]  c5_q,     // Q7..Q1
    input  wire [6:0]  c5_i,     // I7..I1
    input  wire        p4_pclk,
    input  wire [13:0] p4_d,     // D13..D0
    input  wire        p4_irq,
    input  wire        i2c_scl,
    input  wire        i2c_sda
);
    assign spk_en = 1'b0;

    // Explicit, kept input buffers so every wired pin is configured as an input,
    // not left to the packer's default for unused pins.
    localparam N_IN = 1 + 7 + 7 + 1 + 14 + 1 + 2;
    wire [N_IN-1:0] pads = {c5_clk, c5_q, c5_i, p4_pclk, p4_d, p4_irq, i2c_scl, i2c_sda};
    genvar k;
    generate
        for (k = 0; k < N_IN; k = k + 1) begin : in_buf
            (* keep *) IBUF ib (.I(pads[k]), .O());
        end
    endgenerate

    (* keep *) reg [24:0] heartbeat = 25'd0;
    always @(posedge clk_27m) heartbeat <= heartbeat + 25'd1;
endmodule
