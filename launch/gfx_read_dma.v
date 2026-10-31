`include "gfx_abi.vh"
// One outstanding 128-bit INCR burst, 1..256 beats. Address is aligned.
// Timeout reports a fault but never retracts a VALID or abandons accepted data.
module gfx_read_dma #(parameter TIMEOUT_CYCLES=10000000,FIFO_DEPTH=16,FIFO_REGISTERS=0)(
 input wire clk,rst_n, input wire req_valid,output wire req_ready,
 input wire [31:0] req_addr,input wire [8:0] req_beats,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,
 output wire [1:0] arburst,output wire arvalid,input wire arready,
 input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [127:0] out_data,output wire out_valid,input wire out_ready,
 output reg done,output reg [7:0] error,output wire busy
);
 reg active,addr_pending,received_last;
 reg [31:0] addr,wait_cycles;reg [8:0] beats,received,consumed;
 localparam FW=(FIFO_DEPTH<2?1:$clog2(FIFO_DEPTH));
 wire [130:0] fdata;wire fvalid,fready;wire [FW:0] count;
 assign req_ready=!active;assign busy=active;
 assign araddr=addr;assign arlen=beats[7:0]-8'd1;assign arsize=4;assign arburst=1;
 assign arvalid=addr_pending;
 assign rready=active&&!addr_pending&&!received_last&&fready;
 assign out_data=fdata[127:0];assign out_valid=fvalid;
 gfx_fifo #(.WIDTH(131),.DEPTH(FIFO_DEPTH),.REGISTER_STORAGE(FIFO_REGISTERS)) fifo(clk,rst_n,1'b0,rvalid&&rready,fready,{rlast,rresp,rdata},fvalid,out_ready,fdata,count);
 wire progress=(arvalid&&arready)||(rvalid&&rready)||(out_valid&&out_ready);
 always @(posedge clk)begin
  if(!rst_n)begin active<=0;addr_pending<=0;done<=0;error<=0;addr<=0;beats<=1;received<=0;consumed<=0;received_last<=0;wait_cycles<=0;end
  else begin
   done<=0;
   if(req_valid&&req_ready)begin active<=1;addr_pending<=1;addr<=req_addr;beats<=req_beats;error<=0;received<=0;consumed<=0;received_last<=0;wait_cycles<=0;end
   if(arvalid&&arready)addr_pending<=0;
   if(active)begin
    if(progress)wait_cycles<=0;
    else if(wait_cycles<TIMEOUT_CYCLES)wait_cycles<=wait_cycles+1'b1;
    else if(error==0)error<=`GFX_ERR_TIMEOUT;
   end
   if(rvalid&&rready)begin
    received<=received+1'b1;
    if(rresp!=0&&error==0)error<=`GFX_ERR_READ;
    if(rlast!=(received==beats-1)&&error==0)error<=`GFX_ERR_PROTOCOL;
    if(rlast)received_last<=1;
   end
   if(out_valid&&out_ready)begin
    consumed<=consumed+1'b1;
    if(fdata[130])begin active<=0;done<=1;end
   end
  end
 end
endmodule
