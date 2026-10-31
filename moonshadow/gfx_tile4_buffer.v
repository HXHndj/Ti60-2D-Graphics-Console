// Storage-only 16x16x32-bit Tile Buffer. Pixel arithmetic stays outside this
// module so the Tile4 controller can reuse gfx_engine's existing four blend
// lanes instead of duplicating scarce DSPs.
module gfx_tile4_buffer #(parameter LAZY_CLEAR=1)(
 input wire clk,rst_n,
 input wire tile_begin,input wire skip_clear,output wire tile_ready,input wire [31:0] clear_pixel,
 input wire [4:0] tile_width,tile_height,
 input wire read_valid,output wire read_ready,input wire [5:0] read_index,
 output reg response_valid,input wire response_ready,
 output wire [5:0] response_index,output wire [127:0] response_pixels,
 input wire write_valid,output wire write_ready,input wire [5:0] write_index,
 input wire [127:0] write_pixels,input wire tile_end,
 output wire out_valid,input wire out_ready,output wire [127:0] out_pixels,
 output reg [3:0] out_keep,output wire out_last,output wire busy
);
 localparam IDLE=0,CLEAR=1,COMPOSE=2,OUT_REQ=3,OUT_SEND=4;
 reg [2:0] state;reg [5:0] clear_index,out_index,response_index_reg,pending_index;
 reg read_pending;
 reg [4:0] active_width,active_height;reg [5:0] ram_raddr_hold;
 reg [31:0] active_clear;reg [63:0] valid_groups;
 wire [127:0] ram_rdata;
 wire clear_write=state==CLEAR;
 wire ram_write=clear_write||(state==COMPOSE&&write_valid&&write_ready);
 wire [5:0] ram_waddr=clear_write?clear_index:write_index;
 wire [127:0] ram_wdata=clear_write?{4{clear_pixel}}:write_pixels;
 // The RAM output is registered. On an accepted output beat, present the
 // next address before the active edge so the RAM produces the next beat
 // immediately after that edge. When stalled, use the held current address
 // and keep both data and sideband signals stable.
 wire output_advance=state==OUT_SEND&&out_ready&&!out_last;
 wire [5:0] ram_raddr=output_advance?(out_index+1'b1):ram_raddr_hold;
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(6),.OUTPUT_REG("TRUE")) tile_ram(
  .wdata(ram_wdata),.waddr(ram_waddr),.raddr(ram_raddr),.we(ram_write),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(ram_rdata));

 assign tile_ready=state==IDLE;
 assign read_ready=state==COMPOSE&&!read_pending&&(!response_valid||response_ready)&&!tile_end;
 assign response_index=response_index_reg;
 // Lazy clear gives every group the command clear colour logically, without
 // spending 64 clocks writing that same value into RAM. A group becomes
 // physically valid on its first composition write. Reads and final output
 // substitute the held clear colour until then.
 assign response_pixels=LAZY_CLEAR&&!valid_groups[response_index_reg]?{4{active_clear}}:ram_rdata;
 assign write_ready=state==COMPOSE;
 assign out_valid=state==OUT_SEND;
 assign out_pixels=LAZY_CLEAR&&!valid_groups[out_index]?{4{active_clear}}:ram_rdata;
 // Edge tiles emit only their valid rows. Each row is still four 128-bit
 // beats; out_keep masks the unused pixels in the final column.
 assign out_last=state==OUT_SEND&&out_index==({1'b0,active_height,2'b0}-1'b1);
 assign busy=state!=IDLE;

 integer lane;reg [6:0] x_position;
 always @*begin
  out_keep=0;x_position=0;
  for(lane=0;lane<4;lane=lane+1)begin
   x_position={3'b0,out_index[1:0],2'b0}+lane;
   if((out_index>>2)<active_height&&x_position<active_width)out_keep[lane]=1'b1;
  end
 end

 always @(posedge clk)begin
  if(!rst_n)begin
   state<=IDLE;clear_index<=0;out_index<=0;response_valid<=0;read_pending<=0;
   response_index_reg<=0;pending_index<=0;active_width<=0;active_height<=0;ram_raddr_hold<=0;
   active_clear<=0;valid_groups<=0;
  end else begin
   if(response_valid&&response_ready)response_valid<=0;
   if(read_pending)begin
    response_valid<=1;response_index_reg<=pending_index;read_pending<=0;
   end
   case(state)
    IDLE:if(tile_begin)begin
     state<=(skip_clear||LAZY_CLEAR)?COMPOSE:CLEAR;clear_index<=0;active_width<=tile_width;active_height<=tile_height;
     active_clear<=clear_pixel;valid_groups<=0;
    end
    CLEAR:begin
     if(clear_index==63)state<=COMPOSE;else clear_index<=clear_index+1'b1;
    end
    COMPOSE:begin
     if(write_valid&&write_ready)valid_groups[write_index]<=1'b1;
     if(read_valid&&read_ready)begin
      ram_raddr_hold<=read_index;pending_index<=read_index;read_pending<=1;
     end
     if(tile_end&&!read_valid&&!read_pending&&!response_valid)begin
      state<=OUT_REQ;out_index<=0;ram_raddr_hold<=0;
     end
    end
    OUT_REQ:state<=OUT_SEND;
    OUT_SEND:if(out_valid&&out_ready)begin
     if(out_index==({1'b0,active_height,2'b0}-1'b1))state<=IDLE;
     else begin out_index<=out_index+1'b1;ram_raddr_hold<=out_index+1'b1;end
    end
    default:state<=IDLE;
   endcase
  end
 end
endmodule
