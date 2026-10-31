`include "platform_config.vh"
//***************************************************************/
//
// Moudel Name    : ddr3_example_top.v
// Version        : 1.1
// Date Created   : 2023-02-23 10:37:59
// Last Modified  : 2023-02-24 15:27:41
// Abstract       : ---
//
//***************************************************************/
//Modification History
//1.initial
//***************************************************************/
`include "ddr3_controller/ddr3_parameter.vh"

`define Efinity_Debug
`timescale 1ps/1ps
// `define SOFT_JTAG

module ddr_board #
(
parameter                       RANK_RATIO         = 1,       // # of unique CS outputs per rank
parameter                       CK_RATIO           = `CK_RATIO, 
parameter                       ASYN_AXI_CLK       = `ASYN_AXI_CLK, 
parameter                       RANKS              = `RANKS,
parameter                       CK_WIDTH           = `CK_WIDTH,       // # of CK/CK# outputs to memory   
parameter                       CKE_WIDTH          = `CKE_WIDTH,       // # of cke outputs
parameter                       CS_WIDTH           = `CS_WIDTH,       // # of unique CS outputs
parameter                       BANK_WIDTH         = `BANK_WIDTH,       // # of bank bits
parameter                       ROW_WIDTH          = `ROW_WIDTH,       // DRAM address bus width
parameter                       COL_WIDTH          = `COL_WIDTH,      // column address width
parameter                       DM_WIDTH           = `DM_WIDTH,       // # of DM (data mask)
parameter                       DQS_WIDTH          = `DQS_WIDTH,       // # of DQS (strobe)
parameter                       DQ_WIDTH           = `DQ_WIDTH,      // # of DQ (data)
parameter                       ODT_WIDTH          = `ODT_WIDTH,
parameter                       DQ_CNT_WIDTH       = `DQ_CNT_WIDTH,       // = ceil(log2(DQ_WIDTH))
parameter                       DQS_CNT_WIDTH      = `DQS_CNT_WIDTH,       // = ceil(log2(DQS_WIDTH))  
parameter                       DRAM_WIDTH         = `DRAM_WIDTH,       // # of DQ per DQS   
parameter                       DATA_WIDTH         = `DATA_WIDTH,
parameter                       ADDR_WIDTH         = `ADDR_WIDTH,    
parameter                       AXI_ID_WIDTH       = `AXI_ID_WIDTH,
parameter                       AXI_ADDR_WIDTH     = `AXI_ADDR_WIDTH,
parameter                       AXI_DATA_WIDTH     = `AXI_DATA_WIDTH
)
(

   // Clock and reset ports
   input pixel_clk,scan_fast,pixel_locked,axi_clk,clk_25m,
 input [3:0] key_n,
 /* Internal-reconfiguration interface (CONFIG_CTRL0, remote update). The
    APB MIRROR_LAUNCH register at 0xF8100014 latches cfg_CBSEL and raises
    cfg_CONFIG; the FPGA then reloads itself from the selected flash slot. */
 input cfg_ERROR,
 output [1:0] cfg_CBSEL,
 output cfg_CONFIG,
 output cfg_ENA,
output [9:0] tmds_clk_o, output tmds_clk_TX_OE, output tmds_clk_TX_RST,
output [9:0] tmds_data0_o, output tmds_data0_TX_OE, output tmds_data0_TX_RST,
output [9:0] tmds_data1_o, output tmds_data1_TX_OE, output tmds_data1_TX_RST,
output [9:0] tmds_data2_o, output tmds_data2_TX_OE, output tmds_data2_TX_RST,      
   input                              core_clk,     // CORE CLK @ 100MHz
   input                              sdram_clk,    // SDRAM CK @ 400MHz
   input                              rx_cal_clk,   // SDRAM CK @ 400MHz
   input                              tx_cal_clk,   // SDRAM CK @ 400MHz
   input                              tx_cal_clk_90edge,   // SDRAM CK @ 400MHz
   input                              pll_locked,
   input                              user_pll_locked,
  //  input                              peripheralClk,
   //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&
   output  wire                       system_uart_0_io_txd,
   input                              system_uart_0_io_rxd,   
    // debug core ports
`ifdef  Efinity_Debug  //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&& 
`ifdef SOFT_JTAG
  input                               io_jtag_tms,
  input                               io_jtag_tdi,
  output  wire                        io_jtag_tdo,
  input                               io_jtag_tck,
`else
  input                               jtag_inst1_CAPTURE ,
  input                               jtag_inst1_DRCK    ,
  input                               jtag_inst1_RESET   ,
  input                               jtag_inst1_RUNTEST ,
  input                               jtag_inst1_SEL     ,
  input                               jtag_inst1_SHIFT   ,
  input                               jtag_inst1_TCK     ,
  input                               jtag_inst1_TDI     ,
  input                               jtag_inst1_TMS     ,
  input                               jtag_inst1_UPDATE  ,
  output                              jtag_inst1_TDO     ,
`endif 

`endif  //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&
   // PLL status flags  
   output [2:0]                       pll_shift,  
   output [4:0]                       pll_shift_sel,
   output                             pll_shift_ena,  
   // memory interface ports
   output                             ddr_ck_hi,
   output                             ddr_ck_lo,
   output                             ddr_reset_n,
   output [CKE_WIDTH-1:0]             ddr_cke,     
   output [ROW_WIDTH-1:0]             ddr_addr,
   output [BANK_WIDTH-1:0]            ddr_ba,
   output                             ddr_cas_n,

   output [CS_WIDTH*RANK_RATIO-1:0]   ddr_cs_n,
   output                             ddr_ras_n,
   output                             ddr_we_n,
   
   input  [DQS_WIDTH-1:0]             ddr_dqs_in_hi,
   input  [DQS_WIDTH-1:0]             ddr_dqs_in_lo,
   input  [DQ_WIDTH-1:0]              ddr_dq_in_hi,
   input  [DQ_WIDTH-1:0]              ddr_dq_in_lo,
   
   output [DQS_WIDTH-1:0]             ddr_dqs_oe,
   output [DQS_WIDTH-1:0]             ddr_dqs_oe_n,
   output [DQ_WIDTH-1:0]              ddr_dq_oe,  
   output [DQS_WIDTH-1:0]             ddr_dqs_out_hi,
   output [DQS_WIDTH-1:0]             ddr_dqs_out_lo,
   output [DQ_WIDTH-1:0]              ddr_dq_out_hi,
   output [DQ_WIDTH-1:0]              ddr_dq_out_lo,
   output [DM_WIDTH-1:0]              ddr_dm_hi,
   output [DM_WIDTH-1:0]              ddr_dm_lo,
   output [ODT_WIDTH-1:0]             ddr_odt


   );


      
//Parameter Define
parameter FREQ = 200;			// default is 100 MHz.  Redefine as needed.
`ifndef SIM
    localparam CNT_INIT = 1.5*FREQ*1000;
`else
    localparam CNT_INIT = 10;
