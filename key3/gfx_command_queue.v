// A complete command is accepted atomically, then stored as eleven 32-bit
// words in synchronous dual-port RAM. Sixteen words per slot avoid address
// multipliers. count includes packing, stored and prefetched commands.
module gfx_command_queue #(parameter DEPTH=16,PW=(DEPTH<2?1:$clog2(DEPTH)))(
 input wire clk,rst_n,clear,
 input wire in_valid,output wire in_ready,input wire [351:0] in_data,
 output reg out_valid,input wire out_ready,output reg [351:0] out_data,
 output reg [PW:0] count
);
 reg [31:0] memory[0:DEPTH*16-1];reg [31:0] read_data;
 reg [351:0] snapshot;
 reg [PW-1:0] wp,rp;
 // Occupancy counter replaces the per-slot one-hot vector so DEPTH can scale
 // into inferred RAM; push/pop order keeps the same visibility gating.
 reg [PW:0] stored;
 reg packing,reading,read_pending;
 reg [3:0] write_word,read_issue,read_capture;
 assign in_ready=!packing&&count<DEPTH;
 wire push=in_valid&&in_ready,pop=out_valid&&out_ready;
 always @(posedge clk)begin
  if(rst_n&&!clear&&packing)memory[{wp,write_word}]<=snapshot[write_word*32+:32];
  if(rst_n&&!clear&&reading&&read_issue<11)read_data<=memory[{rp,read_issue}];
 end
 always @(posedge clk)begin
  if(!rst_n||clear)begin
   wp<=0;rp<=0;stored<=0;packing<=0;reading<=0;read_pending<=0;
   write_word<=0;read_issue<=0;read_capture<=0;out_valid<=0;count<=0;
  end else begin
   if(push)begin snapshot<=in_data;packing<=1;write_word<=0;end
   if(packing)begin
    if(write_word==10)begin packing<=0;wp<=wp==DEPTH-1?0:wp+1'b1;end
    else write_word<=write_word+1'b1;
   end
   if(!out_valid&&!reading&&stored!=0)begin
    reading<=1;read_issue<=0;read_capture<=0;read_pending<=0;
   end
   if(reading)begin
    read_pending<=read_issue<11;
    if(read_issue<11)read_issue<=read_issue+1'b1;
    if(read_pending)begin
     out_data[read_capture*32+:32]<=read_data;
     if(read_capture==10)begin out_valid<=1;reading<=0;read_pending<=0;end
     else read_capture<=read_capture+1'b1;
    end
   end
   if(pop)begin out_valid<=0;rp<=rp==DEPTH-1?0:rp+1'b1;end
   // Packing completion and consumption can share a cycle. A pair of NBA
   // assignments loses the completed command when pop wins source order.
   case({packing&&write_word==10,pop})
    2'b10:stored<=stored+1'b1;
    2'b01:stored<=stored-1'b1;
    default:begin end
   endcase
   case({push,pop})2'b10:count<=count+1'b1;2'b01:count<=count-1'b1;default:begin end endcase
  end
 end
endmodule
