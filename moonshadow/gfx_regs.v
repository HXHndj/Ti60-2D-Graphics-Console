`include "gfx_abi.vh"
module gfx_regs #(parameter QUEUE_DEPTH=16, APB_WAIT=1,FRAME_BYTES=1228800,CAPS_EXTRA=0)(
 input wire clk,rst_n,input wire psel,penable,pwrite,input wire [11:0] paddr,input wire [31:0] pwdata,
 output wire pready,output reg [31:0] prdata,output reg pslverr,output wire irq,
 input wire [31:0] pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,
 output wire cmd_valid,input wire cmd_ready,output wire [`GFX_CMD_BITS-1:0] cmd,
 input wire engine_busy,engine_safe,engine_done,input wire [31:0] engine_seq,
 input wire [7:0] engine_error,input wire [31:0] engine_fault_addr,
 output reg display_request,output reg [31:0] display_base,display_id,
 input wire display_ready,display_pending,display_ack,input wire [31:0] front_base,presented_id,released_base,underflow,input wire [7:0] display_fault,
 input wire read_beat,write_beat,input wire [15:0] write_strobe,input wire stall,
 output reg cache_enable,cache_flush,input wire [31:0] cache_hits,cache_misses,cache_bypasses,
 input wire [31:0] tile_candidate_refs,tile_executed_refs,tile_skipped_refs,tile_opaque_tiles,
 input wire [31:0] engine_debug_state,engine_debug_seq,engine_debug_position,engine_debug_counts
);
 reg [31:0] shadow[0:10];wire [`GFX_CMD_BITS-1:0] snapshot;
 genvar g;generate for(g=0;g<11;g=g+1)begin:pack assign snapshot[g*32+:32]=shadow[g];end endgenerate
 wire [7:0] validation_error;
 gfx_validate check(snapshot,pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,validation_error);
 reg [7:0] wait_count;
 wire transfer=psel&&penable&&pready;
 wire write=transfer&&pwrite&&!pslverr;
 reg [31:0] irq_status,irq_enable,submit_status,accepted_seq,complete_seq,fault_seq,fault_code,fault_addr;
 reg faulted;reg [31:0] disp_shadow_base,disp_shadow_id;
 wire [31:0] irq_clear=(write&&paddr==`GFX_REG_IRQ_STATUS)?pwdata:0;
 wire [31:0] events=(engine_done?(engine_error==0?32'd1:32'd2):0)|(display_ack?32'd4:0)|((engine_error!=0&&!faulted)?32'd2:0);
 assign irq=|(irq_status&irq_enable);
 localparam QW=(QUEUE_DEPTH<2?1:$clog2(QUEUE_DEPTH));wire [QW:0] queue_count;
 wire qin_ready,qvalid;wire [`GFX_CMD_BITS-1:0] qdata;
 wire space=queue_count+(engine_busy?1:0)<QUEUE_DEPTH;
 // Full/faulted submissions still return an error. Otherwise a COMMIT may
 // wait at most one 11-word packing interval before its snapshot is accepted.
 assign pready=wait_count>=APB_WAIT&&!(psel&&penable&&pwrite&&paddr==`GFX_REG_CONTROL&&pwdata==1&&space&&!faulted&&engine_error==0&&!qin_ready);
 wire commit=write&&paddr==`GFX_REG_CONTROL&&pwdata==1;
 wire reset_request=write&&paddr==`GFX_REG_CONTROL&&pwdata==2;
 wire safe_reset=reset_request&&engine_safe&&!display_pending;
 wire push=commit&&space&&qin_ready&&!faulted&&engine_error==0&&validation_error==0;
 gfx_command_queue #(.DEPTH(QUEUE_DEPTH)) queue(clk,rst_n,safe_reset,push,qin_ready,snapshot,qvalid,cmd_ready&&!faulted&&engine_error==0,qdata,queue_count);
 assign cmd_valid=qvalid&&!faulted&&engine_error==0;assign cmd=qdata;
 reg [63:0] cycles,reads,writes,bytes,stalls;
 reg [63:0] snap_cycles,snap_reads,snap_writes,snap_bytes,snap_stalls;
 reg [31:0] snap_debug_state,snap_debug_seq,snap_debug_position,snap_debug_counts;
 integer k,b;reg [4:0] byte_count;
 always @*begin byte_count=0;for(b=0;b<16;b=b+1)byte_count=byte_count+write_strobe[b];end
 always @*begin
  prdata=0;pslverr=0;
  if(paddr[1:0]!=0)pslverr=1;
  else if(paddr>=`GFX_REG_OP_FLAGS&&paddr<=`GFX_REG_SUBMIT_SEQ)prdata=shadow[(paddr-32)>>2];
  else case(paddr)
   `GFX_REG_DEVICE_ID:prdata=`GFX_DEVICE_ID;
   `GFX_REG_ABI_VERSION:prdata=`GFX_ABI_VERSION;
   `GFX_REG_CAPS:prdata=32'h1f|(QUEUE_DEPTH>1?32'h20:0)|`GFX_CAP_SOURCE_CACHE|`GFX_CAP_TILE4|`GFX_CAP_FILL_LIST|32'h00000800|CAPS_EXTRA;
   `GFX_REG_STATUS:prdata={28'b0,display_pending,faulted,(engine_busy||queue_count!=0),(space&&!faulted)};
   `GFX_REG_CONTROL:if(pwrite&&pwdata!=1&&pwdata!=2&&pwdata!=4&&pwdata!=8)pslverr=1;
   `GFX_REG_IRQ_STATUS:begin prdata=irq_status;if(pwrite&&(pwdata&32'hfffffff8)!=0)pslverr=1;end
   `GFX_REG_IRQ_ENABLE:begin prdata=irq_enable;if(pwrite&&(pwdata&32'hfffffff8)!=0)pslverr=1;end
   `GFX_REG_SUBMIT_STATUS:prdata=submit_status;
   `GFX_REG_ACCEPTED_SEQ:prdata=accepted_seq;
   `GFX_REG_COMPLETE_SEQ:prdata=complete_seq;
   `GFX_REG_FAULT_SEQ:prdata=fault_seq;
   `GFX_REG_FAULT_CODE:prdata=fault_code;
   `GFX_REG_FAULT_ADDR:prdata=fault_addr;
   `GFX_REG_QUEUE_FREE:prdata=QUEUE_DEPTH-queue_count-(engine_busy?1:0);
   `GFX_REG_QUEUE_CAPACITY:prdata=QUEUE_DEPTH;
   `GFX_REG_CACHE_CONTROL:begin prdata={31'b0,cache_enable};if(pwrite&&pwdata>3)pslverr=1;end
   `GFX_REG_CACHE_HITS:prdata=cache_hits;
   `GFX_REG_CACHE_MISSES:prdata=cache_misses;
   `GFX_REG_CACHE_BYPASSES:prdata=cache_bypasses;
   `GFX_REG_DISP_REQUEST_BASE:prdata=disp_shadow_base;
   `GFX_REG_DISP_REQUEST_ID:prdata=disp_shadow_id;
   `GFX_REG_DISP_CONTROL:if(pwrite&&pwdata!=1)pslverr=1;
   `GFX_REG_DISP_STATUS:prdata={22'b0,display_fault,display_pending,display_ready};
   `GFX_REG_DISP_FRONT_BASE:prdata=front_base;
   `GFX_REG_DISP_PRESENTED_ID:prdata=presented_id;
   `GFX_REG_DISP_RELEASED_BASE:prdata=released_base;
   `GFX_REG_DISP_UNDERFLOW:prdata=underflow;
   `GFX_REG_PERF_CYCLES_LO:prdata=snap_cycles[31:0];
   `GFX_REG_PERF_CYCLES_HI:prdata=snap_cycles[63:32];
   `GFX_REG_PERF_READ_BEATS_LO:prdata=snap_reads[31:0];
   `GFX_REG_PERF_READ_BEATS_HI:prdata=snap_reads[63:32];
   `GFX_REG_PERF_WRITE_BEATS_LO:prdata=snap_writes[31:0];
   `GFX_REG_PERF_WRITE_BEATS_HI:prdata=snap_writes[63:32];
   `GFX_REG_PERF_WR_BYTES_LO:prdata=snap_bytes[31:0];
   `GFX_REG_PERF_WR_BYTES_HI:prdata=snap_bytes[63:32];
   `GFX_REG_PERF_STALL_LO:prdata=snap_stalls[31:0];
   `GFX_REG_PERF_STALL_HI:prdata=snap_stalls[63:32];
   `GFX_REG_TILE_CANDIDATE_REFS:prdata=tile_candidate_refs;
   `GFX_REG_TILE_EXECUTED_REFS:prdata=tile_executed_refs;
   `GFX_REG_TILE_SKIPPED_REFS:prdata=tile_skipped_refs;
   `GFX_REG_TILE_OPAQUE_TILES:prdata=tile_opaque_tiles;
   `GFX_REG_DEBUG_ENGINE_STATE:prdata=snap_debug_state;
   `GFX_REG_DEBUG_ENGINE_SEQ:prdata=snap_debug_seq;
   `GFX_REG_DEBUG_ENGINE_POSITION:prdata=snap_debug_position;
   `GFX_REG_DEBUG_ENGINE_COUNTS:prdata=snap_debug_counts;
   default:pslverr=1;
  endcase
  if(pwrite && paddr!=`GFX_REG_CONTROL&&paddr!=`GFX_REG_IRQ_STATUS&&paddr!=`GFX_REG_IRQ_ENABLE&&paddr!=`GFX_REG_CACHE_CONTROL&&
    !(paddr>=`GFX_REG_OP_FLAGS&&paddr<=`GFX_REG_SUBMIT_SEQ)&&paddr!=`GFX_REG_DISP_REQUEST_BASE&&paddr!=`GFX_REG_DISP_REQUEST_ID&&paddr!=`GFX_REG_DISP_CONTROL)pslverr=1;
 end
 always @(posedge clk)begin
  if(!rst_n)begin
   wait_count<=0;irq_status<=0;irq_enable<=0;submit_status<=0;accepted_seq<=0;complete_seq<=0;fault_seq<=0;fault_code<=0;fault_addr<=0;faulted<=0;
   display_request<=0;display_base<=0;display_id<=0;disp_shadow_base<=0;disp_shadow_id<=0;
   cache_enable<=0;cache_flush<=0;
   cycles<=0;reads<=0;writes<=0;bytes<=0;stalls<=0;snap_cycles<=0;snap_reads<=0;snap_writes<=0;snap_bytes<=0;snap_stalls<=0;
   snap_debug_state<=0;snap_debug_seq<=0;snap_debug_position<=0;snap_debug_counts<=0;
   for(k=0;k<11;k=k+1)shadow[k]<=0;
   shadow[8]<=255;shadow[9]<=24'hff00ff;
  end else begin
   if(!psel||!penable||pready)wait_count<=0;else wait_count<=wait_count+1'b1;
   display_request<=0;
   cache_flush<=0;
   cycles<=cycles+1'b1;if(read_beat)reads<=reads+1'b1;if(write_beat)begin writes<=writes+1'b1;bytes<=bytes+byte_count;end
   if(stall)stalls<=stalls+1'b1;
   irq_status<=(irq_status&~irq_clear)|events;
   if(write)begin
    if(paddr==`GFX_REG_CACHE_CONTROL)begin
     if(engine_safe&&queue_count==0)begin cache_enable<=pwdata[0];cache_flush<=1;submit_status<=0;end
     else submit_status<=`GFX_ERR_BUSY;
    end
    if(paddr>=32&&paddr<=72)shadow[(paddr-32)>>2]<=pwdata;
    if(paddr==`GFX_REG_IRQ_ENABLE)irq_enable<=pwdata;
    if(paddr==`GFX_REG_DISP_REQUEST_BASE)disp_shadow_base<=pwdata;
    if(paddr==`GFX_REG_DISP_REQUEST_ID)disp_shadow_id<=pwdata;
    if(paddr==`GFX_REG_DISP_CONTROL)begin
     if(disp_shadow_base[5:0]!=0||disp_shadow_base<pool_start||({32'b0,disp_shadow_base}+FRAME_BYTES)>{32'b0,pool_end}||disp_shadow_base==front_base)submit_status<=`GFX_ERR_RANGE;
     else if(display_ready&&!engine_busy&&queue_count==0&&!faulted)begin display_request<=1;display_base<=disp_shadow_base;display_id<=disp_shadow_id;submit_status<=0;end
     else submit_status<=`GFX_ERR_BUSY;
    end
    if(paddr==`GFX_REG_CONTROL&&pwdata==4)begin snap_cycles<=cycles;snap_reads<=reads;snap_writes<=writes;snap_bytes<=bytes;snap_stalls<=stalls;end
    if(paddr==`GFX_REG_CONTROL&&pwdata==8)begin
     snap_debug_state<=engine_debug_state;snap_debug_seq<=engine_debug_seq;
     snap_debug_position<=engine_debug_position;snap_debug_counts<=engine_debug_counts;
    end
   end
   if(commit)begin
    if(faulted||engine_error!=0)submit_status<=`GFX_ERR_FAULT;
    else if(!space)submit_status<=`GFX_ERR_BUSY;
    else if(validation_error!=0)submit_status<=validation_error;
    else begin accepted_seq<=shadow[10];submit_status<=0;end
   end
   if(reset_request)begin
    if(safe_reset)begin cache_enable<=0;cache_flush<=1;end
    if(safe_reset)begin faulted<=0;fault_code<=0;fault_addr<=0;fault_seq<=0;accepted_seq<=0;complete_seq<=0;irq_status<=events;submit_status<=0;end
    else submit_status<=`GFX_ERR_BUSY;
   end
   if(engine_done)begin
    if(engine_error==0)complete_seq<=engine_seq;
    else begin faulted<=1;fault_seq<=engine_seq;fault_code<=engine_error;fault_addr<=engine_fault_addr;end
   end
   if(engine_error!=0&&!faulted&&!safe_reset)begin faulted<=1;fault_seq<=engine_seq;fault_code<=engine_error;fault_addr<=engine_fault_addr;end
  end
 end
endmodule
