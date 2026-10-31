`include "gfx_abi.vh"
`include "mc_tile4_abi.vh"
`default_nettype none
// Eight-layer, 16x16-tile compositor.  Descriptors carry explicit Z order.
// One shared read DMA loads the 256-byte descriptor table and image spans;
// completed tile rows are the only intermediate results written to DDR.
module gfx_tile4_engine #(parameter TIMEOUT_CYCLES=10000000,
 parameter FOLD_CLEAR_BASE=1,parameter LAZY_TILE_CLEAR=1,
 parameter FOLD_OPAQUE_BASE=1,parameter SHARED_PIXEL_PIPE=0,
 parameter FAST_OPAQUE_IMAGE=1,
 parameter [31:0] BIN_FB0=32'h01000000,BIN_FB1=32'h01400000,
 parameter [31:0] BIN_SLOT0=32'h01660000,BIN_SLOT1=32'h01680000)(
 input wire clk,rst_n,input wire cmd_valid,output wire cmd_ready,input wire [`GFX_CMD_BITS-1:0] cmd,
 output reg done,output reg [31:0] done_seq,output reg [7:0] fault,output reg [31:0] fault_addr,
 output wire busy,output wire recover_safe,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready,
 output reg [31:0] tile_candidate_refs,output reg [31:0] tile_executed_refs,
 output reg [31:0] tile_skipped_refs,output reg [31:0] tile_opaque_tiles,
 output wire pixel_rst_n,pixel_in_valid,pixel_out_ready,pixel_in_last,
 output wire [127:0] pixel_src,pixel_dst,
 output wire [31:0] pixel_color,pixel_op,
 output wire [7:0] pixel_alpha,output wire [23:0] pixel_key,
 input wire [3:0] pixel_ready,pixel_valid,pixel_write,pixel_last,
 input wire [127:0] pixel_result
);
 localparam IDLE=0,DESC_REQ=1,DESC_WAIT=2,TILE_START=3,TILE_CLEAR=4,
  LAYER_START=5,GROUP_PLAN=6,SRC_REQ=7,SRC_WAIT=8,BUF_REQ=9,BUF_WAIT=10,
  PIPE_FEED=11,PIPE_WAIT=12,BUF_WRITE=13,GROUP_NEXT=14,LAYER_NEXT=15,
  TILE_END=16,OUT_WAIT=17,CAPTURE=18,PRODUCE_DRAIN=19,FINISH=20,ABORT=21,
  TILE_SELECT=22,BASE_SCAN=23,SRC_CACHE_PRIME=24,SRC_CACHE_LO=25,SRC_CACHE_HI=26,
  LAYER_EVAL=27,LIST_REQ=28,LIST_WAIT=29,LIST_PRIME=30,LIST_APPLY=31,
  DESC_SORT=32,BIN_HEADER_REQ=33,BIN_HEADER_WAIT=34,BIN_HEADER_CHECK=35,
  BIN_STREAM_REQ=36,BIN_STREAM_WAIT=37,BIN_DESC_PRIME=38,
  BIN_DESC_LO=39,BIN_DESC_HI=40,BIN_ADVANCE=41,
  BIN_DESC_HI2=42,BIN_DESC_HI3=43,FAST_IMAGE_CHECK=44;
 reg [5:0] state;
 reg [31:0] desc_base,dst,width,height,dst_stride,clear_color,seq;
 reg [31:0] render_x0,render_y0,render_x1,render_y1;
 reg [3:0] desc_count,desc_beat;reg [255:0] desc0,desc1,desc2,desc3,desc4,desc5,desc6,desc7,active_desc;
 reg [2:0] selected_index;
 reg [2:0] sorted0,sorted1,sorted2,sorted3,sorted4,sorted5,sorted6,sorted7;
 reg [2:0] sort_position,sort_scan,sort_best;
 reg [7:0] sort_used;
 reg sort_best_valid;
 reg [31:0] tile_x,tile_y;reg [4:0] tile_w,tile_h;reg [3:0] layer;
 reg [5:0] group;reg [2:0] first_lane,active_count;reg [3:0] active_mask;
 reg [31:0] src_addr_aligned,source_row_addr;reg [8:0] src_beats;
 reg [127:0] source0,source1,background,result_pixels,blended_capture;
 reg [3:0] write_capture,source_row_index;reg [6:0] src_rx;reg [1:0] src_offset;
 reg source_row_valid,image_fast;
 reg [2:0] tile_slot;reg [5:0] capture_beat;
 reg [3:0] scan_rank,base_start_layer;reg [31:0] base_clear_pixel;
 reg [3:0] tile_candidate_prefix,tile_skipped_prefix;reg tile_has_opaque;
 reg list_mode;reg [31:0] list_addr;reg [8:0] list_count,list_index,list_write_index;
 reg [127:0] list_beat;reg [1:0] list_lane;reg list_beat_valid,list_dma_done;
 reg bin_enable,bin_overlay_mode,bin_count_invalid;
 reg [31:0] bin_first;reg [6:0] bin_count,bin_index;
 reg [6:0] bin_rx;
 reg [63:0] bin_high,bin_word0,bin_word1,bin_word2;
 reg bin_high_pending,bin_dma_done;
 wire [31:0] bin_slot_base=dst==BIN_FB0?BIN_SLOT0:BIN_SLOT1;
 wire [31:0] bin_header_addr=bin_slot_base+((tile_y>>4)<<10)+tile_x;
 wire [32:0] bin_stream_end={1'b0,bin_first}+({26'b0,bin_count}<<5);

 function [255:0] descriptor_at;
  input [2:0] index;
  begin case(index)
   0:descriptor_at=desc0;1:descriptor_at=desc1;2:descriptor_at=desc2;3:descriptor_at=desc3;
   4:descriptor_at=desc4;5:descriptor_at=desc5;6:descriptor_at=desc6;default:descriptor_at=desc7;
  endcase end
 endfunction
 // Keep the active descriptor as an explicit mux. A function call driven only
 // by selected_index can remain stale in event simulators when index zero does
 // not change while desc0 is filled by DMA (the one-layer scene exposed this).
 wire [255:0] dsel=selected_index==0?desc0:selected_index==1?desc1:
  selected_index==2?desc2:selected_index==3?desc3:selected_index==4?desc4:
  selected_index==5?desc5:selected_index==6?desc6:desc7;
 // Sort descriptor indices once per command.  The previous combinational
 // ranker built all 8x8 Z comparisons in parallel for every Tile; this small
 // sequential selector performs the same deterministic (Z,index) ordering in
 // at most 64 command-setup clocks, then every Tile reads a 24-bit index table.
 function [7:0] descriptor_z;
  input [2:0] index;
  begin case(index)
   0:descriptor_z=desc0[207:200];1:descriptor_z=desc1[207:200];
   2:descriptor_z=desc2[207:200];3:descriptor_z=desc3[207:200];
   4:descriptor_z=desc4[207:200];5:descriptor_z=desc5[207:200];
   6:descriptor_z=desc6[207:200];default:descriptor_z=desc7[207:200];
  endcase end
 endfunction
 wire [3:0] requested_rank=state==TILE_SELECT?4'd0:
                            state==BASE_SCAN?scan_rank+1'b1:
                            (state==TILE_START||state==TILE_CLEAR)?base_start_layer:
                            state==LAYER_NEXT?layer+1'b1:layer;
 // The rank table is written after descriptor DMA. Keep every table entry in
 // the combinational sensitivity set, including the first sorted0 update.
 wire [2:0] ranked_index=requested_rank[2:0]==0?sorted0:
  requested_rank[2:0]==1?sorted1:requested_rank[2:0]==2?sorted2:
  requested_rank[2:0]==3?sorted3:requested_rank[2:0]==4?sorted4:
  requested_rank[2:0]==5?sorted5:requested_rank[2:0]==6?sorted6:sorted7;
 wire sort_take=!sort_used[sort_scan]&&(!sort_best_valid||
  descriptor_z(sort_scan)<descriptor_z(sort_best));
 wire [2:0] sort_choice=sort_take?sort_scan:sort_best;
 wire [31:0] dc=active_desc[31:0],dsource=active_desc[63:32];
 wire [31:0] dxy=active_desc[95:64],dwh=active_desc[127:96];
 wire [31:0] dstride=active_desc[159:128],dcolor=active_desc[191:160];
 wire [31:0] dalpha=active_desc[223:192],dkey=active_desc[255:224];
 wire [31:0] scan_color=dsel[191:160];
 wire signed [8:0] scroll_x=$signed({dalpha[23],dalpha[23:16]});
 wire signed [8:0] scroll_y=$signed({dalpha[31],dalpha[31:24]});
 wire signed [16:0] layer_x=$signed({dxy[15],dxy[15:0]});
 wire signed [16:0] layer_y=$signed({dxy[31],dxy[31:16]});
 wire [15:0] layer_w=dwh[15:0],layer_h=dwh[31:16];
 wire layer_enabled=dc[0]&&layer_w!=0&&layer_h!=0;
 wire layer_image=dc[15:8]==`MC_TILE4_TYPE_IMAGE;
 wire layer_tiled=dc[3];
 wire signed [32:0] tile_left=$signed({1'b0,tile_x}),tile_top=$signed({1'b0,tile_y});
 wire signed [32:0] tile_right=tile_left+$signed({28'b0,tile_w});
 wire signed [32:0] tile_bottom=tile_top+$signed({28'b0,tile_h});
 wire layer_intersects_tile=layer_enabled&&tile_right>layer_x&&tile_left<(layer_x+$signed({1'b0,layer_w}))&&
  tile_bottom>layer_y&&tile_top<(layer_y+$signed({1'b0,layer_h}));
 wire [31:0] screen_y=tile_y+{28'b0,group[5:2]};
 wire [31:0] screen_x=tile_x+{26'b0,group[1:0],2'b0};
 wire signed [32:0] layer_right=layer_x+$signed({1'b0,layer_w});
 wire signed [32:0] layer_bottom=layer_y+$signed({1'b0,layer_h});
 wire [4:0] clip_left=layer_x>tile_left?layer_x-tile_left:5'd0;
 wire [4:0] clip_top=layer_y>tile_top?layer_y-tile_top:5'd0;
 wire [4:0] clip_right=layer_right<tile_right?layer_right-tile_left:tile_w;
 wire [4:0] clip_bottom=layer_bottom<tile_bottom?layer_bottom-tile_top:tile_h;
 wire [4:0] clip_last_x=clip_right-1'b1;
 reg [1:0] group_first_column,group_last_column;
 reg [3:0] group_last_row;
 task advance_group;
  begin
   if(group[5:2]==group_last_row&&group[1:0]==group_last_column)
    state<=bin_overlay_mode?BIN_ADVANCE:LAYER_NEXT;
   else begin
    if(group[1:0]==group_last_column)
     group<={group[5:2]+4'd1,group_first_column};
    else group<=group+1'b1;
    state<=GROUP_PLAN;
   end
  end
 endtask
 integer i,li;reg [3:0] plan_mask;reg [2:0] plan_first,plan_count;
 reg signed [32:0] px,py,lx_end,ly_end,source_x,source_y,row_right,row_first;
 reg signed [63:0] source_byte;reg [31:0] source_addr,linear_source_addr,tiled_tile_base;
 reg [15:0] source_tile_x,source_tile_y;reg [11:0] source_tiles_per_row;
 reg [21:0] source_tile_index;
 reg [9:0] source_in_tile;reg [5:0] source_row_pixels;
 always @*begin
  plan_mask=0;plan_first=0;plan_count=0;py=$signed({1'b0,screen_y});
  lx_end=layer_x+$signed({1'b0,layer_w});ly_end=layer_y+$signed({1'b0,layer_h});
  for(li=0;li<4;li=li+1)begin
   px=$signed({1'b0,screen_x})+li;
   if(layer_enabled&&px>=layer_x&&px<lx_end&&py>=layer_y&&py<ly_end)begin
    plan_mask[li]=1;if(plan_count==0)plan_first=li;plan_count=plan_count+1'b1;
   end
  end
  source_x=$signed({1'b0,screen_x})+$signed({30'b0,plan_first})-layer_x+scroll_x;
  source_y=$signed({1'b0,screen_y})-layer_y+scroll_y;
  source_byte=source_y*$signed({1'b0,dstride})+(source_x<<<2);
  linear_source_addr=dsource+source_byte[31:0];
  source_tile_x=source_x[19:4];source_tile_y=source_y[19:4];
  source_tiles_per_row=dstride[17:6];
  source_tile_index=source_tile_y*source_tiles_per_row+source_tile_x;
  tiled_tile_base=dsource+{source_tile_index,10'b0};
  source_in_tile={source_y[3:0],6'b0}+{source_x[3:0],2'b0};
  source_addr=layer_tiled?tiled_tile_base+source_in_tile:linear_source_addr;
  row_right=tile_right<lx_end?tile_right:lx_end;
  row_first=$signed({1'b0,screen_x})+$signed({30'b0,plan_first});
  source_row_pixels=row_right>row_first?row_right-row_first:0;
 end
 wire [8:0] source_row_beats=({7'b0,source_addr[3:2]}+{3'b0,source_row_pixels}+9'd3)>>2;
 // Bin images are complete 16x16 cells. A screen Tile can see only part of
 // their height; fetch those consecutive source rows in one burst instead of
 // rereading all sixteen rows for every overlapping screen Tile. Background
 // tiled layers keep the original full-source-Tile fetch and wrap semantics.
 wire bin_source_window=bin_overlay_mode&&layer_tiled&&layer_w==16&&
  layer_h==16&&dstride==64&&scroll_x==0&&scroll_y==0;
 wire masked_bin=bin_source_window&&dcolor[31];
 wire signed [32:0] source_first_screen_y=layer_y+
  $signed({28'b0,5'd0});
 wire signed [32:0] source_last_screen_y=layer_y+
  $signed({28'b0,5'd16});
 wire signed [32:0] window_first_screen_y=tile_top>source_first_screen_y?
  tile_top:source_first_screen_y;
 wire signed [32:0] window_last_screen_y=tile_bottom<source_last_screen_y?
  tile_bottom:source_last_screen_y;
 wire signed [32:0] window_first_source_y=window_first_screen_y-layer_y;
 wire signed [32:0] window_height=window_last_screen_y-window_first_screen_y;
 wire [31:0] window_source_offset={22'b0,window_first_source_y[3:0],6'b0};
 wire [8:0] window_source_beats={2'b0,window_height[4:0],2'b0};
 wire [31:0] source_fetch_addr=layer_tiled?
  tiled_tile_base:
  (source_addr&32'hfffffff0);
 wire [8:0] source_fetch_beats=layer_tiled?
  9'd64:source_row_beats;

 wire rd_req_ready,rd_done,rd_busy,rd_valid;wire [7:0] rd_error;wire [127:0] rd_data;
 wire read_request=state==DESC_REQ||state==SRC_REQ||state==LIST_REQ||
  state==BIN_HEADER_REQ||state==BIN_STREAM_REQ;
 wire [31:0] read_addr=state==DESC_REQ?desc_base:state==LIST_REQ?list_addr:
  state==BIN_HEADER_REQ?bin_header_addr:state==BIN_STREAM_REQ?bin_first:src_addr_aligned;
 wire [8:0] read_beats=state==DESC_REQ?9'd16:state==LIST_REQ?((list_count+3)>>2):
  state==BIN_HEADER_REQ?9'd1:state==BIN_STREAM_REQ?{1'b0,bin_count,1'b0}:src_beats;
 wire rd_consume_ready=state==LIST_WAIT?!list_beat_valid:
   state==BIN_STREAM_WAIT?!bin_high_pending:1'b1;
 gfx_read_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.FIFO_DEPTH(8),.FIFO_REGISTERS(1)) reader(
  clk,rst_n,read_request,rd_req_ready,read_addr,read_beats,
  araddr,arlen,arsize,arburst,arvalid,arready,rdata,rresp,rlast,rvalid,rready,
  rd_data,rd_valid,rd_consume_ready,rd_done,rd_error,rd_busy);

 wire [31:0] list_write_data=list_lane==0?list_beat[31:0]:list_lane==1?list_beat[63:32]:
  list_lane==2?list_beat[95:64]:list_beat[127:96];
 wire list_ram_we=state==LIST_WAIT&&list_beat_valid&&list_write_index<list_count;
 wire [31:0] list_run_data;
 simple_dual_port_ram #(.DATA_WIDTH(32),.ADDR_WIDTH(8),.OUTPUT_REG("TRUE")) dirty_run_ram(
  .wdata(list_write_data),.waddr(list_write_index[7:0]),.raddr(list_index[7:0]),.we(list_ram_we),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(list_run_data));
 wire [7:0] bin_raddr={bin_index[5:0],
  state==BIN_DESC_LO?2'd1:state==BIN_DESC_HI?2'd2:
  state==BIN_DESC_HI2?2'd3:2'd0};
 wire [63:0] bin_rdata;
 wire bin_accept=state==BIN_STREAM_WAIT&&rd_valid&&rd_consume_ready;
 // 64-bit width maps the 8 KiB descriptor stream into fewer RAM10 blocks.
 // Backpressure holds each 128-bit AXI beat while its high half is stored.
 simple_dual_port_ram #(.DATA_WIDTH(64),.ADDR_WIDTH(8),.OUTPUT_REG("TRUE")) bin_descriptor_ram(
  .wdata(bin_high_pending?bin_high:rd_data[63:0]),
  .waddr({bin_rx[6:0],bin_high_pending}),.raddr(bin_raddr),
  .we(state==BIN_STREAM_WAIT&&(bin_accept||bin_high_pending)),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(bin_rdata));
 wire [5:0] list_run_x=list_run_data[5:0],list_run_y=list_run_data[11:6];
 wire [3:0] list_run_tiles=list_run_data[15:12];
 wire [6:0] list_run_tile_right={1'b0,list_run_x}+{3'b0,list_run_tiles};
 wire [8:0] list_screen_cols=(width+15)>>4,list_screen_rows=(height+15)>>4;
 wire [31:0] list_run_px={22'b0,list_run_x,4'b0},list_run_py={22'b0,list_run_y,4'b0};
 wire [31:0] list_run_right=list_run_px+{24'b0,list_run_tiles,4'b0};
 wire [31:0] list_run_bottom=list_run_py+32'd16;

 // Linear sources cache one scanline. Tile-major sources cache the whole 16x16
 // source Tile with one 64-beat burst, replacing sixteen row transactions.
 // Tile-major descriptors are 16-pixel aligned, so source words are consumed
 // in burst order. Composition may start while the rest of the burst is still
 // arriving; the same dual-port RAM writes future words and reads only words
 // whose receive count has already advanced past their address.
 // Four directly mapped 1 KiB cells share the existing source RAM.
 // Only complete aligned 16x16 bin images enter the cache. Other sources
 // use slot zero and invalidate it. Tags never survive command boundaries.
 reg [3:0] hot_valid;
 reg [19:0] hot_tags[0:3];
 reg [1:0] source_slot,fill_slot;
 reg source_fill_active,fill_hot,source_cached_hit;
 reg [31:0] fill_source_base;
 wire [1:0] requested_slot=bin_source_window?source_fetch_addr[11:10]:2'd0;
 wire hot_hit=bin_source_window&&hot_valid[requested_slot]&&
  hot_tags[requested_slot]==source_fetch_addr[31:12];
 wire source_ram_we=rd_valid&&rd_consume_ready&&source_fill_active;
 // Register the word index in GROUP_PLAN. Deriving RAM RADDR directly from
 // destination bounds, source stride and source address creates a long path
 // from active_desc through coordinate arithmetic into the RAM setup time.
 reg [5:0] source_ram_base;
 wire [5:0] source_ram_raddr=state==SRC_CACHE_LO?source_ram_base+1'b1:source_ram_base;
 wire source_crosses_word=({1'b0,src_offset}+active_count)>3'd4;
 wire [6:0] source_ram_last={1'b0,source_ram_base}+(source_crosses_word?7'd1:7'd0);
 // A group spanning two RAM words needs both beats. Waiting for only the
 // first can consume stale pixels when the DMA inserts a gap between them.
 wire source_cache_word_ready=source_cached_hit||!rd_busy||(source_ram_last<src_rx);
 wire [127:0] source_ram_data;
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(8),.OUTPUT_REG("TRUE")) source_tile_cache(
  .wdata(rd_data),.waddr({fill_slot,src_rx[5:0]}),.raddr({source_slot,source_ram_raddr}),.we(source_ram_we),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(source_ram_data));
 integer hot_reset;
 always @(posedge clk)begin
  if(!rst_n)begin
   hot_valid<=0;source_slot<=0;fill_slot<=0;source_fill_active<=0;
   fill_hot<=0;source_cached_hit<=0;fill_source_base<=0;
   for(hot_reset=0;hot_reset<4;hot_reset=hot_reset+1)hot_tags[hot_reset]<=0;
  end else begin
   if(state==IDLE&&cmd_valid)hot_valid<=0;
   if(state==GROUP_PLAN&&layer_image&&plan_mask!=0)begin
    if(hot_hit)begin source_slot<=requested_slot;source_cached_hit<=1;end
   end
   // Completion of the old burst can coincide with acceptance of a new
   // one. Publication comes first; new ownership must win that clock edge.
   if(rd_done&&source_fill_active)begin
    source_fill_active<=0;
    if(fill_hot&&rd_error==0)begin
     hot_valid[fill_slot]<=1;hot_tags[fill_slot]<=fill_source_base[31:12];
    end
   end
   if(state==SRC_REQ&&rd_req_ready)begin
    source_slot<=requested_slot;fill_slot<=requested_slot;
    source_fill_active<=1;fill_hot<=bin_source_window;
    fill_source_base<=source_fetch_addr;hot_valid[requested_slot]<=0;
    source_cached_hit<=0;
   end
   if(state==ABORT)begin source_fill_active<=0;hot_valid<=0;end
  end
 end

 wire buf_tile_ready,buf_read_ready,buf_response_valid,buf_write_ready,buf_out_valid,buf_out_last,buf_busy;
 wire [5:0] buf_response_index;wire [127:0] buf_response_pixels,buf_out_pixels;wire [3:0] buf_out_keep;
 wire [31:0] remaining_w=width-tile_x,remaining_h=height-tile_y;
 wire [4:0] start_tile_w=(remaining_w<16)?remaining_w[4:0]:5'd16;
 wire [4:0] start_tile_h=(remaining_h<16)?remaining_h[4:0]:5'd16;
 function opaque_solid_covers_tile;
  input [255:0] desc;input [31:0] tx,ty;input [4:0] tw,th;
  reg [31:0] ctl,xy,wh,alpha;
  reg signed [32:0] dx,dy,txs,tys,dr,db,tr,tb;
  begin
   ctl=desc[31:0];xy=desc[95:64];wh=desc[127:96];alpha=desc[223:192];
   dx=$signed({{17{xy[15]}},xy[15:0]});dy=$signed({{17{xy[31]}},xy[31:16]});
   txs=$signed({1'b0,tx});tys=$signed({1'b0,ty});
   dr=dx+$signed({17'b0,wh[15:0]});db=dy+$signed({17'b0,wh[31:16]});
   tr=txs+$signed({28'b0,tw});tb=tys+$signed({28'b0,th});
   opaque_solid_covers_tile=ctl[0]&&ctl[15:8]==`MC_TILE4_TYPE_SOLID&&
    alpha[7:0]==8'hff&&!ctl[2]&&tw!=0&&th!=0&&
    txs>=dx&&tys>=dy&&tr<=dr&&tb<=db;
  end
 endfunction
 function descriptor_intersects_tile;
  input [255:0] desc;input [31:0] tx,ty;input [4:0] tw,th;
  reg [31:0] ctl,xy,wh;
  reg signed [32:0] dx,dy,txs,tys,dr,db,tr,tb;
  begin
   ctl=desc[31:0];xy=desc[95:64];wh=desc[127:96];
   dx=$signed({{17{xy[15]}},xy[15:0]});dy=$signed({{17{xy[31]}},xy[31:16]});
   txs=$signed({1'b0,tx});tys=$signed({1'b0,ty});
   dr=dx+$signed({17'b0,wh[15:0]});db=dy+$signed({17'b0,wh[31:16]});
   tr=txs+$signed({28'b0,tw});tb=tys+$signed({28'b0,th});
   descriptor_intersects_tile=ctl[0]&&wh[15:0]!=0&&wh[31:16]!=0&&tw!=0&&th!=0&&
    tr>dx&&txs<dr&&tb>dy&&tys<db;
  end
 endfunction
 wire scan_intersects_tile=descriptor_intersects_tile(dsel,tile_x,tile_y,start_tile_w,start_tile_h);
 wire scan_opaque_tile=opaque_solid_covers_tile(dsel,tile_x,tile_y,start_tile_w,start_tile_h);
 wire [3:0] scan_candidate_after=tile_candidate_prefix+(scan_intersects_tile?1'b1:1'b0);
 wire [3:0] scan_skipped_after=scan_opaque_tile?scan_candidate_after:tile_skipped_prefix;
 wire fold_tile_base=!FOLD_OPAQUE_BASE&&FOLD_CLEAR_BASE&&
  opaque_solid_covers_tile(dsel,tile_x,tile_y,start_tile_w,start_tile_h)&&
  scan_color[23:0]==clear_color[23:0];
 wire skip_tile_clear=!FOLD_OPAQUE_BASE&&!fold_tile_base&&(
  (desc_count>=1&&opaque_solid_covers_tile(desc0,tile_x,tile_y,start_tile_w,start_tile_h))||
  (desc_count>=2&&opaque_solid_covers_tile(desc1,tile_x,tile_y,start_tile_w,start_tile_h))||
  (desc_count>=3&&opaque_solid_covers_tile(desc2,tile_x,tile_y,start_tile_w,start_tile_h))||
  (desc_count>=4&&opaque_solid_covers_tile(desc3,tile_x,tile_y,start_tile_w,start_tile_h))||
  (desc_count>=5&&opaque_solid_covers_tile(desc4,tile_x,tile_y,start_tile_w,start_tile_h))||
  (desc_count>=6&&opaque_solid_covers_tile(desc5,tile_x,tile_y,start_tile_w,start_tile_h))||
  (desc_count>=7&&opaque_solid_covers_tile(desc6,tile_x,tile_y,start_tile_w,start_tile_h))||
  (desc_count>=8&&opaque_solid_covers_tile(desc7,tile_x,tile_y,start_tile_w,start_tile_h)));
 wire group_ready,group_write_ready,group_row_valid,group_row_last,group_busy;
 wire [127:0] group_row_pixels;wire [15:0] group_row_strb;wire [8:0] group_row_beats;
 wire [31:0] group_row_x,group_row_y;
 wire wr_req_ready,wr_done,wr_busy,wr_in_ready;wire [7:0] wr_error;
 localparam WR_IDLE=0,WR_ACTIVE=1;reg wr_state;
 wire write_request,group_row_ready;wire [31:0] write_addr;
 wire [31:0] remaining_group_w=render_x1-tile_x;
 wire [7:0] start_group_w=(remaining_group_w<128)?remaining_group_w[7:0]:8'd128;
 wire group_begin=state==TILE_START&&tile_slot==0&&buf_tile_ready&&group_ready;
 wire tile_begin=state==TILE_START&&buf_tile_ready&&(tile_slot!=0||group_ready);
 wire buf_read_valid=state==BUF_REQ;
 wire buf_response_ready=state==BUF_WAIT;
 wire opaque_solid_group=!layer_image&&dalpha[7:0]==8'hff&&!dc[2]&&plan_mask==4'hf;
 wire fold_clear_layer=!FOLD_OPAQUE_BASE&&FOLD_CLEAR_BASE&&layer==0&&
  opaque_solid_covers_tile(dsel,tile_x,tile_y,tile_w,tile_h)&&
  dcolor[23:0]==clear_color[23:0];
 wire buf_write_valid=state==BUF_WRITE;
 wire tile_end=state==TILE_END;
 wire group_write_valid=state==CAPTURE&&buf_out_valid;
 wire [8:0] group_write_index={capture_beat[5:2],5'b0}+{4'b0,tile_slot,2'b0}+capture_beat[1:0];
 wire last_tile_in_group=tile_slot==7||tile_x+16>=render_x1;
 wire group_end=state==CAPTURE&&buf_out_valid&&group_write_ready&&buf_out_last&&last_tile_in_group;
 wire buf_out_ready=state==CAPTURE&&group_write_ready;
 wire group_flush=state==ABORT&&!rd_busy&&!wr_busy;
 gfx_tile4_buffer #(.LAZY_CLEAR(LAZY_TILE_CLEAR)) tile_buffer(clk,rst_n&&state!=ABORT,tile_begin,skip_tile_clear,buf_tile_ready,base_clear_pixel,start_tile_w,start_tile_h,
  buf_read_valid,buf_read_ready,group,buf_response_valid,buf_response_ready,buf_response_index,buf_response_pixels,
  buf_write_valid,buf_write_ready,group,result_pixels,tile_end,
  buf_out_valid,buf_out_ready,buf_out_pixels,buf_out_keep,buf_out_last,buf_busy);
 gfx_tile4_group_pingpong group_buffer(clk,rst_n,group_flush,
  group_begin,group_ready,tile_x,tile_y,start_group_w,start_tile_h,
  group_write_valid,group_write_ready,group_write_index,buf_out_pixels,group_end,
  group_row_valid,group_row_ready,group_row_pixels,group_row_strb,group_row_beats,
  group_row_x,group_row_y,group_row_last,group_busy);

 wire [3:0] pipe_ready,pipe_valid,pipe_write,pipe_last;wire [127:0] pipe_pixels;
 reg [31:0] pipe_op;
 function [31:0] source_pixel;
  input [255:0] pixels;input [2:0] word_index;
  begin case(word_index)
   0:source_pixel=pixels[31:0];1:source_pixel=pixels[63:32];
   2:source_pixel=pixels[95:64];3:source_pixel=pixels[127:96];
   4:source_pixel=pixels[159:128];5:source_pixel=pixels[191:160];
   6:source_pixel=pixels[223:192];default:source_pixel=pixels[255:224];
  endcase end
 endfunction
 wire [255:0] fast_source_words={source1,source0};
 wire [31:0] fast_pixel0=source_pixel(fast_source_words,{1'b0,src_offset});
 wire [31:0] fast_pixel1=source_pixel(fast_source_words,{1'b0,src_offset}+3'd1);
 wire [31:0] fast_pixel2=source_pixel(fast_source_words,{1'b0,src_offset}+3'd2);
 wire [31:0] fast_pixel3=source_pixel(fast_source_words,{1'b0,src_offset}+3'd3);
 wire [127:0] fast_image_pixels={{8'hff,fast_pixel3[23:0]},
  {8'hff,fast_pixel2[23:0]},{8'hff,fast_pixel1[23:0]},
  {8'hff,fast_pixel0[23:0]}};
 // An opaque four-pixel image group overwrites the old Tile unconditionally.
 // Keyed, partially covered and pixel-alpha groups retain the original path.
 wire fast_image_opaque=active_mask==4'hf&&dalpha[7:0]==8'hff&&
  !dc[2]&&(!dc[1]||(fast_pixel0[23:0]!=dkey[23:0]&&
  fast_pixel1[23:0]!=dkey[23:0]&&fast_pixel2[23:0]!=dkey[23:0]&&
  fast_pixel3[23:0]!=dkey[23:0]));
 // A wholly transparent source group leaves the Tile unchanged. Test the
 // actual active lanes after the source RAM read, before fetching destination
 // pixels or entering the Alpha pipeline. Partial coverage must use the same
 // first-lane mapping as pixel_src, rather than the opaque four-lane shortcut.
 wire [3:0] transparent_lane;
 wire transparent_image_group=layer_image&&(&transparent_lane);
 always @*begin
  pipe_op=(dalpha[7:0]!=8'hff||dc[2])?32'd3:32'd2;
  if(layer_image&&dc[1])pipe_op=pipe_op|32'h100;
  if(layer_image&&dc[2])pipe_op=pipe_op|32'h200;
 end
 genvar g;
 assign pixel_rst_n=rst_n&&state!=ABORT;
 assign pixel_in_valid=state==PIPE_FEED;
 assign pixel_out_ready=state==PIPE_WAIT;assign pixel_in_last=1'b0;
 assign pixel_dst=background;assign pixel_color=dcolor;assign pixel_op=pipe_op;
 assign pixel_alpha=dalpha[7:0];assign pixel_key=dkey[23:0];
 generate if(SHARED_PIXEL_PIPE)begin:shared_pixels
  assign pipe_ready=pixel_ready;assign pipe_valid=pixel_valid;
  assign pipe_write=pixel_write;assign pipe_last=pixel_last;
  assign pipe_pixels=pixel_result;
 end else begin:local_pixels
 end endgenerate
 generate for(g=0;g<4;g=g+1)begin:lanes
  localparam [2:0] LANE=g;
  wire [2:0] lane_delta=LANE>=first_lane?LANE-first_lane:0;
  wire [2:0] source_word=src_offset+lane_delta;
  wire [31:0] lane_source=layer_image?source_pixel({source1,source0},source_word):{8'hff,dcolor[23:0]};
  assign pixel_src[g*32+:32]=lane_source;
  assign transparent_lane[g]=!active_mask[g]||dalpha[7:0]==0||
   (dc[1]&&lane_source[23:0]==dkey[23:0])||
   (dc[2]&&lane_source[31:24]==0);
  if(!SHARED_PIXEL_PIPE)begin:local_pixel_lane
   gfx_pixel_pipe pp(clk,rst_n&&state!=ABORT,state==PIPE_FEED,pipe_ready[g],lane_source,background[g*32+:32],dcolor,pipe_op,dalpha[7:0],dkey[23:0],1'b0,
    pipe_valid[g],state==PIPE_WAIT,pipe_pixels[g*32+:32],pipe_write[g],pipe_last[g]);
  end
 end endgenerate
 wire pipes_ready=&pipe_ready,pipes_valid=&pipe_valid;
 always @*begin
  if(FAST_OPAQUE_IMAGE&&image_fast)result_pixels=fast_image_pixels;
  else if(!layer_image&&dalpha[7:0]==8'hff&&active_mask==4'hf)result_pixels={4{{8'hff,dcolor[23:0]}}};
  else begin
   result_pixels=background;
   for(i=0;i<4;i=i+1)if(active_mask[i]&&write_capture[i])result_pixels[i*32+:32]=blended_capture[i*32+:32];
  end
 end

 assign write_request=wr_state==WR_IDLE&&group_row_valid&&state!=ABORT;
 assign write_addr=dst+group_row_y*dst_stride+(group_row_x<<2);
 assign group_row_ready=wr_state==WR_ACTIVE&&wr_in_ready;
 gfx_write_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) writer(clk,rst_n,write_request,wr_req_ready,write_addr,group_row_beats,
  group_row_pixels,group_row_strb,group_row_valid,wr_in_ready,
  awaddr,awlen,awsize,awburst,awvalid,awready,wdata,wstrb,wlast,wvalid,wready,bresp,bvalid,bready,wr_done,wr_error,wr_busy);

 assign cmd_ready=state==IDLE;assign busy=state!=IDLE;assign recover_safe=!busy&&!rd_busy&&!wr_busy&&!buf_busy&&!group_busy;
 always @(posedge clk)begin
  if(!rst_n)begin state<=IDLE;done<=0;done_seq<=0;fault<=0;fault_addr<=0;desc_beat<=0;desc0<=0;desc1<=0;desc2<=0;desc3<=0;desc4<=0;desc5<=0;desc6<=0;desc7<=0;active_desc<=0;selected_index<=0;sorted0<=0;sorted1<=0;sorted2<=0;sorted3<=0;sorted4<=0;sorted5<=0;sorted6<=0;sorted7<=0;sort_position<=0;sort_scan<=0;sort_best<=0;sort_used<=0;sort_best_valid<=0;tile_x<=0;tile_y<=0;render_x0<=0;render_y0<=0;render_x1<=0;render_y1<=0;layer<=0;group<=0;src_rx<=0;src_offset<=0;source_row_addr<=0;source_row_index<=0;source_row_valid<=0;tile_slot<=0;capture_beat<=0;scan_rank<=0;base_start_layer<=0;base_clear_pixel<=0;tile_has_opaque<=0;tile_candidate_prefix<=0;tile_skipped_prefix<=0;tile_candidate_refs<=0;tile_executed_refs<=0;tile_skipped_refs<=0;tile_opaque_tiles<=0;list_mode<=0;list_addr<=0;list_count<=0;list_index<=0;list_write_index<=0;list_beat<=0;list_lane<=0;list_beat_valid<=0;list_dma_done<=0;bin_enable<=0;bin_overlay_mode<=0;bin_count_invalid<=0;bin_first<=0;bin_count<=0;bin_index<=0;bin_rx<=0;bin_high<=0;bin_word0<=0;bin_word1<=0;bin_word2<=0;bin_high_pending<=0;bin_dma_done<=0;wr_state<=WR_IDLE;end
  else begin
   done<=0;
   if(write_request&&wr_req_ready)wr_state<=WR_ACTIVE;
   if(wr_done)wr_state<=WR_IDLE;
   if(fault==0)begin
    if(rd_error!=0)begin fault<=rd_error;fault_addr<=read_addr;end
    else if(wr_error!=0)begin fault<=wr_error;fault_addr<=write_addr;end
   end
   if(rd_valid&&rd_consume_ready)begin
    if(state==DESC_WAIT)begin
     case(desc_beat)
      0:desc0[127:0]<=rd_data;1:desc0[255:128]<=rd_data;
      2:desc1[127:0]<=rd_data;3:desc1[255:128]<=rd_data;
      4:desc2[127:0]<=rd_data;5:desc2[255:128]<=rd_data;
      6:desc3[127:0]<=rd_data;7:desc3[255:128]<=rd_data;
      8:desc4[127:0]<=rd_data;9:desc4[255:128]<=rd_data;
      10:desc5[127:0]<=rd_data;11:desc5[255:128]<=rd_data;
      12:desc6[127:0]<=rd_data;13:desc6[255:128]<=rd_data;
      14:desc7[127:0]<=rd_data;default:desc7[255:128]<=rd_data;
     endcase
     desc_beat<=desc_beat+1'b1;
     end else if(state==LIST_WAIT)begin list_beat<=rd_data;list_beat_valid<=1;list_lane<=0;end
     else if(state==BIN_HEADER_WAIT)begin
      bin_first<=rd_data[31:0];bin_count<=rd_data[38:32];
      bin_count_invalid<=|rd_data[63:39];
     end
     else if(state!=BIN_STREAM_WAIT)src_rx<=src_rx+1'b1;
   end
   if(state==PIPE_WAIT&&pipes_valid)begin blended_capture<=pipe_pixels;write_capture<=pipe_write;end
   case(state)
    IDLE:if(cmd_valid)begin
     desc_base<=cmd[32+:32];dst<=cmd[64+:32];width<=cmd[96+:32];height<=cmd[128+:32];desc_count<=cmd[163:160];dst_stride<=cmd[192+:32];
     clear_color<=cmd[224+:32];seq<=cmd[320+:32];done_seq<=cmd[320+:32];fault<=0;fault_addr<=0;desc_beat<=0;
     bin_enable<=cmd[255];bin_overlay_mode<=0;
     list_mode<=&cmd[287:286];list_addr<={2'b0,cmd[285:256]};list_count<={1'b0,cmd[303:288]};
     list_write_index<=0;list_index<=0;list_beat_valid<=0;list_dma_done<=0;
     render_x0<=cmd[287]&&!cmd[286]?{16'b0,cmd[271:260],4'b0}:0;
     render_y0<=cmd[287]&&!cmd[286]?{17'b0,cmd[285:276],4'b0}:0;
     render_x1<=cmd[287]&&!cmd[286]?({16'b0,cmd[271:256]}+{16'b0,cmd[303:288]}):cmd[96+:32];
     render_y1<=cmd[287]&&!cmd[286]?({17'b0,cmd[286:272]}+{16'b0,cmd[319:304]}):cmd[128+:32];
     tile_candidate_refs<=0;tile_executed_refs<=0;tile_skipped_refs<=0;tile_opaque_tiles<=0;state<=DESC_REQ;
    end
    DESC_REQ:if(rd_req_ready)state<=DESC_WAIT;
    DESC_WAIT:if(rd_done)begin
     if(rd_error!=0)state<=ABORT;
     else begin sort_position<=0;sort_scan<=0;sort_best<=0;sort_used<=0;sort_best_valid<=0;state<=DESC_SORT;end
    end
    DESC_SORT:begin
     if(sort_scan==7)begin
      case(sort_position)
       0:sorted0<=sort_choice;1:sorted1<=sort_choice;2:sorted2<=sort_choice;3:sorted3<=sort_choice;
       4:sorted4<=sort_choice;5:sorted5<=sort_choice;6:sorted6<=sort_choice;default:sorted7<=sort_choice;
      endcase
      sort_used[sort_choice]<=1'b1;
      if({1'b0,sort_position}+1>=desc_count)begin
       if(list_mode)state<=LIST_REQ;
       else begin tile_x<=render_x0;tile_y<=render_y0;tile_slot<=0;state<=TILE_SELECT;end
      end else begin sort_position<=sort_position+1'b1;sort_scan<=0;sort_best_valid<=0;end
     end else begin
      if(sort_take)begin sort_best<=sort_scan;sort_best_valid<=1;end
      sort_scan<=sort_scan+1'b1;
     end
    end
    LIST_REQ:if(rd_req_ready)begin list_write_index<=0;list_beat_valid<=0;list_dma_done<=0;state<=LIST_WAIT;end
    LIST_WAIT:begin
     if(rd_done)list_dma_done<=1;
     if(list_beat_valid)begin
      list_write_index<=list_write_index+1'b1;
      if(list_lane==3||list_write_index+1>=list_count)begin list_beat_valid<=0;list_lane<=0;end
      else list_lane<=list_lane+1'b1;
     end
     if((list_dma_done||rd_done)&&!list_beat_valid&&list_write_index>=list_count)begin list_index<=0;state<=LIST_PRIME;end
    end
    LIST_PRIME:state<=LIST_APPLY;
    LIST_APPLY:begin
     if(!list_run_tiles||list_run_tiles>8||{2'b0,list_run_tile_right}>list_screen_cols||
        {3'b0,list_run_y}>=list_screen_rows)begin
      fault<=`GFX_ERR_SIZE;fault_addr<=list_addr+{21'b0,list_index,2'b0};state<=ABORT;
     end else begin
      render_x0<=list_run_px;render_y0<=list_run_py;
      render_x1<=list_run_right>width?width:list_run_right;
      render_y1<=list_run_bottom>height?height:list_run_bottom;
      tile_x<=list_run_px;tile_y<=list_run_py;tile_slot<=0;state<=TILE_SELECT;
     end
    end
    // Find the highest-Z opaque Solid that fully covers this Tile. Its colour
    // is the exact result after that layer, so every lower layer and the Solid
    // itself can be folded into Tile initialization. Scan one ranked
    // descriptor per clock to avoid a second wide descriptor selection path.
    TILE_SELECT:begin
     scan_rank<=0;base_start_layer<=0;base_clear_pixel<={8'hff,clear_color[23:0]};selected_index<=ranked_index;
     tile_candidate_prefix<=0;tile_skipped_prefix<=0;tile_has_opaque<=0;
     bin_overlay_mode<=0;
     state<=FOLD_OPAQUE_BASE?BASE_SCAN:TILE_START;
    end
    BASE_SCAN:begin
     tile_candidate_prefix<=scan_candidate_after;
     if(scan_opaque_tile)begin
      base_start_layer<=scan_rank+1'b1;base_clear_pixel<={8'hff,scan_color[23:0]};
      tile_skipped_prefix<=scan_candidate_after;tile_has_opaque<=1;
     end
     if(scan_rank+1>=desc_count)begin
      tile_candidate_refs<=tile_candidate_refs+scan_candidate_after;
      tile_skipped_refs<=tile_skipped_refs+scan_skipped_after;
      if(tile_has_opaque||scan_opaque_tile)tile_opaque_tiles<=tile_opaque_tiles+1'b1;
      state<=TILE_START;
     end
     else begin scan_rank<=scan_rank+1'b1;selected_index<=ranked_index;end
    end
    TILE_START:if(buf_tile_ready&&(tile_slot!=0||group_ready))begin
     tile_w<=start_tile_w;tile_h<=start_tile_h;selected_index<=ranked_index;state<=TILE_CLEAR;
    end
    TILE_CLEAR:if(buf_read_ready)begin
     layer<=base_start_layer;selected_index<=ranked_index;
     state<=base_start_layer>=desc_count?(bin_enable?BIN_HEADER_REQ:TILE_END):LAYER_START;
    end
    // The command clear colour is the semantic layer below rank zero.  If
    // rank zero is the same opaque Solid over this whole Tile, initialization
    // has already produced its exact pixels; avoid replaying 64 groups.
    LAYER_START:begin
     group<=0;source_row_valid<=0;active_desc<=dsel;state<=LAYER_EVAL;
    end
    LAYER_EVAL:begin
     // Walk only the destination groups touched by this descriptor. The
     // per-lane mask still handles the two partially covered edge groups.
     group<={clip_top[3:0],clip_left[3:2]};
     group_first_column<=clip_left[3:2];group_last_column<=clip_last_x[3:2];
     group_last_row<=clip_bottom-1'b1;
     if(!bin_overlay_mode&&layer_intersects_tile&&!fold_clear_layer)
      tile_executed_refs<=tile_executed_refs+1'b1;
     state<=fold_clear_layer?LAYER_NEXT:layer_intersects_tile?GROUP_PLAN:
      bin_overlay_mode?BIN_ADVANCE:LAYER_NEXT;
    end
    GROUP_PLAN:begin
     image_fast<=0;
     active_mask<=plan_mask;first_lane<=plan_first;active_count<=plan_count;
     if(plan_mask==0||(masked_bin&&!dcolor[source_y[3:0]]))advance_group();
     else if(layer_image)begin
      src_offset<=source_addr[3:2];
      if(layer_tiled)source_ram_base<=source_in_tile[9:4];
      else if(source_row_valid&&source_row_index==group[5:2])
       source_ram_base<=source_addr[9:4]-source_row_addr[9:4];
      else source_ram_base<=0;
       if(hot_hit)begin
        source_row_addr<=source_fetch_addr;source_row_valid<=1;state<=SRC_CACHE_PRIME;
       end else if(source_row_valid&&(layer_tiled?source_row_addr==source_fetch_addr:
          source_row_index==group[5:2]))state<=SRC_CACHE_PRIME;
      else begin
       src_addr_aligned<=source_fetch_addr;source_row_addr<=source_fetch_addr;
        source_row_index<=group[5:2];source_row_valid<=0;
        src_beats<=source_fetch_beats;src_rx<=0;state<=SRC_REQ;
      end
     end
     // Fully covered opaque Solid groups overwrite all four pixels. Reading
     // the old Tile value and running four Alpha lanes cannot affect output.
     else state<=opaque_solid_group?BUF_WRITE:BUF_REQ;
    end
    SRC_REQ:if(rd_req_ready)begin
     // A tiled source may cross a source-Tile boundary inside one screen
     // Tile when the layer X/Y origin is not 16-pixel aligned.  The previous
     // 64-beat fetch can still be draining after GROUP_PLAN requested the
     // next Tile, so the earlier speculative src_rx clear may be overwritten
     // by those late beats.  Reset at the actual DMA request handshake: no
     // response from the new burst can precede this point.
     src_rx<=0;source_row_valid<=1;state<=SRC_WAIT;
    end
    SRC_WAIT:if(rd_error!=0)state<=ABORT;
     else if((layer_tiled&&src_rx>=2)||rd_done)state<=SRC_CACHE_PRIME;
    SRC_CACHE_PRIME:if(source_cache_word_ready)state<=SRC_CACHE_LO;
    SRC_CACHE_LO:begin
     source0<=source_ram_data;
     // Four source pixels aligned to one 128-bit RAM word never use the
     // second word. Keep the two-word path for unaligned/clipped images.
     if(src_offset==0)begin
      source1<=0;state<=FAST_OPAQUE_IMAGE?FAST_IMAGE_CHECK:BUF_REQ;
     end else state<=SRC_CACHE_HI;
    end
    SRC_CACHE_HI:begin source1<=source_ram_data;
     state<=FAST_OPAQUE_IMAGE?FAST_IMAGE_CHECK:BUF_REQ;end
    FAST_IMAGE_CHECK:begin
     image_fast<=FAST_OPAQUE_IMAGE&&fast_image_opaque;
     if(transparent_image_group)advance_group();
     else state<=fast_image_opaque?BUF_WRITE:BUF_REQ;
    end
    BUF_REQ:if(buf_read_ready)state<=BUF_WAIT;
    BUF_WAIT:if(buf_response_valid)begin background<=buf_response_pixels;state<=PIPE_FEED;end
    PIPE_FEED:if(pipes_ready)state<=PIPE_WAIT;
    PIPE_WAIT:if(pipes_valid)state<=BUF_WRITE;
    // A completed write has no dependency on an extra GROUP_NEXT bubble.
    BUF_WRITE:if(buf_write_ready)advance_group();
    GROUP_NEXT:advance_group();
    LAYER_NEXT:if(layer+1>=desc_count)state<=bin_enable?BIN_HEADER_REQ:TILE_END;
     else begin layer<=layer+1'b1;selected_index<=ranked_index;state<=LAYER_START;end
    BIN_HEADER_REQ:if(rd_req_ready)state<=BIN_HEADER_WAIT;
    BIN_HEADER_WAIT:if(rd_error!=0)state<=ABORT;
     else if(rd_done)state<=BIN_HEADER_CHECK;
    BIN_HEADER_CHECK:begin
     if(bin_count_invalid||bin_count>64||bin_first[3:0]!=0||
       {1'b0,bin_first}<{1'b0,bin_slot_base}+33'd38912||
       bin_stream_end>{1'b0,bin_slot_base}+33'd131072)begin
      fault<=`GFX_ERR_RANGE;fault_addr<=bin_first;state<=ABORT;
     end else if(bin_count==0)state<=TILE_END;
     else begin bin_rx<=0;state<=BIN_STREAM_REQ;end
    end
    BIN_STREAM_REQ:if(rd_req_ready)begin
     bin_rx<=0;bin_high_pending<=0;bin_dma_done<=0;state<=BIN_STREAM_WAIT;
    end
    BIN_STREAM_WAIT:if(rd_error!=0)state<=ABORT;
     else begin
      if(bin_accept)begin bin_high<=rd_data[127:64];bin_high_pending<=1;end
      else if(bin_high_pending)begin bin_high_pending<=0;bin_rx<=bin_rx+1'b1;end
      if(rd_done)bin_dma_done<=1;
      if(bin_dma_done&&!bin_high_pending&&!rd_valid)begin
       bin_index<=0;state<=BIN_DESC_PRIME;
      end
     end
    BIN_DESC_PRIME:state<=BIN_DESC_LO;
    BIN_DESC_LO:begin bin_word0<=bin_rdata;state<=BIN_DESC_HI;end
    BIN_DESC_HI:begin bin_word1<=bin_rdata;state<=BIN_DESC_HI2;end
    BIN_DESC_HI2:begin bin_word2<=bin_rdata;state<=BIN_DESC_HI3;end
    BIN_DESC_HI3:begin
     active_desc<={bin_rdata,bin_word2,bin_word1,bin_word0};group<=0;source_row_valid<=0;
     bin_overlay_mode<=1;state<=LAYER_EVAL;
    end
    BIN_ADVANCE:if(bin_index+1>=bin_count)state<=TILE_END;
     else begin bin_index<=bin_index+1'b1;state<=BIN_DESC_PRIME;end
    TILE_END:begin capture_beat<=0;state<=OUT_WAIT;end
    OUT_WAIT:if(buf_out_valid)state<=CAPTURE;
    CAPTURE:if(buf_out_valid&&group_write_ready)begin
     capture_beat<=capture_beat+1'b1;
     if(buf_out_last)begin
      if(last_tile_in_group)begin
       tile_slot<=0;
       if(tile_x+16>=render_x1)begin
        tile_x<=render_x0;
        if(tile_y+16>=render_y1)state<=PRODUCE_DRAIN;
        else begin tile_y<=tile_y+16;state<=TILE_SELECT;end
       end else begin tile_x<=tile_x+16;state<=TILE_SELECT;end
      end else begin tile_slot<=tile_slot+1'b1;tile_x<=tile_x+16;state<=TILE_SELECT;end
     end
    end
    PRODUCE_DRAIN:if(!group_busy&&!wr_busy&&wr_state==WR_IDLE)begin
     if(list_mode&&list_index+1<list_count)begin list_index<=list_index+1'b1;state<=LIST_PRIME;end
     else state<=FINISH;
    end
    ABORT:if(!rd_busy&&!wr_busy&&!group_busy)state<=FINISH;
    FINISH:begin done<=1;done_seq<=seq;state<=IDLE;end
    default:state<=IDLE;
   endcase
   if((rd_error!=0||wr_error!=0)&&state!=IDLE&&state!=FINISH&&state!=ABORT)state<=ABORT;
  end
 end
endmodule
`default_nettype wire
