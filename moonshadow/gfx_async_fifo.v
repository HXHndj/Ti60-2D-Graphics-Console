// Power-of-two dual-clock FIFO, registered read prefetch. Both domains must be
// reset together. Gray synchronizers need physical bus-skew/max-delay constraints.
module gfx_async_fifo #(parameter WIDTH=128,AW=9)(
 input wire wclk,wrst_n,input wire wvalid,output wire wready,input wire [WIDTH-1:0] wdata,
 input wire rclk,rrst_n,output reg rvalid,input wire rready,output reg [WIDTH-1:0] rdata,
 output wire rempty,output wire [AW:0] wlevel
);
 reg [WIDTH-1:0] mem[0:(1<<AW)-1];
 reg [AW:0] wb,rb;
 // A registered Gray value must cross the domain. Retiming these flops can
 // expose a combinational binary-to-Gray network to the synchronizer.
 (* syn_keep=1 *) reg [AW:0] wg,rg;
 (* async_reg="true" *)reg [AW:0] rg1,rg2,wg1,wg2;
 reg full;wire push=wvalid&&wready;
 wire [AW:0] wn=wb+push,wgn=(wn>>1)^wn;
 wire empty=rg==wg2;
 wire pop=(!rvalid||rready)&&!empty;
 wire [AW:0] rn=rb+pop,rgn=(rn>>1)^rn;
 assign wready=!full;assign rempty=empty&&!rvalid;
 function [AW:0] ungray(input [AW:0] a);integer j;begin ungray[AW]=a[AW];for(j=AW-1;j>=0;j=j-1)ungray[j]=ungray[j+1]^a[j];end endfunction
 assign wlevel=wb-ungray(rg2);
 always @(posedge wclk)begin
  if(!wrst_n)begin wb<=0;wg<=0;rg1<=0;rg2<=0;full<=0;end
  else begin
   rg1<=rg;rg2<=rg1;wb<=wn;wg<=wgn;
   full<=wgn=={~rg2[AW:AW-1],rg2[AW-2:0]};
   if(push)mem[wb[AW-1:0]]<=wdata;
  end
 end
 always @(posedge rclk)begin
  if(!rrst_n)begin rb<=0;rg<=0;wg1<=0;wg2<=0;rvalid<=0;rdata<=0;end
  else begin
   wg1<=wg;wg2<=wg1;rb<=rn;rg<=rgn;
   if(!rvalid||rready)begin
    rvalid<=!empty;
    if(!empty)rdata<=mem[rb[AW-1:0]];
   end
  end
 end
endmodule
