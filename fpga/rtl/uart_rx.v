// 8N1 receiver; valid pulses for one clock with each byte.
module uart_rx #(parameter DIV = 234) (
    input  wire       clk,
    input  wire       rx,
    output reg  [7:0] data = 0,
    output reg        valid = 0
);
    reg [2:0]  sync = 3'b111;
    reg [15:0] cnt = 0;
    reg [3:0]  bitn = 0;
    reg [7:0]  shift = 0;
    wire       line = sync[2];

    always @(posedge clk) begin
        sync <= {sync[1:0], rx};
        valid <= 1'b0;
        if (bitn == 0) begin
            if (!line) begin
                bitn <= 4'd1;
                cnt <= DIV / 2;
            end
        end else if (cnt == DIV - 1) begin
            cnt <= 0;
            if (bitn == 4'd1 && line) begin
                bitn <= 0;  // glitch, not a start bit
            end else if (bitn == 4'd10) begin
                bitn <= 0;
                if (line) begin
                    data <= shift;
                    valid <= 1'b1;
                end
            end else begin
                if (bitn != 4'd1) shift <= {line, shift[7:1]};
                bitn <= bitn + 4'd1;
            end
        end else begin
            cnt <= cnt + 16'd1;
        end
    end
endmodule
