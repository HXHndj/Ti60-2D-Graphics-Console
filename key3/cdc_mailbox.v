// Bundled-data two-way mailbox. Source payload stays fixed until destination
// consumption is acknowledged. Resets must be coordinated across both domains.
module cdc_mailbox #(parameter WIDTH=64)(
 input wire sclk,srst_n,svalid,output wire sready,input wire [WIDTH-1:0] sdata,
 input wire dclk,drst_n,output reg dvalid,input wire dready,output reg [WIDTH-1:0] ddata
);
 reg [WIDTH-1:0] hold_data;reg request,acknowledge,seen;
 (* async_reg="true" *)reg [1:0] ack_sync,request_sync;
 assign sready=request==ack_sync[1];
 always @(posedge sclk)begin
  if(!srst_n)begin request<=0;ack_sync<=0;hold_data<=0;end
  else begin ack_sync<={ack_sync[0],acknowledge};if(svalid&&sready)begin hold_data<=sdata;request<=~request;end end
 end
 always @(posedge dclk)begin
  if(!drst_n)begin acknowledge<=0;seen<=0;request_sync<=0;dvalid<=0;ddata<=0;end
  else begin
   request_sync<={request_sync[0],request};
   if(request_sync[1]!=seen&&!dvalid)begin ddata<=hold_data;dvalid<=1;seen<=request_sync[1];end
   if(dvalid&&dready)begin dvalid<=0;acknowledge<=seen;end
  end
 end
endmodule
