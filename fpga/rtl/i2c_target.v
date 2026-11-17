// I2C target with an 8-bit register pointer: a write sets the pointer then writes registers from it, a read
// returns registers from it, and both step the pointer per byte. No clock stretching; SCL and SDA are
// oversampled on clk, which must run at 20x SCL or more.
module i2c_target #(
    parameter [6:0] ADDR = 7'h42
) (
    input  wire       clk,
    input  wire       scl_in,
    input  wire       sda_in,
    output reg        sda_low = 0,
    output reg  [7:0] reg_addr = 0,
    output reg        wr = 0,
    output reg  [7:0] wr_data = 0,
    // High for one clk as a byte is taken from rd_data, which reads reg_addr.
    output reg        rd = 0,
    input  wire [7:0] rd_data
);
    // Two flops against metastability, then three agreeing samples against glitches.
    reg [4:0] scl_s = 5'h1f, sda_s = 5'h1f;
    reg scl = 1, sda = 1;
    always @(posedge clk) begin
        scl_s <= {scl_s[3:0], scl_in};
        sda_s <= {sda_s[3:0], sda_in};
        if (scl_s[4:2] == 3'b111) scl <= 1;
        else if (scl_s[4:2] == 3'b000) scl <= 0;
        if (sda_s[4:2] == 3'b111) sda <= 1;
        else if (sda_s[4:2] == 3'b000) sda <= 0;
    end

    reg scl_d = 1, sda_d = 1;
    wire scl_rise = scl && !scl_d, scl_fall = !scl && scl_d;
    wire start = scl && scl_d && sda_d && !sda;
    wire stop = scl && scl_d && !sda_d && sda;

    localparam IDLE = 0, ADDRESS = 1, POINTER = 2, WRITE = 3, ACK = 4, READ = 5, READ_ACK = 6;
    reg [2:0] state = IDLE, after_ack = IDLE;
    reg [3:0] bits = 0;
    reg [7:0] shift = 0;
    reg       master_ack = 0;

    always @(posedge clk) begin
        scl_d <= scl;
        sda_d <= sda;
        wr <= 0;
        rd <= 0;
        if (start) begin
            state <= ADDRESS;
            bits <= 0;
            sda_low <= 0;
        end else if (stop) begin
            state <= IDLE;
            sda_low <= 0;
        end else if (scl_rise) begin
            if (state == ADDRESS || state == POINTER || state == WRITE) begin
                shift <= {shift[6:0], sda};
                bits <= bits + 1'd1;
            end else if (state == READ) begin
                bits <= bits + 1'd1;
            end else if (state == READ_ACK) begin
                master_ack <= !sda;
            end
        end else if (scl_fall) begin
            case (state)
                ADDRESS:
                    if (bits == 8) begin
                        if (shift[7:1] == ADDR) begin
                            sda_low <= 1;
                            state <= ACK;
                            after_ack <= shift[0] ? READ : POINTER;
                        end else begin
                            state <= IDLE;
                        end
                    end
                POINTER:
                    if (bits == 8) begin
                        reg_addr <= shift;
                        sda_low <= 1;
                        state <= ACK;
                        after_ack <= WRITE;
                    end
                WRITE:
                    if (bits == 8) begin
                        wr <= 1;
                        wr_data <= shift;
                        sda_low <= 1;
                        state <= ACK;
                        after_ack <= WRITE;
                    end
                ACK: begin
                    bits <= 0;
                    if (after_ack == READ) begin
                        shift <= rd_data;
                        rd <= 1;
                        sda_low <= !rd_data[7];
                        state <= READ;
                    end else begin
                        sda_low <= 0;
                        state <= after_ack;
                    end
                end
                READ:
                    if (bits == 8) begin
                        sda_low <= 0;
                        reg_addr <= reg_addr + 1'd1;
                        state <= READ_ACK;
                    end else begin
                        sda_low <= !shift[7 - bits];
                    end
                READ_ACK:
                    if (master_ack) begin
                        shift <= rd_data;
                        rd <= 1;
                        bits <= 0;
                        sda_low <= !rd_data[7];
                        state <= READ;
                    end else begin
                        state <= IDLE;
                    end
                default: ;
            endcase
        end
        // A write steps the pointer once its byte is taken.
        if (wr) reg_addr <= reg_addr + 1'd1;
    end
endmodule
