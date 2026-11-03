`include "gfx_abi.vh"
// Relocatable system-clock IP. Integration supplies the verified address window
// and protected front/pending spans; no board address is silently invented.
// One 32 KiB source reader is shared by the legacy and bucket engines; each
// engine routes the reader's AXI transaction through its own read port when
// its internal DMA is idle, mirroring the original background/source split.
// HAS_BUCKET=0 builds the game mirror: the bucket engine, its group backend
// and their arbitration are absent; a bucket command faults one cycle after
// dispatch (GFX_ERR_OPCODE) instead of hanging the queue, and gfx_regs drops
// the BUCKET caps bit so cap-negotiating firmware never submits one.
// BUCKET_ROW_PREFETCH=1 (m12 candidate) adds the bucket engine's sprite-row
// source prefetch stage; 0 keeps the original strictly-serial row path.
// BUCKET_RCOPY_PREFETCH=1 (m12 candidate) adds the bucket engine's recipe
// COPY row prefetch; 0 keeps the original serial per-row control DMA.
// COPY_STREAM=1 (key3 candidate) routes plain aligned wide COPY commands to
// the streaming engine; 0 keeps every COPY on the legacy planner path.
// SMALL_PIPE/SPRITE_ROWS/PRESENT_FENCE/STREAM_SUBMIT/RING_FETCH (key3
// candidate) gate the compact pixel pipe, sprite row-slot depth, present
// fencing, stream submission and the DDR command ring fetcher.
module gfx_core #(parameter QUEUE_DEPTH=16,TIMEOUT_CYCLES=10000000,FRAME_BYTES=1228800,CAPS_EXTRA=0,MAX_FILL_BEATS=16,SHARED_PIXEL_PIPE=2,HAS_BUCKET=1,BUCKET_ROW_PREFETCH=0,BUCKET_RCOPY_PREFETCH=0,COPY_STREAM=0,SMALL_PIPE=0,SPRITE_ROWS=4,PRESENT_FENCE=0,STREAM_SUBMIT=0,RING_FETCH=0,BUCKET_SOURCE_TRIM=0,FRAME_TRACE=0)(
 input wire clk,rst_n,input wire psel,penable,pwrite,input wire [11:0] paddr,input wire [31:0] pwdata,
 output wire pready,output wire [31:0] prdata,output wire pslverr,irq,
 input wire [31:0] pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,
 input wire ring_push_valid,output wire ring_push_ready,input wire [`GFX_CMD_BITS-1:0] ring_push_data,
 input wire ring_fault_valid,input wire [7:0] ring_fault_code,output wire queue_clear,
 output wire display_request,output wire [31:0] display_base,display_id,
 input wire display_ready,display_pending,display_ack,input wire [31:0] front_base,presented_id,released_base,underflow,input wire [7:0] display_fault,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready,
 input wire display_urgent,
 input wire [31:0] diag_blanks,diag_shown,diag_new_id,diag_repeat,diag_dbl_rpt,diag_miss,diag_acks,
 input wire [31:0] diag_drain_last,diag_drain_max,diag_desc_last,diag_desc_max,diag_pref_last,diag_pref_max,
 output wire diag_clear,
 output wire [31:0] display_seq,completed_seq,output wire render_fault
);
 wire cv,cr,busy,safe,done;wire [`GFX_CMD_BITS-1:0] cmd;wire [31:0] seq,fa;wire [7:0] err;
 wire legacy_ready,legacy_busy,legacy_safe,legacy_done;wire [31:0] legacy_seq,legacy_fa;wire [7:0] legacy_err;
 wire bucket_ready,bucket_busy,bucket_safe,bucket_done;wire [31:0] bucket_seq,bucket_fa;wire [7:0] bucket_err;
 wire copy_ready,copy_busy,copy_safe,copy_done;wire [31:0] copy_seq,copy_fa;wire [7:0] copy_err;
 reg use_copy,use_small;
 wire small_ready,small_busy,small_safe,small_done;wire [31:0] small_seq,small_fa,small_bg_bypasses;wire [7:0] small_err;
 wire command_is_small=SMALL_PIPE&&(SHARED_PIXEL_PIPE!=0)&&cmd[7:0]==3&&
  cmd[96+:32]==16&&cmd[128+:32]>0&&cmd[128+:32]<=32&&
  cmd[160+:32]==64&&cmd[192+:32]==4096&&cmd[37:32]==0&&cmd[64+:12]<=4032;
 wire [31:0] s_araddr,s_awaddr;wire [7:0] s_arlen,s_awlen;wire [2:0] s_arsize,s_awsize;
 wire [1:0] s_arburst,s_awburst;wire s_arvalid,s_rready,s_awvalid,s_wlast,s_wvalid,s_bready;
 wire [127:0] s_wdata;wire [15:0] s_wstrb;
 wire s_src_req_valid,s_src_arready,s_src_rbeat_valid;wire [31:0] s_src_req_addr;wire [4:0] s_src_req_beats;
 wire s_px_rst,s_px_iv,s_px_or,s_px_last;wire [127:0] s_px_src,s_px_dst;
 wire [31:0] s_px_color,s_px_op;wire [7:0] s_px_alpha;wire [23:0] s_px_key;
 // Full rows/strides must stay aligned; short and partial-lane copies retain
 // legacy semantics. gfx_regs validates range, overlap and protected spans.
 wire command_is_copy=COPY_STREAM&&cmd[31:0]==`GFX_OP_COPY&&
  cmd[35:32]==0&&cmd[67:64]==0&&cmd[96+:32]>=128&&cmd[97:96]==0&&
  cmd[128+:32]!=0&&cmd[163:160]==0&&cmd[195:192]==0;
 wire command_is_bucket=cmd[31:0]==`GFX_OP_BUCKET;reg use_bucket;
 wire cache_enable,cache_flush,opaque_enable;wire [31:0] cache_hits,cache_misses,cache_bypasses;
 gfx_regs #(.QUEUE_DEPTH(QUEUE_DEPTH),.FRAME_BYTES(FRAME_BYTES),.CAPS_EXTRA(CAPS_EXTRA),.HAS_BUCKET(HAS_BUCKET),.PRESENT_FENCE(PRESENT_FENCE),.STREAM_SUBMIT(STREAM_SUBMIT),.RING_FETCH(RING_FETCH),.FRAME_TRACE(FRAME_TRACE)) regs(clk,rst_n,psel,penable,pwrite,paddr,pwdata,pready,prdata,pslverr,irq,
  pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,
  ring_push_valid,ring_push_ready,ring_push_data,ring_fault_valid,ring_fault_code,queue_clear,
  cv,cr,cmd,
  busy,safe,done,seq,err,fa,display_request,display_base,display_id,
  display_ready,display_pending,display_ack,front_base,presented_id,released_base,underflow,display_fault,
  rvalid&&rready,wvalid&&wready,wstrb,(arvalid&&!arready)||(awvalid&&!awready)||(wvalid&&!wready),
  cache_enable,cache_flush,cache_hits,cache_misses,cache_bypasses,
  diag_blanks,diag_shown,diag_new_id,diag_repeat,diag_dbl_rpt,diag_miss,diag_acks,
  diag_drain_last,diag_drain_max,diag_desc_last,diag_desc_max,diag_pref_last,diag_pref_max,diag_clear,opaque_enable,display_seq,completed_seq,render_fault);
 wire soft_reset=psel&&penable&&pready&&pwrite&&!pslverr&&paddr==16&&pwdata==2&&safe&&!display_pending;
 // The queue may only advance when the engine that OWNS the bus right now
 // is idle. Muxing the pop ready by the INCOMING command's engine lets a
 // queued bucket command preempt a running legacy command mid-burst
 // (orphaning its AXI read) - latent until engines were first interleaved
 // in one queue (M3b-1 boundary TB).
 wire engine_ready=use_small?small_ready:use_copy?copy_ready:(use_bucket?bucket_ready:legacy_ready);
 wire target_ready=command_is_small?small_ready:command_is_copy?copy_ready:(command_is_bucket?bucket_ready:legacy_ready);
 assign cr=engine_ready&&target_ready&&!soft_reset;
 // One handshake is the sole dispatch event: it pops the queue, changes the
 // owner mux, and starts exactly the selected engine on the same edge.
 wire dispatch_fire=cv&&cr;
 assign busy=use_small?small_busy:use_copy?copy_busy:(use_bucket?bucket_busy:legacy_busy);assign safe=legacy_safe&&bucket_safe&&copy_safe&&small_safe;
 assign done=use_small?small_done:use_copy?copy_done:(use_bucket?bucket_done:legacy_done);
 assign seq=use_small?small_seq:use_copy?copy_seq:(use_bucket?bucket_seq:legacy_seq);
 assign err=use_small?small_err:use_copy?copy_err:(use_bucket?bucket_err:legacy_err);
 assign fa=use_small?small_fa:use_copy?copy_fa:(use_bucket?bucket_fa:legacy_fa);
 always @(posedge clk)begin
  if(!rst_n||soft_reset)begin use_bucket<=0;use_copy<=0;use_small<=0;end
  else if(dispatch_fire)begin use_bucket<=command_is_bucket;use_copy<=command_is_copy;use_small<=command_is_small;end
 end
 wire [31:0] c_araddr,c_awaddr;wire [7:0] c_arlen,c_awlen;wire [2:0] c_arsize,c_awsize;
 wire [1:0] c_arburst,c_awburst;wire c_arvalid,c_rready,c_awvalid,c_wlast,c_wvalid,c_bready;
 wire [127:0] c_wdata;wire [15:0] c_wstrb;
 wire [31:0] laraddr,lawaddr,baraddr,gawaddr;wire [7:0] larlen,lawlen,barlen,gawlen;
 wire [2:0] larsize,lawsize,barsize,gawsize;wire [1:0] larburst,lawburst,barburst,gawburst;
 wire larvalid,lrready,lawvalid,lwlast,lwvalid,lbready,barvalid,brready,gawvalid,gwlast,gwvalid,gbready;
 wire [127:0] lwdata,gwdata;wire [15:0] lwstrb,gwstrb;
 // M3b-1 shared group backend: one dual-bank group store and write-back
 // service, serving the bucket engine (built only with HAS_BUCKET).
 wire b_grp_begin,b_grp_ready,b_fill_we,b_grp_commit,b_desc_pitch32,b_grp_flush,b_grp_idle;
 wire gb_fill_open;
 wire [8:0] b_fill_waddr,b_fill_raddr;
 wire [127:0] b_fill_wdata,b_fill_rdata;
 wire [31:0] b_desc_base,b_desc_stride,b_grp_error_addr;
 wire [5:0] b_desc_rows;wire [4:0] b_desc_beats_m1;
 wire [7:0] b_desc_width,b_grp_error;

 assign araddr=use_small?s_araddr:use_copy?c_araddr:(use_bucket?baraddr:laraddr);assign arlen=use_small?s_arlen:use_copy?c_arlen:(use_bucket?barlen:larlen);assign arsize=use_small?s_arsize:use_copy?c_arsize:(use_bucket?barsize:larsize);assign arburst=use_small?s_arburst:use_copy?c_arburst:(use_bucket?barburst:larburst);assign arvalid=use_small?s_arvalid:use_copy?c_arvalid:(use_bucket?barvalid:larvalid);assign rready=use_small?s_rready:use_copy?c_rready:(use_bucket?brready:lrready);
 assign awaddr=use_small?s_awaddr:use_copy?c_awaddr:(use_bucket?gawaddr:lawaddr);assign awlen=use_small?s_awlen:use_copy?c_awlen:(use_bucket?gawlen:lawlen);assign awsize=use_small?s_awsize:use_copy?c_awsize:(use_bucket?gawsize:lawsize);assign awburst=use_small?s_awburst:use_copy?c_awburst:(use_bucket?gawburst:lawburst);assign awvalid=use_small?s_awvalid:use_copy?c_awvalid:(use_bucket?gawvalid:lawvalid);
 assign wdata=use_small?s_wdata:use_copy?c_wdata:(use_bucket?gwdata:lwdata);assign wstrb=use_small?s_wstrb:use_copy?c_wstrb:(use_bucket?gwstrb:lwstrb);assign wlast=use_small?s_wlast:use_copy?c_wlast:(use_bucket?gwlast:lwlast);assign wvalid=use_small?s_wvalid:use_copy?c_wvalid:(use_bucket?gwvalid:lwvalid);assign bready=use_small?s_bready:use_copy?c_bready:(use_bucket?gbready:lbready);
 // Shared source reader, selected per engine by use_bucket. Each engine routes
// the reader's AXI transaction through its own read port whenever its
// internal DMA does not own it, so exactly one AR is presented at a time.
 wire l_src_req_valid,b_src_req_valid,l_src_req_ready,b_src_req_ready;
 wire [31:0] l_src_req_addr,b_src_req_addr;wire [4:0] l_src_req_beats,b_src_req_beats;
 wire [31:0] rs_araddr;wire [7:0] rs_arlen;wire [2:0] rs_arsize;wire [1:0] rs_arburst;
 wire rs_arvalid,rs_arready,rs_req_ready,rready_from_reader;
 wire rs_out_valid,rs_done,rs_busy,rs_cache_serving;wire [7:0] rs_error;
 wire [31:0] rs_hits,rs_misses,rs_bypasses;
 wire [127:0] rs_out_data;
 wire l_src_arready,b_src_arready,l_src_rbeat_valid,b_src_rbeat_valid;
 wire [31:0] legacy_bg_bypasses;
  wire legacy_row_wr_en,bucket_row_wr_en;
  wire [3:0] legacy_row_wr_addr,legacy_row_rd_addr,bucket_row_wr_addr,bucket_row_rd_addr;
  wire [127:0] legacy_row_wr_data,bucket_row_wr_data,shared_row_rd_data;
  wire shared_row_wr_en=use_bucket?bucket_row_wr_en:legacy_row_wr_en;
  wire [3:0] shared_row_wr_addr=use_bucket?bucket_row_wr_addr:legacy_row_wr_addr;
  wire [127:0] shared_row_wr_data=use_bucket?bucket_row_wr_data:legacy_row_wr_data;
  wire [3:0] shared_row_rd_addr=use_bucket?bucket_row_rd_addr:legacy_row_rd_addr;
  gfx_source_row_ram shared_source_rows(clk,rst_n&&!soft_reset,shared_row_wr_en,
   shared_row_wr_addr,shared_row_wr_data,shared_row_rd_addr,shared_row_rd_data);
 assign rs_arready=use_small?s_src_arready:use_bucket?b_src_arready:l_src_arready;
 assign l_src_req_ready=(use_bucket||use_small)?1'b0:rs_req_ready;assign b_src_req_ready=use_bucket?rs_req_ready:1'b0;
 wire reader_rvalid=!use_copy&&(use_small?s_src_rbeat_valid:(use_bucket?b_src_rbeat_valid:l_src_rbeat_valid));
 wire l_src_rready=(use_bucket||use_small)?1'b0:rready_from_reader;
 wire b_src_rready=use_bucket?rready_from_reader:1'b0;
 gfx_source_reader #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) source_reader(
  clk,rst_n&&!soft_reset,
  use_small?s_src_req_valid:(use_bucket?b_src_req_valid:l_src_req_valid),rs_req_ready,
  use_small?s_src_req_addr:(use_bucket?b_src_req_addr:l_src_req_addr),use_small?s_src_req_beats:(use_bucket?b_src_req_beats:l_src_req_beats),
  rs_araddr,rs_arlen,rs_arsize,rs_arburst,rs_arvalid,rs_arready,rdata,rresp,rlast,reader_rvalid,rready_from_reader,
  rs_out_data,rs_out_valid,1'b1,rs_done,rs_error,rs_busy,
  1'b1,cache_enable,cache_flush,awvalid&&awready,awaddr,awlen,rs_hits,rs_misses,rs_bypasses,rs_cache_serving);
 assign cache_hits=rs_hits;assign cache_misses=rs_misses;assign cache_bypasses=rs_bypasses+legacy_bg_bypasses+small_bg_bypasses;
 // Command-level ownership reuses the pixel ALU without per-beat arbitration.
 // All clients keep their existing three-stage pipeline timing and tags.
 // Mode 0=local, 1=legacy shared, 2=legacy+bucket shared. Pixel sharing
 // never arbitrates with the independent group-bank drain/writeback path.
 wire l_px_rst,l_px_iv,l_px_or,l_px_last;
 wire [127:0] l_px_src,l_px_dst,px_result;
 wire [31:0] l_px_color,l_px_op;
 wire [7:0] l_px_alpha;wire [23:0] l_px_key;
 wire [3:0] px_ready,px_valid,px_write,px_last;
 wire b_px_rst,b_px_iv,b_px_or,b_px_last;
 wire [127:0] b_px_src,b_px_dst;wire [31:0] b_px_color,b_px_op;
 wire [7:0] b_px_alpha;wire [23:0] b_px_key;
 wire px_owner_active=!use_bucket||SHARED_PIXEL_PIPE==2;
 wire px_rst_n=rst_n&&!soft_reset&&px_owner_active&&(use_small?s_px_rst:(use_bucket?b_px_rst:l_px_rst));
 wire px_iv=px_owner_active&&(use_small?s_px_iv:(use_bucket?b_px_iv:l_px_iv));
 wire px_or=use_small?s_px_or:(use_bucket?b_px_or:l_px_or);
 wire [127:0] px_src=use_small?s_px_src:(use_bucket?b_px_src:l_px_src);
 wire [127:0] px_dst=use_small?s_px_dst:(use_bucket?b_px_dst:l_px_dst);
 wire [31:0] px_color=use_small?s_px_color:(use_bucket?b_px_color:l_px_color);
 wire [31:0] px_op=use_small?s_px_op:(use_bucket?b_px_op:l_px_op);
 wire [7:0] px_alpha=use_small?s_px_alpha:(use_bucket?b_px_alpha:l_px_alpha);
 wire [23:0] px_key=use_small?s_px_key:(use_bucket?b_px_key:l_px_key);
 wire px_ilast=use_small?s_px_last:(use_bucket?b_px_last:l_px_last);
 genvar px_lane;
 generate if(SHARED_PIXEL_PIPE)begin:shared_pixel_service
  for(px_lane=0;px_lane<4;px_lane=px_lane+1)begin:lanes
   gfx_pixel_pipe pipeline(clk,px_rst_n,px_iv,px_ready[px_lane],
    px_src[px_lane*32+:32],px_dst[px_lane*32+:32],px_color,px_op,px_alpha,px_key,px_ilast,
    px_valid[px_lane],px_or,px_result[px_lane*32+:32],px_write[px_lane],px_last[px_lane]);
  end
 end else begin:local_pixel_services
  assign px_ready=4'b0;assign px_valid=4'b0;assign px_write=4'b0;
  assign px_last=4'b0;assign px_result=128'b0;
 end endgenerate
  gfx_engine #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.MAX_FILL_BEATS(MAX_FILL_BEATS),.SHARED_SOURCE_ROW(1),.SHARED_PIXEL_PIPE(SHARED_PIXEL_PIPE)) engine(clk,rst_n&&!soft_reset,dispatch_fire&&!command_is_bucket&&!command_is_copy&&!command_is_small,legacy_ready,cmd,legacy_done,legacy_seq,legacy_err,legacy_fa,legacy_busy,legacy_safe,
  laraddr,larlen,larsize,larburst,larvalid,arready&&!use_bucket&&!use_copy&&!use_small,rdata,rresp,rlast,rvalid&&!use_bucket&&!use_copy&&!use_small,lrready,
  lawaddr,lawlen,lawsize,lawburst,lawvalid,awready&&!use_bucket&&!use_copy&&!use_small,lwdata,lwstrb,lwlast,lwvalid,wready&&!use_bucket&&!use_copy&&!use_small,bresp,bvalid&&!use_bucket&&!use_copy&&!use_small,lbready,
  display_urgent,opaque_enable,
  l_src_req_valid,l_src_req_ready,l_src_req_addr,l_src_req_beats,
  (use_bucket||use_small)?128'b0:rs_out_data,(use_bucket||use_small)?1'b0:rs_out_valid,(use_bucket||use_small)?1'b0:rs_done,(use_bucket||use_small)?8'b0:rs_error,
  (use_bucket||use_small)?1'b0:rs_busy,(use_bucket||use_small)?1'b0:rs_cache_serving,
  rs_araddr,rs_arlen,rs_arsize,rs_arburst,rs_arvalid,l_src_arready,
  l_src_rbeat_valid,l_src_rready,legacy_bg_bypasses,
  legacy_row_wr_en,legacy_row_wr_addr,legacy_row_wr_data,legacy_row_rd_addr,shared_row_rd_data,
  l_px_rst,l_px_iv,l_px_or,l_px_last,l_px_src,l_px_dst,l_px_color,l_px_op,l_px_alpha,l_px_key,
  {4{!use_bucket&&!use_small}}&px_ready,{4{!use_bucket&&!use_small}}&px_valid,px_write,px_last,px_result);

 generate if(SMALL_PIPE)begin:sprite_hw
  gfx_sprite_pipe #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.ROW_SLOTS(SPRITE_ROWS)) engine(
   clk,rst_n&&!soft_reset,dispatch_fire&&command_is_small,small_ready,cmd,small_done,small_seq,small_err,small_fa,small_busy,small_safe,
   s_araddr,s_arlen,s_arsize,s_arburst,s_arvalid,arready&&use_small,rdata,rresp,rlast,rvalid&&use_small,s_rready,
   s_awaddr,s_awlen,s_awsize,s_awburst,s_awvalid,awready&&use_small,s_wdata,s_wstrb,s_wlast,s_wvalid,wready&&use_small,bresp,bvalid&&use_small,s_bready,
   opaque_enable,
   s_src_req_valid,rs_req_ready&&use_small,s_src_req_addr,s_src_req_beats,
   rs_out_data,rs_out_valid&&use_small,rs_done&&use_small,rs_error,rs_busy,rs_cache_serving,
   rs_araddr,rs_arlen,rs_arsize,rs_arburst,rs_arvalid,s_src_arready,s_src_rbeat_valid,rready_from_reader,
   s_px_rst,s_px_iv,s_px_or,s_px_last,s_px_src,s_px_dst,s_px_color,s_px_op,s_px_alpha,s_px_key,
   px_ready,px_valid&{4{use_small}},px_write,px_last,px_result,small_bg_bypasses,cache_flush,awvalid&&awready,awaddr,awlen);
 end else begin:sprite_absent
  assign small_ready=1;assign small_busy=0;assign small_safe=1;assign small_done=0;
  assign small_seq=0;assign small_err=0;assign small_fa=0;assign small_bg_bypasses=0;
  assign s_araddr=0;assign s_arlen=0;assign s_arsize=0;assign s_arburst=0;assign s_arvalid=0;assign s_rready=0;
  assign s_awaddr=0;assign s_awlen=0;assign s_awsize=0;assign s_awburst=0;assign s_awvalid=0;
  assign s_wdata=0;assign s_wstrb=0;assign s_wlast=0;assign s_wvalid=0;assign s_bready=0;
  assign s_src_req_valid=0;assign s_src_req_addr=0;assign s_src_req_beats=0;assign s_src_arready=0;assign s_src_rbeat_valid=0;
  assign s_px_rst=0;assign s_px_iv=0;assign s_px_or=0;assign s_px_last=0;
  assign s_px_src=0;assign s_px_dst=0;assign s_px_color=0;assign s_px_op=0;assign s_px_alpha=0;assign s_px_key=0;
 end endgenerate
 generate if(COPY_STREAM)begin:stream_hw
  gfx_copy_stream #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) engine(
   clk,rst_n&&!soft_reset,dispatch_fire&&command_is_copy,copy_ready,cmd,
   copy_done,copy_seq,copy_err,copy_fa,copy_busy,copy_safe,
   c_araddr,c_arlen,c_arsize,c_arburst,c_arvalid,arready&&use_copy,rdata,rresp,rlast,rvalid&&use_copy,c_rready,
   c_awaddr,c_awlen,c_awsize,c_awburst,c_awvalid,awready&&use_copy,c_wdata,c_wstrb,c_wlast,c_wvalid,wready&&use_copy,bresp,bvalid&&use_copy,c_bready,
   display_urgent);
 end else begin:stream_absent
  assign copy_ready=1'b1;assign copy_busy=1'b0;assign copy_safe=1'b1;assign copy_done=1'b0;
  assign copy_seq=0;assign copy_err=0;assign copy_fa=0;
  assign c_araddr=0;assign c_arlen=0;assign c_arsize=0;assign c_arburst=0;assign c_arvalid=0;assign c_rready=0;
  assign c_awaddr=0;assign c_awlen=0;assign c_awsize=0;assign c_awburst=0;assign c_awvalid=0;
  assign c_wdata=0;assign c_wstrb=0;assign c_wlast=0;assign c_wvalid=0;assign c_bready=0;
 end endgenerate
 generate if(HAS_BUCKET) begin:bucket_hw
  gfx_bucket_engine #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.SHARED_SOURCE_ROW(1),.SHARED_PIXEL_PIPE(SHARED_PIXEL_PIPE==2),.SPRITE_ROW_PREFETCH(BUCKET_ROW_PREFETCH),.RCOPY_ROW_PREFETCH(BUCKET_RCOPY_PREFETCH),.SOURCE_HALF_TRIM(BUCKET_SOURCE_TRIM)) bucket_engine(clk,rst_n&&!soft_reset,dispatch_fire&&command_is_bucket,bucket_ready,cmd,bucket_done,bucket_seq,bucket_err,bucket_fa,bucket_busy,bucket_safe,
  baraddr,barlen,barsize,barburst,barvalid,arready&&use_bucket,rdata,rresp,rlast,rvalid&&use_bucket,brready,
  b_src_req_valid,b_src_req_ready,b_src_req_addr,b_src_req_beats,
  use_bucket?rs_out_data:128'b0,use_bucket?rs_out_valid:1'b0,use_bucket?rs_done:1'b0,use_bucket?rs_error:8'b0,
  use_bucket?rs_busy:1'b0,use_bucket?rs_cache_serving:1'b0,
  rs_araddr,rs_arlen,rs_arsize,rs_arburst,rs_arvalid,b_src_arready,
  b_src_rbeat_valid,b_src_rready,
  display_urgent,
  b_grp_begin,b_grp_ready,b_fill_waddr,b_fill_wdata,b_fill_we,b_fill_raddr,b_fill_rdata,
  b_grp_commit,b_desc_base,b_desc_stride,b_desc_rows,b_desc_beats_m1,b_desc_width,b_desc_pitch32,
  b_grp_flush,b_grp_idle,b_grp_error,b_grp_error_addr,
  bucket_row_wr_en,bucket_row_wr_addr,bucket_row_wr_data,bucket_row_rd_addr,shared_row_rd_data,
  b_px_rst,b_px_iv,b_px_or,b_px_last,b_px_src,b_px_dst,b_px_color,b_px_op,b_px_alpha,b_px_key,
  {4{use_bucket}}&px_ready,{4{use_bucket}}&px_valid,px_write,px_last,px_result);

 // M3b-1 shared group backend: one dual-bank group store and write-back
 // service. The bucket engine is its only client; the active command owns it.
 gfx_group_backend #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) group_backend(clk,rst_n&&!soft_reset,
  use_bucket&&b_grp_begin,b_grp_ready,b_fill_waddr,b_fill_wdata,b_fill_we,b_fill_raddr,b_fill_rdata,
  use_bucket&&b_grp_commit,b_desc_base,b_desc_stride,b_desc_rows,b_desc_beats_m1,b_desc_width,b_desc_pitch32,
  b_grp_flush,b_grp_idle,b_grp_error,b_grp_error_addr,gb_fill_open,
  gawaddr,gawlen,gawsize,gawburst,gawvalid,awready&&use_bucket,gwdata,gwstrb,gwlast,gwvalid,wready&&use_bucket,bresp,bvalid&&use_bucket,gbready);
 end else begin:bucket_absent
  // Bucket-less mirror: a submitted bucket command must not hang the queue.
  // It faults one cycle after dispatch (use_bucket has settled by then) with
  // GFX_ERR_OPCODE, matching the caps bit this build does not advertise.
  reg stub_done,stub_active;reg [31:0] stub_seq;
  always @(posedge clk)begin
   if(!rst_n||soft_reset)begin stub_done<=0;stub_active<=0;stub_seq<=0;end
   else begin
    stub_done<=dispatch_fire&&command_is_bucket;
    if(dispatch_fire)begin stub_active<=command_is_bucket;stub_seq<=cmd[320+:32];end
   end
  end
  assign bucket_ready=1'b1;assign bucket_busy=1'b0;assign bucket_safe=1'b1;
  assign bucket_done=stub_done;assign bucket_seq=stub_seq;
  assign bucket_err=stub_active?`GFX_ERR_OPCODE:8'b0;assign bucket_fa=32'b0;
  assign baraddr=32'b0;assign barlen=8'b0;assign barsize=3'b0;assign barburst=2'b0;
  assign barvalid=1'b0;assign brready=1'b0;
  assign b_src_req_valid=1'b0;assign b_src_req_addr=32'b0;assign b_src_req_beats=5'b0;
  assign bucket_row_wr_en=1'b0;assign bucket_row_wr_addr=4'b0;assign bucket_row_wr_data=128'b0;assign bucket_row_rd_addr=4'b0;
  assign b_px_rst=1'b0;assign b_px_iv=1'b0;assign b_px_or=1'b0;assign b_px_last=1'b0;
  assign b_px_src=128'b0;assign b_px_dst=128'b0;assign b_px_color=32'b0;assign b_px_op=32'b0;
  assign b_px_alpha=8'b0;assign b_px_key=24'b0;
  assign b_grp_begin=1'b0;assign b_grp_commit=1'b0;assign b_grp_flush=1'b0;assign b_fill_we=1'b0;
  assign b_fill_waddr=9'b0;assign b_fill_wdata=128'b0;assign b_fill_raddr=9'b0;
  assign b_desc_base=32'b0;assign b_desc_stride=32'b0;assign b_desc_rows=6'b0;
  assign b_desc_beats_m1=5'b0;assign b_desc_width=8'b0;assign b_desc_pitch32=1'b0;
  assign b_grp_idle=1'b1;assign b_grp_error=8'b0;assign b_grp_error_addr=32'b0;
  assign gb_fill_open=1'b0;assign b_fill_rdata=128'b0;
  assign gawaddr=32'b0;assign gawlen=8'b0;assign gawsize=3'b0;assign gawburst=2'b0;
  assign gawvalid=1'b0;assign gwdata=128'b0;assign gwstrb=16'b0;assign gwlast=1'b0;
  assign gwvalid=1'b0;assign gbready=1'b0;
 end endgenerate
endmodule
