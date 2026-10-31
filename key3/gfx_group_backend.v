`include "gfx_abi.vh"
`default_nettype none
// M3b-1 shared group backend: the ONE dual-bank group store and write-back
// service for every engine that composites groups (bucket 64x32 groups,
// tile4 128x16 groups). One instance replaces the bucket engine's internal
// banks/drain and the tile4 group_pingpong buffer plus its row writer.
//
// Protocol (three hard rules from the M3b plan):
//  1. Command ownership: the client stays owner until `idle` - resources
//     are never handed to the next command while a bank is committed.
//  2. Bank ownership is explicit: FREE -> FILLING (after grp_begin) ->
//     COMMITTED (grp_commit latches the descriptor) -> draining. A bank's
//     read port belongs to the drain from commit until it finishes; the
//     producer never routes around that, whatever its internal selectors do.
//  3. Sharing resources never cancels overlap: the drain of one bank runs
//     concurrently with the client filling the other - that is the whole
//     performance contract of M3a and stays.
//
// Descriptor generalisations vs the M3a bucket drain: variable row beats
// (1..32, tile4 partial groups), per-beat trailing WSTRB from desc_width
// (tile4 partial rows), selectable bank row pitch (16 words for bucket
// groups {row,half,word}, 32 words for tile4 groups), and the row address
// advances by += stride (no per-row multiply).
module gfx_group_backend #(parameter TIMEOUT_CYCLES=10000000)(
 input wire clk,rst_n,
 // Producer side.
 input wire grp_begin,output wire grp_ready,
 input wire [8:0] fill_waddr,input wire [127:0] fill_wdata,input wire fill_we,
 input wire [8:0] fill_raddr,output wire [127:0] fill_rdata,
 // Commit: latch the descriptor and hand the filled bank to the drain.
 input wire grp_commit,
 input wire [31:0] desc_base,input wire [31:0] desc_stride,
 input wire [5:0] desc_rows,      // 1..32 rows (6 bits: 32 does not fit in 5)
 input wire [4:0] desc_beats_m1,  // beats per row, minus one (0..31)
 input wire [7:0] desc_width,     // row width in pixels (trailing-beat strb)
 input wire desc_pitch32,         // bank row pitch: 0=16 words, 1=32 words
 // Discard ownership without draining (client guarantees writer idle).
 input wire flush,
 // Status: nothing committed, drain idle, writer idle.
 output wire idle,
 // Latched writer fault surfaced to the owning client (it owns the command).
 output wire [7:0] grp_error,output wire [31:0] grp_error_addr,
 // High while a group is being filled (write port belongs to the client).
 output wire fill_open,
 // AXI write channel.
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready
);
 localparam DS_IDLE=0,DS_REQ=1,DS_RUN=2,DS_WAIT=3;
 reg [1:0] dstate;reg [1:0] bank_full;reg fill_active,fill_sel,drain_sel;
 reg [31:0] bank_base0,bank_base1,bank_stride0,bank_stride1;
 reg [5:0] bank_rows0,bank_rows1;reg [4:0] bank_beats0,bank_beats1;
 reg [7:0] bank_width0,bank_width1;reg bank_pitch0,bank_pitch1;
 reg [4:0] drain_row;reg [31:0] drain_row_addr;reg [8:0] drain_raddr;
 wire [31:0] drain_base=drain_sel?bank_base1:bank_base0;
 wire [31:0] drain_stride=drain_sel?bank_stride1:bank_stride0;
 wire [5:0] drain_rows=drain_sel?bank_rows1:bank_rows0;
 wire [4:0] drain_beats_m1=drain_sel?bank_beats1:bank_beats0;
 wire [7:0] drain_width=drain_sel?bank_width1:bank_width0;
 wire drain_pitch32=drain_sel?bank_pitch1:bank_pitch0;
 wire [8:0] store_beats={4'b0,drain_beats_m1}+9'd1;
 wire [127:0] bank0_rdata,bank1_rdata;
 // Read-port ownership: from commit to drain-done the bank belongs to the
 // drain; otherwise it serves the producer. Never inferred from a
 // client-side role signal.
 wire [8:0] bank0_raddr=bank_full[0]?drain_raddr:fill_raddr;
 wire [8:0] bank1_raddr=bank_full[1]?drain_raddr:fill_raddr;
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(9),.OUTPUT_REG("TRUE")) bank0(
  .wdata(fill_wdata),.waddr(fill_waddr),.raddr(bank0_raddr),.we(fill_we&&!fill_sel),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(bank0_rdata));
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(9),.OUTPUT_REG("TRUE")) bank1(
  .wdata(fill_wdata),.waddr(fill_waddr),.raddr(bank1_raddr),.we(fill_we&&fill_sel),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(bank1_rdata));
 assign fill_rdata=fill_sel?bank1_rdata:bank0_rdata;
 // Skid between the bank read port and the write DMA (M3a protocol,
 // widened to 32-beat rows).
 reg [5:0] st_issued,st_sent;reg st_vq,st_vb,st_addr_pend;reg [127:0] st_buf;
 wire [5:0] st_next_beat=st_issued+6'd1;
 wire [8:0] drain_next=drain_raddr+9'd1;
 wire wr_req_ready,wr_done,wr_busy,wr_data_idle_unused;wire [7:0] wr_error;wire wr_in_ready_unused;
  reg [7:0] first_error;reg [31:0] first_error_addr;
 wire st_run=dstate==DS_RUN;
 wire st_fire=st_run&&st_vb&&wvalid&&wready;
 wire st_move=st_run&&st_vq&&(!st_vb||st_fire);
 wire st_row_start=dstate==DS_REQ&&wr_req_ready;
 wire st_advance=st_run&&(st_issued<store_beats)&&!st_addr_pend&&(!st_vq||st_move)||st_row_start;
 wire [127:0] store_rdata=drain_sel?bank1_rdata:bank0_rdata;
 // Trailing-beat byte lanes from the row width (full-width clients like
 // the bucket get 16'hffff exactly as before).
 reg [15:0] drain_strb;integer sl;reg [8:0] strb_px;
 always @*begin
  drain_strb=16'hffff;
  for(sl=0;sl<4;sl=sl+1)begin
   strb_px={st_sent[5:0],2'b00}+{6'b0,sl[1:0]};
   if(strb_px>={1'b0,drain_width})drain_strb[sl*4+:4]=4'b0;
  end
 end
 gfx_write_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) writer(clk,rst_n,st_row_start,wr_req_ready,
  drain_row_addr,store_beats,
  st_buf,drain_strb,st_run&&st_vb,wr_in_ready_unused,
  awaddr,awlen,awsize,awburst,awvalid,awready,wdata,wstrb,wlast,wvalid,wready,bresp,bvalid,bready,wr_done,wr_error,wr_busy,wr_data_idle_unused);
 assign grp_ready=!fill_active&&!bank_full[fill_sel];
 assign fill_open=fill_active;
  // Keep the first row-write failure visible after the writer drops busy at B.
  // The owner uses this sticky pair to stop producing and drain the accepted
  // backend work before it flushes the partial bank.
  assign grp_error=first_error!=0?first_error:((wr_busy||wr_done)?wr_error:8'd0);
  assign grp_error_addr=first_error!=0?first_error_addr:awaddr;
 assign idle=(|bank_full)==2'b00&&dstate==DS_IDLE&&!wr_busy;
 always @(posedge clk)begin
  if(!rst_n)begin
   dstate<=DS_IDLE;bank_full<=0;fill_active<=0;fill_sel<=0;drain_sel<=0;
   drain_row<=0;drain_row_addr<=0;drain_raddr<=0;
   bank_base0<=0;bank_base1<=0;bank_stride0<=0;bank_stride1<=0;
   bank_rows0<=0;bank_rows1<=0;bank_beats0<=0;bank_beats1<=0;
   bank_width0<=0;bank_width1<=0;bank_pitch0<=0;bank_pitch1<=0;
   first_error<=0;first_error_addr<=0;
  end else if(flush)begin
   // Drop everything without draining; the client only asserts flush when
   // the writer is already idle, so no burst is cut mid-flight.
   dstate<=DS_IDLE;bank_full<=0;fill_active<=0;drain_row<=0;
   first_error<=0;first_error_addr<=0;
  end else begin
   if(first_error==0&&(wr_busy||wr_done)&&wr_error!=0)begin first_error<=wr_error;first_error_addr<=awaddr;end
   if(grp_begin&&grp_ready)fill_active<=1'b1;
   if(grp_commit&&fill_active)begin
    if(fill_sel)begin
     bank_base1<=desc_base;bank_stride1<=desc_stride;bank_rows1<=desc_rows;
     bank_beats1<=desc_beats_m1;bank_width1<=desc_width;bank_pitch1<=desc_pitch32;
    end else begin
     bank_base0<=desc_base;bank_stride0<=desc_stride;bank_rows0<=desc_rows;
     bank_beats0<=desc_beats_m1;bank_width0<=desc_width;bank_pitch0<=desc_pitch32;
    end
    bank_full[fill_sel]<=1'b1;
    fill_sel<=!fill_sel;
    fill_active<=1'b0;
   end
   // Drain FSM. A commit and a drain-finish can land on the same edge only
   // for different banks (a group never begins on a committed bank), so
   // there is no bank_full write conflict.
   case(dstate)
    DS_IDLE:if(bank_full[drain_sel])begin
     drain_row<=5'd0;
     drain_row_addr<=drain_base;
     dstate<=DS_REQ;
    end
    DS_REQ:if(wr_req_ready)begin
     st_issued<=6'd0;st_sent<=6'd0;
     drain_raddr<=drain_pitch32?{drain_row[3:0],5'd0}:{drain_row,4'd0};
     dstate<=DS_RUN;
    end
    DS_RUN:begin
     if(st_advance)begin
      st_issued<=st_issued+6'd1;
      drain_raddr<=drain_next;
     end
     if(st_fire)st_sent<=st_sent+6'd1;
     if(st_sent>=store_beats)dstate<=DS_WAIT;
    end
    DS_WAIT:if(wr_done)begin
     if({2'b0,drain_row}+7'd1>={1'b0,drain_rows})begin
      bank_full[drain_sel]<=1'b0;
      drain_sel<=!drain_sel;
      dstate<=DS_IDLE;
     end else begin
      drain_row<=drain_row+5'd1;
      drain_row_addr<=drain_row_addr+drain_stride;
      dstate<=DS_REQ;
     end
    end
    default:dstate<=DS_IDLE;
   endcase
  end
 end
 // Skid: single-in-flight bank reads feed the write DMA input.
 always @(posedge clk)begin
  if(!rst_n)begin st_vq<=0;st_vb<=0;st_buf<=0;st_addr_pend<=0;end
  else if(st_row_start)begin st_vq<=0;st_vb<=0;st_addr_pend<=1;end
  else if(st_run)begin
   st_addr_pend<=st_advance;
   st_vq<=st_addr_pend?1'b1:(st_advance?1'b0:(st_vq&&!st_move));
   if(st_move)st_buf<=store_rdata;
   st_vb<=st_move||(st_vb&&!st_fire);
  end else begin st_vq<=0;st_vb<=0;st_addr_pend<=0;end
 end
endmodule
`default_nettype wire
