`include "gfx_abi.vh"
module gfx_regs #(parameter QUEUE_DEPTH=16, APB_WAIT=1,FRAME_BYTES=1228800,CAPS_EXTRA=0,HAS_BUCKET=1,PRESENT_FENCE=0,STREAM_SUBMIT=0,RING_FETCH=0,FRAME_TRACE=0)(
 input wire clk,rst_n,input wire psel,penable,pwrite,input wire [11:0] paddr,input wire [31:0] pwdata,
 output wire pready,output reg [31:0] prdata,output reg pslverr,output wire irq,
 input wire [31:0] pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,
 input wire ring_push_valid,output wire ring_push_ready,input wire [`GFX_CMD_BITS-1:0] ring_push_data,
 input wire ring_fault_valid,input wire [7:0] ring_fault_code,output wire queue_clear,
 output wire cmd_valid,input wire cmd_ready,output wire [`GFX_CMD_BITS-1:0] cmd,
 input wire engine_busy,engine_safe,engine_done,input wire [31:0] engine_seq,
 input wire [7:0] engine_error,input wire [31:0] engine_fault_addr,
 output reg display_request,output reg [31:0] display_base,display_id,
 input wire display_ready,display_pending,display_ack,input wire [31:0] front_base,presented_id,released_base,underflow,input wire [7:0] display_fault,
 input wire read_beat,write_beat,input wire [15:0] write_strobe,input wire stall,
 output reg cache_enable,cache_flush,input wire [31:0] cache_hits,cache_misses,cache_bypasses,
 input wire [31:0] diag_blanks,diag_shown,diag_new_id,diag_repeat,diag_dbl_rpt,diag_miss,diag_acks,
 input wire [31:0] diag_drain_last,diag_drain_max,diag_desc_last,diag_desc_max,diag_pref_last,diag_pref_max,
 output wire diag_clear,output reg opaque_enable,
 output reg [31:0] display_seq,output wire [31:0] completed_seq,output wire render_fault
);
 reg [31:0] shadow[0:10];wire [`GFX_CMD_BITS-1:0] snapshot;
 genvar g;generate for(g=0;g<11;g=g+1)begin:pack assign snapshot[g*32+:32]=shadow[g];end endgenerate
 wire [7:0] validation_error;
 gfx_validate check(snapshot,pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,validation_error);
 reg [7:0] wait_count;
 wire transfer=psel&&penable&&pready;
 wire write=transfer&&pwrite&&!pslverr;
 reg [31:0] irq_status,irq_enable,submit_status,accepted_seq,complete_seq,fault_seq,fault_code,fault_addr;
 reg faulted;reg [31:0] disp_shadow_base,disp_shadow_id,disp_shadow_seq;
 assign completed_seq=complete_seq;assign render_fault=faulted||engine_error!=0;
 wire [31:0] irq_clear=(write&&paddr==`GFX_REG_IRQ_STATUS)?pwdata:0;
 wire [31:0] events=(engine_done?(engine_error==0?32'd1:32'd2):0)|(display_ack?32'd4:0)|((engine_error!=0&&!faulted)?32'd2:0)|((ring_fault_valid&&!faulted)?32'd2:0);
 assign irq=|(irq_status&irq_enable);
 localparam QW=(QUEUE_DEPTH<2?1:$clog2(QUEUE_DEPTH));wire [QW:0] queue_count;
 wire qin_ready,qvalid;wire [`GFX_CMD_BITS-1:0] qdata;
 wire space=queue_count+(engine_busy?1:0)<QUEUE_DEPTH;
 // Full/faulted submissions still return an error. Otherwise a COMMIT may
 // wait at most one 11-word packing interval before its snapshot is accepted.
 wire stream_address=STREAM_SUBMIT&&paddr==`GFX_REG_STREAM_DST;
 assign pready=wait_count>=APB_WAIT&&!(psel&&penable&&pwrite&&((paddr==`GFX_REG_CONTROL&&pwdata==1)||stream_address)&&space&&!faulted&&engine_error==0&&!qin_ready);
 wire commit=write&&((paddr==`GFX_REG_CONTROL&&pwdata==1)||stream_address);
 wire reset_request=write&&paddr==`GFX_REG_CONTROL&&pwdata==2;
 wire safe_reset=reset_request&&engine_safe&&!display_pending;
 wire push=commit&&space&&qin_ready&&!faulted&&engine_error==0&&validation_error==0;
 // The DDR ring fetcher pushes validated descriptors beside APB commits.
 // Commits keep priority: they are rare once the firmware runs the ring and
 // pready never depends on the fetcher. The fetcher owns no sequence state;
 // every accepted entry takes the next accepted_seq snapshot here.
 wire ring_fire=RING_FETCH&&ring_push_valid&&qin_ready&&!faulted&&engine_error==0&&!push&&!reset_request;
 assign ring_push_ready=RING_FETCH&&qin_ready&&!faulted&&engine_error==0&&!push&&!reset_request;
 wire [`GFX_CMD_BITS-1:0] ring_entry={(accepted_seq+1'b1),ring_push_data[`GFX_CMD_BITS-33:0]};
 assign queue_clear=safe_reset;
 generate if(QUEUE_DEPTH>=512)begin:g_deep_queue
  gfx_command_queue_wide #(.DEPTH(QUEUE_DEPTH)) queue(clk,rst_n,safe_reset,push||ring_fire,qin_ready,push?snapshot:ring_entry,qvalid,cmd_ready&&!faulted&&engine_error==0,qdata,queue_count);
 end else begin:g_packed_queue
  gfx_command_queue #(.DEPTH(QUEUE_DEPTH)) queue(clk,rst_n,safe_reset,push||ring_fire,qin_ready,push?snapshot:ring_entry,qvalid,cmd_ready&&!faulted&&engine_error==0,qdata,queue_count);
 end endgenerate
 assign cmd_valid=qvalid&&!faulted&&engine_error==0;assign cmd=qdata;
 reg probe_active; reg [31:0] probe_cycles,probe_empty;
 reg [63:0] cycles,reads,writes,bytes,stalls;
 reg [63:0] snap_cycles,snap_reads,snap_writes,snap_bytes,snap_stalls;
 // Any APB write to the diag version register pulses the counter clear.
 assign diag_clear=write&&paddr==`GFX_REG_DISP_DIAG_VERSION;
 integer k,b;reg [4:0] byte_count;
 // One-shot observer only: never gates command, DMA or display handshakes.
 // ARM precedes the CPU doorbell; FIRST is actual first descriptor acceptance.
 reg [31:0] trace_flags,trace_first_seq,trace_last_seq,trace_arm,trace_first,
  trace_done,trace_request,trace_ack,trace_frame;
 always @(posedge clk)begin
  if(!rst_n || !FRAME_TRACE)begin
   trace_flags<=0;trace_first_seq<=0;trace_last_seq<=0;trace_arm<=0;
   trace_first<=0;trace_done<=0;trace_request<=0;trace_ack<=0;trace_frame<=0;
  end else if(reset_request&&safe_reset)trace_flags<=0;
  else begin
   if(write&&paddr==`GFX_REG_TRACE_FIRST_SEQ)trace_first_seq<=pwdata;
   if(write&&paddr==`GFX_REG_TRACE_LAST_SEQ)trace_last_seq<=pwdata;
   if(write&&paddr==`GFX_REG_TRACE_CONTROL)begin
    trace_flags<=pwdata[0]?1:0;trace_arm<=cycles[31:0];
    trace_first<=0;trace_done<=0;trace_request<=0;trace_ack<=0;trace_frame<=0;
   end else if(trace_flags[0])begin
    if(ring_fire&&accepted_seq+1'b1==trace_first_seq&&!trace_flags[1])begin trace_first<=cycles[31:0];trace_flags[1]<=1;end
    if(engine_done&&engine_seq==trace_last_seq&&!trace_flags[2])begin
     if(engine_error==0)begin trace_done<=cycles[31:0];trace_flags[2]<=1;end
     else trace_flags[5]<=1;
    end
    if(display_request&&display_seq==trace_last_seq&&!trace_flags[3])begin
     trace_request<=cycles[31:0];trace_frame<=display_id;trace_flags[3]<=1;
    end
    if(display_ack&&trace_flags[3]&&presented_id==trace_frame&&!trace_flags[4])begin trace_ack<=cycles[31:0];trace_flags[4]<=1;end
    if(faulted||engine_error!=0||ring_fault_valid)trace_flags[5]<=1;
   end
  end
 end
 always @*begin byte_count=0;for(b=0;b<16;b=b+1)byte_count=byte_count+write_strobe[b];end
 always @*begin
  prdata=0;pslverr=0;
  if(paddr[1:0]!=0)pslverr=1;
  else if(paddr>=`GFX_REG_OP_FLAGS&&paddr<=`GFX_REG_SUBMIT_SEQ)prdata=shadow[(paddr-32)>>2];
  else case(paddr)
   `GFX_REG_DEVICE_ID:prdata=`GFX_DEVICE_ID;
   `GFX_REG_ABI_VERSION:prdata=`GFX_ABI_VERSION;
   `GFX_REG_CAPS:prdata=32'h1f|(QUEUE_DEPTH>1?32'h20:0)|`GFX_CAP_SOURCE_CACHE|`GFX_CAP_OPAQUE_BYPASS|(HAS_BUCKET?(`GFX_CAP_BUCKET|`GFX_CAP_BUCKET_1536):32'b0)|`GFX_CAP_SUPPLY_PROBE|(PRESENT_FENCE?`GFX_CAP_PRESENT_FENCE:0)|(STREAM_SUBMIT?`GFX_CAP_STREAM_SUBMIT:0)|(RING_FETCH?`GFX_CAP_RING_FETCH:0)|(FRAME_TRACE?`GFX_CAP_FRAME_TRACE:0)|CAPS_EXTRA;
   `GFX_REG_STATUS:prdata={28'b0,display_pending,faulted,(engine_busy||queue_count!=0),(space&&!faulted)};
   `GFX_REG_CONTROL:if(pwrite&&pwdata!=1&&pwdata!=2&&pwdata!=4)pslverr=1;
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
   `GFX_REG_STREAM_DST:if(!STREAM_SUBMIT||!pwrite)pslverr=1;
   `GFX_REG_CACHE_CONTROL:begin prdata={31'b0,cache_enable};if(pwrite&&pwdata>3)pslverr=1;end
   `GFX_REG_CACHE_HITS:prdata=cache_hits;
   `GFX_REG_CACHE_MISSES:prdata=cache_misses;
   `GFX_REG_CACHE_BYPASSES:prdata=cache_bypasses;
   `GFX_REG_OPAQUE_CONTROL:begin prdata={31'b0,opaque_enable};if(pwrite&&pwdata>1)pslverr=1;end
   `GFX_REG_DISP_REQUEST_BASE:prdata=disp_shadow_base;
   `GFX_REG_DISP_REQUEST_ID:prdata=disp_shadow_id;
   `GFX_REG_DISP_REQUEST_SEQ:begin prdata=disp_shadow_seq;if(!PRESENT_FENCE)pslverr=1;end
   `GFX_REG_DISP_CONTROL:if(pwrite&&pwdata!=1)pslverr=1;
   `GFX_REG_DISP_STATUS:prdata={22'b0,display_fault,display_pending,display_ready};
   `GFX_REG_DISP_FRONT_BASE:prdata=front_base;
   `GFX_REG_DISP_PRESENTED_ID:prdata=presented_id;
   `GFX_REG_DISP_RELEASED_BASE:prdata=released_base;
   `GFX_REG_DISP_UNDERFLOW:prdata=underflow;
   `GFX_REG_DISP_DIAG_VERSION:prdata=32'h00005a01;
   `GFX_REG_DISP_DIAG_BLANKS:prdata=diag_blanks;
   `GFX_REG_DISP_DIAG_SHOWN:prdata=diag_shown;
   `GFX_REG_DISP_DIAG_NEW_ID:prdata=diag_new_id;
   `GFX_REG_DISP_DIAG_REPEAT:prdata=diag_repeat;
   `GFX_REG_DISP_DIAG_DBL_RPT:prdata=diag_dbl_rpt;
   `GFX_REG_DISP_DIAG_MISS:prdata=diag_miss;
   `GFX_REG_DISP_DIAG_ACKS:prdata=diag_acks;
   `GFX_REG_DIAG_DRAIN_LAST:prdata=diag_drain_last;
   `GFX_REG_DIAG_DRAIN_MAX:prdata=diag_drain_max;
   `GFX_REG_DIAG_DESC_LAST:prdata=diag_desc_last;
   `GFX_REG_DIAG_DESC_MAX:prdata=diag_desc_max;
   `GFX_REG_DIAG_PREF_LAST:prdata=diag_pref_last;
   `GFX_REG_DIAG_PREF_MAX:prdata=diag_pref_max;
   `GFX_REG_PROBE_CONTROL:begin prdata={31'b0,probe_active};if(pwrite&&pwdata>1)pslverr=1;end
   `GFX_REG_TRACE_CONTROL:begin prdata=trace_flags;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_FIRST_SEQ:begin prdata=trace_first_seq;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_LAST_SEQ:begin prdata=trace_last_seq;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_ARM:begin prdata=trace_arm;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_FIRST:begin prdata=trace_first;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_DONE:begin prdata=trace_done;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_REQUEST:begin prdata=trace_request;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_ACK:begin prdata=trace_ack;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_TRACE_FRAME:begin prdata=trace_frame;if(!FRAME_TRACE)pslverr=1;end
   `GFX_REG_PROBE_CYCLES:prdata=probe_cycles;
   `GFX_REG_PROBE_EMPTY:prdata=probe_empty;
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
   default:pslverr=1;
  endcase
  if(pwrite && !(FRAME_TRACE&&(paddr==`GFX_REG_TRACE_CONTROL||paddr==`GFX_REG_TRACE_FIRST_SEQ||paddr==`GFX_REG_TRACE_LAST_SEQ))&&paddr!=`GFX_REG_PROBE_CONTROL&&paddr!=`GFX_REG_CONTROL&&paddr!=`GFX_REG_IRQ_STATUS&&paddr!=`GFX_REG_IRQ_ENABLE&&paddr!=`GFX_REG_CACHE_CONTROL&&paddr!=`GFX_REG_OPAQUE_CONTROL&&
    !(paddr>=`GFX_REG_OP_FLAGS&&paddr<=`GFX_REG_SUBMIT_SEQ)&&paddr!=`GFX_REG_DISP_REQUEST_BASE&&paddr!=`GFX_REG_DISP_REQUEST_ID&&paddr!=`GFX_REG_DISP_REQUEST_SEQ&&paddr!=`GFX_REG_DISP_CONTROL&&
    paddr!=`GFX_REG_DISP_DIAG_VERSION&&paddr!=`GFX_REG_STREAM_DST)pslverr=1;
 end
 always @(posedge clk)begin
  if(!rst_n)begin
   wait_count<=0;irq_status<=0;irq_enable<=0;submit_status<=0;accepted_seq<=0;complete_seq<=0;fault_seq<=0;fault_code<=0;fault_addr<=0;faulted<=0;
   display_request<=0;display_base<=0;display_id<=0;disp_shadow_base<=0;disp_shadow_id<=0;disp_shadow_seq<=0;display_seq<=0;
   cache_enable<=0;cache_flush<=0;opaque_enable<=0;
   probe_active<=0;probe_cycles<=0;probe_empty<=0;
   cycles<=0;reads<=0;writes<=0;bytes<=0;stalls<=0;snap_cycles<=0;snap_reads<=0;snap_writes<=0;snap_bytes<=0;snap_stalls<=0;
   for(k=0;k<11;k=k+1)shadow[k]<=0;
   shadow[8]<=255;shadow[9]<=24'hff00ff;
  end else begin
   if(!psel||!penable||pready)wait_count<=0;else wait_count<=wait_count+1'b1;
   display_request<=0;
   cache_flush<=0;
   // Stage the fused fields in APB SETUP. The access phase validates and
   // accepts the same complete snapshot as CONTROL=1; no validation bypass
   // or additional mux/adder is inserted into the commit critical path.
   if(STREAM_SUBMIT&&psel&&!penable&&pwrite&&stream_address)begin
    shadow[2]<=pwdata;shadow[10]<=accepted_seq+1'b1;
   end
   cycles<=cycles+1'b1;if(read_beat)reads<=reads+1'b1;if(write_beat)begin writes<=writes+1'b1;bytes<=bytes+byte_count;end
   if(stall)stalls<=stalls+1'b1;
   if(probe_active)begin
    probe_cycles<=probe_cycles+1'b1;
    if(!engine_busy&&queue_count==0)probe_empty<=probe_empty+1'b1;
   end
   irq_status<=(irq_status&~irq_clear)|events;
   if(write)begin
    if(paddr==`GFX_REG_PROBE_CONTROL)begin probe_active<=pwdata[0];if(pwdata[0])begin probe_cycles<=0;probe_empty<=0;end end
    if(paddr==`GFX_REG_CACHE_CONTROL)begin
     if(engine_safe&&queue_count==0)begin cache_enable<=pwdata[0];cache_flush<=1;submit_status<=0;end
     else submit_status<=`GFX_ERR_BUSY;
    end
    if(paddr==`GFX_REG_OPAQUE_CONTROL)begin
     if(engine_safe&&queue_count==0)begin opaque_enable<=pwdata[0];submit_status<=0;end
     else submit_status<=`GFX_ERR_BUSY;
    end
    if(paddr>=32&&paddr<=72)shadow[(paddr-32)>>2]<=pwdata;
    if(paddr==`GFX_REG_IRQ_ENABLE)irq_enable<=pwdata;
    if(paddr==`GFX_REG_DISP_REQUEST_BASE)disp_shadow_base<=pwdata;
    if(paddr==`GFX_REG_DISP_REQUEST_ID)disp_shadow_id<=pwdata;
    if(paddr==`GFX_REG_DISP_REQUEST_SEQ)disp_shadow_seq<=pwdata;
    if(paddr==`GFX_REG_DISP_CONTROL)begin
     if(disp_shadow_base[5:0]!=0||disp_shadow_base<pool_start||({32'b0,disp_shadow_base}+FRAME_BYTES)>{32'b0,pool_end}||disp_shadow_base==front_base)submit_status<=`GFX_ERR_RANGE;
     else if(display_ready&&(PRESENT_FENCE||(!engine_busy&&queue_count==0))&&!faulted&&engine_error==0)begin display_request<=1;display_base<=disp_shadow_base;display_id<=disp_shadow_id;display_seq<=disp_shadow_seq;submit_status<=0;end
     else submit_status<=`GFX_ERR_BUSY;
    end
    if(paddr==`GFX_REG_CONTROL&&pwdata==4)begin snap_cycles<=cycles;snap_reads<=reads;snap_writes<=writes;snap_bytes<=bytes;snap_stalls<=stalls;end
   end
   if(commit)begin
    if(faulted||engine_error!=0)submit_status<=`GFX_ERR_FAULT;
    else if(!space)submit_status<=`GFX_ERR_BUSY;
    else if(validation_error!=0)submit_status<=validation_error;
    else begin accepted_seq<=shadow[10];submit_status<=0;end
   end
   if(ring_fire)accepted_seq<=accepted_seq+1'b1;
   // A rejected ring descriptor halts the pipeline exactly like an engine
   // error; the sequence it would have received names the failing entry.
   if(ring_fault_valid&&!faulted)begin faulted<=1;fault_seq<=accepted_seq+1'b1;fault_code<=ring_fault_code;fault_addr<=32'd0;end
   if(reset_request)begin
    if(safe_reset)probe_active<=0;
    if(safe_reset)begin cache_enable<=0;cache_flush<=1;opaque_enable<=0;end
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
