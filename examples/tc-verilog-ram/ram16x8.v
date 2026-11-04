module ram16x8(
    input wire clk,
    input wire write_enable,
    input wire read_enable,
    input wire [3:0] address,
    input wire [7:0] write_data,
    output wire [7:0] read_data
);

reg [7:0] memory [0:15];

always @(posedge clk) begin
    if (write_enable)
        memory[address] <= write_data;
end

assign read_data = read_enable ? memory[address] : 8'bz;

endmodule
