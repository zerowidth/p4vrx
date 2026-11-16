// C5 to P4 pass-through that ignores extra edges on the C5's clock. Crosstalk from the lanes adds edges to
// it, and each one would be an extra sample at the P4. A C5 clock edge sets the clean clock only once the
// lockout has passed since the last one it took; a 135 MHz count from the board's oscillator clears it half
// a period later and re-arms it after about 55 ns, before the next real edge at 75 ns. The rising edge comes
// straight from the C5 clock, so it adds no jitter. Otherwise as passthru_top.
module passthru_lockout_top (
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

    wire fast, lock;
    rPLL #(
        .FCLKIN("27"), .IDIV_SEL(0), .FBDIV_SEL(4), .ODIV_SEL(8), .DEVICE("GW2A-18C")
    ) pll (
        .CLKIN(clk_27m), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
        .PSDA(4'd0), .DUTYDA(4'd0), .FDLY(4'd0), .CLKOUT(fast), .CLKOUTP(), .LOCK(lock), .CLKOUTD(), .CLKOUTD3()
    );

    reg armed = 1, clr = 0, q = 0;
    always @(posedge c5_clk or posedge clr)
        if (clr) q <= 1'b0;
        else q <= q | armed;

    // Cycles of the fast clock since the clean clock was seen to rise, counted from 1 to 15.
    reg [2:0] q_s = 0;
    reg [3:0] since = 4'd15;
    always @(posedge fast) begin
        q_s <= {q_s[1:0], q};
        if (q_s[1] && !q_s[2]) since <= 4'd1;
        else if (since != 4'd15) since <= since + 4'd1;
        clr <= since >= 4'd3 && since < 4'd5;
        armed <= since >= 4'd6;
    end

    reg [13:0] lanes = 0;
    always @(posedge q) lanes <= {c5_q, c5_i};

    reg [1:0] en = 0;
    always @(posedge q) en <= {en[0], p4_en};

    // The P4 samples on PCLK's rising edge, which is the clean clock's falling one, half a period on.
    assign p4_pclk = en[1] ? ~q : 1'bz;
    assign p4_d    = en[1] ? lanes : 14'bz;

    (* keep *) IBUF ib_scl (.I(i2c_scl), .O());
    (* keep *) IBUF ib_sda (.I(i2c_sda), .O());
endmodule
