// Resampling pass-through: a PLL locks to the C5's 13.33 MHz clock at 6x (80 MHz, the rate its bus changes
// words), samples all 14 lanes at one adjustable phase and keeps every sixth word, with PCLK made from the
// same count. The phase (16 steps a word) comes over the BL616 UART as '0'..'9', 'a'..'f'; 'r' reports
// "phase N lock L". Drives the P4 bus only while the P4 holds the IRQ wire high.
module passthru_pll_top (
    input  wire        clk_27m,
    output wire        spk_en,
    output wire        uart_tx,
    input  wire        uart_rx,
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

    reg [3:0] phase = 4'd0;
    wire clk80, clk80p, lock;
    rPLL #(
        .FCLKIN("13.333"), .IDIV_SEL(0), .FBDIV_SEL(5), .ODIV_SEL(8), .DYN_DA_EN("true"),
        .DUTYDA_SEL("1000"), .CLKFB_SEL("internal"), .DEVICE("GW2A-18C")
    ) pll (
        .CLKIN(c5_clk), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
        .PSDA(phase), .DUTYDA(4'b1000), .FDLY(4'b0), .CLKOUT(clk80), .CLKOUTP(clk80p), .LOCK(lock),
        .CLKOUTD(), .CLKOUTD3()
    );

    reg [13:0] word = 0, kept = 0;
    reg [2:0] count = 0;
    reg pclk = 0;
    reg [1:0] en = 0;
    always @(posedge clk80p) begin
        word <= {c5_q, c5_i};
        count <= count == 3'd5 ? 3'd0 : count + 3'd1;
        if (count == 3'd0) kept <= word;
        pclk <= count >= 3'd3;
        en <= {en[0], p4_en & lock};
    end
    assign p4_pclk = en[1] ? pclk : 1'bz;
    assign p4_d    = en[1] ? kept : 14'bz;

    // Phase control from the host, in the 27 MHz domain; PSDA is static between commands.
    wire [7:0] rx_data;
    wire rx_valid;
    uart_rx urx (.clk(clk_27m), .rx(uart_rx), .data(rx_data), .valid(rx_valid));
    reg [7:0] tx_data = 0;
    reg tx_start = 0;
    wire tx_busy;
    uart_tx utx (.clk(clk_27m), .start(tx_start), .data(tx_data), .tx(uart_tx), .busy(tx_busy));
    reg [1:0] lock_s = 0;
    always @(posedge clk_27m) lock_s <= {lock_s[0], lock};
    function [7:0] hex(input [3:0] n);
        hex = n < 10 ? 8'd48 + n : 8'd87 + n;
    endfunction
    reg [3:0] idx = 0;
    reg sending = 0;
    reg [7:0] ch;
    always @(*) begin
        case (idx)
            0: ch = "p"; 1: ch = "h"; 2: ch = "a"; 3: ch = "s"; 4: ch = "e"; 5: ch = " "; 6: ch = hex(phase);
            7: ch = " "; 8: ch = "l"; 9: ch = "o"; 10: ch = "c"; 11: ch = "k"; 12: ch = " "; 13: ch = lock_s[1] ? "1" : "0";
            14: ch = 8'd13; default: ch = 8'd10;
        endcase
    end
    always @(posedge clk_27m) begin
        tx_start <= 1'b0;
        if (rx_valid) begin
            if (rx_data >= "0" && rx_data <= "9") phase <= rx_data - "0";
            else if (rx_data >= "a" && rx_data <= "f") phase <= rx_data - "a" + 4'd10;
            else if (rx_data == "r" && !sending) begin
                idx <= 0;
                sending <= 1'b1;
            end
        end
        if (sending && !tx_busy && !tx_start) begin
            tx_data <= ch;
            tx_start <= 1'b1;
            if (idx == 15) sending <= 1'b0;
            else idx <= idx + 4'd1;
        end
    end

    (* keep *) IBUF ib_scl (.I(i2c_scl), .O());
    (* keep *) IBUF ib_sda (.I(i2c_sda), .O());
endmodule
