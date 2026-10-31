// Three 128-bit masters. READ_SLOTS outstanding reads (default 1 = the
// historical single-outstanding behaviour) and one outstanding write across
// the fabric. Read requests are latched into an internal FIFO as soon as
// arbitration and space allow. With two slots the queued entry's address is
// ISSUED to the memory while the head burst is still returning, so a
// controller with address lookahead can pipeline the next burst's latency
// under the current one. AXI4 returns are contiguous per burst; same-id
// pairs stay in acceptance order and a cross-id returning burst is matched
// by id, so beats always route back to the owning master in order. Display
// (master 2) gets a 64-beat urgent budget before a waiting peer, checked
// between bursts; an accepted burst is never cut short.
`ifndef FABRIC_READ_SLOTS
`define FABRIC_READ_SLOTS 2
`endif
module axi_fabric #(parameter IDW=8,parameter READ_SLOTS=`FABRIC_READ_SLOTS)(
 input wire clk,rst_n,display_urgent,
 input wire [95:0] s_araddr,input wire [23:0] s_arlen,input wire [8:0] s_arsize,input wire [5:0] s_arburst,input wire [3*IDW-1:0] s_arid,
 input wire [2:0] s_arvalid,output wire [2:0] s_arready,
 output wire [383:0] s_rdata,output wire [5:0] s_rresp,output wire [3*IDW-1:0] s_rid,output wire [2:0] s_rlast,s_rvalid,input wire [2:0] s_rready,
 input wire [95:0] s_awaddr,input wire [23:0] s_awlen,input wire [8:0] s_awsize,input wire [5:0] s_awburst,input wire [3*IDW-1:0] s_awid,
 input wire [2:0] s_awvalid,output wire [2:0] s_awready,input wire [383:0] s_wdata,input wire [47:0] s_wstrb,input wire [2:0] s_wlast,s_wvalid,output wire [2:0] s_wready,
 output wire [5:0] s_bresp,output wire [3*IDW-1:0] s_bid,output wire [2:0] s_bvalid,input wire [2:0] s_bready,
 output wire [31:0] m_araddr,output wire [7:0] m_arlen,output wire [2:0] m_arsize,output wire [1:0] m_arburst,output wire [IDW-1:0] m_arid,
 output wire m_arvalid,input wire m_arready,input wire [127:0] m_rdata,input wire [1:0] m_rresp,input wire [IDW-1:0] m_rid,input wire m_rlast,m_rvalid,output wire m_rready,
 output wire [31:0] m_awaddr,output wire [7:0] m_awlen,output wire [2:0] m_awsize,output wire [1:0] m_awburst,output wire [IDW-1:0] m_awid,
 output wire m_awvalid,input wire m_awready,output wire [127:0] m_wdata,output wire [15:0] m_wstrb,output wire m_wlast,m_wvalid,input wire m_wready,
 input wire [1:0] m_bresp,input wire [IDW-1:0] m_bid,input wire m_bvalid,output wire m_bready
);
 // read-request FIFO: entry0 = oldest, entry1 = queued (slot mode 2 only).
 // ar_issued marks that the entry's address handshake completed; ret_active
 // marks the entry whose burst is currently returning (contiguous AXI4), 2
 // while no burst is mid-flight.
 reg [31:0] fq_a0,fq_a1;reg [7:0] fq_l0,fq_l1;reg [IDW-1:0] fq_i0,fq_i1;reg [1:0] fq_o0,fq_o1;
 reg [1:0] fq_cnt;reg a0_issued,a1_issued;reg [1:0] ret_active;
 reg wactive,waddress,wdata_done;reg [1:0] wo,wnext,rnext;reg peer_next;
 reg [6:0] urgent_count;reg [8:0] active_read_beats;
 reg [IDW-1:0] expected_bid;
 integer j,candidate;reg [1:0] rpick,wpick;reg rfound,wfound;
 always @*begin
  rpick=0;wpick=0;rfound=0;wfound=0;candidate=0;
  for(j=0;j<3;j=j+1)begin
   candidate=rnext+j;if(candidate>=3)candidate=candidate-3;
   if(!rfound&&s_arvalid[candidate]&&!(candidate==2&&urgent_count>=64&&|s_arvalid[1:0]))begin rfound=1;rpick=candidate;end
  end
  if(display_urgent&&s_arvalid[2]&&(urgent_count<64||!(|s_arvalid[1:0])))begin rfound=1;rpick=2;end
  // Display completions reset the three-way cursor to CPU. Keep a separate
  // peer cursor so repeated urgent-display runs cannot starve graphics.
  if(urgent_count>=64&&|s_arvalid[1:0])begin
   rfound=1;rpick=s_arvalid[peer_next]?{1'b0,peer_next}:{1'b0,!peer_next};
  end
  for(j=0;j<3;j=j+1)begin
   candidate=wnext+j;if(candidate>=3)candidate=candidate-3;
   if(!wfound&&(s_awvalid[candidate]||s_wvalid[candidate]))begin wfound=1;wpick=candidate;end
  end
 end
 wire fq_space=fq_cnt<READ_SLOTS;
 wire push_now=fq_space&&rfound;
 wire issue_head=fq_cnt>=1&&!a0_issued;
 wire issue_queued=READ_SLOTS!=1&&fq_cnt==2&&a0_issued&&!a1_issued;
 assign m_arvalid=issue_head||issue_queued;
 assign m_araddr=issue_queued?fq_a1:fq_a0;assign m_arlen=issue_queued?fq_l1:fq_l0;
 assign m_arsize=4;assign m_arburst=1;assign m_arid=issue_queued?fq_i1:fq_i0;
 // Beat routing: the actively returning entry owns the channel until its
 // rlast; a fresh burst is identified by id, a same-id pair resolving to the
 // older entry (AXI same-id ordering).
 wire e0_ret=fq_cnt>=1&&a0_issued;
 wire e1_ret=READ_SLOTS!=1&&fq_cnt==2&&a1_issued;
 wire rid_match1=e1_ret&&m_rid==fq_i1&&(!e0_ret||m_rid!=fq_i0);
 wire ret_pick1=ret_active==1||(ret_active==2&&rid_match1);
 wire [1:0] ret_owner=ret_pick1?fq_o1:fq_o0;
 wire [IDW-1:0] ret_id=ret_pick1?fq_i1:fq_i0;
 wire beat_fire=m_rvalid&&(e0_ret||e1_ret)&&s_rready[ret_owner];
 wire last_fire=beat_fire&&m_rlast;
 wire [9:0] urgent_sum={3'b0,urgent_count}+{1'b0,active_read_beats};
 assign m_rready=(e0_ret||e1_ret)&&s_rready[ret_owner];
 assign m_awaddr=s_awaddr[wo*32+:32];assign m_awlen=s_awlen[wo*8+:8];assign m_awsize=s_awsize[wo*3+:3];assign m_awburst=s_awburst[wo*2+:2];assign m_awid=s_awid[wo*IDW+:IDW];
 assign m_awvalid=wactive&&!waddress&&s_awvalid[wo];
 assign m_wdata=s_wdata[wo*128+:128];assign m_wstrb=s_wstrb[wo*16+:16];assign m_wlast=s_wlast[wo];assign m_wvalid=wactive&&!wdata_done&&s_wvalid[wo];
 assign m_bready=wactive&&waddress&&s_bready[wo];
 genvar n;generate for(n=0;n<3;n=n+1)begin:route
  assign s_arready[n]=fq_space&&rfound&&rpick==n;
  assign s_rdata[n*128+:128]=m_rdata;assign s_rresp[n*2+:2]=(m_rid==ret_id)?m_rresp:2'b10;assign s_rid[n*IDW+:IDW]=m_rid;
  assign s_rlast[n]=m_rlast;assign s_rvalid[n]=m_rvalid&&ret_owner==n&&(e0_ret||e1_ret);
  assign s_awready[n]=wactive&&!waddress&&wo==n&&m_awready;
  assign s_wready[n]=wactive&&!wdata_done&&wo==n&&m_wready;
  assign s_bresp[n*2+:2]=(m_bid==expected_bid)?m_bresp:2'b10;assign s_bid[n*IDW+:IDW]=m_bid;assign s_bvalid[n]=wactive&&waddress&&wo==n&&m_bvalid;
 end endgenerate
 // A push lands in entry0 only when the whole queue empties this cycle
 // (nothing queued and the retiring entry is entry0); otherwise entry1.
 wire push_head=(fq_cnt==0)||(last_fire&&!ret_pick1&&fq_cnt==1);
 always @(posedge clk)begin
  if(!rst_n)begin wactive<=0;waddress<=0;wdata_done<=0;wo<=0;wnext<=0;rnext<=0;peer_next<=0;urgent_count<=0;active_read_beats<=1;expected_bid<=0;
   fq_cnt<=0;a0_issued<=0;a1_issued<=0;ret_active<=2;fq_a0<=0;fq_a1<=0;fq_l0<=0;fq_l1<=0;fq_i0<=0;fq_i1<=0;fq_o0<=0;fq_o1<=0;end
  else begin
   if(push_now)begin
    if(push_head)begin fq_a0<=s_araddr[rpick*32+:32];fq_l0<=s_arlen[rpick*8+:8];fq_i0<=s_arid[rpick*IDW+:IDW];fq_o0<=rpick;a0_issued<=0;end
    else begin fq_a1<=s_araddr[rpick*32+:32];fq_l1<=s_arlen[rpick*8+:8];fq_i1<=s_arid[rpick*IDW+:IDW];fq_o1<=rpick;a1_issued<=0;end
   end
   if(m_arvalid&&m_arready)begin
    if(issue_queued)begin a1_issued<=1;active_read_beats<={1'b0,fq_l1}+9'd1;end
    else begin a0_issued<=1;active_read_beats<={1'b0,fq_l0}+9'd1;end
   end
   if(beat_fire&&ret_active==2)ret_active<=ret_pick1?1:0;
   if(last_fire)begin
    if(ret_pick1)a1_issued<=0; // entry1 retires; entry0 keeps its place
    else if(fq_cnt==2)begin
     // the shift adopts the queued entry; if its address handshake completed
     // THIS cycle it is issued, if it was merely being presented (arvalid
     // held, not yet accepted) it must stay presented - AXI forbids dropping
     // or changing an asserted AR before its handshake
     fq_a0<=fq_a1;fq_l0<=fq_l1;fq_i0<=fq_i1;fq_o0<=fq_o1;
     a0_issued<=(m_arvalid&&m_arready&&issue_queued)?1'b1:a1_issued;a1_issued<=0;
    end
    else a0_issued<=0;
    ret_active<=2;
    rnext<=ret_owner==2?0:ret_owner+1'b1;
    if(ret_owner==2)urgent_count<=urgent_sum>=64?7'd64:urgent_sum[6:0];
    else begin urgent_count<=0;peer_next<=!ret_owner[0];end
   end
   fq_cnt<=fq_cnt+(push_now?1'b1:1'b0)-(last_fire?1'b1:1'b0);
   if(!wactive&&wfound)begin wactive<=1;wo<=wpick;waddress<=0;wdata_done<=0;end
   if(m_wvalid&&m_wready&&m_wlast)wdata_done<=1;
   if(m_awvalid&&m_awready)begin waddress<=1;expected_bid<=m_awid;end
   if(m_bvalid&&m_bready)begin wactive<=0;wnext<=wo==2?0:wo+1'b1;end
  end
 end
endmodule
