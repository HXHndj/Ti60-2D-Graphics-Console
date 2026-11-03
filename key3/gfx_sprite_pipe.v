`include "gfx_abi.vh"
// Ordered 16-pixel sprites. Two row slots decouple source/background reads
// from blend/writeback; a later command cannot observe an unfinished B.
module gfx_sprite_pipe #(parameter TIMEOUT_CYCLES=10000000,SHARED_PIXEL_PIPE=1,ROW_SLOTS=2)(
 input wire clk,rst_n,cmd_valid,output wire cmd_ready,input wire [`GFX_CMD_BITS-1:0] cmd,
 output reg done,output reg [31:0] done_seq,output reg [7:0] fault,output reg [31:0] fault_addr,
 output wire busy,recover_safe,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire [31:0] awaddr,output wire [7:0] awlen,output wire [2:0] awsize,output wire [1:0] awburst,
 output wire awvalid,input wire awready,output wire [127:0] wdata,output wire [15:0] wstrb,
 output wire wlast,wvalid,input wire wready,input wire [1:0] bresp,input wire bvalid,output wire bready,
 input wire opaque_enable,
 output wire src_req_valid,input wire src_req_ready,output wire [31:0] src_req_addr,output wire [4:0] src_req_beats,
 input wire [127:0] rd_data,input wire rd_valid,rd_done,input wire [7:0] rd_error,input wire src_busy,cache_serving,
 input wire [31:0] src_araddr,input wire [7:0] src_arlen,input wire [2:0] src_arsize,input wire [1:0] src_arburst,
 input wire src_arvalid,output wire src_arready,output wire src_rbeat_valid,input wire src_rready,
 output wire pixel_rst_n,pixel_in_valid,pixel_out_ready,pixel_in_last,
 output wire [127:0] pixel_src,pixel_dst,output wire [31:0] pixel_color,pixel_op,
 output wire [7:0] pixel_alpha,output wire [23:0] pixel_key,
 input wire [3:0] pixel_ready,pixel_valid,pixel_write,pixel_last,input wire [127:0] pixel_result,
 output reg [31:0] bg_bypass_total,
 input wire bounds_flush,bounds_write,input wire [31:0] bounds_write_addr,input wire [7:0] bounds_write_len
);
 localparam L_IDLE=0,L_SOURCE=1,L_SOURCE_WAIT=2,L_BG=3,L_BG_WAIT=4,L_ABORT=5;
 localparam W_IDLE=0,W_PIXELS=1,W_NEXT=2;
 reg active;reg [2:0] load_state;reg [1:0] write_state;
 reg [31:0] op,src,dst,height,alpha,key,seq;
 reg opaque_cmd;
 reg [5:0] load_row,write_row;
 localparam BANK_BITS=ROW_SLOTS<=2?1:$clog2(ROW_SLOTS);
 reg [BANK_BITS-1:0] load_bank,write_bank;reg [ROW_SLOTS-1:0] owned,ready;
 wire [BANK_BITS-1:0] next_load_bank=load_bank==ROW_SLOTS-1?0:load_bank+1'b1;
 wire [BANK_BITS-1:0] next_write_bank=write_bank==ROW_SLOTS-1?0:write_bank+1'b1;
 reg [127:0] source_rows[0:ROW_SLOTS*4-1];reg [127:0] background_rows[0:ROW_SLOTS*5-1];
 reg [31:0] bank_addr[0:ROW_SLOTS-1];reg [2:0] bank_beats[0:ROW_SLOTS-1];reg [1:0] bank_lane[0:ROW_SLOTS-1];
 reg [ROW_SLOTS-1:0] bank_opaque,bank_skip;
 reg [2:0] source_count,bg_count;
 reg [2:0] row_first,row_last;
 wire [31:0] load_src=src+{20'b0,load_row,6'b0};
 reg [4:0] source_first,source_end,beat_source_first,beat_source_end;
 reg [511:0] bounds_valid;reg [16:0] bounds_window;reg [23:0] bounds_key;
 reg [9:0] bounds_ram[0:511];reg [9:0] hint_data;reg hint_valid;
 wire [32:0] bounds_write_end={1'b0,bounds_write_addr}+(({25'b0,bounds_write_len}+33'd1)<<4)-33'd1;
 wire bounds_invalidate=bounds_flush||(bounds_write&&(bounds_write_addr[31:15]==bounds_window||bounds_write_end[31:15]==bounds_window));
 wire bounds_tag_match=op[8]&&load_src[31:15]==bounds_window&&key[23:0]==bounds_key;
 wire [2:0] hint_first=({1'b0,hint_data[9:5]}+{4'b0,dst[3:2]})>>2;
 wire [2:0] hint_end=({1'b0,hint_data[4:0]}+{4'b0,dst[3:2]}+6'd3)>>2;
 wire hint_empty=hint_valid&&hint_data[4:0]==0;
 always @(posedge clk)begin
  if(active&&load_state==L_IDLE&&load_row<height&&!owned[load_bank])hint_data<=bounds_ram[load_src[14:6]];
  if(active&&load_state==L_SOURCE_WAIT&&rd_done&&!rd_error&&!fault&&bounds_tag_match)
   bounds_ram[load_src[14:6]]<={source_first,source_end};
 end
 always @(posedge clk)begin
  if(!rst_n)begin bounds_valid<=0;bounds_window<=0;bounds_key<=0;end
  else begin
   if(cmd_valid&&cmd_ready&&(cmd[63:47]!=bounds_window||cmd[288+:24]!=bounds_key||!cmd[8]))begin
    bounds_valid<=0;bounds_window<=cmd[63:47];bounds_key<=cmd[288+:24];
   end
   if(active&&load_state==L_SOURCE_WAIT&&rd_done&&!rd_error&&!fault&&bounds_tag_match)bounds_valid[load_src[14:6]]<=1;
   if(bounds_invalidate)bounds_valid<=0;
  end
 end
 reg [2:0] bank_first[0:ROW_SLOTS-1],bank_end[0:ROW_SLOTS-1];
 reg [2:0] beat_first,beat_last;integer trim_pixel,trim_index;
 always @*begin
  beat_first=5;beat_last=0;trim_index=0;beat_source_first=16;beat_source_end=0;
  for(trim_pixel=0;trim_pixel<4;trim_pixel=trim_pixel+1)begin
   if(!op[8]||rd_data[trim_pixel*32+:24]!=key[23:0])begin
    trim_index=source_count*4+trim_pixel;
    if(trim_index<beat_source_first)beat_source_first=trim_index;
    if(trim_index+1>beat_source_end)beat_source_end=trim_index+1;
    trim_index=(source_count*4+trim_pixel+dst[3:2])/4;
    if(trim_index<beat_first)beat_first=trim_index;
    if(trim_index+1>beat_last)beat_last=trim_index+1;
   end
  end
 end
 reg source_nonkey,source_nonopaque,bg_started,bg_complete;

 wire [31:0] load_dst=dst+{14'b0,load_row,12'b0};
 wire [2:0] load_beats=dst[3:2]==0?3'd4:3'd5;
 wire [31:0] load_aligned={load_dst[31:4],4'b0};
 wire [7:0] bg_error;wire bg_ready,bg_done,bg_busy;
 reg [1:0] bgq_valid,bgq_issued;reg [ROW_SLOTS-1:0] source_complete,background_complete;
 reg bgq_head,bgq_tail;reg [BANK_BITS-1:0] bgq_bank[0:1];reg [31:0] bgq_addr[0:1];
 reg [2:0] bgq_first[0:1],bgq_beats[0:1];reg [2:0] bg_return_beat;reg [31:0] bg_wait;
 reg source_external;
 wire bg_issue=bgq_issued[bgq_head]?!bgq_head:bgq_head;
 wire bg_arvalid=bgq_valid[bg_issue]&&!bgq_issued[bg_issue]&&!source_external;
 wire bg_return=bgq_valid[bgq_head]&&bgq_issued[bgq_head]&&!source_external;
 wire bg_fire=rvalid&&bg_return;
 wire bg_last=bg_fire&&rlast;
 wire bg_timeout=(|bgq_valid)&&bg_wait>=TIMEOUT_CYCLES;
 assign bg_error=(bg_fire&&rresp!=0)?`GFX_ERR_READ:
                 (bg_fire&&(rlast!=(bg_return_beat==bgq_beats[bgq_head]-1)))?`GFX_ERR_PROTOCOL:
                 bg_timeout?`GFX_ERR_TIMEOUT:8'b0;
 assign bg_ready=!bgq_valid[bgq_tail];assign bg_done=bg_last;assign bg_busy=|bgq_valid;
 wire [31:0] bg_fault_addr=bgq_addr[bgq_head];
 wire beat_nonkey=!op[8]||rd_data[23:0]!=key[23:0]||rd_data[55:32]!=key[23:0]||rd_data[87:64]!=key[23:0]||rd_data[119:96]!=key[23:0];
 wire beat_nonopaque=
  ((!op[8]||rd_data[23:0]!=key[23:0])&&(alpha[7:0]!=255||(op[9]&&rd_data[31:24]!=255)))||
  ((!op[8]||rd_data[55:32]!=key[23:0])&&(alpha[7:0]!=255||(op[9]&&rd_data[63:56]!=255)))||
  ((!op[8]||rd_data[87:64]!=key[23:0])&&(alpha[7:0]!=255||(op[9]&&rd_data[95:88]!=255)))||
  ((!op[8]||rd_data[119:96]!=key[23:0])&&(alpha[7:0]!=255||(op[9]&&rd_data[127:120]!=255)));
 wire bg_request=active&&!fault&&!bg_started&&
  ((load_state==L_SOURCE_WAIT&&cache_serving&&source_nonkey&&(!opaque_cmd||source_nonopaque))||load_state==L_BG);
 wire bg_push=bg_request&&bg_ready;
 assign src_req_valid=active&&load_state==L_SOURCE&&!hint_empty;
 assign src_req_addr=load_src;assign src_req_beats=5'd4;
 assign araddr=bg_arvalid?bgq_addr[bg_issue]:src_araddr;
 assign arlen=bg_arvalid?{5'b0,bgq_beats[bg_issue]}-8'd1:src_arlen;
 assign arsize=4;assign arburst=1;
 assign arvalid=bg_arvalid||(src_arvalid&&!(|bgq_valid)&&!source_external);
 assign rready=bg_return?1'b1:src_rready;
 assign src_arready=arready&&!(|bgq_valid)&&!source_external;
 assign src_rbeat_valid=rvalid&&!bg_return;
 // Two in-order background bursts may be issued before either returns.
 // Source-cache hits prepare the other row while these reads are in flight;
 // a cache miss waits for their drain before using the same AXI return port.
 always @(posedge clk)begin
  if(!rst_n)begin bgq_valid<=0;bgq_issued<=0;bgq_head<=0;bgq_tail<=0;bg_return_beat<=0;bg_wait<=0;source_external<=0;end
  else begin
   if(src_arvalid&&src_arready)source_external<=1;
   if(source_external&&rvalid&&src_rready&&rlast)source_external<=0;
   if(bg_push)begin
    bgq_valid[bgq_tail]<=1;bgq_issued[bgq_tail]<=0;bgq_addr[bgq_tail]<=load_aligned+{25'b0,(hint_valid?hint_first:row_first),4'b0};
    bgq_first[bgq_tail]<=hint_valid?hint_first:row_first;
    // The first visible source beat is final as soon as observed; trailing
    // extent is known only after source completion. Never speculate pixels.
    bgq_beats[bgq_tail]<=hint_valid?hint_end-hint_first:(load_state==L_BG?row_last:load_beats)-row_first;
    bgq_bank[bgq_tail]<=load_bank;bgq_tail<=!bgq_tail;
   end
   if(bg_arvalid&&arready)bgq_issued[bg_issue]<=1;
   if(bg_fire)bg_return_beat<=bg_return_beat+1'b1;
   if(bg_last)begin bgq_valid[bgq_head]<=0;bgq_issued[bgq_head]<=0;bgq_head<=!bgq_head;bg_return_beat<=0;end
   if(!( |bgq_valid)||(bg_arvalid&&arready)||bg_fire)bg_wait<=0;
   else if(bg_wait<TIMEOUT_CYCLES)bg_wait<=bg_wait+1'b1;
  end
 end
 reg [2:0] feed,out_beat;
 reg [2:0] write_first;reg write_started;
 reg [31:0] write_addr;reg [2:0] wn;reg [1:0] lane;reg write_opaque;
 reg [127:0] source_lanes,direct_rgb;reg [15:0] result_mask,direct_mask;integer i,index,source_word;
 always @*begin
  source_lanes=0;result_mask=0;direct_rgb=0;direct_mask=0;index=0;source_word=0;
  for(i=0;i<4;i=i+1)begin
   index=feed*4+i-lane;
   if(index>=0&&index<16)begin
    source_word=(write_bank*4)+(index>>2);
    source_lanes[i*32+:32]=source_rows[source_word]>>(32*(index&3));
    direct_rgb[i*32+:32]={8'hff,source_lanes[i*32+:24]};
    if(!op[8]||source_lanes[i*32+:24]!=key[23:0])direct_mask[i*4+:4]=4'hf;
   end
   index=out_beat*4+i-lane;
   if(index>=0&&index<16&&pixel_write[i])result_mask[i*4+:4]=4'hf;
  end
 end
 assign pixel_rst_n=rst_n;
 // Proven opaque non-key pixels have exact output FF:sourceRGB. Keep
 // the shared Alpha pipeline for all fractional-alpha rows and bypass-off.
 wire direct_valid=active&&write_state==W_PIXELS&&write_opaque&&feed<wn;
 wire row_result_valid=write_opaque?direct_valid:pixel_valid[0];
 wire [143:0] row_result=write_opaque?{direct_mask,direct_rgb}:{result_mask,pixel_result};
 wire row_result_last=write_opaque?(direct_valid&&feed==wn-1):(pixel_valid[0]&&pixel_last[0]);
 assign pixel_in_valid=active&&write_state==W_PIXELS&&!write_opaque&&feed<wn;
 assign pixel_src=source_lanes;
 assign pixel_dst=write_opaque?128'b0:background_rows[(write_bank*5)+feed];
 assign pixel_color=0;assign pixel_op=op;assign pixel_alpha=alpha[7:0];assign pixel_key=key[23:0];
 assign pixel_in_last=feed==wn-1;
 wire [143:0] fifo_data;wire fifo_valid,fifo_ready,fifo_space;wire [4:0] fifo_count;
 assign pixel_out_ready=fifo_space;
 gfx_fifo #(.WIDTH(144),.DEPTH(16),.REGISTER_STORAGE(1),.SYNC_RAM(1)) result_fifo(
  clk,rst_n,1'b0,row_result_valid,fifo_space,row_result,fifo_valid,fifo_ready,fifo_data,fifo_count);
 wire wr_ready,wr_done,wr_busy,wr_data_idle;wire [7:0] wr_error;
 wire write_request=active&&write_state==W_PIXELS&&!write_started;
 gfx_write_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) writer(
  clk,rst_n,write_request,wr_ready,write_addr,{6'b0,(wn-write_first)},
  fifo_data[127:0],fifo_data[143:128],fifo_valid,fifo_ready,
  awaddr,awlen,awsize,awburst,awvalid,awready,wdata,wstrb,wlast,wvalid,wready,bresp,bvalid,bready,wr_done,wr_error,wr_busy,wr_data_idle);
 assign cmd_ready=!active;assign busy=active;assign recover_safe=!active&&!src_busy&&!bg_busy&&!wr_busy;
 // RAM data is unreset; slot ownership prevents reading it before a full row.
 always @(posedge clk)begin
  if(rd_valid&&load_state==L_SOURCE_WAIT&&source_count<4)source_rows[(load_bank*4)+source_count]<=rd_data;
  if(bg_fire&&bg_return_beat<5)background_rows[(bgq_bank[bgq_head]*5)+bgq_first[bgq_head]+bg_return_beat]<=rdata;
 end
 always @(posedge clk)begin
  if(!rst_n)begin
   active<=0;done<=0;done_seq<=0;fault<=0;fault_addr<=0;load_state<=L_IDLE;write_state<=W_IDLE;
   load_row<=0;write_row<=0;load_bank<=0;write_bank<=0;owned<=0;ready<=0;
   source_count<=0;bg_count<=0;source_nonkey<=0;source_nonopaque<=0;bg_started<=0;bg_complete<=0;
   feed<=0;out_beat<=0;write_first<=0;write_started<=0;row_first<=5;row_last<=0;source_first<=16;source_end<=0;hint_valid<=0;bank_opaque<=0;bank_skip<=0;opaque_cmd<=0;source_complete<=0;background_complete<=0;bg_bypass_total<=0;
  end else begin
   done<=0;
   if(cmd_valid&&cmd_ready)begin
    active<=1;op<=cmd[0+:32];src<=cmd[32+:32];dst<=cmd[64+:32];height<=cmd[128+:32];
    alpha<=cmd[256+:32];key<=cmd[288+:32];seq<=cmd[320+:32];done_seq<=cmd[320+:32];opaque_cmd<=opaque_enable;
    fault<=0;fault_addr<=0;load_row<=0;write_row<=0;load_bank<=0;write_bank<=0;owned<=0;ready<=0;
    load_state<=L_IDLE;write_state<=W_IDLE;bg_started<=0;bg_complete<=0;source_complete<=0;background_complete<=0;
   end
   if(active)begin
    if(!fault)begin
     if(src_busy&&rd_error)begin fault<=rd_error;fault_addr<=load_src;end
     else if((bg_busy||bg_done)&&bg_error)begin fault<=bg_error;fault_addr<=bg_fault_addr;end
     else if((wr_busy||wr_done)&&wr_error)begin fault<=wr_error;fault_addr<=awaddr;end
    end
    if(rd_valid&&load_state==L_SOURCE_WAIT)begin
     source_count<=source_count+1'b1;
     if(beat_nonkey)source_nonkey<=1;
     if(beat_source_first<source_first)source_first<=beat_source_first;
     if(beat_source_end>source_end)source_end<=beat_source_end;
     if(beat_first<row_first)row_first<=beat_first;
     if(beat_last>row_last)row_last<=beat_last;
     if(beat_nonopaque)source_nonopaque<=1;
    end
    if(bg_push)begin bg_started<=1;bg_bypass_total<=bg_bypass_total+1'b1;end
    if(bg_done)begin
     background_complete[bgq_bank[bgq_head]]<=1;
     if(!fault&&!bg_error&&source_complete[bgq_bank[bgq_head]])ready[bgq_bank[bgq_head]]<=1;
    end
    case(load_state)
     L_IDLE:if(!fault&&load_row<height&&!owned[load_bank])begin
      owned[load_bank]<=1;source_complete[load_bank]<=0;background_complete[load_bank]<=0;bank_addr[load_bank]<=load_aligned;bank_beats[load_bank]<=load_beats;bank_lane[load_bank]<=dst[3:2];
      source_count<=0;bg_count<=0;source_nonkey<=0;source_nonopaque<=0;bg_started<=0;bg_complete<=0;row_first<=5;row_last<=0;source_first<=16;source_end<=0;
      hint_valid<=bounds_tag_match&&bounds_valid[load_src[14:6]]&&!bounds_invalidate;
      load_state<=L_SOURCE;
     end
     L_SOURCE:if(hint_empty)begin
      source_complete[load_bank]<=1;bank_first[load_bank]<=0;bank_end[load_bank]<=0;bank_skip[load_bank]<=1;bank_opaque[load_bank]<=1;ready[load_bank]<=1;
      load_row<=load_row+1'b1;load_bank<=next_load_bank;load_state<=L_IDLE;
     end else if(src_req_ready)load_state<=L_SOURCE_WAIT;
     L_SOURCE_WAIT:if(rd_done)begin
      source_complete[load_bank]<=1;bank_first[load_bank]<=row_first;bank_end[load_bank]<=row_last;
      if(rd_error||fault)load_state<=L_ABORT;
      else if(!source_nonkey||(opaque_cmd&&!source_nonopaque))begin
       bank_skip[load_bank]<=!source_nonkey;bank_opaque[load_bank]<=1;ready[load_bank]<=1;
       load_row<=load_row+1'b1;load_bank<=next_load_bank;load_state<=L_IDLE;
      end else begin
       bank_skip[load_bank]<=0;bank_opaque[load_bank]<=0;
       if(bg_started)begin
        if(background_complete[load_bank]||(bg_done&&bgq_bank[bgq_head]==load_bank&&!bg_error))ready[load_bank]<=1;
        load_row<=load_row+1'b1;load_bank<=next_load_bank;load_state<=L_IDLE;
       end else load_state<=L_BG;
      end
     end
     L_BG:if(fault)load_state<=L_ABORT;else if(bg_push)begin load_row<=load_row+1'b1;load_bank<=next_load_bank;load_state<=L_IDLE;end
     L_ABORT:if(!src_busy&&!bg_busy)load_state<=L_IDLE;
     default:load_state<=L_ABORT;
    endcase
    case(write_state)
     W_IDLE:if(!fault&&write_row<height&&ready[write_bank])begin
      ready[write_bank]<=0;feed<=bank_first[write_bank];out_beat<=bank_first[write_bank];write_first<=bank_first[write_bank];write_started<=0;
      write_addr<=bank_addr[write_bank]+{25'b0,bank_first[write_bank],4'b0};wn<=bank_end[write_bank];lane<=bank_lane[write_bank];write_opaque<=bank_opaque[write_bank];
      if(bank_skip[write_bank])begin owned[write_bank]<=0;write_state<=W_NEXT;end
      else write_state<=W_PIXELS;
     end
     W_PIXELS:begin
      if((write_opaque&&direct_valid&&fifo_space)||(!write_opaque&&pixel_in_valid&&pixel_ready[0]))begin
       feed<=feed+1'b1;if(feed==wn-1)owned[write_bank]<=0;
      end
      if(write_request&&wr_ready)write_started<=1;
      if(row_result_valid&&fifo_space)out_beat<=out_beat+1'b1;
      if((out_beat==wn||(row_result_last&&fifo_space))&&
         (write_started||(write_request&&wr_ready)))write_state<=W_NEXT;
     end
     W_NEXT:begin write_row<=write_row+1'b1;write_bank<=next_write_bank;write_state<=W_IDLE;end
    endcase
    if((fault||write_row==height)&&load_state==L_IDLE&&write_state==W_IDLE&&!src_busy&&!bg_busy&&!wr_busy&&!fifo_valid)begin
     active<=0;done<=1;done_seq<=seq;
    end
   end
  end
 end
endmodule
