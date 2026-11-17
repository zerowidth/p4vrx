// iverilog -o /tmp/i2c_tb fpga/sim/i2c_target_tb.v fpga/rtl/i2c_target.v && vvp /tmp/i2c_tb
`timescale 1ns / 1ps
module i2c_target_tb;
    reg clk = 0;
    always #18.5 clk = ~clk; // 27 MHz

    reg scl = 1, m_low = 0;
    wire t_low;
    wire sda = !(m_low || t_low);

    wire [7:0] reg_addr, wr_data;
    wire wr, rd;
    reg [7:0] regs [0:15];
    integer i;
    initial for (i = 0; i < 16; i = i + 1) regs[i] = 8'h10 + i;
    always @(posedge clk) if (wr) regs[reg_addr[3:0]] <= wr_data;

    i2c_target #(.ADDR(7'h42)) dut (
        .clk(clk), .scl_in(scl), .sda_in(sda), .sda_low(t_low),
        .reg_addr(reg_addr), .wr(wr), .wr_data(wr_data), .rd(rd), .rd_data(regs[reg_addr[3:0]])
    );

    localparam Q = 625; // quarter of a 400 kHz SCL period
    task start; begin m_low = 0; scl = 1; #Q; m_low = 1; #Q; scl = 0; #Q; end endtask
    task stop; begin m_low = 1; #Q; scl = 1; #Q; m_low = 0; #(2 * Q); end endtask
    task send(input [7:0] b, output ack);
        integer k;
        begin
            for (k = 7; k >= 0; k = k - 1) begin
                m_low = !b[k]; #Q; scl = 1; #(2 * Q); scl = 0; #Q;
            end
            m_low = 0; #Q; scl = 1; #Q; ack = !sda; #Q; scl = 0; #Q;
        end
    endtask
    task recv(input more, output [7:0] b);
        integer k;
        begin
            m_low = 0;
            for (k = 7; k >= 0; k = k - 1) begin
                #Q; scl = 1; #Q; b[k] = sda; #Q; scl = 0; #Q;
            end
            m_low = more; #Q; scl = 1; #(2 * Q); scl = 0; #Q; m_low = 0;
        end
    endtask

    reg ack;
    reg [7:0] b;
    integer errors = 0;
    initial begin
        #2000;
        // Write 0xa5, 0x5a to registers 9 and 10.
        start; send({7'h42, 1'b0}, ack); if (!ack) errors = errors + 1;
        send(8'h09, ack); send(8'ha5, ack); send(8'h5a, ack); stop;
        if (regs[9] !== 8'ha5 || regs[10] !== 8'h5a) begin errors = errors + 1; $display("write: %h %h", regs[9], regs[10]); end
        // Another address gets no ack.
        start; send({7'h5d, 1'b0}, ack); if (ack) errors = errors + 1; stop;
        // Read registers 8..11 after a repeated start.
        start; send({7'h42, 1'b0}, ack); send(8'h08, ack);
        start; send({7'h42, 1'b1}, ack); if (!ack) errors = errors + 1;
        for (i = 8; i < 12; i = i + 1) begin
            recv(i < 11, b);
            if (b !== regs[i]) begin errors = errors + 1; $display("read %0d: %h, want %h", i, b, regs[i]); end
        end
        stop;
        if (t_low) begin errors = errors + 1; $display("SDA held after stop"); end
        if (errors) $display("FAIL: %0d errors", errors); else $display("PASS");
        $finish;
    end
endmodule
