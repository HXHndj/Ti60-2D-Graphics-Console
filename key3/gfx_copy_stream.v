`include "gfx_abi.vh"
// Aligned plain COPY only (eligibility is enforced by gfx_core). Two complete
// source blocks alternate ownership: FREE -> READING -> READY -> WRITING -> FREE.
// A block is never written until its entire read has succeeded. One AR and one
// AW/B may overlap; a bank is released only after its last B. Faults stop new
// work, drain both DMAs, then discard unread/unwritten buffered data. No rollback.
module gfx_copy_stream #(parameter TIMEOUT_CYCLES=10000000)(
 input wire clk,rst_n,cmd_valid,output wire cmd_ready,input wire [`GFX_CMD_BITS-1:0] cmd,
 output reg done,output reg [31:0] done_seq,output reg [7:0] fault,output reg [31:0] fault_addr,
 output reg busy,output wire recover_safe,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready,
 input wire display_urgent
);
 reg [31:0] src,dst,ss,ds,width,height,row,col,seq;
 reg next_read,next_write,read_bank,write_bank,read_running,write_running,all_read;
 reg [1:0] occupied,ready;
 reg [31:0] bank_addr[0:1];reg [8:0] bank_beats[0:1];
 reg [31:0] read_addr;reg [8:0] write_beats;
 wire rd_ready,rd_busy,rd_done,rd_valid,wr_ready,wr_busy,wr_done;
 wire [7:0] rd_error,wr_error;wire [127:0] rd_data;
 wire stop_work=fault!=0||((read_running||rd_done)&&rd_error!=0)||((write_running||wr_done)&&wr_error!=0);
 wire [31:0] cur_src=src+(col<<2),cur_dst=dst+(col<<2);
 reg [31:0] pixels,limit,page;
 always @*begin
  limit=display_urgent?64:256;
  pixels=width-col;if(pixels>limit)pixels=limit;
  page=(4096-{20'b0,cur_src[11:0]})>>2;if(pixels>page)pixels=page;
  page=(4096-{20'b0,cur_dst[11:0]})>>2;if(pixels>page)pixels=page;
 end
 wire read_req=busy&&!all_read&&!read_running&&!occupied[next_read]&&!stop_work;
 // Urgent can rise after prefetch. Split even a previously filled 64-beat bank
 // into <=16-beat writes. Once a DMA request is latched, VALID stays immutable.
 wire [8:0] write_n=(display_urgent&&bank_beats[next_write]>16)?9'd16:bank_beats[next_write];
 wire write_req=busy&&!write_running&&ready[next_write]&&!stop_work;
 wire [1:0] fv,fi,fo;wire [127:0] fd[0:1];wire [6:0] count[0:1];
 wire clear_banks=!busy;
 wire rd_accept=read_req&&rd_ready,wr_accept=write_req&&wr_ready;
 gfx_read_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.FIFO_DEPTH(2),.FIFO_REGISTERS(1)) reader(
  clk,rst_n,read_req,rd_ready,cur_src,{2'b0,pixels[8:2]},
  araddr,arlen,arsize,arburst,arvalid,arready,rdata,rresp,rlast,rvalid,rready,
  rd_data,rd_valid,fi[read_bank],rd_done,rd_error,rd_busy);
 // COPY always produces opaque output, including when source X/alpha != 255.
 wire [127:0] opaque_data={8'hff,rd_data[119:96],8'hff,rd_data[87:64],8'hff,rd_data[55:32],8'hff,rd_data[23:0]};
 wire writer_ready;
 genvar b;
 generate for(b=0;b<2;b=b+1)begin:banks
  gfx_fifo #(.WIDTH(128),.DEPTH(64),.SYNC_RAM(1)) buffer(
   clk,rst_n,clear_banks,rd_valid&&read_bank==b,fi[b],opaque_data,
   fv[b],fo[b],fd[b],count[b]);
  assign fo[b]=write_bank==b&&writer_ready;
 end endgenerate
 wire unused_data_idle;
 gfx_write_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) writer(
  clk,rst_n,write_req,wr_ready,bank_addr[next_write],write_n,
  fd[write_bank],16'hffff,fv[write_bank],writer_ready,
  awaddr,awlen,awsize,awburst,awvalid,awready,wdata,wstrb,wlast,wvalid,wready,bresp,bvalid,bready,wr_done,wr_error,wr_busy,unused_data_idle);
 assign cmd_ready=!busy;
 assign recover_safe=!busy&&!rd_busy&&!wr_busy;
 always @(posedge clk)begin
  if(!rst_n)begin
   busy<=0;done<=0;done_seq<=0;fault<=0;fault_addr<=0;
   next_read<=0;next_write<=0;read_bank<=0;write_bank<=0;
   read_running<=0;write_running<=0;occupied<=0;ready<=0;all_read<=0;
   row<=0;col<=0;src<=0;dst<=0;ss<=0;ds<=0;width<=0;height<=0;seq<=0;
   read_addr<=0;write_beats<=0;bank_addr[0]<=0;bank_addr[1]<=0;bank_beats[0]<=0;bank_beats[1]<=0;
  end else begin
   done<=0;
   if(cmd_valid&&cmd_ready)begin
    src<=cmd[32+:32];dst<=cmd[64+:32];width<=cmd[96+:32];height<=cmd[128+:32];
    ss<=cmd[160+:32];ds<=cmd[192+:32];seq<=cmd[320+:32];done_seq<=cmd[320+:32];
    row<=0;col<=0;next_read<=0;next_write<=0;occupied<=0;ready<=0;
    all_read<=0;fault<=0;fault_addr<=0;busy<=1;
   end
   if(rd_accept)begin
    read_running<=1;read_bank<=next_read;occupied[next_read]<=1;
    read_addr<=cur_src;bank_addr[next_read]<=cur_dst;bank_beats[next_read]<=pixels[8:2];
    next_read<=!next_read;
    if(col+pixels==width)begin
     col<=0;row<=row+1'b1;src<=src+ss;dst<=dst+ds;
     if(row+1==height)all_read<=1;
    end else col<=col+pixels;
   end
   if(rd_done)begin read_running<=0;if(!stop_work)ready[read_bank]<=1;end
   if(wr_accept)begin
    write_running<=1;write_bank<=next_write;write_beats<=write_n;
   end
   if(wr_done)begin
    write_running<=0;
    if(bank_beats[write_bank]==write_beats)begin
     occupied[write_bank]<=0;ready[write_bank]<=0;next_write<=!next_write;
    end else begin
     bank_beats[write_bank]<=bank_beats[write_bank]-write_beats;
     bank_addr[write_bank]<=bank_addr[write_bank]+(write_beats<<4);
    end
   end
   if(busy&&fault==0)begin
    if((read_running||rd_done)&&rd_error!=0)begin fault<=rd_error;fault_addr<=read_addr;end
    else if((write_running||wr_done)&&wr_error!=0)begin fault<=wr_error;fault_addr<=awaddr;end
   end
   // Running flags also cover the cycle between a request and DMA BUSY.
   if(busy&&!read_running&&!write_running&&!rd_busy&&!wr_busy&&
      (stop_work||(all_read&&occupied==0)))begin
    busy<=0;done<=1;done_seq<=seq;occupied<=0;ready<=0;
   end
  end
 end
endmodule
