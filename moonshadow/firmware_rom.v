`include "firmware_size.vh"
// Ti60 RAM10's 2048x5 mode uses all 10240 bits. Narrow output keeps ROM bank
// selection smaller than 1024x40. APB+4 low 5 bits, +8 logical word count.
module firmware_rom(input wire clk,rst_n,psel,penable,pwrite,
 input wire [11:0] paddr,input wire [31:0] pwdata,output wire [31:0] prdata);
 reg [17:0] index;
 localparam BANKS=`CONSOLE_FIRMWARE_PACKED05_BANKS;
 wire [4:0] bank_data[0:BANKS-1];reg [6:0] selected;reg in_range;
 function [7:0] digit(input integer n);begin digit=8'd48+n;end endfunction
 genvar b;
 generate for(b=0;b<BANKS;b=b+1)begin:bank
  localparam [8*24-1:0] INIT_FILE={"console_packed05_",digit(b/100),digit((b/10)%10),digit(b%10),".hex"};
  reg [4:0] memory[0:2047];reg [4:0] word_data;
  initial $readmemh(INIT_FILE,memory);
  always @(posedge clk)word_data<=memory[index[10:0]];
  assign bank_data[b]=word_data;
 end endgenerate
 wire [4:0] value=in_range?bank_data[selected]:5'b0;
 always @(posedge clk)begin
  if(!rst_n)index<=0;
  else if(psel&&penable&&pwrite&&paddr==0)index<=pwdata[17:0];
  selected<=index[17:11];in_range<=index<((`CONSOLE_FIRMWARE_WORDS*32+4)/5);
 end
 assign prdata=paddr==0?{14'b0,index}:paddr==4?{27'b0,value}:
  paddr==8?`CONSOLE_FIRMWARE_WORDS:paddr==16?32'h504b3035:32'b0;
endmodule
