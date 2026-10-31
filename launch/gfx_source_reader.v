// Optional 32 KiB source window cache. One window tag avoids wide, replicated
// per-row tag lookups. Each 128-byte row has two 64-byte valid halves, allowing
// aligned 4- or 8-beat source requests without speculative overfetch.
// External CPU/DMA writers require an idle flush before the next batch.
// Graphics writes invalidate at AW acceptance. The engine never writes a
// source span concurrently with reading that span (validated non-overlap).
module gfx_source_reader #(parameter TIMEOUT_CYCLES=10000000,LINE_BITS=8)(
 input wire clk,rst_n,input wire req_valid,output wire req_ready,
 input wire [31:0] req_addr,input wire [4:0] req_beats,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,
 output wire [1:0] arburst,output wire arvalid,input wire arready,
 input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [127:0] out_data,output wire out_valid,input wire out_ready,
 output wire done,output wire [7:0] error,output wire busy,
 input wire request_is_source,cache_enable,cache_flush,
 input wire write_accept,input wire [31:0] write_addr,input wire [7:0] write_len,
 output reg [31:0] hits,misses,bypasses,
 // A hit is served only from RAM; the shared external read port is idle.
 output wire cache_serving
);
 localparam IDLE=0,DMA=1,HIT=2;
 reg [1:0] state;
 localparam LINES=1<<LINE_BITS;
 reg [2*LINES-1:0] valid_halves;
 reg [24-LINE_BITS:0] window_tag;
 reg [127:0] data[0:LINES*8-1];
 reg [31:0] fill_addr;
 reg fill_allowed,fill_cancelled;
 reg [4:0] fill_count;
 reg [LINE_BITS-1:0] line_index;
 reg fill_half;reg [4:0] transfer_beats;
 reg [3:0] hit_next;
 reg hit_valid,hit_done;
 reg [127:0] hit_data;
 wire [32:0] write_end={1'b0,write_addr}+(({25'b0,write_len}+33'd1)<<4);
 wire [32:0] fill_end={1'b0,fill_addr}+{24'b0,transfer_beats,4'b0};
 wire fill_overlap=write_accept&&{1'b0,write_addr}<fill_end&&write_end>{1'b0,fill_addr};
 wire request_overlap=write_accept&&{1'b0,write_addr}<({1'b0,req_addr}+{24'b0,req_beats,4'b0})&&write_end>{1'b0,req_addr};
 wire eligible=cache_enable&&!cache_flush&&request_is_source&&
  ((req_beats==8&&req_addr[6:0]==0)||(req_beats==4&&req_addr[5:0]==0));
 wire [LINE_BITS-1:0] request_line=req_addr[LINE_BITS+6:7];
 wire [LINE_BITS:0] request_half=req_addr[LINE_BITS+6:6];
 wire same_window=window_tag==req_addr[31:LINE_BITS+7];
 wire hit=eligible&&same_window&&valid_halves[request_half]&&
  (req_beats==4||valid_halves[{request_line,1'b1}])&&!request_overlap;
 wire dma_ready,dma_done,dma_busy,dma_valid;
 wire [7:0] dma_error;wire [127:0] dma_data;
 assign req_ready=state==IDLE&&dma_ready;
 assign busy=state!=IDLE;
 assign cache_serving=state==HIT;
 assign done=(state==DMA&&dma_done)||hit_done;
 assign error=state==DMA?dma_error:8'b0;
 assign out_valid=state==HIT?hit_valid:(state==DMA&&dma_valid);
 assign out_data=state==HIT?hit_data:dma_data;
 gfx_read_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.FIFO_DEPTH(2),.FIFO_REGISTERS(1)) dma(
  clk,rst_n,req_valid&&state==IDLE&&!hit,dma_ready,req_addr,{4'b0,req_beats},
  araddr,arlen,arsize,arburst,arvalid,arready,rdata,rresp,rlast,rvalid,rready,
  dma_data,dma_valid,out_ready,dma_done,dma_error,dma_busy);
 // Synchronous RAM read with an elastic output register. Neither stalls nor
 // a flush retract an accepted output; invalidation affects later requests.
 always @(posedge clk)begin
  if(state==DMA&&dma_valid&&out_ready&&fill_allowed&&fill_count<transfer_beats)
   data[{line_index,3'b0}+{fill_half,2'b0}+fill_count[2:0]]<=dma_data;
  if(state==HIT&&(!hit_valid||out_ready)&&hit_next<transfer_beats)
   hit_data<=data[{line_index,3'b0}+{fill_half,2'b0}+hit_next[2:0]];
 end
 // At most 16 beats per engine write: an unaligned 256-byte burst intersects
 // at most three cache lines. Longer streamed bursts (Fill) either miss the
 // whole 32 KiB window -- e.g. framebuffer fills against an atlas window, which
 // must retain hits -- or conservatively flush it.
 wire [31:0] window_base={window_tag,{(LINE_BITS+7){1'b0}}};
 wire [32:0] window_end={1'b0,window_base}+(33'd1<<(LINE_BITS+7));
 wire [31:0] write_line0={write_addr[31:7],7'b0};
 wire [31:0] write_line1=write_line0+32'd128;
 wire [31:0] write_line2=write_line0+32'd256;
 wire long_write_in_window={1'b0,write_addr}<window_end&&write_end>{1'b0,window_base};
 always @(posedge clk)begin
  if(!rst_n)begin
   state<=IDLE;valid_halves<=0;window_tag<=0;hits<=0;misses<=0;bypasses<=0;
   fill_allowed<=0;fill_cancelled<=0;fill_count<=0;fill_addr<=0;
   line_index<=0;fill_half<=0;transfer_beats<=0;hit_next<=0;hit_valid<=0;hit_done<=0;
  end else begin
   hit_done<=0;
   if(req_valid&&req_ready)begin
    line_index<=request_line;fill_half<=req_addr[6];transfer_beats<=req_beats;
    fill_addr<=req_addr;fill_count<=0;
    fill_allowed<=eligible;fill_cancelled<=request_overlap;
    if(hit)begin state<=HIT;hits<=hits+1'b1;hit_next<=0;hit_valid<=0;end
    else begin
     state<=DMA;
     if(eligible)begin
      misses<=misses+1'b1;
      if(!same_window)begin valid_halves<=0;window_tag<=req_addr[31:LINE_BITS+7];end
      valid_halves[request_half]<=0;
      if(req_beats==8)valid_halves[{request_line,1'b1}]<=0;
     end
     else bypasses<=bypasses+1'b1;
    end
   end
   if(state==DMA)begin
    if(dma_valid&&out_ready)fill_count<=fill_count+1'b1;
    if(dma_done)begin
     state<=IDLE;
     if(fill_allowed&&!fill_cancelled&&fill_count==transfer_beats&&dma_error==0&&cache_enable&&!cache_flush&&!fill_overlap)begin
      valid_halves[{line_index,fill_half}]<=1;
      if(transfer_beats==8)valid_halves[{line_index,1'b1}]<=1;
     end
    end
   end
   if(state==HIT&&(!hit_valid||out_ready))begin
    if(hit_next<transfer_beats)begin hit_next<=hit_next+1'b1;hit_valid<=1;end
    else begin hit_valid<=0;hit_done<=1;state<=IDLE;end
   end
   if(write_accept)begin
    if(write_len>15)begin
     if(long_write_in_window)valid_halves<=0;
    end
    else begin
     if(window_tag==write_line0[31:LINE_BITS+7])valid_halves[{write_line0[LINE_BITS+6:7],1'b0}+:2]<=0;
     if({1'b0,write_line1}<write_end&&window_tag==write_line1[31:LINE_BITS+7])valid_halves[{write_line1[LINE_BITS+6:7],1'b0}+:2]<=0;
     if({1'b0,write_line2}<write_end&&window_tag==write_line2[31:LINE_BITS+7])valid_halves[{write_line2[LINE_BITS+6:7],1'b0}+:2]<=0;
    end
    if(fill_overlap)fill_cancelled<=1;
   end
   if(cache_flush||!cache_enable)begin valid_halves<=0;fill_cancelled<=1;end
  end
 end
endmodule
