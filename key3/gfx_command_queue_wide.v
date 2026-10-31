// Whole-command synchronous RAM for deep queues. A 352-bit word packs into
// the RAM10 native widths without the five unused words per legacy slot.
// count includes the output/prefetch registers, so accepted work stays visible.
module gfx_command_queue_wide #(parameter DEPTH=1024,PW=(DEPTH<2?1:$clog2(DEPTH)))(
 input wire clk,rst_n,clear,
 input wire in_valid,output wire in_ready,input wire [351:0] in_data,
 output reg out_valid,input wire out_ready,output reg [351:0] out_data,
 output reg [PW:0] count
);
 reg [351:0] memory[0:DEPTH-1],read_data;
 reg [PW-1:0] wp,rp;reg [PW:0] stored;
 reg read_pending;
 assign in_ready=count<DEPTH;
 wire push=in_valid&&in_ready,pop=out_valid&&out_ready;
 wire fetch=!out_valid&&!read_pending&&stored!=0;
 always @(posedge clk)begin
  if(rst_n&&!clear&&push)memory[wp]<=in_data;
  if(rst_n&&!clear&&fetch)read_data<=memory[rp];
 end
 always @(posedge clk)begin
  if(!rst_n||clear)begin wp<=0;rp<=0;stored<=0;read_pending<=0;out_valid<=0;count<=0;end
  else begin
   if(push)wp<=wp==DEPTH-1?0:wp+1'b1;
   read_pending<=fetch;
   if(fetch)rp<=rp==DEPTH-1?0:rp+1'b1;
   if(read_pending)begin out_data<=read_data;out_valid<=1;end
   if(pop)out_valid<=0;
   case({push,fetch})2'b10:stored<=stored+1'b1;2'b01:stored<=stored-1'b1;default:begin end endcase
   case({push,pop})2'b10:count<=count+1'b1;2'b01:count<=count-1'b1;default:begin end endcase
  end
 end
endmodule
