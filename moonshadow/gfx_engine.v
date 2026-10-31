`include "gfx_abi.vh"
// Ordered rectangular engine. Tile buffers separate source/background from the
// output FIFO. Each tile has real multi-beat AXI transfers (not word requests).
module gfx_engine #(parameter TIMEOUT_CYCLES=10000000,MAX_FILL_BEATS=16,SHARED_PIXEL_PIPE=0)(
 input wire clk,rst_n,input wire cmd_valid,output wire cmd_ready,input wire [`GFX_CMD_BITS-1:0] cmd,
 output reg done,output reg [31:0] done_seq,output reg [7:0] fault,output reg [31:0] fault_addr,
 output wire busy,output wire recover_safe,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready,
 input wire cache_enable,cache_flush,output wire [31:0] cache_hits,cache_misses,cache_bypasses,
 input wire display_urgent,
 output wire [31:0] debug_state,debug_seq,debug_position,debug_counts,
 output wire pixel_rst_n,pixel_in_valid,pixel_out_ready,pixel_in_last,
 output wire [127:0] pixel_src,pixel_dst,
 output wire [31:0] pixel_color,pixel_op,
 output wire [7:0] pixel_alpha,output wire [23:0] pixel_key,
 input wire [3:0] pixel_ready,pixel_valid,pixel_write,pixel_last,
 input wire [127:0] pixel_result
);
 localparam IDLE=0,PLAN=1,SRC_REQ=2,SRC_WAIT=3,BG_REQ=4,BG_WAIT=5,PIXELS=6,WAIT_PREVIOUS_WRITE=7,WRITE_WAIT=8,NEXT=9,FINISH=10,FAST_BEATS=11,ABORT_DRAIN=12,LIST_REQ=13,LIST_WAIT=14,LIST_DECODE=15;
 reg [3:0] state;
 reg [31:0] op,src,dst,width,height,ss,ds,color,alpha,key,seq;
 // Fill-list children reuse otherwise idle command registers: src is the
 // current descriptor address, key is the framebuffer base and alpha[8:0]
 // is the remaining count.  Descriptors stream one at a time: this avoids a
 // second asynchronous read port on the already large source tile buffer.
 // Pixel work and queue-command compaction are unchanged; only 16-byte list
 // read transactions are less coalesced.
 reg list_mode;reg [95:0] list_data;
 wire [15:0] list_width=list_data[47:32],list_height=list_data[63:48];
 wire [15:0] list_x=list_data[15:0],list_y=list_data[31:16];
 wire [31:0] list_color=list_data[95:64];
 wire list_entry_bad=list_width==0||list_height==0||
  {1'b0,list_x}+{1'b0,list_width}>17'd1024||{1'b0,list_y}+{1'b0,list_height}>17'd600;
 reg [31:0] row,col;
 reg [10:0] n;
 reg [4:0] feed,output_beat;
 reg [4:0] sn,rx,bg_rx;reg [8:0] dn;
 reg [31:0] sa,da;
 reg [1:0] sl,dl;
 reg [127:0] sbuf[0:15],dbuf[0:15];
 wire [31:0] cur_src=src+row*ss+(col<<2),cur_dst=dst+row*ds+(col<<2);
 wire [6:0] plan_n;wire [31:0] plan_sa,plan_da;wire [4:0] plan_sn,plan_dn;
 gfx_burst_plan planner(cur_src,cur_dst,width-col,op[1:0]!=1,plan_n,plan_sa,plan_da,plan_sn,plan_dn);
 // Fill has no source/background tile storage requirement. Stream a longer
 // write only with display headroom; never cross a row or a 4 KiB boundary.
 reg [31:0] fill_n,fill_limit,fill_page;
 always @*begin
  fill_limit=(display_urgent?16:MAX_FILL_BEATS)*4-cur_dst[3:2];
  fill_page=(4096-{20'b0,cur_dst[11:0]})>>2;
  fill_n=width-col;if(fill_n>fill_limit)fill_n=fill_limit;if(fill_n>fill_page)fill_n=fill_page;
 end
 wire rd_req_ready,rd_done,rd_busy,rd_valid,src_busy,cache_serving;wire [7:0] rd_error;wire [127:0] rd_data;
 wire wr_req_ready,wr_done,wr_busy;wire [7:0] wr_error;
 wire [31:0] source_araddr,bg_araddr,source_bypasses;
 wire [7:0] source_arlen,bg_arlen,bg_error;
 wire [2:0] source_arsize,bg_arsize;wire [1:0] source_arburst,bg_arburst;
 wire source_arvalid,source_rready,bg_arvalid,bg_rready,bg_ready,bg_done,bg_busy,bg_valid;
 wire [127:0] bg_data;
 reg bg_started,bg_complete;reg [31:0] bg_bypasses;
 // Transparent-tile screening: while source beats stream into sbuf, flag the
 // first used source word whose RGB differs from the key. A Blend|Key tile
 // whose used words are all key-colored writes nothing (WSTRB keeps the whole
 // destination word), so its background read and write burst can be skipped
 // outright; meanwhile a non-key word releases the background read early and
 // the tile proceeds exactly as before.
 reg src_nonkey_seen;
 wire src_word_used0=(rx*4)>=sl&&(rx*4)<sl+n,src_word_used1=(rx*4+1)>=sl&&(rx*4+1)<sl+n;
 wire src_word_used2=(rx*4+2)>=sl&&(rx*4+2)<sl+n,src_word_used3=(rx*4+3)>=sl&&(rx*4+3)<sl+n;
 wire src_beat_nonkey=
  (src_word_used0&&rd_data[23:0]!=key[23:0])||(src_word_used1&&rd_data[55:32]!=key[23:0])||
  (src_word_used2&&rd_data[87:64]!=key[23:0])||(src_word_used3&&rd_data[119:96]!=key[23:0]);
 wire tile_all_key=op[1:0]==3&&op[8]&&!src_nonkey_seen;
 // The two consumers have separate buffers, but still share ONE external
 // read transaction. A source hit uses only RAM, so background AR may start
 // while the cached source words are still being copied into sbuf. Key
 // screened tiles hold the background request until a non-key word appears.
 wire bg_request=op[1:0]==3&&!bg_started&&fault==0&&wr_error==0&&
  ((state==SRC_WAIT&&cache_serving&&(!op[8]||src_nonkey_seen))||state==BG_REQ);
 assign rd_busy=src_busy||bg_busy;
 assign cache_bypasses=source_bypasses+bg_bypasses;
 assign araddr=bg_started?bg_araddr:source_araddr;
 assign arlen=bg_started?bg_arlen:source_arlen;
 assign arsize=bg_started?bg_arsize:source_arsize;
 assign arburst=bg_started?bg_arburst:source_arburst;
 assign arvalid=bg_started?bg_arvalid:source_arvalid;
 assign rready=bg_started?bg_rready:source_rready;
 gfx_source_reader #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) reader(
  clk,rst_n,state==SRC_REQ||state==LIST_REQ,rd_req_ready,sa,sn,
  source_araddr,source_arlen,source_arsize,source_arburst,source_arvalid,arready&&!bg_started,rdata,rresp,rlast,rvalid&&!bg_started,source_rready,
  rd_data,rd_valid,1'b1,rd_done,rd_error,src_busy,
  1'b1,cache_enable,cache_flush,awvalid&&awready,awaddr,awlen,cache_hits,cache_misses,source_bypasses,cache_serving);
 gfx_read_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.FIFO_DEPTH(2),.FIFO_REGISTERS(1)) background_reader(
  clk,rst_n,bg_request,bg_ready,da,dn,
  bg_araddr,bg_arlen,bg_arsize,bg_arburst,bg_arvalid,arready&&bg_started,rdata,rresp,rlast,rvalid&&bg_started,bg_rready,
  bg_data,bg_valid,1'b1,bg_done,bg_error,bg_busy);
 wire [143:0] fifo_data;wire fifo_valid,fifo_ready,fifo_in_ready;wire [4:0] fifo_count;
 reg [8:0] fast_beat;
 reg fast_write_started,write_inflight,blend_finished;
 reg [127:0] fast_data,source_lanes;reg [15:0] fast_strb;
 reg [31:0] fast_source;
 integer k,fast_dst_index,fast_src_index;
 // Four independent pixel lanes share the existing 128-bit AXI/FIFO path.
 // Unaligned Copy may splice adjacent source beats. Only valid destination
 // lanes are selected; source indices are bounded by the joint tile planner.
 always @*begin
  fast_data=0;source_lanes=0;fast_strb=0;fast_source=0;fast_dst_index=0;fast_src_index=0;
  for(k=0;k<4;k=k+1)begin
   fast_dst_index=(op[1:0]==3?feed:fast_beat)*4+k;
   if(fast_dst_index>=dl&&fast_dst_index<dl+n)begin
    if(op[1:0]==1)fast_source={8'hff,color[23:0]};
    else begin
     fast_src_index=fast_dst_index+sl-dl;
     fast_source=sbuf[fast_src_index>>2]>>(32*(fast_src_index&3));
    end
    fast_data[k*32+:32]={8'hff,fast_source[23:0]};
    source_lanes[k*32+:32]=fast_source;
    fast_strb[k*4+:4]=(op[1:0]==2&&op[8]&&fast_source[23:0]==key[23:0])?4'h0:4'hf;
   end
  end
 end
 wire fast_valid=state==FAST_BEATS&&fast_beat<dn;
 wire write_request=(state==FAST_BEATS||state==PIXELS)&&!fast_write_started;
 wire [3:0] lane_ready,lane_valid,lane_write,lane_last;
 wire [127:0] blended_lanes;
 wire [127:0] background_lanes=feed<dn?dbuf[feed[3:0]]:128'b0;
 wire pin_ready=lane_ready[0],pout_valid=lane_valid[0],pout_last=lane_last[0];
 // Background beats remain in the full tile buffer even if the output FIFO
 // stalls behind the prior write. Compute as beats arrive, but issue this
 // tile's write only after its read succeeds and the prior B has completed.
 wire blend_feed=op[1:0]==3&&(state==BG_WAIT||state==WAIT_PREVIOUS_WRITE||state==PIXELS)&&feed<dn&&feed<bg_rx;
 /* AXI does not permit an accepted write burst to be abandoned.  If the
  * pixel producer itself stops after AW acceptance, the writer's timeout
  * used to leave PIXELS/FAST_BEATS waiting forever: no more FIFO words could
  * arrive, yet ABORT_DRAIN was deliberately excluded while producing.  Drain
  * only the outstanding transport with zero strobes, which completes the AXI
  * contract without changing destination memory, then report the real fault. */
 wire abort_write=state==ABORT_DRAIN&&wr_busy;
 wire writer_in_ready;
 assign fifo_ready=writer_in_ready&&!abort_write;
 wire discard_tile=state==ABORT_DRAIN&&!wr_busy&&!rd_busy;
 wire pout_ready=fifo_in_ready;
 reg [15:0] next_strb;
 integer output_lane,output_index;
 always @*begin
  next_strb=0;output_index=0;
  for(output_lane=0;output_lane<4;output_lane=output_lane+1)begin
   output_index=output_beat*4+output_lane;
   if(output_index>=dl&&output_index<dl+n&&lane_write[output_lane])next_strb[output_lane*4+:4]=4'hf;
  end
 end
 // Identical valid/ready/last control keeps all four lanes in lockstep.
 // The exact per-pixel alpha rounding and Key precedence remain in the
 // existing tested pipeline; inactive edge lanes are masked at the output.
 assign pixel_rst_n=rst_n&&state!=ABORT_DRAIN;
 assign pixel_in_valid=blend_feed;assign pixel_out_ready=pout_ready;
 assign pixel_in_last=feed==dn-1;assign pixel_src=source_lanes;
 assign pixel_dst=background_lanes;assign pixel_color=color;assign pixel_op=op;
 assign pixel_alpha=alpha[7:0];assign pixel_key=key[23:0];
 genvar lane;
 generate if(SHARED_PIXEL_PIPE)begin:shared_pixels
  assign lane_ready=pixel_ready;assign lane_valid=pixel_valid;
  assign lane_write=pixel_write;assign lane_last=pixel_last;
  assign blended_lanes=pixel_result;
 end else begin:local_pixels
  for(lane=0;lane<4;lane=lane+1)begin:blend_lane
   gfx_pixel_pipe pipeline(clk,rst_n&&state!=ABORT_DRAIN,blend_feed,lane_ready[lane],source_lanes[lane*32+:32],background_lanes[lane*32+:32],color,op,alpha[7:0],key[23:0],feed==dn-1,
    lane_valid[lane],pout_ready,blended_lanes[lane*32+:32],lane_write[lane],lane_last[lane]);
  end
 end endgenerate
 gfx_fifo #(.WIDTH(144),.DEPTH(16),.REGISTER_STORAGE(1)) result_fifo(clk,rst_n,discard_tile,fast_valid||pout_valid,fifo_in_ready,state==FAST_BEATS?{fast_strb,fast_data}:{next_strb,blended_lanes},fifo_valid,fifo_ready,fifo_data,fifo_count);
 gfx_write_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) writer(clk,rst_n,write_request,wr_req_ready,da,dn,
  abort_write?128'b0:fifo_data[127:0],abort_write?16'b0:fifo_data[143:128],
  abort_write?1'b1:fifo_valid,writer_in_ready,
  awaddr,awlen,awsize,awburst,awvalid,awready,wdata,wstrb,wlast,wvalid,wready,bresp,bvalid,bready,wr_done,wr_error,wr_busy);
 assign cmd_ready=state==IDLE;assign busy=state!=IDLE;
 assign recover_safe=!busy&&!rd_busy&&!wr_busy;
 // Read-only timeout diagnostics.  gfx_regs snapshots these signals only on
 // an explicit CONTROL=8 request, so normal APB reads do not add a live
 // combinational path back into the engine state machine.
 assign debug_state={wready,wvalid,rready,rvalid,arready,arvalid,
  wr_error!=0,bg_error!=0,rd_error!=0,display_urgent,pout_ready,pout_valid,
  fifo_ready,fifo_valid,blend_finished,fast_write_started,wr_done,
  write_inflight,wr_busy,bg_valid,bg_done,bg_complete,bg_started,bg_busy,
  rd_valid,rd_done,cache_serving,src_busy,state};
 assign debug_seq=seq;
 assign debug_position={row[15:0],col[15:0]};
 assign debug_counts={3'b0,dn,output_beat,feed,bg_rx,rx};
 always @(posedge clk)begin
  if(!rst_n)begin state<=IDLE;done<=0;done_seq<=0;fault<=0;fault_addr<=0;row<=0;col<=0;feed<=0;output_beat<=0;rx<=0;bg_rx<=0;bg_started<=0;bg_complete<=0;bg_bypasses<=0;fast_beat<=0;fast_write_started<=0;write_inflight<=0;blend_finished<=0;src_nonkey_seen<=0;list_mode<=0;list_data<=0;end
  else begin
   done<=0;
   // Expose faults immediately, while the DMA continues draining accepted work.
   if(fault==0)begin
    if(src_busy&&rd_error!=0)begin fault<=rd_error;fault_addr<=sa;end
    else if((bg_busy||bg_done)&&bg_error!=0)begin fault<=bg_error;fault_addr<=da;end
    else if((wr_busy||wr_done)&&wr_error!=0)begin fault<=wr_error;fault_addr<=awaddr;end
   end
   if(write_request&&wr_req_ready)write_inflight<=1;
   if(wr_done)write_inflight<=0;
   if(bg_request&&bg_ready)begin bg_started<=1;bg_bypasses<=bg_bypasses+1'b1;end
   if(bg_done)bg_complete<=1;
   if(rd_valid)begin
    if(state==SRC_WAIT)sbuf[rx]<=rd_data;
    if(state==LIST_WAIT&&rx==0)list_data<=rd_data[95:0];
    if(src_beat_nonkey)src_nonkey_seen<=1;
    rx<=rx+1'b1;
   end
   if(bg_valid)begin
    if(bg_rx<dn)dbuf[bg_rx]<=bg_data;
    bg_rx<=bg_rx+1'b1;
   end
   if(blend_feed&&pin_ready)feed<=feed+1'b1;
   if(fast_valid&&fifo_in_ready)fast_beat<=fast_beat+1'b1;
   if((state==FAST_BEATS||state==PIXELS)&&write_request&&wr_req_ready)fast_write_started<=1;
   if(pout_valid&&pout_ready)begin
    output_beat<=output_beat+1'b1;
    if(pout_last)blend_finished<=1;
   end
   case(state)
    IDLE:if(cmd_valid)begin
     op<=cmd[0+:32];src<=cmd[32+:32];dst<=cmd[64+:32];width<=cmd[96+:32];height<=cmd[128+:32];
     ss<=cmd[160+:32];ds<=cmd[192+:32];color<=cmd[224+:32];alpha<=cmd[256+:32];key<=cmd[288+:32];seq<=cmd[320+:32];done_seq<=cmd[320+:32];
     row<=0;col<=0;fault<=0;fault_addr<=0;list_mode<=cmd[0+:32]==`GFX_OP_FILL_LIST;
     if(cmd[0+:32]==`GFX_OP_FILL_LIST)begin
      key<=cmd[64+:32];
      sa<=cmd[32+:32];sn<=1;
      rx<=0;bg_started<=0;bg_complete<=0;state<=LIST_REQ;
     end else state<=(cmd[96+:32]==0||cmd[128+:32]==0)?FINISH:PLAN;
    end
    PLAN:begin
     n<=op[1:0]==1?fill_n[10:0]:{4'b0,plan_n};sn<=plan_sn;
     dn<=op[1:0]==1?(fill_n+cur_dst[3:2]+3)>>2:{4'b0,plan_dn};
     sa<=plan_sa;da<=plan_da;sl<=cur_src[3:2];dl<=cur_dst[3:2];
     feed<=0;output_beat<=0;rx<=0;bg_rx<=0;bg_started<=0;bg_complete<=0;
     fast_beat<=0;fast_write_started<=0;blend_finished<=0;src_nonkey_seen<=0;
     state<=op[1:0]==1?WAIT_PREVIOUS_WRITE:SRC_REQ;
    end
    SRC_REQ:if(rd_req_ready)state<=SRC_WAIT;
    SRC_WAIT:if(rd_done)begin
     if(rd_error!=0)begin if(fault==0)begin fault<=rd_error;fault_addr<=sa;end state<=ABORT_DRAIN;end
     // A fully key-screened tile has no visible effect (every lane keeps the
     // destination word) and nothing in flight, so advance without the
     // background read, the blend pipe, or the write burst.
     else if(tile_all_key)state<=NEXT;
     else state<=op[1:0]==3?(bg_started?BG_WAIT:BG_REQ):WAIT_PREVIOUS_WRITE;
    end
    BG_REQ:if(bg_request&&bg_ready)state<=BG_WAIT;
    BG_WAIT:if(bg_done||bg_complete)begin
     if(bg_error!=0)begin if(fault==0)begin fault<=bg_error;fault_addr<=da;end state<=ABORT_DRAIN;end
     else state<=WAIT_PREVIOUS_WRITE;
    end
    // Prefetch uses buffers whose prior pixels have already entered the result
    // FIFO. Early Blend computation may enqueue behind them, but the next
    // write request must still wait for the previous B response.
    WAIT_PREVIOUS_WRITE:if(!write_inflight)state<=op[1:0]==3?PIXELS:FAST_BEATS;
    PIXELS:if(blend_finished&&fast_write_started)state<=NEXT;
    FAST_BEATS:if(fast_beat==dn&&fast_write_started)state<=NEXT;
    WRITE_WAIT:if(!write_inflight)begin
     if(list_mode&&alpha[8:0]>1)begin
      alpha[8:0]<=alpha[8:0]-1'b1;src<=src+16;
      sa<=src+16;sn<=1;rx<=0;state<=LIST_REQ;
     end else state<=FINISH;
    end
    NEXT:begin
     if(col+n==width)begin col<=0;row<=row+1'b1;state<=(row+1==height)?WRITE_WAIT:PLAN;end
     else begin col<=col+n;state<=PLAN;end
    end
    ABORT_DRAIN:if(!rd_busy&&!wr_busy)state<=FINISH;
    LIST_REQ:if(rd_req_ready)state<=LIST_WAIT;
    LIST_WAIT:if(rd_done)begin
     if(rd_error!=0)begin if(fault==0)begin fault<=rd_error;fault_addr<=src;end state<=ABORT_DRAIN;end
     else state<=LIST_DECODE;
    end
    LIST_DECODE:begin
     if(list_entry_bad)begin fault<=`GFX_ERR_RANGE;fault_addr<=src;state<=FINISH;end
     else begin
      op<=`GFX_OP_FILL;dst<=key+({16'b0,list_y}<<12)+({16'b0,list_x}<<2);width<=list_width;height<=list_height;
      ds<=4096;color<=list_color;row<=0;col<=0;state<=PLAN;
     end
    end
    FINISH:begin done<=1;done_seq<=seq;state<=IDLE;end
    default:begin end
   endcase
   // ABORT_DRAIN supplies zero-strobe beats for any accepted write, so even a
   // producer-side stall in PIXELS/FAST_BEATS can now terminate safely.
   if((fault!=0||wr_error!=0)&&state!=IDLE&&state!=FINISH&&state!=ABORT_DRAIN)
    state<=ABORT_DRAIN;
  end
 end
endmodule
