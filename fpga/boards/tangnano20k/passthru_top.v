// C5 to P4 pass-through: registers the 14 I/Q lanes on the C5's sample clock and forwards them, with the
// clock as PCLK, to the P4's D13..D0 (Q7..Q1 then I7..I1). The P4 enables the bus by holding the IRQ line
// high, so nothing reaches its GPIO 39-48 before it has powered that bank, and a P4 in reset sees high-Z.
module passthru_top (
    input  wire        clk_27m,
    output wire        spk_en,
    input  wire        c5_clk,
    input  wire [6:0]  c5_q,
    input  wire [6:0]  c5_i,
    output wire        p4_pclk,
    output wire [13:0] p4_d,
    input  wire        p4_en,
    input  wire        i2c_scl,
    input  wire        i2c_sda
);
    assign spk_en = 1'b0;

    reg [13:0] lanes = 0;
    always @(posedge c5_clk) lanes <= {c5_q, c5_i};

    // Sampled in the C5 clock domain so the output enable never changes mid-word.
    reg [1:0] en = 0;
    always @(posedge c5_clk) en <= {en[0], p4_en};

    // The P4 samples on PCLK's rising edge, half a period after the lanes change.
    assign p4_pclk = en[1] ? ~c5_clk : 1'bz;
    assign p4_d    = en[1] ? lanes : 14'bz;

    (* keep *) IBUF ib_scl (.I(i2c_scl), .O());
    (* keep *) IBUF ib_sda (.I(i2c_sda), .O());
endmodule
