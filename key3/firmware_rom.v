`include "firmware_size.vh"
module firmware_rom(input wire clk,rst_n,psel,penable,pwrite,
 input wire [11:0] paddr,input wire [31:0] pwdata,output wire [31:0] prdata);
 reg [14:0] index;
 // Fixed 4 KiB banks avoid a large mapper capacity jump just above 80 KiB.
 localparam BANKS=(`CONSOLE_FIRMWARE_WORDS+1023)/1024;
 wire [31:0] bank_data[0:BANKS-1];reg [4:0] selected;reg in_range;
 function [7:0] digit(input integer n);begin digit=8'd48+n;end endfunction
 genvar b;
 generate for(b=0;b<BANKS;b=b+1)begin:bank
  localparam [8*23-1:0] INIT_FILE={"console_firmware_",digit(b/10),digit(b%10),".hex"};
  // A partially occupied final bank needs only its used 1 KiB RAM slices.
  // Out-of-image indices are still masked by the registered in_range below.
  localparam USED_WORDS=(b+1)*1024<=`CONSOLE_FIRMWARE_WORDS?1024:`CONSOLE_FIRMWARE_WORDS-b*1024;
  localparam DEPTH=((USED_WORDS+255)/256)*256;
  localparam AW=$clog2(DEPTH);
  reg [31:0] memory[0:DEPTH-1];reg [31:0] word_data;
  initial $readmemh(INIT_FILE,memory,0,DEPTH-1);
  always @(posedge clk)if(index[14:10]==b)word_data<=memory[index[AW-1:0]];
  assign bank_data[b]=word_data;
 end endgenerate
 wire [31:0] value=in_range?bank_data[selected]:32'b0;
 always @(posedge clk)begin
  if(!rst_n)index<=0;
  else if(psel&&penable&&pwrite&&paddr==0)index<=pwdata[14:0];
  selected<=index[14:10];in_range<=index<`CONSOLE_FIRMWARE_WORDS;
 end
 assign prdata=paddr==0?{17'b0,index}:paddr==4?value:paddr==8?`CONSOLE_FIRMWARE_WORDS:32'b0;
endmodule
