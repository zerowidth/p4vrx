// One write port and one registered read port, zeroed at configuration, for yosys to map to block RAM.
module sdp_ram #(
    parameter WIDTH = 32,
    parameter DEPTH = 1565,
    parameter ABITS = 11
) (
    input  wire             clk,
    input  wire             we,
    input  wire [ABITS-1:0] waddr,
    input  wire [WIDTH-1:0] wdata,
    input  wire [ABITS-1:0] raddr,
    output reg  [WIDTH-1:0] rdata = 0
);
    reg [WIDTH-1:0] mem [0:DEPTH-1];
    integer k;
    initial for (k = 0; k < DEPTH; k = k + 1) mem[k] = 0;
    always @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        rdata <= mem[raddr];
    end
endmodule
