// Unmeasured inputs reset to zero. Software supplies measured FPS * 10.
// One line: SW000.0 HW000.0 N0000 M0. Registers latch only at frame start.
module diag_hud(input wire clk,rst_n,frame_start,input wire [11:0] x,y,
 input wire [31:0] sw_fps10,hw_fps10,sprites,mode,input wire [23:0] in_rgb,output wire [23:0] out_rgb);
 reg [31:0] sw,hw,n,m;
 always @(posedge clk)begin
  if(!rst_n)begin sw<=0;hw<=0;n<=0;m<=0;end
  else if(frame_start)begin sw<=sw_fps10;hw<=hw_fps10;n<=sprites;m<=mode;end
 end
 function [34:0] glyph(input [7:0] c);begin
  case(c)
   "0":glyph=35'b01110_10001_10011_10101_11001_10001_01110;
   "1":glyph=35'b00100_01100_00100_00100_00100_00100_01110;
   "2":glyph=35'b01110_10001_00001_00010_00100_01000_11111;
   "3":glyph=35'b11110_00001_00001_01110_00001_00001_11110;
   "4":glyph=35'b00010_00110_01010_10010_11111_00010_00010;
   "5":glyph=35'b11111_10000_10000_11110_00001_00001_11110;
   "6":glyph=35'b01110_10000_10000_11110_10001_10001_01110;
   "7":glyph=35'b11111_00001_00010_00100_01000_01000_01000;
   "8":glyph=35'b01110_10001_10001_01110_10001_10001_01110;
   "9":glyph=35'b01110_10001_10001_01111_00001_00001_01110;
   "S":glyph=35'b01111_10000_10000_01110_00001_00001_11110;
   "W":glyph=35'b10001_10001_10001_10101_10101_11011_10001;
   "H":glyph=35'b10001_10001_10001_11111_10001_10001_10001;
   "N":glyph=35'b10001_11001_11001_10101_10011_10011_10001;
   "M":glyph=35'b10001_11011_10101_10101_10001_10001_10001;
   ".":glyph=35'b00000_00000_00000_00000_00000_00110_00110;
   default:glyph=0;
  endcase
 end endfunction
 reg [7:0] c;reg [34:0] bits;
 always @*begin
  case(x/6)
   0:c="S";1:c="W";2:c="0"+(sw/1000)%10;3:c="0"+(sw/100)%10;4:c="0"+(sw/10)%10;5:c=".";6:c="0"+sw%10;
   8:c="H";9:c="W";10:c="0"+(hw/1000)%10;11:c="0"+(hw/100)%10;12:c="0"+(hw/10)%10;13:c=".";14:c="0"+hw%10;
   16:c="N";17:c="0"+(n/1000)%10;18:c="0"+(n/100)%10;19:c="0"+(n/10)%10;20:c="0"+n%10;
   22:c="M";23:c="0"+m%10;default:c=" ";
  endcase
  bits=glyph(c);
 end
 assign out_rgb=(y<8&&x<144)?((y<7&&x%6<5&&bits[34-y*5-x%6])?24'hffffff:24'h101018):in_rgb;
endmodule
