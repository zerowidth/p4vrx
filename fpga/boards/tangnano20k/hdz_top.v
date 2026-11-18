// The HDZero image: fm_top with the HDZero front end (fpga/rtl/hdz_front.v) in place of FM demod and CVBS sync,
// which don't fit beside it. Analog still decodes on the P4 from the raw lanes. Registers as fm_top's.
module hdz_top (
    input  wire        clk_27m,
    output wire        spk_en,
    input  wire        c5_clk,
    input  wire [6:0]  c5_q,
    input  wire [6:0]  c5_i,
    output wire        p4_pclk,
    output wire [13:0] p4_d,
    input  wire        p4_en,
    input  wire        i2c_scl,
    inout  wire        i2c_sda
);
    fm_top #(.BUILD(8'h07), .WITH_FM(0), .WITH_CVBS(0), .WITH_HDZ(1)) top (
        .clk_27m(clk_27m), .spk_en(spk_en), .c5_clk(c5_clk), .c5_q(c5_q), .c5_i(c5_i), .p4_pclk(p4_pclk), .p4_d(p4_d),
        .p4_en(p4_en), .i2c_scl(i2c_scl), .i2c_sda(i2c_sda)
    );
endmodule
