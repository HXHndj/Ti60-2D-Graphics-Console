`include "firmware_size.vh"
module firmware_rom(input wire clk,rst_n,psel,penable,pwrite,
 input wire [11:0] paddr,input wire [31:0] pwdata,output wire [31:0] prdata);
 reg [14:0] index;
 localparam BANKS=`CONSOLE_FIRMWARE_PACKED40_BANKS;
 localparam BYTES=`CONSOLE_FIRMWARE_WORDS*4;
 wire [39:0] bank_data[0:BANKS-1];reg [4:0] selected;reg in_range;
 function [7:0] digit(input integer n);begin digit=8'd48+n;end endfunction
 genvar b;
 generate for(b=0;b<BANKS;b=b+1)begin:bank
  localparam [8*23-1:0] INIT_FILE={"console_packed40_",digit(b/10),digit(b%10),".hex"};
  reg [39:0] memory[0:1023];reg [39:0] word_data;
  initial $readmemh(INIT_FILE,memory);
  always @(posedge clk)if(index[14:10]==b)word_data<=memory[index[9:0]];
  assign bank_data[b]=word_data;
 end endgenerate
 wire [39:0] value=in_range?bank_data[selected]:40'b0;
 always @(posedge clk)begin
  if(!rst_n)index<=0;
  else if(psel&&penable&&pwrite&&paddr==0)index<=pwdata[14:0];
  selected<=index[14:10];in_range<=index<(BYTES+4)/5;
 end
 assign prdata=paddr==0?{17'b0,index}:paddr==4?value[31:0]:
  paddr==8?`CONSOLE_FIRMWARE_WORDS:paddr==12?{24'b0,value[39:32]}:
  paddr==16?32'h504b3430:32'b0;
endmodule
