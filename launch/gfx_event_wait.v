`include "gfx_abi.vh"
// Bounded APB read wait. Does not depend on CPU WFI support or interrupt entry.
// A graphics IRQ level or the independently counted deadline releases PREADY.
module gfx_event_wait #(parameter MAX_CYCLES=100000)(
 input wire clk,rst_n,psel,penable,pwrite,input wire [11:0] paddr,input wire [31:0] pwdata,
 output wire pready,output reg [31:0] prdata,output reg pslverr,input wire event_irq
);
 reg [31:0] limit,elapsed,calls;reg [63:0] stalled;
 wire waiting=psel&&penable&&!pwrite&&paddr==`GFX_REG_WAIT_EVENT;
 assign pready=!waiting||event_irq||elapsed>=limit||!rst_n;
 wire transfer=psel&&penable&&pready&&!pslverr;
 always @*begin
  prdata=0;pslverr=paddr[1:0]!=0;
  case(paddr)
   `GFX_REG_WAIT_EVENT:begin prdata={event_irq,!event_irq,elapsed[29:0]};if(pwrite)pslverr=1;end
   `GFX_REG_WAIT_LIMIT:begin prdata=limit;if(pwrite&&(pwdata==0||pwdata>MAX_CYCLES))pslverr=1;end
   `GFX_REG_WAIT_CALLS:begin prdata=calls;if(pwrite)pslverr=1;end
   `GFX_REG_WAIT_TICKS_LO:begin prdata=stalled[31:0];if(pwrite)pslverr=1;end
   `GFX_REG_WAIT_TICKS_HI:begin prdata=stalled[63:32];if(pwrite)pslverr=1;end
   default:pslverr=1;
  endcase
 end
 always @(posedge clk)begin
  if(!rst_n)begin limit<=MAX_CYCLES;elapsed<=0;calls<=0;stalled<=0;end
  else begin
   if(waiting&&!pready)begin elapsed<=elapsed+1'b1;stalled<=stalled+1'b1;end
   else elapsed<=0;
   if(transfer&&pwrite&&paddr==`GFX_REG_WAIT_LIMIT)limit<=pwdata;
   if(transfer&&!pwrite&&paddr==`GFX_REG_WAIT_EVENT)calls<=calls+1'b1;
  end
 end
endmodule
