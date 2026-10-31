// RGB byte order R,G,B; reflected CRC-32 matches zlib.crc32 over active pixels.
// Stable frame payload crosses through the tested bundled-data mailbox.
module scan_crc(input wire pclk,prst_n,input wire de,frame_end,input wire [23:0] rgb,
 input wire clk,rst_n,output reg [31:0] frames,pixels,crc,ticks);
 reg [31:0] accum,pixel_count,frame_count;reg [95:0] message;reg pending;
 wire ready,valid;wire [95:0] received;
 reg [31:0] clock_ticks;
 function [31:0] next_crc(input [31:0] seed,input [23:0] data);
  reg [31:0] c;integer byte_no,bit_no;
  begin
   c=seed;
   for(byte_no=2;byte_no>=0;byte_no=byte_no-1)for(bit_no=0;bit_no<8;bit_no=bit_no+1)
    if(c[0]^data[byte_no*8+bit_no])c=(c>>1)^32'hedb88320;else c=c>>1;
   next_crc=c;
  end
 endfunction
 cdc_mailbox #(.WIDTH(96)) mailbox(pclk,prst_n,pending,ready,message,clk,rst_n,valid,1'b1,received);
 always @(posedge pclk)begin
  if(!prst_n)begin accum<=32'hffffffff;pixel_count<=0;frame_count<=0;pending<=0;message<=0;end
  else begin
   if(pending&&ready)pending<=0;
   if(de)begin accum<=next_crc(accum,rgb);pixel_count<=pixel_count+1'b1;end
   if(frame_end)begin
    frame_count<=frame_count+1'b1;
    if(!pending||ready)begin message<={frame_count+1'b1,pixel_count,~accum};pending<=1;end
    accum<=32'hffffffff;pixel_count<=0;
   end
  end
 end
 always @(posedge clk)begin
  if(!rst_n)begin clock_ticks<=0;frames<=0;pixels<=0;crc<=0;ticks<=0;end
  else begin
   clock_ticks<=clock_ticks+1'b1;
   if(valid)begin frames<=received[95:64];pixels<=received[63:32];crc<=received[31:0];ticks<=clock_ticks;end
  end
 end
endmodule
