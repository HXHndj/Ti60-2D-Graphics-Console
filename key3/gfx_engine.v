`include "gfx_abi.vh"
// Ordered rectangular engine. Tile buffers separate source/background from the
// output FIFO. Each tile has real multi-beat AXI transfers (not word requests).
module gfx_engine #(parameter TIMEOUT_CYCLES=10000000,MAX_FILL_BEATS=16,SHARED_SOURCE_ROW=0,SHARED_PIXEL_PIPE=0)(
 input wire clk,rst_n,input wire cmd_valid,output wire cmd_ready,input wire [`GFX_CMD_BITS-1:0] cmd,
 output reg done,output reg [31:0] done_seq,output reg [7:0] fault,output reg [31:0] fault_addr,
 output wire busy,output wire recover_safe,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready,
 input wire display_urgent,input wire opaque_enable,
 // The 32 KiB source reader is instantiated once in gfx_core and shared with
 // the bucket engine. This engine issues requests and receives beats through
 // the src_req/rd_* contract; the reader's AXI side passes through src_ar_*
 // and is muxed here against the background read DMA exactly as before.
 output wire src_req_valid,input wire src_req_ready,
 output wire [31:0] src_req_addr,output wire [4:0] src_req_beats,
 input wire [127:0] rd_data,input wire rd_valid,input wire rd_done,input wire [7:0] rd_error,
 input wire src_busy,input wire cache_serving,
 input wire [31:0] src_araddr,input wire [7:0] src_arlen,input wire [2:0] src_arsize,
 input wire [1:0] src_arburst,input wire src_arvalid,output wire src_arready,
 output wire src_rbeat_valid,input wire src_rready,
 output wire [31:0] bg_bypass_total,
 // Optional core-owned source-row RAM. The local default keeps direct engine
 // testbenches self-contained; a core sets SHARED_SOURCE_ROW=1 and muxes these
 // ports against the bucket client around one physical 16x128 RAM.
 output wire source_row_wr_en,output wire [3:0] source_row_wr_addr,
 output wire [127:0] source_row_wr_data,output wire [3:0] source_row_rd_addr,
 input wire [127:0] source_row_rd_data,
 // Optional command-owned four-lane pixel service. Local mode remains the
 // standalone oracle; external mode adds no register or handshake latency.
 output wire pixel_rst_n,pixel_in_valid,pixel_out_ready,pixel_in_last,
 output wire [127:0] pixel_src,pixel_dst,
 output wire [31:0] pixel_color,pixel_op,
 output wire [7:0] pixel_alpha,output wire [23:0] pixel_key,
 input wire [3:0] pixel_ready,pixel_valid,pixel_write,pixel_last,
 input wire [127:0] pixel_result
);
 localparam IDLE=0,PLAN=1,SRC_REQ=2,SRC_WAIT=3,BG_REQ=4,BG_WAIT=5,PIXELS=6,WAIT_PREVIOUS_WRITE=7,WRITE_WAIT=8,NEXT=9,FINISH=10,FAST_BEATS=11,ABORT_DRAIN=12;
 reg [3:0] state;
 reg [31:0] op,src,dst,width,height,ss,ds,color,alpha,key,seq;
 reg opaque_enable_cmd;
 reg [31:0] row,col;
 reg [10:0] n;
 reg [4:0] feed,output_beat;
 reg [4:0] sn,rx,bg_rx;reg [8:0] dn;
 reg [8:0] fast_beat;
 reg [31:0] sa,da;
 reg [1:0] sl,dl;
 reg [127:0] dbuf[0:15];
 // src/dst hold this row's bases. Both advance once per completed row,
 // replacing two repeated 32x32 row/stride multipliers without a new cycle.
 wire [31:0] cur_src=src+(col<<2),cur_dst=dst+(col<<2);
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
 wire rd_busy;
 wire wr_req_ready,wr_done,wr_busy;wire [7:0] wr_error;
 wire [31:0] bg_araddr;
 wire [7:0] bg_arlen,bg_error;
 wire [2:0] bg_arsize;wire [1:0] bg_arburst;
 wire bg_arvalid,bg_rready,bg_ready,bg_done,bg_busy,bg_valid;
 wire [127:0] bg_data;
 reg bg_started,bg_complete;reg [31:0] bg_bypasses;
 // Tile classification runs against current source data, key, and alpha
 // flags. It ignores clipped lanes and Key pixels, skips all-Key tiles, and
 // can defer the background read until a used non-Key pixel actually needs it.
 reg src_nonkey_seen,src_nonopaque_seen,opaque_tile_fast;
 wire src_word_used0=(rx*4)>=sl&&(rx*4)<sl+n,src_word_used1=(rx*4+1)>=sl&&(rx*4+1)<sl+n;
 wire src_word_used2=(rx*4+2)>=sl&&(rx*4+2)<sl+n,src_word_used3=(rx*4+3)>=sl&&(rx*4+3)<sl+n;
 wire src_beat_nonkey=
  (src_word_used0&&rd_data[23:0]!=key[23:0])||(src_word_used1&&rd_data[55:32]!=key[23:0])||
  (src_word_used2&&rd_data[87:64]!=key[23:0])||(src_word_used3&&rd_data[119:96]!=key[23:0]);
 wire src_beat_nonopaque=opaque_enable_cmd&&op[1:0]==3&&(
  (src_word_used0&&(!op[8]||rd_data[23:0]!=key[23:0])&&(alpha[7:0]!=8'hff||(op[9]&&rd_data[31:24]!=8'hff)))||
  (src_word_used1&&(!op[8]||rd_data[55:32]!=key[23:0])&&(alpha[7:0]!=8'hff||(op[9]&&rd_data[63:56]!=8'hff)))||
  (src_word_used2&&(!op[8]||rd_data[87:64]!=key[23:0])&&(alpha[7:0]!=8'hff||(op[9]&&rd_data[95:88]!=8'hff)))||
  (src_word_used3&&(!op[8]||rd_data[119:96]!=key[23:0])&&(alpha[7:0]!=8'hff||(op[9]&&rd_data[127:120]!=8'hff))));
 wire src_nonkey_after_beat=src_nonkey_seen||(rd_valid&&src_beat_nonkey);
 wire src_nonopaque_after_beat=src_nonopaque_seen||(rd_valid&&src_beat_nonopaque);
 wire tile_all_key=op[1:0]==3&&op[8]&&!src_nonkey_after_beat;
 // The two consumers have separate buffers, but share one external read
 // transaction. Cached source hits can overlap background AR; Key pixels are
 // excluded and opaque tiles keep that request suppressed.
 wire bg_request=op[1:0]==3&&!bg_started&&fault==0&&wr_error==0&&
  ((state==SRC_WAIT&&cache_serving&&(!op[8]||src_nonkey_seen)&&(!opaque_enable_cmd||src_nonopaque_after_beat))||
   (state==BG_REQ&&(!opaque_enable_cmd||src_nonopaque_seen)));
 assign rd_busy=src_busy||bg_busy;
 assign bg_bypass_total=bg_bypasses;
 assign araddr=bg_started?bg_araddr:src_araddr;
 assign arlen=bg_started?bg_arlen:src_arlen;
 assign arsize=bg_started?bg_arsize:src_arsize;
 assign arburst=bg_started?bg_arburst:src_arburst;
 assign arvalid=bg_started?bg_arvalid:src_arvalid;
 assign rready=bg_started?bg_rready:src_rready;
 // The reader's AXI transaction lives at gfx_core level; it owns this port
 // whenever the background read DMA does not.
 assign src_arready=arready&&!bg_started;
 assign src_rbeat_valid=rvalid&&!bg_started;
 assign src_req_valid=state==SRC_REQ;assign src_req_addr=sa;assign src_req_beats=sn;
 // Legacy source beats are written directly to the same 16x128 storage used
 // by the bucket sprite-row client. The RAM read port is synchronous; the
 // client holds the prior word and advances it only when its consumer accepts
 // a beat, so stalls preserve the pair and an unstalled row stays one beat/cycle.
 reg [1:0] source_row_prime;
 reg [127:0] source_row_prev;
 wire [127:0] source_row_local_data;
 wire [127:0] source_row_data=SHARED_SOURCE_ROW?source_row_rd_data:source_row_local_data;
 wire source_row_window_valid=source_row_prime==3;
 function [3:0] source_base_word;
  input [8:0] beat_index;input [1:0] src_lane,dst_lane;
  integer pixel_base;
  begin
   pixel_base=beat_index*4+src_lane-dst_lane;
   if(pixel_base<0)source_base_word=4'd0;
   else source_base_word=(pixel_base>>2)&15;
  end
 endfunction
 wire [8:0] source_out_beat=(op[1:0]==3&&!opaque_tile_fast)?{4'b0,feed}:fast_beat;
 wire [3:0] source_base_now=source_base_word(source_out_beat,sl,dl);
 wire [3:0] source_prime_base=source_base_word(9'd0,sl,dl);
 wire [3:0] source_base_next=source_base_word(source_out_beat+9'd1,sl,dl);
 wire source_row_window_shift=source_base_next!=source_base_now;
 wire source_fast_advance;
 wire source_blend_advance;
 wire source_row_advance;
 assign source_row_wr_en=rd_valid&&state==SRC_WAIT;
 assign source_row_wr_addr=rx[3:0];
 assign source_row_wr_data=rd_data;
 reg [3:0] source_row_rd_addr_i;
 assign source_row_rd_addr=source_row_rd_addr_i;
 always @*begin
  if(source_row_prime==1)source_row_rd_addr_i=source_prime_base;
  else if(source_row_prime==2)source_row_rd_addr_i=source_prime_base+4'd1;
  else if(source_row_prime==3&&source_row_advance)source_row_rd_addr_i=source_base_next+4'd1;
  else if(source_row_prime==3)source_row_rd_addr_i=source_base_now+4'd1;
  else source_row_rd_addr_i=source_prime_base;
 end
 generate if(SHARED_SOURCE_ROW==0)begin:local_source_row_ram
  gfx_source_row_ram local_rows(clk,rst_n,source_row_wr_en,source_row_wr_addr,
   source_row_wr_data,source_row_rd_addr,source_row_local_data);
 end else begin:external_source_row_ram
  assign source_row_local_data=128'b0;
 end endgenerate
 always @(posedge clk)begin
  if(!rst_n)begin source_row_prime<=0;source_row_prev<=0;end
  else begin
   if(state==PLAN)source_row_prime<=0;
   else if(state==SRC_WAIT&&rd_done&&rd_error==0&&!tile_all_key&&op[1:0]!=1)
    source_row_prime<=1;
   else if(source_row_prime==1)source_row_prime<=2;
   else if(source_row_prime==2)source_row_prime<=3;
   if(source_row_prime==2||(source_row_advance&&source_row_window_shift))source_row_prev<=source_row_data;
  end
 end
 gfx_read_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.FIFO_DEPTH(2),.FIFO_REGISTERS(1)) background_reader(
  clk,rst_n,bg_request,bg_ready,da,dn,
  bg_araddr,bg_arlen,bg_arsize,bg_arburst,bg_arvalid,arready&&bg_started,rdata,rresp,rlast,rvalid&&bg_started,bg_rready,
  bg_data,bg_valid,1'b1,bg_done,bg_error,bg_busy);
 wire [143:0] fifo_data;wire fifo_valid,fifo_ready,fifo_in_ready;wire [4:0] fifo_count;
 reg fast_write_started,write_inflight,blend_finished;
 reg [127:0] fast_data,source_lanes;reg [15:0] fast_strb;
 reg [31:0] fast_source;
 integer k,fast_dst_index,fast_src_index,fast_src_word,fast_src_lane,fast_source_base;
 // Four independent pixel lanes share the existing 128-bit AXI/FIFO path.
 // Unaligned Copy may splice adjacent source beats. Only valid destination
 // lanes are selected; source indices are bounded by the joint tile planner.
 always @*begin
  fast_data=0;source_lanes=0;fast_strb=0;fast_source=0;fast_dst_index=0;fast_src_index=0;
  fast_src_word=0;fast_src_lane=0;fast_source_base=source_base_now;
  for(k=0;k<4;k=k+1)begin
   fast_dst_index=(op[1:0]==3&&!opaque_tile_fast?feed:fast_beat)*4+k;
   if(fast_dst_index>=dl&&fast_dst_index<dl+n)begin
    if(op[1:0]==1)fast_source={8'hff,color[23:0]};
    else begin
     fast_src_index=fast_dst_index+sl-dl;
     fast_src_word=fast_src_index>>2;fast_src_lane=fast_src_index&3;
     if(fast_src_word==fast_source_base)fast_source=source_row_prev>>(32*fast_src_lane);
     else if(fast_src_word==((fast_source_base+1)&15))fast_source=source_row_data>>(32*fast_src_lane);
     else fast_source=0;
    end
    fast_data[k*32+:32]={8'hff,fast_source[23:0]};
    source_lanes[k*32+:32]=fast_source;
    fast_strb[k*4+:4]=((op[1:0]==2||(op[1:0]==3&&opaque_tile_fast))&&op[8]&&fast_source[23:0]==key[23:0])?4'h0:4'hf;
   end
  end
 end
 wire fast_valid=state==FAST_BEATS&&fast_beat<dn&&(op[1:0]==1||source_row_window_valid);
 wire write_request=(state==FAST_BEATS||state==PIXELS)&&!fast_write_started;
 wire [3:0] lane_ready,lane_valid,lane_write,lane_last;
 wire [127:0] blended_lanes;
 wire [127:0] background_lanes=feed<dn?dbuf[feed[3:0]]:128'b0;
 wire pin_ready=lane_ready[0],pout_valid=lane_valid[0],pout_last=lane_last[0];
 // Background beats remain in the full tile buffer even if the output FIFO
 // stalls behind the prior write. Compute as beats arrive, but issue this
 // tile's write only after its read succeeds and the prior B has completed.
 wire blend_feed=op[1:0]==3&&!opaque_tile_fast&&(state==BG_WAIT||state==WAIT_PREVIOUS_WRITE||state==PIXELS)&&feed<dn&&feed<bg_rx&&source_row_window_valid;
 assign source_fast_advance=op[1:0]!=1&&fast_valid&&fifo_in_ready;
 assign source_blend_advance=blend_feed&&pin_ready;
 assign source_row_advance=source_fast_advance||source_blend_advance;
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
 genvar lane;
 assign pixel_rst_n=rst_n&&state!=ABORT_DRAIN;
 assign pixel_in_valid=blend_feed;
 assign pixel_out_ready=pout_ready;
 assign pixel_in_last=feed==dn-1;
 assign pixel_src=source_lanes;assign pixel_dst=background_lanes;
 assign pixel_color=color;assign pixel_op=op;
 assign pixel_alpha=alpha[7:0];assign pixel_key=key[23:0];
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
 gfx_fifo #(.WIDTH(144),.DEPTH(16),.REGISTER_STORAGE(1),.SYNC_RAM(1)) result_fifo(clk,rst_n,discard_tile,fast_valid||pout_valid,fifo_in_ready,state==FAST_BEATS?{fast_strb,fast_data}:{next_strb,blended_lanes},fifo_valid,fifo_ready,fifo_data,fifo_count);
 wire writer_data_idle;
 gfx_write_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) writer(clk,rst_n,write_request,wr_req_ready,da,dn,
  fifo_data[127:0],fifo_data[143:128],fifo_valid,fifo_ready,
  awaddr,awlen,awsize,awburst,awvalid,awready,wdata,wstrb,wlast,wvalid,wready,bresp,bvalid,bready,wr_done,wr_error,wr_busy,writer_data_idle);
 assign cmd_ready=state==IDLE;assign busy=state!=IDLE;
 assign recover_safe=!busy&&!rd_busy&&!wr_busy;
 always @(posedge clk)begin
   if(!rst_n)begin state<=IDLE;done<=0;done_seq<=0;fault<=0;fault_addr<=0;row<=0;col<=0;feed<=0;output_beat<=0;rx<=0;bg_rx<=0;bg_started<=0;bg_complete<=0;bg_bypasses<=0;fast_beat<=0;fast_write_started<=0;write_inflight<=0;blend_finished<=0;src_nonkey_seen<=0;src_nonopaque_seen<=0;opaque_tile_fast<=0;opaque_enable_cmd<=0;end
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
    if(src_beat_nonkey)src_nonkey_seen<=1;
    if(src_beat_nonopaque)src_nonopaque_seen<=1;
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
     if(opaque_enable==1'b1)opaque_enable_cmd<=1'b1;else opaque_enable_cmd<=1'b0;
     row<=0;col<=0;fault<=0;fault_addr<=0;state<=(cmd[96+:32]==0||cmd[128+:32]==0)?FINISH:PLAN;
    end
    PLAN:begin
     n<=op[1:0]==1?fill_n[10:0]:{4'b0,plan_n};sn<=plan_sn;
     dn<=op[1:0]==1?(fill_n+cur_dst[3:2]+3)>>2:{4'b0,plan_dn};
     sa<=plan_sa;da<=plan_da;sl<=cur_src[3:2];dl<=cur_dst[3:2];
     feed<=0;output_beat<=0;rx<=0;bg_rx<=0;bg_started<=0;bg_complete<=0;
     fast_beat<=0;fast_write_started<=0;blend_finished<=0;src_nonkey_seen<=0;src_nonopaque_seen<=0;opaque_tile_fast<=0;
     state<=op[1:0]==1?WAIT_PREVIOUS_WRITE:SRC_REQ;
    end
    SRC_REQ:if(src_req_ready)state<=SRC_WAIT;
    SRC_WAIT:if(rd_done)begin
     if(rd_error!=0)begin if(fault==0)begin fault<=rd_error;fault_addr<=sa;end state<=ABORT_DRAIN;end
     // A fully key-screened tile has no visible effect (every lane keeps the
     // destination word) and nothing in flight, so advance without the
     // background read, the blend pipe, or the write burst.
     else if(tile_all_key)state<=NEXT;
     else if(op[1:0]==3&&opaque_enable_cmd&&!src_nonopaque_after_beat)begin opaque_tile_fast<=1;state<=WAIT_PREVIOUS_WRITE;end
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
    WAIT_PREVIOUS_WRITE:if(!write_inflight)state<=op[1:0]==3&&!opaque_tile_fast?PIXELS:FAST_BEATS;
    PIXELS:if(blend_finished&&fast_write_started)state<=NEXT;
    FAST_BEATS:if(fast_beat==dn&&fast_write_started)state<=NEXT;
    WRITE_WAIT:if(!write_inflight)state<=FINISH;
    NEXT:begin
     if(col+n==width)begin col<=0;row<=row+1'b1;src<=src+ss;dst<=dst+ds;state<=(row+1==height)?WRITE_WAIT:PLAN;end
     else begin col<=col+n;state<=PLAN;end
    end
    ABORT_DRAIN:if(!rd_busy&&!wr_busy)state<=FINISH;
    FINISH:begin done<=1;done_seq<=seq;state<=IDLE;end
    default:begin end
   endcase
   // A timed-out writer may still require all W beats. Keep its producer
   // running until NEXT, then drain; never clear or abandon accepted traffic.
   if((fault!=0||wr_error!=0)&&state!=IDLE&&state!=FINISH&&state!=PIXELS&&state!=FAST_BEATS&&state!=ABORT_DRAIN)
    state<=ABORT_DRAIN;
  end
 end
endmodule
