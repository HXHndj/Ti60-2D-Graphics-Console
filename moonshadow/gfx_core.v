`include "gfx_abi.vh"
// Relocatable system-clock IP. Integration supplies the verified address window
// and protected front/pending spans; no board address is silently invented.
module gfx_core #(parameter QUEUE_DEPTH=16,TIMEOUT_CYCLES=10000000,FRAME_BYTES=1228800,CAPS_EXTRA=0,MAX_FILL_BEATS=16,SHARED_PIXEL_PIPE=1)(
 input wire clk,rst_n,input wire psel,penable,pwrite,input wire [11:0] paddr,input wire [31:0] pwdata,
 output wire pready,output wire [31:0] prdata,output wire pslverr,irq,
 input wire [31:0] pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,
 output wire display_request,output wire [31:0] display_base,display_id,
 input wire display_ready,display_pending,display_ack,input wire [31:0] front_base,presented_id,released_base,underflow,input wire [7:0] display_fault,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready,
 input wire display_urgent
);
 wire cv,cr,busy,safe,done;wire [`GFX_CMD_BITS-1:0] cmd;wire [31:0] seq,fa;wire [7:0] err;
 wire legacy_ready,legacy_busy,legacy_safe,legacy_done;wire [31:0] legacy_seq,legacy_fa;wire [7:0] legacy_err;
 wire tile_ready,tile_busy,tile_safe,tile_done;wire [31:0] tile_seq,tile_fa;wire [7:0] tile_err;
 wire [31:0] tile_candidate_refs,tile_executed_refs,tile_skipped_refs,tile_opaque_tiles;
 wire command_is_tile=cmd[31:0]==`GFX_OP_TILE4;reg use_tile;
 wire cache_enable,cache_flush;wire [31:0] cache_hits,cache_misses,cache_bypasses;
 wire [31:0] legacy_debug_state,legacy_debug_seq,legacy_debug_position,legacy_debug_counts;
 gfx_regs #(.QUEUE_DEPTH(QUEUE_DEPTH),.FRAME_BYTES(FRAME_BYTES),.CAPS_EXTRA(CAPS_EXTRA)) regs(clk,rst_n,psel,penable,pwrite,paddr,pwdata,pready,prdata,pslverr,irq,
  pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,cv,cr,cmd,
  busy,safe,done,seq,err,fa,display_request,display_base,display_id,
  display_ready,display_pending,display_ack,front_base,presented_id,released_base,underflow,display_fault,
  rvalid&&rready,wvalid&&wready,wstrb,(arvalid&&!arready)||(awvalid&&!awready)||(wvalid&&!wready),
  cache_enable,cache_flush,cache_hits,cache_misses,cache_bypasses,
  tile_candidate_refs,tile_executed_refs,tile_skipped_refs,tile_opaque_tiles,
  legacy_debug_state,legacy_debug_seq,legacy_debug_position,legacy_debug_counts);
 wire soft_reset=psel&&penable&&pready&&pwrite&&!pslverr&&paddr==16&&pwdata==2&&safe&&!display_pending;
 /* Both engines share one physical AXI master selected by use_tile.  A target
    engine being idle is not sufficient to accept a command: switching the
    mux while the other engine still owns an accepted AR/AW transaction cuts
    off its response and can lock the shared DDR read fabric, including CPU
    instruction fetch and display scanout.  Dispatch only at a true boundary
    where neither engine is active. */
 wire engines_idle=!legacy_busy&&!tile_busy;
 assign cr=engines_idle&&(command_is_tile?tile_ready:legacy_ready);
 assign busy=legacy_busy||tile_busy;assign safe=legacy_safe&&tile_safe;
 assign done=use_tile?tile_done:legacy_done;assign seq=use_tile?tile_seq:legacy_seq;
 assign err=use_tile?tile_err:legacy_err;assign fa=use_tile?tile_fa:legacy_fa;
 always @(posedge clk)begin
  if(!rst_n||soft_reset)use_tile<=0;
  else if(cv&&cr)use_tile<=command_is_tile;
 end
 wire [31:0] laraddr,lawaddr,taraddr,tawaddr;wire [7:0] larlen,lawlen,tarlen,tawlen;
 wire [2:0] larsize,lawsize,tarsize,tawsize;wire [1:0] larburst,lawburst,tarburst,tawburst;
 wire larvalid,lrready,lawvalid,lwlast,lwvalid,lbready,tarvalid,trready,tawvalid,twlast,twvalid,tbready;
 wire [127:0] lwdata,twdata;wire [15:0] lwstrb,twstrb;
 assign araddr=use_tile?taraddr:laraddr;assign arlen=use_tile?tarlen:larlen;assign arsize=use_tile?tarsize:larsize;assign arburst=use_tile?tarburst:larburst;assign arvalid=use_tile?tarvalid:larvalid;assign rready=use_tile?trready:lrready;
 assign awaddr=use_tile?tawaddr:lawaddr;assign awlen=use_tile?tawlen:lawlen;assign awsize=use_tile?tawsize:lawsize;assign awburst=use_tile?tawburst:lawburst;assign awvalid=use_tile?tawvalid:lawvalid;
 assign wdata=use_tile?twdata:lwdata;assign wstrb=use_tile?twstrb:lwstrb;assign wlast=use_tile?twlast:lwlast;assign wvalid=use_tile?twvalid:lwvalid;assign bready=use_tile?tbready:lbready;
 /* Queue VALID remains asserted while READY is low.  Gate each engine with
    the actual top-level handshake; otherwise an idle target engine consumes
    the held command even though the queue has not advanced. */
 wire dispatch=cv&&cr;
 // One command owns the renderer at a time, so Legacy and Tile4 can share
 // the existing four-lane pixel ALU without per-beat arbitration.
 wire l_px_rst,l_px_iv,l_px_or,l_px_last,t_px_rst,t_px_iv,t_px_or,t_px_last;
 wire [127:0] l_px_src,l_px_dst,t_px_src,t_px_dst,px_result;
 wire [31:0] l_px_color,l_px_op,t_px_color,t_px_op;
 wire [7:0] l_px_alpha,t_px_alpha;wire [23:0] l_px_key,t_px_key;
 wire px_rst_n=rst_n&&!soft_reset&&(use_tile?t_px_rst:l_px_rst);
 wire px_iv=use_tile?t_px_iv:l_px_iv;wire px_or=use_tile?t_px_or:l_px_or;
 wire [127:0] px_src=use_tile?t_px_src:l_px_src;
 wire [127:0] px_dst=use_tile?t_px_dst:l_px_dst;
 wire [31:0] px_color=use_tile?t_px_color:l_px_color;
 wire [31:0] px_op=use_tile?t_px_op:l_px_op;
 wire [7:0] px_alpha=use_tile?t_px_alpha:l_px_alpha;
 wire [23:0] px_key=use_tile?t_px_key:l_px_key;
 wire px_ilast=use_tile?t_px_last:l_px_last;
 wire [3:0] px_ready,px_valid,px_write,px_last;
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
 gfx_engine #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.MAX_FILL_BEATS(MAX_FILL_BEATS),.SHARED_PIXEL_PIPE(SHARED_PIXEL_PIPE)) engine(clk,rst_n&&!soft_reset,dispatch&&!command_is_tile,legacy_ready,cmd,legacy_done,legacy_seq,legacy_err,legacy_fa,legacy_busy,legacy_safe,
  laraddr,larlen,larsize,larburst,larvalid,arready&&!use_tile,rdata,rresp,rlast,rvalid&&!use_tile,lrready,
  lawaddr,lawlen,lawsize,lawburst,lawvalid,awready&&!use_tile,lwdata,lwstrb,lwlast,lwvalid,wready&&!use_tile,bresp,bvalid&&!use_tile,lbready,
  cache_enable,cache_flush,cache_hits,cache_misses,cache_bypasses,display_urgent,
  legacy_debug_state,legacy_debug_seq,legacy_debug_position,legacy_debug_counts,
  l_px_rst,l_px_iv,l_px_or,l_px_last,l_px_src,l_px_dst,l_px_color,l_px_op,l_px_alpha,l_px_key,
  {4{!use_tile}}&px_ready,{4{!use_tile}}&px_valid,px_write,px_last,px_result);
 gfx_tile4_engine #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.SHARED_PIXEL_PIPE(SHARED_PIXEL_PIPE)) tile_engine(clk,rst_n&&!soft_reset,dispatch&&command_is_tile,tile_ready,cmd,tile_done,tile_seq,tile_err,tile_fa,tile_busy,tile_safe,
  taraddr,tarlen,tarsize,tarburst,tarvalid,arready&&use_tile,rdata,rresp,rlast,rvalid&&use_tile,trready,
  tawaddr,tawlen,tawsize,tawburst,tawvalid,awready&&use_tile,twdata,twstrb,twlast,twvalid,wready&&use_tile,bresp,bvalid&&use_tile,tbready,
  tile_candidate_refs,tile_executed_refs,tile_skipped_refs,tile_opaque_tiles,
  t_px_rst,t_px_iv,t_px_or,t_px_last,t_px_src,t_px_dst,t_px_color,t_px_op,t_px_alpha,t_px_key,
  {4{use_tile}}&px_ready,{4{use_tile}}&px_valid,px_write,px_last,px_result);
endmodule
