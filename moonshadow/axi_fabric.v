// Three 128-bit masters. One outstanding read and one outstanding write across
// the fabric; preserve the selected master's ID and route all response fields.
// Display (master 2) gets a 64-beat urgent budget before a waiting peer.
// The budget is checked between bursts; an accepted burst is never cut short.
module axi_fabric #(parameter IDW=8)(
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
 reg ractive,wactive,raddress,waddress,wdata_done;reg [1:0] ro,wo,rnext,wnext;reg [6:0] urgent_count;
 reg [8:0] active_read_beats;reg peer_next;
 wire [9:0] urgent_sum={3'b0,urgent_count}+{1'b0,active_read_beats};
 reg [IDW-1:0] expected_rid,expected_bid;
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
 assign m_araddr=s_araddr[ro*32+:32];assign m_arlen=s_arlen[ro*8+:8];assign m_arsize=s_arsize[ro*3+:3];assign m_arburst=s_arburst[ro*2+:2];assign m_arid=s_arid[ro*IDW+:IDW];
 assign m_arvalid=ractive&&!raddress&&s_arvalid[ro];assign m_rready=ractive&&raddress&&s_rready[ro];
 assign m_awaddr=s_awaddr[wo*32+:32];assign m_awlen=s_awlen[wo*8+:8];assign m_awsize=s_awsize[wo*3+:3];assign m_awburst=s_awburst[wo*2+:2];assign m_awid=s_awid[wo*IDW+:IDW];
 assign m_awvalid=wactive&&!waddress&&s_awvalid[wo];
 assign m_wdata=s_wdata[wo*128+:128];assign m_wstrb=s_wstrb[wo*16+:16];assign m_wlast=s_wlast[wo];assign m_wvalid=wactive&&!wdata_done&&s_wvalid[wo];
 assign m_bready=wactive&&waddress&&s_bready[wo];
 genvar n;generate for(n=0;n<3;n=n+1)begin:route
  assign s_arready[n]=ractive&&!raddress&&ro==n&&m_arready;
  assign s_rdata[n*128+:128]=m_rdata;assign s_rresp[n*2+:2]=(m_rid==expected_rid)?m_rresp:2'b10;assign s_rid[n*IDW+:IDW]=m_rid;
  assign s_rlast[n]=m_rlast;assign s_rvalid[n]=ractive&&raddress&&ro==n&&m_rvalid;
  assign s_awready[n]=wactive&&!waddress&&wo==n&&m_awready;
  assign s_wready[n]=wactive&&!wdata_done&&wo==n&&m_wready;
  assign s_bresp[n*2+:2]=(m_bid==expected_bid)?m_bresp:2'b10;assign s_bid[n*IDW+:IDW]=m_bid;assign s_bvalid[n]=wactive&&waddress&&wo==n&&m_bvalid;
 end endgenerate
 always @(posedge clk)begin
  if(!rst_n)begin ractive<=0;wactive<=0;raddress<=0;waddress<=0;wdata_done<=0;ro<=0;wo<=0;rnext<=0;wnext<=0;peer_next<=0;urgent_count<=0;active_read_beats<=1;expected_rid<=0;expected_bid<=0;end
  else begin
   if(!ractive&&rfound)begin ractive<=1;ro<=rpick;raddress<=0;end
   if(m_arvalid&&m_arready)begin raddress<=1;expected_rid<=m_arid;active_read_beats<={1'b0,m_arlen}+9'd1;end
   if(m_rvalid&&m_rready&&m_rlast)begin
    ractive<=0;rnext<=ro==2?0:ro+1'b1;
    if(ro==2)urgent_count<=urgent_sum>=64?7'd64:urgent_sum[6:0];else begin urgent_count<=0;peer_next<=!ro[0];end
   end
   if(!wactive&&wfound)begin wactive<=1;wo<=wpick;waddress<=0;wdata_done<=0;end
   if(m_wvalid&&m_wready&&m_wlast)wdata_done<=1;
   if(m_awvalid&&m_awready)begin waddress<=1;expected_bid<=m_awid;end
   if(m_bvalid&&m_bready)begin wactive<=0;wnext<=wo==2?0:wo+1'b1;end
  end
 end
endmodule
