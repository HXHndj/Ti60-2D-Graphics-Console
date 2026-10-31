`include "gfx_abi.vh"
// AW and W progress independently. DONE is strictly after the B handshake.
module gfx_write_dma #(parameter TIMEOUT_CYCLES=10000000)(
 input wire clk,rst_n,input wire req_valid,output wire req_ready,
 input wire [31:0] req_addr,input wire [8:0] req_beats,
 input wire [127:0] in_data,input wire [15:0] in_strb,input wire in_valid,output wire in_ready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,
 output wire [127:0] wdata,output wire [15:0] wstrb,output wire wlast,wvalid,input wire wready,
 input wire [1:0] bresp,input wire bvalid,output wire bready,
 output reg done,output reg [7:0] error,output wire busy
);
 reg active,addr_pending,data_done;reg [31:0] addr,wait_cycles;reg [8:0] beats,sent;
 assign req_ready=!active;assign busy=active;
 assign awaddr=addr;assign awlen=beats[7:0]-8'd1;assign awsize=4;assign awburst=1;assign awvalid=addr_pending;
 assign wdata=in_data;assign wstrb=in_strb;assign wlast=(sent==beats-1);
 assign wvalid=active&&!data_done&&in_valid;assign in_ready=active&&!data_done&&wready;
 assign bready=active&&!addr_pending&&data_done;
 wire progress=(awvalid&&awready)||(wvalid&&wready)||(bvalid&&bready);
 always @(posedge clk)begin
  if(!rst_n)begin active<=0;addr_pending<=0;data_done<=0;done<=0;error<=0;addr<=0;beats<=1;sent<=0;wait_cycles<=0;end
  else begin
   done<=0;
   if(req_valid&&req_ready)begin active<=1;addr_pending<=1;data_done<=0;addr<=req_addr;beats<=req_beats;sent<=0;error<=0;wait_cycles<=0;end
   if(awvalid&&awready)addr_pending<=0;
   if(active)begin
    if(progress)wait_cycles<=0;
    else if(wait_cycles<TIMEOUT_CYCLES)wait_cycles<=wait_cycles+1'b1;
    else if(error==0)error<=`GFX_ERR_TIMEOUT;
   end
   if(wvalid&&wready)begin sent<=sent+1'b1;if(wlast)data_done<=1;end
   if(bvalid&&bready)begin active<=0;done<=1;if(bresp!=0&&error==0)error<=`GFX_ERR_WRITE;end
  end
 end
endmodule
