// Pin mapping test. Reads every wired pin and reports it over the BL616 UART (115200 8N1, pins 69/70).
// Drives only P4-facing pins, one command at a time, so the P4 must have that pin as an input.
// Commands: 'r' reports "c5=XXXX p4=XXXX i2c=X"; 'z' releases every pin; 'h' or 'l' then 'a'+n drives
// P4-facing pin n (0 PCLK, 1..14 D13..D0, 15 IRQ, 16 SCL, 17 SDA) high or low. I2C pins are only pulled low.
module pintest_top (
    input  wire        clk_27m,
    output wire        spk_en,
    output wire        uart_tx,
    input  wire        uart_rx,
    input  wire        c5_clk,
    input  wire [6:0]  c5_q,
    input  wire [6:0]  c5_i,
    inout  wire        p4_pclk,
    inout  wire [13:0] p4_d,
    inout  wire        p4_irq,
    inout  wire        i2c_scl,
    inout  wire        i2c_sda
);
    assign spk_en = 1'b0;

    reg [17:0] oe = 0;
    reg [17:0] val = 0;
    assign p4_pclk = oe[17] ? val[17] : 1'bz;
    genvar k;
    generate
        for (k = 0; k < 14; k = k + 1) begin : d
            assign p4_d[13 - k] = oe[16 - k] ? val[16 - k] : 1'bz;
        end
    endgenerate
    assign p4_irq  = oe[2] ? val[2] : 1'bz;
    assign i2c_scl = oe[1] ? 1'b0 : 1'bz;
    assign i2c_sda = oe[0] ? 1'b0 : 1'bz;

    // Bit 17 is PCLK and bit 0 SDA, so pin n of the protocol is bit 17 - n.
    wire [14:0] c5_raw = {c5_clk, c5_q, c5_i};
    wire [15:0] p4_raw = {p4_pclk, p4_d, p4_irq};
    wire [1:0]  i2c_raw = {i2c_scl, i2c_sda};
    reg  [32:0] s1 = 0, s2 = 0;
    always @(posedge clk_27m) begin
        s1 <= {c5_raw, p4_raw, i2c_raw};
        s2 <= s1;
    end

    wire [7:0] rx_data;
    wire       rx_valid;
    uart_rx urx (.clk(clk_27m), .rx(uart_rx), .data(rx_data), .valid(rx_valid));

    reg  [7:0] tx_data = 0;
    reg        tx_start = 0;
    wire       tx_busy;
    uart_tx utx (.clk(clk_27m), .start(tx_start), .data(tx_data), .tx(uart_tx), .busy(tx_busy));

    function [7:0] hex(input [3:0] n);
        hex = n < 10 ? 8'd48 + n : 8'd87 + n;
    endfunction

    // "c5=XXXX p4=XXXX i2c=X\r\n": 23 characters.
    reg [32:0] snap = 0;
    reg [4:0]  idx = 0;
    reg        sending = 0;
    reg [7:0]  op = 0;
    wire [15:0] c5w = {1'b0, snap[32:18]};
    wire [15:0] p4w = snap[17:2];
    reg [7:0] ch;
    always @(*) begin
        case (idx)
            0: ch = "c"; 1: ch = "5"; 2: ch = "=";
            3: ch = hex(c5w[15:12]); 4: ch = hex(c5w[11:8]); 5: ch = hex(c5w[7:4]); 6: ch = hex(c5w[3:0]);
            7: ch = " "; 8: ch = "p"; 9: ch = "4"; 10: ch = "=";
            11: ch = hex(p4w[15:12]); 12: ch = hex(p4w[11:8]); 13: ch = hex(p4w[7:4]); 14: ch = hex(p4w[3:0]);
            15: ch = " "; 16: ch = "i"; 17: ch = "2"; 18: ch = "c"; 19: ch = "=";
            20: ch = hex({2'b00, snap[1:0]}); 21: ch = 8'd13; default: ch = 8'd10;
        endcase
    end

    wire [4:0] n = rx_data[4:0] - 5'd1;  // 'a' is 0x61
    always @(posedge clk_27m) begin
        tx_start <= 1'b0;
        if (rx_valid) begin
            if (op == "h" || op == "l") begin
                if (rx_data >= "a" && rx_data <= "r") begin
                    oe[17 - n] <= 1'b1;
                    val[17 - n] <= op == "h";
                end
                op <= 0;
            end else if (rx_data == "z") begin
                oe <= 0;
            end else if (rx_data == "r" && !sending) begin
                snap <= s2;
                idx <= 0;
                sending <= 1'b1;
            end else begin
                op <= rx_data;
            end
        end
        if (sending && !tx_busy && !tx_start) begin
            tx_data <= ch;
            tx_start <= 1'b1;
            if (idx == 22) sending <= 1'b0;
            else idx <= idx + 5'd1;
        end
    end
endmodule
