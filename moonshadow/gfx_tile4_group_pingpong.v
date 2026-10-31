// Two-bank 128x16-pixel staging buffer for the Tile4 compositor.
//
// One bank accepts eight adjacent 16x16 tiles while the other streams a
// completed group to DDR as one 1..32-beat burst per raster row.  The banks
// alternate in commit order, so composition and DDR response latency overlap
// without changing pixel order.
`default_nettype none
module gfx_tile4_group_pingpong(
 input wire clk,rst_n,input wire flush,
 input wire begin_group,output wire group_ready,
 input wire [31:0] group_x,input wire [31:0] group_y,
 input wire [7:0] group_width,input wire [4:0] group_height,
 input wire tile_write_valid,output wire tile_write_ready,
 input wire [8:0] tile_write_index,input wire [127:0] tile_write_pixels,
 input wire group_end,
 output wire row_valid,input wire row_ready,output wire [127:0] row_pixels,
 output reg [15:0] row_strb,output wire [8:0] row_beats,
 output wire [31:0] row_x,output wire [31:0] row_y,
 output wire row_group_last,output wire busy
);
 localparam D_IDLE=0,D_REQ=1,D_SEND=2;
 reg [1:0] drain_state;
 reg [1:0] bank_full;
 reg fill_active,fill_sel,drain_sel;
 reg [31:0] bank_x0,bank_x1,bank_y0,bank_y1;
 reg [7:0] bank_w0,bank_w1;
 reg [4:0] bank_h0,bank_h1;
 reg [4:0] drain_row;reg [5:0] drain_beat;
 reg [8:0] ram_raddr_hold;

 wire [7:0] drain_width=drain_sel?bank_w1:bank_w0;
 wire [4:0] drain_height=drain_sel?bank_h1:bank_h0;
 wire [5:0] beats_per_row=(drain_width+3)>>2;
 wire beat_last=drain_beat+1'b1>=beats_per_row;
 wire group_last=drain_row+1'b1>=drain_height;
 wire output_advance=drain_state==D_SEND&&row_ready;
 wire [8:0] next_raddr=beat_last?{drain_row+1'b1,5'b0}:ram_raddr_hold+1'b1;
 // Keep prefetching throughout the final row.  Only its final beat has no
 // successor; suppressing prefetch for the whole row makes synchronous RAM
 // data lag one beat and corrupts non-uniform pixels in that row.
 wire [8:0] ram_raddr=output_advance&&!(beat_last&&group_last)?next_raddr:ram_raddr_hold;
 wire [127:0] ram_rdata0,ram_rdata1;
 wire ram_we0=fill_active&&!fill_sel&&tile_write_valid&&tile_write_ready;
 wire ram_we1=fill_active&&fill_sel&&tile_write_valid&&tile_write_ready;

 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(9),.OUTPUT_REG("TRUE")) bank0(
  .wdata(tile_write_pixels),.waddr(tile_write_index),.raddr(ram_raddr),.we(ram_we0),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(ram_rdata0));
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(9),.OUTPUT_REG("TRUE")) bank1(
  .wdata(tile_write_pixels),.waddr(tile_write_index),.raddr(ram_raddr),.we(ram_we1),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(ram_rdata1));

 assign group_ready=!fill_active&&!bank_full[fill_sel];
 assign tile_write_ready=fill_active;
 assign row_valid=drain_state==D_SEND;
 assign row_pixels=drain_sel?ram_rdata1:ram_rdata0;
 assign row_beats={3'b0,beats_per_row};
 assign row_x=drain_sel?bank_x1:bank_x0;
 assign row_y=(drain_sel?bank_y1:bank_y0)+drain_row;
 assign row_group_last=row_valid&&beat_last&&group_last;
 assign busy=fill_active||(|bank_full)||(drain_state!=D_IDLE);

 integer lane;reg [8:0] pixel_position;
 always @*begin
  row_strb=0;pixel_position=0;
  for(lane=0;lane<4;lane=lane+1)begin
   pixel_position={1'b0,drain_beat,2'b0}+lane;
   if(pixel_position<drain_width)row_strb[lane*4+:4]=4'hf;
  end
 end

 always @(posedge clk)begin
  if(!rst_n)begin
   drain_state<=D_IDLE;bank_full<=0;fill_active<=0;fill_sel<=0;drain_sel<=0;
   bank_x0<=0;bank_x1<=0;bank_y0<=0;bank_y1<=0;bank_w0<=0;bank_w1<=0;
   bank_h0<=0;bank_h1<=0;drain_row<=0;drain_beat<=0;ram_raddr_hold<=0;
  end else if(flush)begin
   drain_state<=D_IDLE;bank_full<=0;fill_active<=0;fill_sel<=0;drain_sel<=0;
   drain_row<=0;drain_beat<=0;ram_raddr_hold<=0;
  end else begin
   if(begin_group&&group_ready)begin
    fill_active<=1;
    if(fill_sel==0)begin bank_x0<=group_x;bank_y0<=group_y;bank_w0<=group_width;bank_h0<=group_height;end
    else begin bank_x1<=group_x;bank_y1<=group_y;bank_w1<=group_width;bank_h1<=group_height;end
   end
   if(group_end&&fill_active)begin
    bank_full[fill_sel]<=1;fill_active<=0;fill_sel<=!fill_sel;
   end
   case(drain_state)
    D_IDLE:if(bank_full[drain_sel])begin
     drain_row<=0;drain_beat<=0;ram_raddr_hold<=0;drain_state<=D_REQ;
    end
    D_REQ:drain_state<=D_SEND;
    D_SEND:if(row_valid&&row_ready)begin
     if(beat_last)begin
      if(group_last)begin
       bank_full[drain_sel]<=0;drain_sel<=!drain_sel;drain_state<=D_IDLE;
      end else begin
       drain_row<=drain_row+1'b1;drain_beat<=0;
       ram_raddr_hold<={drain_row+1'b1,5'b0};drain_state<=D_REQ;
      end
     end else begin
      drain_beat<=drain_beat+1'b1;ram_raddr_hold<=ram_raddr_hold+1'b1;
     end
    end
    default:drain_state<=D_IDLE;
   endcase
  end
 end
endmodule
`default_nettype wire
