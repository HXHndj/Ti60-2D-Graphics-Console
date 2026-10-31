// Shared 16-beat, 128-bit source-row storage for the legacy and bucket clients.
// Keep the same Efinix simple dual-port RAM and one-cycle OUTPUT_REG latency
// used by gfx_bucket_engine's original srow_ram instance.
module gfx_source_row_ram(
 input wire clk,rst_n,
 input wire wr_en,input wire [3:0] wr_addr,input wire [127:0] wr_data,
 input wire [3:0] rd_addr,output wire [127:0] rd_data
);
 wire unused_reset = rst_n;
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(4),.OUTPUT_REG("TRUE")) source_rows(
  .wdata(wr_data),.waddr(wr_addr),.raddr(rd_addr),.we(wr_en),.wclk(clk),
  .re(1'b1),.rclk(clk),.rdata(rd_data));
endmodule
