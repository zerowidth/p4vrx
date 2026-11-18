// A delay of DEPTH events: on each clock with ce, `rdata` is what was written DEPTH events earlier, and `wdata`
// takes its place. The read address runs one ahead while ce is high, so a block RAM's registered read is ready
// whether events come every clock or with gaps.
module ce_ring #(
    parameter WIDTH = 36,
    parameter DEPTH = 512,
    parameter ABITS = 9
) (
    input  wire             clk,
    input  wire             ce,
    input  wire [WIDTH-1:0] wdata,
    output reg  [WIDTH-1:0] rdata = 0
);
    reg [WIDTH-1:0] mem [0:DEPTH-1];
    integer k;
    initial for (k = 0; k < DEPTH; k = k + 1) mem[k] = 0;
    reg [ABITS-1:0] ptr = 0;
    wire [ABITS-1:0] next = ptr == DEPTH - 1 ? {ABITS{1'b0}} : ptr + 1'd1;
    wire [ABITS-1:0] raddr = ce ? next : ptr;
    always @(posedge clk) begin
        rdata <= mem[raddr];
        if (ce) begin
            mem[ptr] <= wdata;
            ptr <= next;
        end
    end
endmodule
