// passthru_lockout_top with an I2C register file at 0x42 on the P4's touch bus, for the P4's self-check:
//   0x00-0x01 "PV", 0x02 build
//   0x03 status: bit 0 PLL lock, 1 bus enable (IRQ wire), 2 C5 clock taken in the last ms, 3 lanes from the pattern
//   0x04-0x05 C5 clock edges taken per ms, 0x06-0x07 C5 clock edges seen per ms (low byte first; reading the
//             low byte holds the high byte for the next read)
//   0x08 control: bit 0 lanes from the pattern, bit 1 PCLK from the 27 MHz clock halved rather than the C5's
//   0x09-0x0a lane pattern, D7..D0 then D13..D8
// The bus is still driven only while the P4 holds IRQ high.
module lockout_regs_top #(
    parameter [7:0] BUILD = 8'h01
) (
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
    assign spk_en = 1'b0;

    wire fast, lock;
    rPLL #(
        .FCLKIN("27"), .IDIV_SEL(0), .FBDIV_SEL(4), .ODIV_SEL(8), .DEVICE("GW2A-18C")
    ) pll (
        .CLKIN(clk_27m), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
        .PSDA(4'd0), .DUTYDA(4'd0), .FDLY(4'd0), .CLKOUT(fast), .CLKOUTP(), .LOCK(lock), .CLKOUTD(), .CLKOUTD3()
    );

    // The lockout, as passthru_lockout_top.
    reg armed = 1, clr = 0, q = 0;
    always @(posedge c5_clk or posedge clr)
        if (clr) q <= 1'b0;
        else q <= q | armed;

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

    // Edges counted in their own clocks as Gray codes, which cross into clk_27m one bit change at a time.
    reg [15:0] taken = 0, seen = 0;
    always @(posedge q) taken <= taken + 1'd1;
    always @(posedge c5_clk) seen <= seen + 1'd1;
    wire [15:0] taken_g = taken ^ (taken >> 1), seen_g = seen ^ (seen >> 1);
    reg [15:0] taken_g1 = 0, taken_g2 = 0, seen_g1 = 0, seen_g2 = 0;

    function [15:0] from_gray(input [15:0] g);
        integer i;
        begin
            from_gray[15] = g[15];
            for (i = 14; i >= 0; i = i - 1) from_gray[i] = from_gray[i + 1] ^ g[i];
        end
    endfunction

    reg [14:0] ms_count = 0;
    reg [15:0] taken_last = 0, seen_last = 0, taken_per_ms = 0, seen_per_ms = 0;
    reg [1:0] en27 = 0;
    reg half = 0;
    always @(posedge clk_27m) begin
        taken_g1 <= taken_g;
        taken_g2 <= taken_g1;
        seen_g1 <= seen_g;
        seen_g2 <= seen_g1;
        en27 <= {en27[0], p4_en};
        half <= ~half;
        if (ms_count == 15'd26999) begin
            ms_count <= 0;
            taken_per_ms <= from_gray(taken_g2) - taken_last;
            seen_per_ms <= from_gray(seen_g2) - seen_last;
            taken_last <= from_gray(taken_g2);
            seen_last <= from_gray(seen_g2);
        end else begin
            ms_count <= ms_count + 1'd1;
        end
    end

    wire sda_low;
    wire [7:0] reg_addr, wr_data;
    wire wr, rd;
    reg [7:0] rd_data;
    reg [1:0] control = 0;
    reg [13:0] pattern = 0;
    reg [7:0] held = 0;

    i2c_target #(.ADDR(7'h42)) i2c (
        .clk(clk_27m), .scl_in(i2c_scl), .sda_in(i2c_sda), .sda_low(sda_low),
        .reg_addr(reg_addr), .wr(wr), .wr_data(wr_data), .rd(rd), .rd_data(rd_data)
    );
    assign i2c_sda = sda_low ? 1'b0 : 1'bz;

    always @(*) begin
        case (reg_addr)
            8'h00: rd_data = "P";
            8'h01: rd_data = "V";
            8'h02: rd_data = BUILD;
            8'h03: rd_data = {4'd0, control[0], taken_per_ms != 0, en27[1], lock};
            8'h04: rd_data = taken_per_ms[7:0];
            8'h05: rd_data = held;
            8'h06: rd_data = seen_per_ms[7:0];
            8'h07: rd_data = held;
            8'h08: rd_data = {6'd0, control};
            8'h09: rd_data = pattern[7:0];
            8'h0a: rd_data = {2'd0, pattern[13:8]};
            default: rd_data = 8'h00;
        endcase
    end

    always @(posedge clk_27m) begin
        if (rd && reg_addr == 8'h04) held <= taken_per_ms[15:8];
        if (rd && reg_addr == 8'h06) held <= seen_per_ms[15:8];
        if (wr && reg_addr == 8'h08) control <= wr_data[1:0];
        if (wr && reg_addr == 8'h09) pattern[7:0] <= wr_data;
        if (wr && reg_addr == 8'h0a) pattern[13:8] <= wr_data[5:0];
    end

    // The P4 samples on PCLK's rising edge, the clean clock's falling one, half a period on.
    wire own_clock = control[1];
    wire drive = own_clock ? en27[1] : en[1];
    assign p4_pclk = drive ? (own_clock ? ~half : ~q) : 1'bz;
    assign p4_d    = drive ? (control[0] ? pattern : lanes) : 14'bz;

endmodule
