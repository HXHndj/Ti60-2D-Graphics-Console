`include "gfx_abi.vh"
// Two framebuffer-owned text banks, 16 non-overlapping 16x16-character runs.
// CPU publishes characters through dual-clock RAM, then sends stable run
// descriptors through the mailbox and waits for acknowledgement before present.
// RGB/DE/HS/VS all have exactly three pixel clocks of latency.
module gfx_text_overlay #(
 parameter FB0=32'h01000000,FB1=32'h01400000,WIDTH=1024,HEIGHT=600
)(
 input wire clk,rst_n,pclk,prst_n,
 input wire psel,penable,pwrite,input wire [11:0] paddr,input wire [31:0] pwdata,
 output wire pready,output reg [31:0] prdata,output reg pslverr,
 input wire [31:0] front_base,pending_base,request_base,input wire pending,request,
 input wire [31:0] pixel_base,input wire [11:0] x,y,input wire [23:0] rgb,
 input wire de,hs,vs,output reg [23:0] out_rgb,output reg out_de,out_hs,out_vs,
 output wire present_ready
);
 reg selected,session_valid;reg [7:0] word_index;reg [3:0] desc_index;
 reg [7:0] result;
 reg [31:0] descriptors[0:15];wire [511:0] packed_desc;
 reg [31:0] characters[0:511];reg [7:0] font[0:511];
 initial $readmemh("console_font.hex",font);
 genvar g;generate for(g=0;g<16;g=g+1)begin:pack assign packed_desc[g*32+:32]=descriptors[g];end endgenerate
 wire transfer=psel&&penable;
 wire write=transfer&&pwrite&&!pslverr;
 wire [31:0] target_base=selected?FB1:FB0;
 wire [31:0] selection_base=pwdata[0]?FB1:FB0;
 wire locked=target_base==front_base||(pending&&target_base==pending_base)||(request&&target_base==request_base);
 wire selection_locked=selection_base==front_base||(pending&&selection_base==pending_base)||(request&&selection_base==request_base);
 wire mailbox_ready,received_valid;wire [512:0] received;
 wire allowed=mailbox_ready&&session_valid&&!locked;
 assign present_ready=mailbox_ready&&!session_valid;
 wire commit=write&&paddr==`GFX_REG_TEXT_CONTROL&&allowed;
 cdc_mailbox #(.WIDTH(513)) mailbox(clk,rst_n,commit,mailbox_ready,{selected,packed_desc},
  pclk,prst_n,received_valid,1'b1,received);
 assign pready=1;
 always @*begin
  prdata=0;pslverr=paddr[1:0]!=0;
  case(paddr)
   `GFX_REG_TEXT_ID:begin prdata=32'h54585431;if(pwrite)pslverr=1;end
   `GFX_REG_TEXT_STATUS:begin prdata={15'b0,selected,result,7'b0,mailbox_ready};if(pwrite)pslverr=1;end
   `GFX_REG_TEXT_SELECT:begin prdata={31'b0,selected};if(pwrite&&pwdata>1)pslverr=1;end
   `GFX_REG_TEXT_INDEX:begin prdata={24'b0,word_index};if(pwrite&&pwdata>255)pslverr=1;end
   `GFX_REG_TEXT_DATA:if(pwrite&&(pwdata&32'hc0c0c0c0)!=0)pslverr=1;
   `GFX_REG_TEXT_DESC_INDEX:begin prdata={28'b0,desc_index};if(pwrite&&pwdata>15)pslverr=1;end
   `GFX_REG_TEXT_DESC_DATA:if(pwrite&&(pwdata[31:27]!=0||pwdata[26:20]>64||pwdata[9:0]>=WIDTH||pwdata[19:10]>=HEIGHT))pslverr=1;
   `GFX_REG_TEXT_CONTROL:if(pwrite&&pwdata!=1)pslverr=1;
   default:pslverr=1;
  endcase
 end
 // Do not reset RAM contents: invalid descriptors hide all uninitialized data.
 always @(posedge clk)if(write&&allowed&&paddr==`GFX_REG_TEXT_DATA)
  characters[{selected,word_index}]<=pwdata;
 integer i;
 always @(posedge clk)begin
  if(!rst_n)begin
   selected<=0;session_valid<=0;word_index<=0;desc_index<=0;result<=0;
   for(i=0;i<16;i=i+1)descriptors[i]<=0;
  end else if(write)begin
   if(paddr==`GFX_REG_TEXT_SELECT)begin
    if(!mailbox_ready)result<=`GFX_ERR_BUSY;
    else if(selection_locked)result<=`GFX_ERR_PROTECTED;
    else begin
     selected<=pwdata[0];session_valid<=1;word_index<=0;desc_index<=0;result<=0;
     for(i=0;i<16;i=i+1)descriptors[i]<=0;
    end
   end else if(!mailbox_ready||!session_valid)result<=`GFX_ERR_BUSY;
   else if(locked)result<=`GFX_ERR_PROTECTED;
   else begin
    result<=0;
    case(paddr)
     `GFX_REG_TEXT_INDEX:word_index<=pwdata[7:0];
     `GFX_REG_TEXT_DATA:word_index<=word_index+1'b1;
     `GFX_REG_TEXT_DESC_INDEX:desc_index<=pwdata[3:0];
     `GFX_REG_TEXT_DESC_DATA:begin descriptors[desc_index]<=pwdata;desc_index<=desc_index+1'b1;end
     `GFX_REG_TEXT_CONTROL:session_valid<=0;
     default:begin end
    endcase
   end
  end
 end
  // Separate constant-index banks avoid a dynamic write address in synthesis.
  reg [31:0] pixel_desc0[0:15],pixel_desc1[0:15];
  integer d;
  always @(posedge pclk)begin
   if(!prst_n)for(d=0;d<16;d=d+1)begin pixel_desc0[d]<=0;pixel_desc1[d]<=0;end
   else if(received_valid)begin
    if(received[512])for(d=0;d<16;d=d+1)pixel_desc1[d]<=received[d*32+:32];
    else for(d=0;d<16;d=d+1)pixel_desc0[d]<=received[d*32+:32];
   end
 end
 wire bank=pixel_base==FB1;
 reg chosen;reg [3:0] chosen_run;reg [11:0] dx,dy;
 reg [31:0] descriptor;integer r;
 always @*begin
  chosen=0;chosen_run=0;dx=0;dy=0;descriptor=0;
  for(r=0;r<16;r=r+1)begin
    descriptor=bank?pixel_desc1[r]:pixel_desc0[r];
   if(de&&(pixel_base==FB0||pixel_base==FB1)&&descriptor[26:20]!=0&&
    x>=descriptor[9:0]&&{1'b0,x}<({3'b0,descriptor[9:0]}+{2'b0,descriptor[26:20],4'b0})&&
    y>=descriptor[19:10]&&{1'b0,y}<({3'b0,descriptor[19:10]}+13'd16))begin
    chosen=1;chosen_run=r;dx=x-descriptor[9:0];dy=y-descriptor[19:10];
   end
  end
 end
 reg [31:0] char_word;reg [7:0] glyph_row;
 reg [1:0] byte_lane;reg [2:0] font_row,font_col0,font_col1;
 reg valid0,valid1;reg [23:0] rgb0,rgb1;reg [1:0] de_pipe,hs_pipe,vs_pipe;
 wire [7:0] glyph=char_word>>(byte_lane*8);
 always @(posedge pclk)begin
  char_word<=characters[{bank,chosen_run,dx[9:6]}];
  glyph_row<=font[{glyph[5:0],font_row}];
  if(!prst_n)begin
   byte_lane<=0;font_row<=0;font_col0<=0;font_col1<=0;valid0<=0;valid1<=0;
   rgb0<=0;rgb1<=0;de_pipe<=0;hs_pipe<=3;vs_pipe<=3;out_rgb<=0;out_de<=0;out_hs<=1;out_vs<=1;
  end else begin
   byte_lane<=dx[5:4];font_row<=dy[3:1];font_col0<=dx[3:1];font_col1<=font_col0;
   valid0<=chosen;valid1<=valid0;rgb0<=rgb;rgb1<=rgb0;
   de_pipe<={de_pipe[0],de};hs_pipe<={hs_pipe[0],hs};vs_pipe<={vs_pipe[0],vs};
   out_rgb<=valid1&&glyph_row[7-font_col1]?24'he0edff:rgb1;
   out_de<=de_pipe[1];out_hs<=hs_pipe[1];out_vs<=vs_pipe[1];
  end
 end
endmodule
