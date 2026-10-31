// Fixed-format scanout with an explicit quiesce/drain/descriptor/ack cycle in
// every vertical blank. Old FIFO data and accepted AXI bursts drain before a
// new base is published. An underflow poisons the rest of the frame, preserving
// alignment; the next blank recovers. No unilateral asynchronous FIFO reset.
module display_scanout #(
 parameter WIDTH=640,HEIGHT=480,H_FRONT=16,H_SYNC=96,H_BACK=48,V_FRONT=10,V_SYNC=2,V_BACK=33,
 parameter INITIAL_BASE=32'h01000000,FIFO_AW=9,ALIGN_VSYNC=0,LATE_PRESENT=1,PREFETCH_LINES=6,MAX_READ_BEATS=16
)(input wire clk,rst_n,pclk,prst_n,
 input wire request,input wire [31:0] request_base,request_id,output wire request_ready,output reg pending,
 output reg ack,output reg [31:0] front_base,presented_id,released_base,underflows,
 output wire [31:0] protected_pending_base,output wire low_water,
 output wire [23:0] rgb,output wire de,hsync_n,vsync_n,output wire [11:0] x,y,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output reg [7:0] fault,output wire [31:0] pixel_base,
 // R5-A present diagnostics. All counters live in clk; pclk contributes only
 // registered single-bit toggles, never multi-bit values. diag_clear zeroes
 // the counters/timers only (display content state such as last shown id is
 // kept, so a cleared window never invents a spurious "new id" event).
 input wire diag_clear,
 output wire [31:0] diag_version,diag_blanks,diag_shown,diag_new_id,diag_repeat,diag_dbl_rpt,diag_miss,diag_acks,
 output wire [31:0] diag_drain_last,diag_drain_max,diag_desc_last,diag_desc_max,diag_pref_last,diag_pref_max
);
 localparam FRAME_BYTES=WIDTH*HEIGHT*4;
 // Keep at least half of the RAM depth as latency headroom when waiting for
 // a complete-burst credit. Small model FIFOs therefore clamp large settings.
 localparam READ_BEATS=MAX_READ_BEATS<(1<<(FIFO_AW-1))?MAX_READ_BEATS:(1<<(FIFO_AW-1));
 localparam WAIT_BLANK=0,DRAIN=1,WAIT_DRAIN=2,DESCRIPTOR=3,FETCH=4,READ=5,FRAME_DONE=6,SELECT_FRAME=7;
 localparam BLANK_LINES=V_FRONT+V_SYNC+V_BACK;
 localparam GUARD_LINES=BLANK_LINES>1?((PREFETCH_LINES<1)?1:((PREFETCH_LINES<BLANK_LINES)?PREFETCH_LINES:BLANK_LINES-1)):0;
 reg [2:0] state;reg [31:0] offset,next_base,next_id,active_base,active_id;
 reg [31:0] pending_base,pending_id;
 reg blank_toggle,drained_toggle,desc_toggle,desc_ack,shown_toggle;
 reg [31:0] shown_id,shown_base,pix_base,pix_id;
 assign pixel_base=pix_base;
 reg draining,armed,started,poisoned;
 (* syn_keep=1 *)reg admit_late,poison_report;
 reg ever_started;
 reg [1:0] lane;
 (* async_reg="true" *)reg [1:0] blank_sync,drain_sync,desc_ack_sync,shown_sync;
 (* async_reg="true" *)reg [1:0] desc_sync,quiescent_sync;
 (* async_reg="true" *)reg [1:0] window_sync;
 reg blank_seen,desc_seen,shown_seen;
 // R5-A event sources (pclk): new-id/repeat classification of each shown
 // frame. last_disp_id stays in pclk; only toggles cross to clk.
 reg [31:0] last_disp_id;reg was_repeat;
 reg fs_tgl,new_tgl,rpt_tgl,dbl_tgl;
 wire frame_start,vblank_start;
 display_timing #(.H_ACTIVE(WIDTH),.H_FRONT(H_FRONT),.H_SYNC(H_SYNC),.H_BACK(H_BACK),.V_ACTIVE(HEIGHT),.V_FRONT(V_FRONT),.V_SYNC(V_SYNC),.V_BACK(V_BACK),.ALIGN_VSYNC(ALIGN_VSYNC)) timing(pclk,prst_n,x,y,de,hsync_n,vsync_n,frame_start,vblank_start);
 wire fifo_valid,fifo_ready,fifo_empty,fifo_write_ready;wire [127:0] fifo_pixel;wire [FIFO_AW:0] level;
 wire rd_valid,rd_done,rd_ready,rd_busy;wire [127:0] rd_pixel;wire [7:0] rd_error;
 wire draining_sys=(state==DRAIN||state==WAIT_DRAIN);
 wire [31:0] bytes_left=FRAME_BYTES-offset;
 wire [31:0] addr=active_base+offset;
 wire [31:0] page_bytes=4096-{20'b0,addr[11:0]};
 wire [31:0] size0=bytes_left<READ_BEATS*16?bytes_left:READ_BEATS*16;
 wire [31:0] burst_bytes=size0<page_bytes?size0:page_bytes;
 wire [8:0] burst_beats=burst_bytes>>4;
 // wlevel uses the synchronized read pointer and conservatively counts RAM
 // occupancy. FETCH has no previous DMA data still pending, so this reserves
 // space for the complete next burst, even if the pixel clock stops consuming.
 wire [FIFO_AW:0] free_beats=(1<<FIFO_AW)-level;
 wire read_credit=free_beats>=burst_beats;
 wire read_request=state==FETCH&&read_credit&&blank_sync[1]==blank_seen;
 gfx_read_dma #(.FIFO_DEPTH(2),.FIFO_REGISTERS(1)) reader(clk,rst_n,read_request,rd_ready,addr,burst_beats,
  araddr,arlen,arsize,arburst,arvalid,arready,rdata,rresp,rlast,rvalid,rready,
  rd_pixel,rd_valid,fifo_write_ready,rd_done,rd_error,rd_busy);
 gfx_async_fifo #(.WIDTH(128),.AW(FIFO_AW)) fifo(clk,rst_n,rd_valid,fifo_write_ready,rd_pixel,
  pclk,prst_n,fifo_valid,fifo_ready,fifo_pixel,fifo_empty,level);
 wire scanning=frame_start?(armed&&fifo_valid):started;
 wire bad_frame=frame_start?(!armed||!fifo_valid):poisoned;
 assign fifo_ready=draining||(de&&scanning&&!bad_frame&&lane==3);
 assign rgb=(!scanning||bad_frame||!fifo_valid)?24'hff00ff:(fifo_pixel>>(lane*32));
 assign request_ready=!pending;assign protected_pending_base=pending?pending_base:0;
 assign low_water=level<(1<<(FIFO_AW-1));
 wire quiescent=draining_sys&&!rd_busy;
 always @(posedge pclk)begin
   if(!prst_n)begin
   blank_toggle<=0;drained_toggle<=0;desc_ack<=0;shown_toggle<=0;shown_id<=0;shown_base<=INITIAL_BASE;
   draining<=0;armed<=0;started<=0;poisoned<=0;admit_late<=0;ever_started<=0;poison_report<=0;lane<=0;desc_sync<=0;quiescent_sync<=0;desc_seen<=0;pix_base<=INITIAL_BASE;pix_id<=0;
   last_disp_id<=32'hffffffff;was_repeat<=0;fs_tgl<=0;new_tgl<=0;rpt_tgl<=0;dbl_tgl<=0;
  end else begin
   // Registered single-bit CDC source; no combinational poison/armed glitch.
   poison_report<=poisoned&&ever_started;
   desc_sync<={desc_sync[0],desc_toggle};quiescent_sync<={quiescent_sync[0],quiescent};
   if(vblank_start)begin blank_toggle<=~blank_toggle;draining<=1;armed<=0;lane<=0;admit_late<=LATE_PRESENT&&BLANK_LINES>1;end
   if(x==0&&y==HEIGHT+BLANK_LINES-GUARD_LINES)admit_late<=0;
   if(draining&&quiescent_sync[1]&&fifo_empty)drained_toggle<=blank_toggle;
   if(desc_sync[1]!=desc_seen)begin
    // Payload held by clk domain until the round-trip acknowledgement.
    desc_seen<=desc_sync[1];pix_base<=next_base;pix_id<=next_id;desc_ack<=desc_sync[1];draining<=0;armed<=1;lane<=0;poisoned<=0;
   end
   if(frame_start)begin
    admit_late<=0;
    fs_tgl<=~fs_tgl;
    lane<=0;poisoned<=!armed||!fifo_valid;started<=armed&&fifo_valid;
    if(armed&&fifo_valid)begin
     ever_started<=1;shown_id<=pix_id;shown_base<=pix_base;shown_toggle<=~shown_toggle;
     if(pix_id!=last_disp_id)begin last_disp_id<=pix_id;new_tgl<=~new_tgl;was_repeat<=0;end
     else begin rpt_tgl<=~rpt_tgl;if(was_repeat)dbl_tgl<=~dbl_tgl;was_repeat<=1;end
    end
   end
   if(de&&scanning&&!bad_frame)begin
    if(!fifo_valid)poisoned<=1;
    else lane<=lane+1'b1;
   end
  end
 end
 (* async_reg="true" *)reg [1:0] poison_sync;
 reg poison_seen;
 // R5-A clk-domain diagnostics: event sync + counters + blank-phase timers.
 (* async_reg="true" *)reg [1:0] fs_sync,new_sync,rpt_sync,dbl_sync;
 reg fs_seen,new_seen,rpt_seen,dbl_seen;
 reg [31:0] d_blanks,d_shown,d_new_id,d_repeat,d_dbl_rpt,d_miss,d_acks;
 reg [31:0] d_drain_last,d_drain_max,d_desc_last,d_desc_max,d_pref_last,d_pref_max;
 reg drain_run,desc_run,pref_run;reg [15:0] drain_cnt,desc_cnt,pref_cnt;
 wire fs_edge=fs_sync[1]!=fs_seen,new_edge=new_sync[1]!=new_seen,
      rpt_edge=rpt_sync[1]!=rpt_seen,dbl_edge=dbl_sync[1]!=dbl_seen;
 localparam [31:0] DIAG_VERSION=32'h00005a01;
 assign diag_version=DIAG_VERSION;assign diag_blanks=d_blanks;assign diag_shown=d_shown;
 assign diag_new_id=d_new_id;assign diag_repeat=d_repeat;assign diag_dbl_rpt=d_dbl_rpt;
 assign diag_miss=d_miss;assign diag_acks=d_acks;
 assign diag_drain_last=d_drain_last;assign diag_drain_max=d_drain_max;
 assign diag_desc_last=d_desc_last;assign diag_desc_max=d_desc_max;
 assign diag_pref_last=d_pref_last;assign diag_pref_max=d_pref_max;
 always @(posedge clk)begin
  if(!rst_n)begin
   state<=WAIT_BLANK;offset<=0;next_base<=INITIAL_BASE;next_id<=0;active_base<=INITIAL_BASE;active_id<=0;
   pending<=0;pending_base<=0;pending_id<=0;ack<=0;front_base<=INITIAL_BASE;presented_id<=0;released_base<=0;underflows<=0;fault<=0;
   blank_sync<=0;drain_sync<=0;desc_ack_sync<=0;shown_sync<=0;window_sync<=0;blank_seen<=0;shown_seen<=0;desc_toggle<=0;poison_sync<=0;poison_seen<=0;
   fs_sync<=0;new_sync<=0;rpt_sync<=0;dbl_sync<=0;fs_seen<=0;new_seen<=0;rpt_seen<=0;dbl_seen<=0;
   d_blanks<=0;d_shown<=0;d_new_id<=0;d_repeat<=0;d_dbl_rpt<=0;d_miss<=0;d_acks<=0;
   d_drain_last<=0;d_drain_max<=0;d_desc_last<=0;d_desc_max<=0;d_pref_last<=0;d_pref_max<=0;
   drain_run<=0;desc_run<=0;pref_run<=0;drain_cnt<=0;desc_cnt<=0;pref_cnt<=0;
  end else begin
   blank_sync<={blank_sync[0],blank_toggle};drain_sync<={drain_sync[0],drained_toggle};desc_ack_sync<={desc_ack_sync[0],desc_ack};shown_sync<={shown_sync[0],shown_toggle};
   window_sync<={window_sync[0],admit_late};
   poison_sync<={poison_sync[0],poison_report};poison_seen<=poison_sync[1];ack<=0;
   fs_sync<={fs_sync[0],fs_tgl};new_sync<={new_sync[0],new_tgl};
   rpt_sync<={rpt_sync[0],rpt_tgl};dbl_sync<={dbl_sync[0],dbl_tgl};
   if(poison_sync[1]&&!poison_seen)underflows<=underflows+1'b1;
   if(request&&request_ready)begin pending<=1;pending_base<=request_base;pending_id<=request_id;end
   if(shown_sync[1]!=shown_seen)begin
    shown_seen<=shown_sync[1];
    if(!diag_clear)d_shown<=d_shown+1'b1;
    if(pending&&shown_base==pending_base&&shown_id==pending_id)begin
     released_base<=front_base;front_base<=shown_base;presented_id<=shown_id;pending<=0;ack<=1;
    end
   end
   if(diag_clear)begin
    fs_seen<=fs_sync[1];new_seen<=new_sync[1];rpt_seen<=rpt_sync[1];dbl_seen<=dbl_sync[1];
    d_blanks<=0;d_shown<=0;d_new_id<=0;d_repeat<=0;d_dbl_rpt<=0;d_miss<=0;d_acks<=0;
    d_drain_last<=0;d_drain_max<=0;d_desc_last<=0;d_desc_max<=0;d_pref_last<=0;d_pref_max<=0;
    drain_run<=0;desc_run<=0;pref_run<=0;drain_cnt<=0;desc_cnt<=0;pref_cnt<=0;
   end else begin
    if(fs_edge)begin fs_seen<=fs_sync[1];d_blanks<=d_blanks+1'b1;end
    if(new_edge)begin new_seen<=new_sync[1];d_new_id<=d_new_id+1'b1;end
    if(rpt_edge)begin rpt_seen<=rpt_sync[1];d_repeat<=d_repeat+1'b1;end
    if(dbl_edge)begin dbl_seen<=dbl_sync[1];d_dbl_rpt<=d_dbl_rpt+1'b1;end
    if(ack)d_acks<=d_acks+1'b1;
    // Blank-phase timers (clk cycles). drain: blank edge -> drained handshake;
    // desc: select transition -> descriptor round trip; pref: descriptor ->
    // first prefetch burst completion. run flags make each latch once per blank.
    if(blank_sync[1]!=blank_seen)begin drain_run<=1;drain_cnt<=0;end
    else if(drain_run)begin
     if(state==WAIT_DRAIN&&drain_sync[1]==blank_seen)begin
      drain_run<=0;d_drain_last<={16'b0,drain_cnt};
      if({16'b0,drain_cnt}>d_drain_max)d_drain_max<={16'b0,drain_cnt};
     end else drain_cnt<=drain_cnt+1'b1;
    end
    if(desc_run)begin
     if(state==DESCRIPTOR&&desc_ack_sync[1]==desc_toggle)begin
      desc_run<=0;d_desc_last<={16'b0,desc_cnt};
      if({16'b0,desc_cnt}>d_desc_max)d_desc_max<={16'b0,desc_cnt};
     end else desc_cnt<=desc_cnt+1'b1;
    end
    if(pref_run)begin
     if(rd_done)begin
      pref_run<=0;d_pref_last<={16'b0,pref_cnt};
      if({16'b0,pref_cnt}>d_pref_max)d_pref_max<={16'b0,pref_cnt};
     end else pref_cnt<=pref_cnt+1'b1;
    end
   end
   if(rd_done&&rd_error!=0)fault<=rd_error;
   // Blank wins over fetch completion. Accepted bursts remain with reader.
   if(blank_sync[1]!=blank_seen)begin
    blank_seen<=blank_sync[1];state<=DRAIN;
   end
   else case(state)
    DRAIN:if(!rd_busy)state<=WAIT_DRAIN;
    WAIT_DRAIN:if(drain_sync[1]==blank_seen)state<=SELECT_FRAME;
    SELECT_FRAME:if(pending||!window_sync[1])begin
     // Wait for a ready frame within blank, but reserve GUARD_LINES for the
     // descriptor round trip and FIFO prefill. A request at/after the cutoff
     // may deliberately wait for the next blank. Release is still at shown.
     next_base<=pending?pending_base:front_base;next_id<=pending?pending_id:presented_id;
     if(!pending&&!diag_clear)d_miss<=d_miss+1'b1;
     if(!diag_clear)begin desc_run<=1;desc_cnt<=0;end
     desc_toggle<=~desc_toggle;state<=DESCRIPTOR;
    end
    DESCRIPTOR:if(desc_ack_sync[1]==desc_toggle)begin active_base<=next_base;active_id<=next_id;offset<=0;state<=FETCH;
     if(!diag_clear)begin pref_run<=1;pref_cnt<=0;end end
    FETCH:if(read_request&&rd_ready)state<=READ;
    READ:if(rd_done)begin offset<=offset+burst_bytes;state<=(offset+burst_bytes>=FRAME_BYTES)?FRAME_DONE:FETCH;end
    default:begin end
   endcase
  end
 end
endmodule