`endif    
  localparam   AIW               = AXI_ID_WIDTH      ;
  localparam   ADW               = AXI_DATA_WIDTH    ;
  localparam   ABN               = AXI_DATA_WIDTH/8   ;
  /////////////////////////////////////////////////////////
  // Wire declarations
  reg  [19:0]                       cnt;    
  wire                              clk;
  wire                              rst;
  wire                              mmcm_locked;
  reg                               aresetn;
  wire                              app_sr_active;
  wire                              app_ref_ack;
  wire                              app_zq_ack;

  wire                              cal_done;

  // Slave Interface Write Address Ports
  wire [AXI_ID_WIDTH-1:0]           s_axi_awid;
  wire [AXI_ADDR_WIDTH-1:0]         s_axi_awaddr;
  wire [7:0]                        s_axi_awlen;
  wire [2:0]                        s_axi_awsize;
  wire [1:0]                        s_axi_awburst;
  wire [0:0]                        s_axi_awlock;
  wire [3:0]                        s_axi_awcache;
  wire [2:0]                        s_axi_awprot;
  wire                              s_axi_awvalid;
  wire                              s_axi_awready;
   // Slave Interface Write Data Ports
  wire [AXI_DATA_WIDTH-1:0]         s_axi_wdata;
  wire [(AXI_DATA_WIDTH/8)-1:0]     s_axi_wstrb;
  wire                              s_axi_wlast;
  wire                              s_axi_wvalid;
  wire                              s_axi_wready;
   // Slave Interface Write Response Ports
  wire                              s_axi_bready;
  wire [AXI_ID_WIDTH-1:0]           s_axi_bid;
  wire [1:0]                        s_axi_bresp;
  wire                              s_axi_bvalid;
   // Slave Interface Read Address Ports
  wire [AXI_ID_WIDTH-1:0]           s_axi_arid;
  wire [AXI_ADDR_WIDTH-1:0]         s_axi_araddr;
  wire [7:0]                        s_axi_arlen;
  wire [2:0]                        s_axi_arsize;
  wire [1:0]                        s_axi_arburst;
  wire [0:0]                        s_axi_arlock;
  wire [3:0]                        s_axi_arcache;
  wire [2:0]                        s_axi_arprot;
  wire                              s_axi_arvalid;
  wire                              s_axi_arready;
   // Slave Interface Read Data Ports
  wire                              s_axi_rready;
  wire [AXI_ID_WIDTH-1:0]           s_axi_rid;
  wire [AXI_DATA_WIDTH-1:0]         s_axi_rdata;
  wire [1:0]                        s_axi_rresp;
  wire                              s_axi_rlast;
  wire                              s_axi_rvalid;


//***************************************************************************
  wire [2:0]                        vio_pll_shift;  
  wire [4:0]                        vio_pll_shift_sel;
  wire                              vio_pll_shift_ena; 
  wire                              pos_pll_shift_ena; 
  wire [2:0]                        phy_pll_shift;  
  wire [4:0]                        phy_pll_shift_sel;
  wire                              phy_pll_shift_ena; 
  wire                              ddr_reset; 
  wire                              sys_rst;
  wire  [2:0]                       phy_wr_pll_shift;
  wire                              idelay_ld       ;
  wire                              mpr_rdlvl_dly   ;
  wire  [7:0]                       wrlvl_dq_check  ;
  wire  [7:0]                       rd_level_dqs_check;  
  wire  [2:0]                       rdlvl_shift;  
  wire  [2:0]                       wrlvl_shift;  
  wire  [15:0]                      debug_fifo;  
  wire  [15:0]                      overflow_fifo;  
  wire  [6:0]                       init_cur_state;  
  wire  [35:0]                      ddr_debug_port;  
  wire                              app_rdy;  
  wire                              user_clk;  
  wire                              ddr_rstn;  
  wire                                phy_rddata_valid;
  wire [2*CK_RATIO*DQ_WIDTH-1:0]      phy_rd_data;
  wire [2*CK_RATIO*DATA_WIDTH-1:0]    app_rd_data_to_axi; 
  wire                                app_rd_data_end;
  wire                                app_rd_data_valid;
  wire [2*CK_RATIO*DQ_WIDTH-1:0]      rd_data;          
  wire                                rd_data_en;            
  wire                                rd_data_end;   
   
  
// Start of User Design top instance
//***************************************************************************
// The User design is instantiated below. The memory interface ports are
// connected to the top-level and the application interface ports are
// connected to the traffic generator module. This provides a reference
// for connecting the memory controller to system.
//***************************************************************************
always @(posedge user_clk or negedge sys_rst) begin
	if (!sys_rst)
		cnt <= 0;
    else if (cnt != CNT_INIT) 
        cnt <= cnt + 20'd1;
	else 
        cnt <= cnt;
end
assign ddr_rstn = (cnt == CNT_INIT);


generate
if (ASYN_AXI_CLK) begin
assign user_clk = axi_clk;

end else begin
assign user_clk = core_clk;
end
endgenerate
//***************************************************************************
// The traffic generation module instantiated below drives traffic (patterns)
// on the application interface of the memory controller
//***************************************************************************
//***************************************************************************

assign sys_rst       = pll_locked & user_pll_locked;

//***************************************************************************
ddr3_top                 u_ddr3_top
      (
      
    .axi_clk                (user_clk             ),
    .core_clk               (core_clk            ),
    .sdram_clk              (sdram_clk           ),  
    .rx_cal_clk             (rx_cal_clk          ),
    .tx_cal_clk             (tx_cal_clk          ),
    .tx_cal_clk_90edge      (tx_cal_clk_90edge   ),
    .rstn                   (ddr_rstn            ),      
    .pll_shift              (pll_shift           ),
    .pll_shift_sel          (pll_shift_sel       ),
    .pll_shift_ena          (pll_shift_ena       ), 
///////////////DDR BUS
    .ddr_ck_hi              (ddr_ck_hi           ),
    .ddr_ck_lo              (ddr_ck_lo           ),
    .ddr_cke                (ddr_cke             ),    
    .ddr_reset_n            (ddr_reset_n         ),
    .ddr_cs_n               (ddr_cs_n            ),
    .ddr_ras_n              (ddr_ras_n           ),
    .ddr_cas_n              (ddr_cas_n           ),
    .ddr_we_n               (ddr_we_n            ),     
    .ddr_addr               (ddr_addr            ),
    .ddr_ba                 (ddr_ba              ),

    .ddr_dqs_oe             (ddr_dqs_oe          ),
    .ddr_dqs_oe_n           (ddr_dqs_oe_n        ),
    .ddr_dq_oe              (ddr_dq_oe           ),
    .ddr_dqs_in_hi          (ddr_dqs_in_hi       ),
    .ddr_dqs_in_lo          (ddr_dqs_in_lo       ),
    .ddr_dq_in_hi           (ddr_dq_in_hi        ),
    .ddr_dq_in_lo           (ddr_dq_in_lo        ),

    .ddr_dqs_out_hi         (ddr_dqs_out_hi      ),
    .ddr_dqs_out_lo         (ddr_dqs_out_lo      ),
    .ddr_dq_out_hi          (ddr_dq_out_hi       ),
    .ddr_dq_out_lo          (ddr_dq_out_lo       ),
    
    .ddr_dm_hi              (ddr_dm_hi           ),
    .ddr_dm_lo              (ddr_dm_lo           ),
    .ddr_odt                (ddr_odt             ),

// Application interface ports
       .app_sr_req                     (1'b0),
       .app_ref_req                    (1'b0),
       .app_zq_req                     (1'b0),
       .app_sr_active                  (app_sr_active),
       .app_ref_ack                    (app_ref_ack),
       .app_zq_ack                     (app_zq_ack),

// Slave Interface Write Address Ports
       .s_axi_awid                     (s_axi_awid        ),
       .s_axi_awaddr                   (s_axi_awaddr      ),
       .s_axi_awlen                    (s_axi_awlen       ),
       .s_axi_awsize                   (s_axi_awsize      ),
       .s_axi_awburst                  (s_axi_awburst     ),
       .s_axi_awlock                   (s_axi_awlock      ),
       .s_axi_awcache                  (s_axi_awcache     ),
       .s_axi_awprot                   (s_axi_awprot      ),
       .s_axi_awqos                    (4'h0              ),
       .s_axi_awvalid                  (s_axi_awvalid     ),
       .s_axi_awready                  (s_axi_awready     ),
// Slave Interface Write Data Ports
       .s_axi_wdata                    (s_axi_wdata       ),
       .s_axi_wstrb                    (s_axi_wstrb       ),
       .s_axi_wlast                    (s_axi_wlast       ),
       .s_axi_wvalid                   (s_axi_wvalid      ),
       .s_axi_wready                   (s_axi_wready      ),
// Slave Interface Write Response Ports
       .s_axi_bid                      (s_axi_bid         ),
       .s_axi_bresp                    (s_axi_bresp       ),
       .s_axi_bvalid                   (s_axi_bvalid      ),
       .s_axi_bready                   (s_axi_bready      ),
// Slave Interface Read Address Ports
       .s_axi_arid                     (s_axi_arid        ),
       .s_axi_araddr                   (s_axi_araddr      ),
       .s_axi_arlen                    (s_axi_arlen       ),
       .s_axi_arsize                   (s_axi_arsize      ),
       .s_axi_arburst                  (s_axi_arburst     ),
       .s_axi_arlock                   (s_axi_arlock      ),
       .s_axi_arcache                  (s_axi_arcache     ),
       .s_axi_arprot                   (s_axi_arprot      ),
       .s_axi_arqos                    (4'h0              ),
       .s_axi_arvalid                  (s_axi_arvalid     ),
       .s_axi_arready                  (s_axi_arready     ),
// Slave Interface Read Data Ports
       .s_axi_rid                      (s_axi_rid         ),
       .s_axi_rdata                    (s_axi_rdata       ),
       .s_axi_rresp                    (s_axi_rresp       ),
       .s_axi_rlast                    (s_axi_rlast       ),
       .s_axi_rvalid                   (s_axi_rvalid      ),
       .s_axi_rready                   (s_axi_rready      ),
//DEBUG       
       .wrlvl_dq_check                 (wrlvl_dq_check    ) ,
       .rd_level_dqs_check             (rd_level_dqs_check) ,
       .rdlvl_shift                    (rdlvl_shift) ,
       .wrlvl_shift                    (wrlvl_shift) ,
       .init_cur_state                 (init_cur_state    ) ,
       .idelay_ld                      (idelay_ld         ) ,
       .mpr_rdlvl_dly                  (mpr_rdlvl_dly     ) ,
       .ddr_debug_port                 (ddr_debug_port    ) ,
       .cal_done                       (cal_done          ) 
       );
// End of User Design top instance

//***************************************************************************


//***************************************************************************
wire [15:0] apb_paddr;
 wire text_select=(apb_paddr[15:5]==11'h08a);
 wire text_ready,text_error,text_de,text_hs,text_vs,text_present_ready;
 wire [31:0] text_rdata,text_pixel_base;wire [23:0] text_rgb;
 wire wait_select=(apb_paddr[15:5]==11'h08c);
 wire wait_ready,wait_error;wire [31:0] wait_rdata;
 // Ring fetcher window 0x1a4..0x1b8 inside the graphics bank; 0x1a0 is the
 // accelerator's STREAM_DST and stays with the accelerator.
 wire ring_select=(apb_paddr[15:5]==11'h08d)&&(apb_paddr[4:2]>=3'd1)&&(apb_paddr[4:2]<=3'd6);
 wire ring_pready,ring_perror;wire [31:0] ring_prdata;
wire apb_psel,apb_penable,apb_pwrite;wire [31:0] apb_pwdata,apb_prdata;wire cpu_reset;reg [31:0] test_result;reg [3:0] cal_meta,cal_sync;
always @(posedge user_clk)begin cal_meta<={ddr_rstn,user_pll_locked,pll_locked,cal_done};cal_sync<=cal_meta;if(cpu_reset)test_result<=0;else if(apb_psel&&apb_penable&&apb_pwrite&&apb_paddr==4)test_result<=apb_pwdata;end
assign apb_prdata=apb_paddr[15:12]==3?rom_data:apb_paddr==16?{28'b0,~keys_sync}:apb_paddr==20?{28'b0,mirror_error,mirror_req,mirror_target}:wait_select?wait_rdata:text_select?text_rdata:ring_select?ring_prdata:gfx_select?gfx_rdata:scan_select?scan_rdata:apb_paddr==0?{28'b0,cal_sync}:apb_paddr==4?test_result:apb_paddr==8?ddr_debug_port[31:0]:32'h44524435;
wire scan_select=apb_paddr[15:12]==2;reg scan_enabled;wire scan_sys_rst,scan_pix_rst;
always @(posedge user_clk)begin if(cpu_reset)scan_enabled<=0;else if(apb_psel&&apb_penable&&apb_pwrite&&apb_paddr==16'h2000&&apb_pwdata[0])scan_enabled<=1;end
wire scan_arst=ddr_rstn&&cal_sync[0]&&!cpu_reset&&scan_enabled&&pixel_locked;
reset_sync srs(user_clk,scan_arst,scan_sys_rst);reset_sync prs(pixel_clk,scan_arst,scan_pix_rst);
wire request,ready,pending,present_ack,urgent,render_fault;wire [31:0] req_base,req_id,req_seq,completed_seq,front,shown,released,underflow,pending_base;wire [7:0] display_fault;
wire r5_diag_clear;wire [31:0] r5_diag_blanks,r5_diag_shown,r5_diag_new_id,r5_diag_repeat,r5_diag_dbl_rpt,r5_diag_miss,r5_diag_acks;
wire [31:0] r5_diag_drain_last,r5_diag_drain_max,r5_diag_desc_last,r5_diag_desc_max,r5_diag_pref_last,r5_diag_pref_max;
wire [23:0] scan_rgb;wire scan_de,scan_hs,scan_vs;wire [11:0] scan_x,scan_y;wire [31:0] capture_frames,capture_pixels,capture_crc,capture_ticks;
wire [31:0] scan_rdata=apb_paddr==16'h2000 ? {30'b0,pixel_locked,scan_enabled}:apb_paddr==16'h2004 ? capture_frames:apb_paddr==16'h2008 ? capture_crc:apb_paddr==16'h200c ? capture_ticks:apb_paddr==16'h2010 ? capture_pixels:apb_paddr==16'h2014 ? {24'b0,display_fault}:32'b0;
scan_crc scan_monitor(pixel_clk,scan_pix_rst,scan_de,scan_x==0&&scan_y==`GFX_HEIGHT,scan_rgb,user_clk,scan_sys_rst,capture_frames,capture_pixels,capture_crc,capture_ticks);
display_scanout #(.PRESENT_FENCE(1),.WIDTH(`GFX_WIDTH),.HEIGHT(`GFX_HEIGHT),.H_FRONT(`GFX_H_FRONT),.H_SYNC(`GFX_H_SYNC),.H_BACK(`GFX_H_BACK),.V_FRONT(`GFX_V_FRONT),.V_SYNC(`GFX_V_SYNC),.V_BACK(`GFX_V_BACK),.ALIGN_VSYNC(1),.LATE_PRESENT(`GFX_LATE_PRESENT),.PREFETCH_LINES(`GFX_PREFETCH_LINES),.MAX_READ_BEATS(`GFX_MAX_SCAN_READ_BEATS),.INITIAL_BASE(`GFX_FB0_BASE)) scanout(.clk(user_clk),.rst_n(scan_sys_rst),.pclk(pixel_clk),.prst_n(scan_pix_rst),
.request_seq(req_seq),.complete_seq(completed_seq),.render_fault(render_fault),.request(request),.request_base(req_base),.request_id(req_id),.request_ready(ready),.pending(pending),.ack(present_ack),.front_base(front),.presented_id(shown),.released_base(released),.underflows(underflow),.protected_pending_base(pending_base),.low_water(urgent),
.rgb(scan_rgb),.de(scan_de),.hsync_n(scan_hs),.vsync_n(scan_vs),.x(scan_x),.y(scan_y),.fault(display_fault),.pixel_base(text_pixel_base),
.diag_clear(r5_diag_clear),.diag_version(),.diag_blanks(r5_diag_blanks),.diag_shown(r5_diag_shown),.diag_new_id(r5_diag_new_id),.diag_repeat(r5_diag_repeat),.diag_dbl_rpt(r5_diag_dbl_rpt),.diag_miss(r5_diag_miss),.diag_acks(r5_diag_acks),
.diag_drain_last(r5_diag_drain_last),.diag_drain_max(r5_diag_drain_max),.diag_desc_last(r5_diag_desc_last),.diag_desc_max(r5_diag_desc_max),.diag_pref_last(r5_diag_pref_last),.diag_pref_max(r5_diag_pref_max),
.araddr(scan_axi_araddr),
.arlen(scan_axi_arlen),
.arsize(scan_axi_arsize),
.arburst(scan_axi_arburst),
.arvalid(scan_axi_arvalid),
.arready(scan_axi_arready),
.rdata(scan_axi_rdata),
.rresp(scan_axi_rresp),
.rlast(scan_axi_rlast),
.rvalid(scan_axi_rvalid),
.rready(scan_axi_rready));
assign scan_axi_arid=8'd2;
wire gfx_ready,gfx_error,gfx_irq;wire [31:0] gfx_rdata;
wire gfx_select=(apb_paddr[15:12]==1);wire apb_pready=wait_select?wait_ready:text_select?text_ready:ring_select?ring_pready:gfx_select?gfx_ready:1'b1;wire apb_error=wait_select?wait_error:text_select?text_error:ring_select?ring_perror:gfx_select?gfx_error:1'b0;
assign s_axi_arlock=1'b0;assign s_axi_awlock=1'b0;assign s_axi_arcache=4'b0;assign s_axi_awcache=4'b0;assign s_axi_arprot=3'b0;assign s_axi_awprot=3'b0;
wire [31:0] cpu_axi_araddr,gfx_axi_araddr,scan_axi_araddr,bus_araddr;
// The command engines and the DDR ring fetcher share fabric master 1. The
// engines keep priority; the fetcher preempts after 64 waiting cycles so a
// continuous engine stream cannot starve the descriptor stream. Once granted,
// the grant holds until the AR handshake: AXI forbids dropping an asserted
// VALID, and the engines likewise hold their requests while preempted.
wire [31:0] core_araddr,ring_araddr_i;
wire [7:0] core_arlen,ring_arlen_i,cpu_axi_arlen,gfx_axi_arlen,scan_axi_arlen,bus_arlen;
wire [2:0] core_arsize,ring_arsize_i,cpu_axi_arsize,gfx_axi_arsize,scan_axi_arsize,bus_arsize;
wire [1:0] core_arburst,ring_arburst_i,cpu_axi_arburst,gfx_axi_arburst,scan_axi_arburst,bus_arburst;
wire [7:0] cpu_axi_arid,gfx_axi_arid,scan_axi_arid,bus_arid;
wire core_arvalid,ring_arvalid_i,core_arready,ring_arready_i;
wire cpu_axi_arvalid,gfx_axi_arvalid,scan_axi_arvalid,bus_arvalid;
wire [95:0] masters_araddr;
wire [23:0] masters_arlen;
wire [8:0] masters_arsize;
wire [5:0] masters_arburst;
wire [23:0] masters_arid;
wire [2:0] masters_arvalid;
wire cpu_axi_arready,gfx_axi_arready,scan_axi_arready,bus_arready;
wire [2:0] masters_arready;
reg [6:0] ring_starve;reg ring_hold;
wire ring_grant_comb=ring_arvalid_i&&(!core_arvalid||ring_starve[6]);
wire ring_grant=ring_hold||ring_grant_comb;
always @(posedge user_clk)begin
 if(!(ddr_rstn&&!cpu_reset))begin ring_hold<=0;ring_starve<=0;end
 else begin
  if(ring_hold)begin if(gfx_axi_arready)ring_hold<=0;end
  else if(ring_grant_comb&&!gfx_axi_arready)ring_hold<=1;
  if(ring_arvalid_i&&!ring_grant)begin if(ring_starve<7'd127)ring_starve<=ring_starve+1'b1;end
  else ring_starve<=0;
 end
end
assign gfx_axi_araddr=ring_grant?ring_araddr_i:core_araddr;
assign masters_araddr={scan_axi_araddr,gfx_axi_araddr,cpu_axi_araddr};
assign s_axi_araddr=bus_araddr;
assign gfx_axi_arlen=ring_grant?ring_arlen_i:core_arlen;
assign masters_arlen={scan_axi_arlen,gfx_axi_arlen,cpu_axi_arlen};
assign s_axi_arlen=bus_arlen;
assign gfx_axi_arsize=ring_grant?ring_arsize_i:core_arsize;
assign masters_arsize={scan_axi_arsize,gfx_axi_arsize,cpu_axi_arsize};
assign s_axi_arsize=bus_arsize;
assign gfx_axi_arburst=ring_grant?ring_arburst_i:core_arburst;
assign masters_arburst={scan_axi_arburst,gfx_axi_arburst,cpu_axi_arburst};
assign s_axi_arburst=bus_arburst;
// Beat returns are demultiplexed by rid: engines read with id 1, the ring
// fetcher with id 5, exactly as the fabric matches outstanding entries.
assign gfx_axi_arid=ring_grant?8'd5:8'd1;
assign masters_arid={scan_axi_arid,gfx_axi_arid,cpu_axi_arid};
assign s_axi_arid=bus_arid;
assign gfx_axi_arvalid=ring_grant?ring_arvalid_i:core_arvalid;
assign masters_arvalid={scan_axi_arvalid,gfx_axi_arvalid,cpu_axi_arvalid};
assign s_axi_arvalid=bus_arvalid;
assign cpu_axi_arready=masters_arready[0+:1];assign gfx_axi_arready=masters_arready[1+:1];assign scan_axi_arready=masters_arready[2+:1];
assign bus_arready=s_axi_arready;
assign core_arready=gfx_axi_arready&&!ring_grant;
assign ring_arready_i=gfx_axi_arready&&ring_grant;
wire [127:0] cpu_axi_rdata,gfx_axi_rdata,scan_axi_rdata,bus_rdata;
wire [383:0] masters_rdata;
assign cpu_axi_rdata=masters_rdata[0+:128];assign gfx_axi_rdata=masters_rdata[128+:128];assign scan_axi_rdata=masters_rdata[256+:128];
assign bus_rdata=s_axi_rdata;
wire [1:0] cpu_axi_rresp,gfx_axi_rresp,scan_axi_rresp,bus_rresp;
wire [5:0] masters_rresp;
assign cpu_axi_rresp=masters_rresp[0+:2];assign gfx_axi_rresp=masters_rresp[2+:2];assign scan_axi_rresp=masters_rresp[4+:2];
assign bus_rresp=s_axi_rresp;
wire [7:0] cpu_axi_rid,gfx_axi_rid,scan_axi_rid,bus_rid;
wire [23:0] masters_rid;
assign cpu_axi_rid=masters_rid[0+:8];assign gfx_axi_rid=masters_rid[8+:8];assign scan_axi_rid=masters_rid[16+:8];
assign bus_rid=s_axi_rid;
wire cpu_axi_rlast,gfx_axi_rlast,scan_axi_rlast,bus_rlast;
wire [2:0] masters_rlast;
assign cpu_axi_rlast=masters_rlast[0+:1];assign gfx_axi_rlast=masters_rlast[1+:1];assign scan_axi_rlast=masters_rlast[2+:1];
assign bus_rlast=s_axi_rlast;
wire cpu_axi_rvalid,gfx_axi_rvalid,scan_axi_rvalid,bus_rvalid;
wire [2:0] masters_rvalid;
assign cpu_axi_rvalid=masters_rvalid[0+:1];assign gfx_axi_rvalid=masters_rvalid[1+:1];assign scan_axi_rvalid=masters_rvalid[2+:1];
assign bus_rvalid=s_axi_rvalid;
wire cpu_axi_rready,gfx_axi_rready,scan_axi_rready,bus_rready;
// Shared-port beat routing: rid 1 feeds the engines, rid 5 the ring fetcher.
// rdata/rresp/rlast are shared; each consumer only samples them under its own
// rvalid, and rready follows whichever id currently owns the returning beat.
wire [127:0] core_rdata,ring_rdata_i;
wire [1:0] core_rresp,ring_rresp_i;
wire core_rlast,ring_rlast_i,core_rvalid,ring_rvalid_i,core_rready,ring_rready_i;
wire ring_beat=gfx_axi_rvalid&&gfx_axi_rid==8'd5;
assign core_rvalid=gfx_axi_rvalid&&gfx_axi_rid==8'd1;
assign ring_rvalid_i=ring_beat;
assign core_rdata=gfx_axi_rdata;assign ring_rdata_i=gfx_axi_rdata;
assign core_rresp=gfx_axi_rresp;assign ring_rresp_i=gfx_axi_rresp;
assign core_rlast=gfx_axi_rlast;assign ring_rlast_i=gfx_axi_rlast;
wire gfx_port_rready=ring_beat?ring_rready_i:core_rready;
wire [2:0] masters_rready;
assign masters_rready={scan_axi_rready,gfx_port_rready,cpu_axi_rready};
assign s_axi_rready=bus_rready;
wire [31:0] cpu_axi_awaddr,gfx_axi_awaddr,scan_axi_awaddr,bus_awaddr;
wire [95:0] masters_awaddr;
assign masters_awaddr={32'd0,gfx_axi_awaddr,cpu_axi_awaddr};
assign s_axi_awaddr=bus_awaddr;
wire [7:0] cpu_axi_awlen,gfx_axi_awlen,scan_axi_awlen,bus_awlen;
wire [23:0] masters_awlen;
assign masters_awlen={8'd0,gfx_axi_awlen,cpu_axi_awlen};
assign s_axi_awlen=bus_awlen;
wire [2:0] cpu_axi_awsize,gfx_axi_awsize,scan_axi_awsize,bus_awsize;
wire [8:0] masters_awsize;
assign masters_awsize={3'd0,gfx_axi_awsize,cpu_axi_awsize};
assign s_axi_awsize=bus_awsize;
wire [1:0] cpu_axi_awburst,gfx_axi_awburst,scan_axi_awburst,bus_awburst;
wire [5:0] masters_awburst;
assign masters_awburst={2'd0,gfx_axi_awburst,cpu_axi_awburst};
assign s_axi_awburst=bus_awburst;
wire [7:0] cpu_axi_awid,gfx_axi_awid,scan_axi_awid,bus_awid;
wire [23:0] masters_awid;
assign masters_awid={8'd0,gfx_axi_awid,cpu_axi_awid};
assign s_axi_awid=bus_awid;
wire cpu_axi_awvalid,gfx_axi_awvalid,scan_axi_awvalid,bus_awvalid;
wire [2:0] masters_awvalid;
assign masters_awvalid={1'd0,gfx_axi_awvalid,cpu_axi_awvalid};
assign s_axi_awvalid=bus_awvalid;
wire cpu_axi_awready,gfx_axi_awready,scan_axi_awready,bus_awready;
wire [2:0] masters_awready;
assign cpu_axi_awready=masters_awready[0+:1];assign gfx_axi_awready=masters_awready[1+:1];assign scan_axi_awready=masters_awready[2+:1];
assign bus_awready=s_axi_awready;
wire [127:0] cpu_axi_wdata,gfx_axi_wdata,scan_axi_wdata,bus_wdata;
wire [383:0] masters_wdata;
assign masters_wdata={128'd0,gfx_axi_wdata,cpu_axi_wdata};
assign s_axi_wdata=bus_wdata;
wire [15:0] cpu_axi_wstrb,gfx_axi_wstrb,scan_axi_wstrb,bus_wstrb;
wire [47:0] masters_wstrb;
assign masters_wstrb={16'd0,gfx_axi_wstrb,cpu_axi_wstrb};
assign s_axi_wstrb=bus_wstrb;
wire cpu_axi_wlast,gfx_axi_wlast,scan_axi_wlast,bus_wlast;
wire [2:0] masters_wlast;
assign masters_wlast={1'd0,gfx_axi_wlast,cpu_axi_wlast};
assign s_axi_wlast=bus_wlast;
wire cpu_axi_wvalid,gfx_axi_wvalid,scan_axi_wvalid,bus_wvalid;
wire [2:0] masters_wvalid;
assign masters_wvalid={1'd0,gfx_axi_wvalid,cpu_axi_wvalid};
assign s_axi_wvalid=bus_wvalid;
wire cpu_axi_wready,gfx_axi_wready,scan_axi_wready,bus_wready;
wire [2:0] masters_wready;
assign cpu_axi_wready=masters_wready[0+:1];assign gfx_axi_wready=masters_wready[1+:1];assign scan_axi_wready=masters_wready[2+:1];
assign bus_wready=s_axi_wready;
wire [1:0] cpu_axi_bresp,gfx_axi_bresp,scan_axi_bresp,bus_bresp;
wire [5:0] masters_bresp;
assign cpu_axi_bresp=masters_bresp[0+:2];assign gfx_axi_bresp=masters_bresp[2+:2];assign scan_axi_bresp=masters_bresp[4+:2];
assign bus_bresp=s_axi_bresp;
wire [7:0] cpu_axi_bid,gfx_axi_bid,scan_axi_bid,bus_bid;
wire [23:0] masters_bid;
assign cpu_axi_bid=masters_bid[0+:8];assign gfx_axi_bid=masters_bid[8+:8];assign scan_axi_bid=masters_bid[16+:8];
assign bus_bid=s_axi_bid;
wire cpu_axi_bvalid,gfx_axi_bvalid,scan_axi_bvalid,bus_bvalid;
wire [2:0] masters_bvalid;
assign cpu_axi_bvalid=masters_bvalid[0+:1];assign gfx_axi_bvalid=masters_bvalid[1+:1];assign scan_axi_bvalid=masters_bvalid[2+:1];
assign bus_bvalid=s_axi_bvalid;
wire cpu_axi_bready,gfx_axi_bready,scan_axi_bready,bus_bready;
wire [2:0] masters_bready;
assign masters_bready={1'd0,gfx_axi_bready,cpu_axi_bready};
assign s_axi_bready=bus_bready;
assign gfx_axi_awid=8'd1;
wire ring_push_i,ring_push_ready_i,ring_fault_i,queue_clear_i;wire [7:0] ring_fault_code_i;wire [351:0] ring_push_data_i;
gfx_ring_fetch #(.ENTRIES(4096)) ring_fetcher(.clk(user_clk),.rst_n(ddr_rstn&&cal_sync[0]&&!cpu_reset),
.psel(apb_psel&&ring_select),.penable(apb_penable),.pwrite(apb_pwrite),.paddr(apb_paddr[4:0]),.pwdata(apb_pwdata),
.pready(ring_pready),.prdata(ring_prdata),.pslverr(ring_perror),
.araddr(ring_araddr_i),.arlen(ring_arlen_i),.arsize(ring_arsize_i),.arburst(ring_arburst_i),.arvalid(ring_arvalid_i),.arready(ring_arready_i),
.rdata(ring_rdata_i),.rresp(ring_rresp_i),.rlast(ring_rlast_i),.rvalid(ring_rvalid_i),.rready(ring_rready_i),
.push_valid(ring_push_i),.push_ready(ring_push_ready_i),.push_data(ring_push_data_i),
.fault_valid(ring_fault_i),.fault_code(ring_fault_code_i),.queue_clear(queue_clear_i),
.pool_start(32'h01000000),.pool_end(32'h02400000),
.protect0_start(scan_enabled?front:32'd0),.protect0_end(scan_enabled?front+`GFX_FRAME_BYTES:32'd0),
.protect1_start(pending?pending_base:32'd0),.protect1_end(pending?pending_base+`GFX_FRAME_BYTES:32'd0));
gfx_core #(.QUEUE_DEPTH(1024),.PRESENT_FENCE(1),.STREAM_SUBMIT(1),.RING_FETCH(1),.FRAME_BYTES(`GFX_FRAME_BYTES),.CAPS_EXTRA(384),.MAX_FILL_BEATS(`GFX_MAX_FILL_BEATS),.HAS_BUCKET(1'b0),.COPY_STREAM(1),.SMALL_PIPE(1),.SPRITE_ROWS(3),.FRAME_TRACE(1)) accelerator(.clk(user_clk),.rst_n(ddr_rstn&&cal_sync[0]&&!cpu_reset),.psel(apb_psel&&gfx_select&&!text_select&&!wait_select&&!ring_select),.penable(apb_penable),.pwrite(apb_pwrite),.paddr(apb_paddr[11:0]),.pwdata(apb_pwdata),.pready(gfx_ready),.prdata(gfx_rdata),.pslverr(gfx_error),.irq(gfx_irq),.display_urgent(urgent),
.ring_push_valid(ring_push_i),.ring_push_ready(ring_push_ready_i),.ring_push_data(ring_push_data_i),
.ring_fault_valid(ring_fault_i),.ring_fault_code(ring_fault_code_i),.queue_clear(queue_clear_i),
.pool_start(32'h01000000),.pool_end(32'h02400000),.protect0_start(scan_enabled?front:32'd0),.protect0_end(scan_enabled?front+`GFX_FRAME_BYTES:32'd0),.protect1_start(pending?pending_base:32'd0),.protect1_end(pending?pending_base+`GFX_FRAME_BYTES:32'd0),
.display_seq(req_seq),.completed_seq(completed_seq),.render_fault(render_fault),.display_request(request),.display_base(req_base),.display_id(req_id),.display_ready(scan_enabled&&ready&&text_present_ready),.display_pending(pending),.display_ack(present_ack),.front_base(front),.presented_id(shown),.released_base(released),.underflow(underflow),.display_fault(display_fault),
.diag_blanks(r5_diag_blanks),.diag_shown(r5_diag_shown),.diag_new_id(r5_diag_new_id),.diag_repeat(r5_diag_repeat),.diag_dbl_rpt(r5_diag_dbl_rpt),.diag_miss(r5_diag_miss),.diag_acks(r5_diag_acks),
.diag_drain_last(r5_diag_drain_last),.diag_drain_max(r5_diag_drain_max),.diag_desc_last(r5_diag_desc_last),.diag_desc_max(r5_diag_desc_max),.diag_pref_last(r5_diag_pref_last),.diag_pref_max(r5_diag_pref_max),.diag_clear(r5_diag_clear),
.araddr(core_araddr),
.arlen(core_arlen),
.arsize(core_arsize),
.arburst(core_arburst),
.arvalid(core_arvalid),
.arready(core_arready),
.rdata(core_rdata),
.rresp(core_rresp),
.rlast(core_rlast),
.rvalid(core_rvalid),
.rready(core_rready),
.awaddr(gfx_axi_awaddr),
.awlen(gfx_axi_awlen),
.awsize(gfx_axi_awsize),
.awburst(gfx_axi_awburst),
.awvalid(gfx_axi_awvalid),
.awready(gfx_axi_awready),
.wdata(gfx_axi_wdata),
.wstrb(gfx_axi_wstrb),
.wlast(gfx_axi_wlast),
.wvalid(gfx_axi_wvalid),
.wready(gfx_axi_wready),
.bresp(gfx_axi_bresp),
.bvalid(gfx_axi_bvalid),
.bready(gfx_axi_bready));
axi_fabric arbiter(.clk(user_clk),.rst_n(ddr_rstn&&!cpu_reset),.display_urgent(urgent),
.s_araddr(masters_araddr),.m_araddr(bus_araddr),
.s_arlen(masters_arlen),.m_arlen(bus_arlen),
.s_arsize(masters_arsize),.m_arsize(bus_arsize),
.s_arburst(masters_arburst),.m_arburst(bus_arburst),
.s_arid(masters_arid),.m_arid(bus_arid),
.s_arvalid(masters_arvalid),.m_arvalid(bus_arvalid),
.s_arready(masters_arready),.m_arready(bus_arready),
.s_rdata(masters_rdata),.m_rdata(bus_rdata),
.s_rresp(masters_rresp),.m_rresp(bus_rresp),
.s_rid(masters_rid),.m_rid(bus_rid),
.s_rlast(masters_rlast),.m_rlast(bus_rlast),
.s_rvalid(masters_rvalid),.m_rvalid(bus_rvalid),
.s_rready(masters_rready),.m_rready(bus_rready),
.s_awaddr(masters_awaddr),.m_awaddr(bus_awaddr),
.s_awlen(masters_awlen),.m_awlen(bus_awlen),
.s_awsize(masters_awsize),.m_awsize(bus_awsize),
.s_awburst(masters_awburst),.m_awburst(bus_awburst),
.s_awid(masters_awid),.m_awid(bus_awid),
.s_awvalid(masters_awvalid),.m_awvalid(bus_awvalid),
.s_awready(masters_awready),.m_awready(bus_awready),
.s_wdata(masters_wdata),.m_wdata(bus_wdata),
.s_wstrb(masters_wstrb),.m_wstrb(bus_wstrb),
.s_wlast(masters_wlast),.m_wlast(bus_wlast),
.s_wvalid(masters_wvalid),.m_wvalid(bus_wvalid),
.s_wready(masters_wready),.m_wready(bus_wready),
.s_bresp(masters_bresp),.m_bresp(bus_bresp),
.s_bid(masters_bid),.m_bid(bus_bid),
.s_bvalid(masters_bvalid),.m_bvalid(bus_bvalid),
.s_bready(masters_bready),.m_bready(bus_bready));

soc u_sapphire_soc(
.io_systemClk(user_clk),
.io_ddrA_w_payload_strb(cpu_axi_wstrb),
.io_ddrA_w_payload_data(cpu_axi_wdata),
.axiA_awready('b0),
.axiA_awlen(),
.axiA_awsize(),
.axiA_arburst(),
.axiA_awlock(),
.axiA_arcache(),
.axiA_awqos(),
.axiA_awprot(),
.axiA_arsize(),
.axiA_arregion(),
.axiA_arready('b0),
.axiA_arqos(),
.axiA_arprot(),
.axiA_arlock(),
.axiA_arlen(),
.axiA_arid(),
.axiA_awcache(),
.axiA_awburst(),
.axiA_awaddr(),
.axiAInterrupt('b0),
.axiA_rlast('b0),
.jtagCtrl_enable(jtag_inst1_SEL),
.jtagCtrl_tdi(jtag_inst1_TDI),
.jtagCtrl_capture(jtag_inst1_CAPTURE),
.jtagCtrl_shift(jtag_inst1_SHIFT),
.jtagCtrl_update(jtag_inst1_UPDATE),
.jtagCtrl_reset(jtag_inst1_RESET),
.jtagCtrl_tdo(jtag_inst1_TDO),
.jtagCtrl_tck(jtag_inst1_TCK),
.axiA_araddr(),
.axiA_wvalid(),
.axiA_wready('b0),
.axiA_wdata(),
.axiA_wstrb(),
.axiA_wlast(),
.axiA_bvalid('b0),
.axiA_bready(),
.axiA_bid('b0),
.axiA_bresp('b0),
.axiA_rvalid('b0),
.axiA_rready(),
.axiA_rdata('b0),
.axiA_rid('b0),
.axiA_rresp('b0),
.axiA_arvalid(),
.axiA_awid(),
.axiA_awregion(),
.axiA_awvalid(),
.io_ddrA_r_payload_last(cpu_axi_rlast),
.io_ddrA_r_payload_resp(cpu_axi_rresp),
.io_ddrA_r_payload_id(cpu_axi_rid),
.io_ddrA_r_payload_data(cpu_axi_rdata),
.io_ddrA_r_ready(cpu_axi_rready),
.io_ddrA_r_valid(cpu_axi_rvalid),
.io_ddrA_b_payload_resp(cpu_axi_bresp),
.io_ddrA_b_payload_id(cpu_axi_bid),
.io_ddrA_b_ready(cpu_axi_bready),
.io_ddrA_b_valid(cpu_axi_bvalid),
.io_ddrA_w_payload_last(cpu_axi_wlast),
.io_ddrA_w_ready(cpu_axi_wready),
.io_ddrA_w_valid(cpu_axi_wvalid),
.io_ddrA_aw_payload_prot(),
.io_ddrA_aw_payload_qos(),
.io_ddrA_aw_payload_cache(),
.io_ddrA_aw_payload_lock(),
.io_ddrA_aw_payload_burst(cpu_axi_awburst),
.io_ddrA_aw_payload_size(cpu_axi_awsize),
.io_ddrA_aw_payload_len(cpu_axi_awlen),
.io_ddrA_aw_payload_region(),
.io_ddrA_aw_payload_id(cpu_axi_awid),
.io_ddrA_aw_payload_addr(cpu_axi_awaddr),
.io_ddrA_aw_ready(cpu_axi_awready),
.io_ddrA_aw_valid(cpu_axi_awvalid),
.io_ddrA_ar_payload_prot(),
.io_ddrA_ar_payload_qos(),
.io_ddrA_ar_payload_cache(),
.io_ddrA_ar_payload_lock(),
.io_ddrA_ar_payload_burst(cpu_axi_arburst),
.io_ddrA_ar_payload_size(cpu_axi_arsize),
.io_ddrA_ar_payload_len(cpu_axi_arlen),
.io_ddrA_ar_payload_region(),
.io_ddrA_ar_payload_id(cpu_axi_arid),
.io_ddrA_ar_payload_addr(cpu_axi_araddr),
.io_ddrA_ar_ready(cpu_axi_arready),
.io_ddrA_ar_valid(cpu_axi_arvalid),
.userInterruptA(gfx_irq),
.io_apbSlave_0_PADDR(apb_paddr),
.io_apbSlave_0_PENABLE(apb_penable),
.io_apbSlave_0_PRDATA(apb_prdata),
.io_apbSlave_0_PREADY(apb_pready),
.io_apbSlave_0_PSEL(apb_psel),
.io_apbSlave_0_PSLVERROR(apb_error),
.io_apbSlave_0_PWDATA(apb_pwdata),
.io_apbSlave_0_PWRITE(apb_pwrite),
.io_asyncReset(!sys_rst),
.io_memoryClk(user_clk),
.io_systemReset(cpu_reset),
.system_uart_0_io_txd(system_uart_0_io_txd),
.io_memoryReset(),
.system_uart_0_io_rxd(system_uart_0_io_rxd),
.system_gpio_0_io_writeEnable(),
.io_ddrMasters_0_reset(),
.io_ddrMasters_0_clk(user_clk),
.io_ddrMasters_0_r_payload_last(),
.io_ddrMasters_0_r_payload_resp(),
.io_ddrMasters_0_r_payload_id(),
.io_ddrMasters_0_r_payload_data(),
.io_ddrMasters_0_r_ready('b0),
.io_ddrMasters_0_r_valid(),
.io_ddrMasters_0_ar_payload_prot('b0),
.io_ddrMasters_0_ar_payload_qos('b0),
.io_ddrMasters_0_ar_payload_cache('b0),
.io_ddrMasters_0_ar_payload_lock('b0),
.io_ddrMasters_0_ar_payload_burst('b0),
.io_ddrMasters_0_ar_payload_size('b0),
.io_ddrMasters_0_ar_payload_len('b0),
.io_ddrMasters_0_ar_payload_region('b0),
.io_ddrMasters_0_ar_payload_id('b0),
.io_ddrMasters_0_ar_payload_addr('b0),
.io_ddrMasters_0_ar_ready(),
.io_ddrMasters_0_ar_valid('b0),
.io_ddrMasters_0_b_payload_resp(),
.io_ddrMasters_0_b_payload_id(),
.io_ddrMasters_0_b_ready('b0),
.io_ddrMasters_0_b_valid(),
.io_ddrMasters_0_w_payload_last('b0),
.io_ddrMasters_0_w_payload_strb('b0),
.io_ddrMasters_0_w_payload_data('b0),
.io_ddrMasters_0_w_ready(),
.io_ddrMasters_0_w_valid('b0),
.io_ddrMasters_0_aw_payload_prot('b0),
.io_ddrMasters_0_aw_payload_qos('b0),
.io_ddrMasters_0_aw_payload_cache('b0),
.io_ddrMasters_0_aw_payload_lock('b0),
.io_ddrMasters_0_aw_payload_burst('b0),
.io_ddrMasters_0_aw_payload_size('b0),
.io_ddrMasters_0_aw_payload_len('b0),
.io_ddrMasters_0_aw_payload_region('b0),
.io_ddrMasters_0_aw_payload_id('b0),
.io_ddrMasters_0_aw_payload_addr('b0),
.io_ddrMasters_0_aw_ready(),
.io_ddrMasters_0_aw_valid('b0),
.system_gpio_0_io_write(),
.system_gpio_0_io_read(1)
);
//***************************************************************************


//***************************************************************************


 (* async_reg="true" *)reg [3:0] keys_meta,keys_sync;
 always @(posedge user_clk)begin
  if(cpu_reset)begin keys_meta<=15;keys_sync<=15;end
  else begin keys_meta<=key_n;keys_sync<=keys_meta;end
 end
 wire [1:0] mirror_target;wire mirror_req,mirror_error;
 mirror_reconfig mirror_control(
  .user_clk(user_clk),.user_rst_n(ddr_rstn&&cal_sync[0]&&!cpu_reset),
  .cfg_clk(clk_25m),.cfg_arst_n(ddr_rstn&&!cpu_reset),
  .write_valid(apb_psel&&apb_penable&&apb_pwrite&&apb_paddr==20),
  .write_data(apb_pwdata[2:0]),.cfg_error(cfg_ERROR),
  .staged_slot(mirror_target),.requested(mirror_req),.error_user(mirror_error),
  .cfg_cbsel(cfg_CBSEL),.cfg_ena(cfg_ENA),.cfg_config(cfg_CONFIG));
 wire [31:0] rom_data;
 firmware_rom firmware(user_clk,!cpu_reset,apb_psel&&apb_paddr[15:12]==3,apb_penable,apb_pwrite,apb_paddr[11:0],apb_pwdata,rom_data);
 wire [9:0] tx_clk,tx_data0,tx_data1,tx_data2;
 gfx_event_wait event_wait(user_clk,ddr_rstn&&cal_sync[0]&&!cpu_reset,apb_psel&&wait_select,apb_penable,apb_pwrite,apb_paddr[11:0],apb_pwdata,wait_ready,wait_rdata,wait_error,gfx_irq);
 gfx_text_overlay #(.BANKS(3)) text_layer(.clk(user_clk),.rst_n(scan_sys_rst),.pclk(pixel_clk),.prst_n(scan_pix_rst),
  .psel(apb_psel&&text_select),.penable(apb_penable),.pwrite(apb_pwrite),.paddr(apb_paddr[11:0]),.pwdata(apb_pwdata),.pready(text_ready),.prdata(text_rdata),.pslverr(text_error),
  .front_base(front),.pending_base(pending_base),.request_base(req_base),.pending(pending),.request(request),
  .pixel_base(text_pixel_base),.x(scan_x),.y(scan_y),.rgb(scan_rgb),.de(scan_de),.hs(scan_hs),.vs(scan_vs),
  .out_rgb(text_rgb),.out_de(text_de),.out_hs(text_hs),.out_vs(text_vs),.present_ready(text_present_ready));
 dvi_encoder tx(.pixelclk(pixel_clk),.rstin(!scan_pix_rst),.blue_din(text_rgb[7:0]),.green_din(text_rgb[15:8]),.red_din(text_rgb[23:16]),
  .hsync(text_hs),.vsync(~text_vs),.de(text_de),.tmds_clk(tx_clk),.tmds_data0(tx_data0),.tmds_data1(tx_data1),.tmds_data2(tx_data2));
assign tmds_clk_o=~tx_clk;assign tmds_clk_TX_OE=1'b1;assign tmds_clk_TX_RST=1'b0;
assign tmds_data0_o=~tx_data0;assign tmds_data0_TX_OE=1'b1;assign tmds_data0_TX_RST=1'b0;
assign tmds_data1_o=~tx_data1;assign tmds_data1_TX_OE=1'b1;assign tmds_data1_TX_RST=1'b0;
assign tmds_data2_o=~tx_data2;assign tmds_data2_TX_OE=1'b1;assign tmds_data2_TX_RST=1'b0;

endmodule
