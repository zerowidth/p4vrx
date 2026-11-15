// 8N1 transmitter; DIV is clock cycles per bit.
module uart_tx #(parameter DIV = 234) (
    input  wire       clk,
    input  wire       start,
    input  wire [7:0] data,
    output reg        tx = 1'b1,
    output wire       busy
);
    reg [15:0] cnt = 0;
    reg [3:0]  bitn = 0;
    reg [8:0]  shift = 9'h1ff;
    assign busy = bitn != 0;

    always @(posedge clk) begin
        if (!busy) begin
            if (start) begin
                shift <= {1'b1, data};
                tx <= 1'b0;
                bitn <= 4'd10;
                cnt <= 0;
            end
        end else if (cnt == DIV - 1) begin
            cnt <= 0;
            bitn <= bitn - 4'd1;
            tx <= shift[0];
            shift <= {1'b1, shift[8:1]};
        end else begin
            cnt <= cnt + 16'd1;
        end
    end
endmodule
