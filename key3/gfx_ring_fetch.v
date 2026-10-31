`include "gfx_abi.vh"
// DDR command-ring fetcher. The CPU appends 64-byte descriptors (the 11-word
// command plus pad) to a power-of-two ring in DDR and rings the doorbell with
// the absolute submitted count. Each entry is streamed with one outstanding
// 4-beat INCR burst, validated exactly like an APB commit, and pushed into the
// same command queue; gfx_regs assigns the sequence number. 64-byte slots can
// never cross an AXI 4 KiB boundary when the base is 64-byte aligned because
// ENTRIES*64 is a multiple of 4096. The parent muxes this master onto the
// graphics fabric port with arid 5; beat returns are demultiplexed by rid.
module gfx_ring_fetch #(parameter ENTRIES=4096,TIMEOUT_CYCLES=10000000)(
 input wire clk,rst_n,
 input wire psel,penable,pwrite,input wire [4:0] paddr,input wire [31:0] pwdata,
 output wire pready,output reg [31:0] prdata,output reg pslverr,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,
 output wire [1:0] arburst,output wire arvalid,input wire arready,
 input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 output wire push_valid,input wire push_ready,output wire [351:0] push_data,
 output wire fault_valid,output reg [7:0] fault_code,
 input wire queue_clear,
 input wire [31:0] pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end
);
 localparam AW=(ENTRIES<2)?1:$clog2(ENTRIES);
 localparam [31:0] CAPACITY=ENTRIES;
 localparam S_IDLE=2'd0,S_AR=2'd1,S_DATA=2'd2,S_PUSH=2'd3;
 reg [31:0] base,submitted,fetched,issued;
 reg enabled,faulted;
 reg [7:0] fault_code_latched;
 reg [1:0] state,beats;
 reg [127:0] beat0,beat1,beat2;
 reg [31:0] wait_cycles;
 reg fault_pulse;
 // Words 0..7 come from beats 0..1 and words 8..10 from the low 96 bits of
 // beat 2; beat 3 is pad. On the fourth beat all three registers are stable,
 // so the validator sees the complete entry combinationally.
 wire [31:0] w8=beat2[31:0],w9=beat2[63:32],w10=beat2[95:64];
 wire [351:0] entry={w10,w9,w8,beat1,beat0};
 wire [7:0] validation_error;
 gfx_validate check(entry,pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,validation_error);
 wire [31:0] slot_addr=base+{{(32-AW-6){1'b0}},issued[AW-1:0],6'b0};
 wire transfer=psel&&penable&&pready;
 wire [2:0] pidx=paddr[4:2];
 wire busy=state!=S_IDLE;
 wire doorbell_overflow;
 assign pready=1'b1;
 assign araddr=slot_addr;
 assign arvalid=(state==S_AR);
 assign arlen=8'd3;assign arsize=3'd4;assign arburst=2'd1;
 assign rready=(state==S_DATA);
 assign push_valid=(state==S_PUSH);
 assign push_data=entry;
 assign fault_valid=fault_pulse;
 wire axi_wait=(state==S_AR&&!arready)||(state==S_DATA&&!rvalid);
 always @*begin
  prdata=32'b0;pslverr=1'b0;
  if(paddr[1:0]!=0)pslverr=1;
  else case(pidx)
   3'd1:begin prdata=base;if(pwrite&&pwdata[5:0]!=6'd0)pslverr=1;end // RING_BASE
   3'd2:begin prdata=CAPACITY;if(pwrite)pslverr=1;end  // RING_CAPACITY
   3'd3:begin prdata=submitted;                        // RING_DOORBELL
    if(pwrite&&(!enabled||faulted||pwdata<submitted))pslverr=1;
   end
   3'd4:begin prdata=fetched;if(pwrite)pslverr=1;end   // RING_FETCHED
   3'd5:begin prdata={fault_code_latched,5'b0,busy,faulted,enabled};if(pwrite)pslverr=1;end // RING_STATUS
   3'd6:begin if(pwrite&&pwdata>1)pslverr=1;end        // RING_CONTROL
   default:pslverr=1;
  endcase
  if(doorbell_overflow)pslverr=1;
 end
 assign doorbell_overflow=transfer&&pwrite&&paddr[1:0]==0&&pidx==3'd3&&enabled&&!faulted&&
   pwdata>=submitted&&(pwdata-fetched>CAPACITY);
 always @(posedge clk)begin
  if(!rst_n)begin
   base<=0;submitted<=0;fetched<=0;issued<=0;enabled<=0;faulted<=0;
   fault_code_latched<=0;state<=S_IDLE;beats<=0;beat0<=0;beat1<=0;beat2<=0;
   wait_cycles<=0;fault_pulse<=0;fault_code<=0;
  end else begin
   fault_pulse<=0;
   if(queue_clear)begin
    // CONTROL=2 drained the queue: the whole ring contract restarts disabled.
    enabled<=0;submitted<=0;fetched<=0;issued<=0;state<=S_IDLE;beats<=0;faulted<=0;fault_code_latched<=0;
   end else begin
    if(transfer&&!pslverr&&!doorbell_overflow&&pwrite)begin
     if(pidx==3'd1)base<=pwdata;
     if(pidx==3'd3)submitted<=pwdata;
     if(pidx==3'd6)begin
      enabled<=pwdata[0];
      // Enabling starts a fresh contract; pointers and the local fault clear.
      submitted<=0;fetched<=0;issued<=0;faulted<=0;fault_code_latched<=0;state<=S_IDLE;beats<=0;
     end
    end
    if(doorbell_overflow)begin
     fault_pulse<=1;fault_code<=`GFX_ERR_BUSY;fault_code_latched<=`GFX_ERR_BUSY;faulted<=1;
    end
    case(state)
     S_IDLE:if(enabled&&!faulted&&issued!=submitted)begin state<=S_AR;wait_cycles<=0;end
     S_AR:if(arvalid&&arready)begin issued<=issued+1'b1;state<=S_DATA;beats<=0;wait_cycles<=0;end
     S_DATA:if(rvalid&&rready)begin
      wait_cycles<=0;
      case(beats)
       2'd0:beat0<=rdata;
       2'd1:beat1<=rdata;
       2'd2:beat2<=rdata;
       default:begin end
      endcase
      beats<=beats+1'b1;
      if(rresp!=2'd0)begin
       fault_pulse<=1;fault_code<=`GFX_ERR_READ;fault_code_latched<=`GFX_ERR_READ;faulted<=1;state<=S_IDLE;
      end else if(rlast&&beats!=2'd3)begin
       fault_pulse<=1;fault_code<=`GFX_ERR_PROTOCOL;fault_code_latched<=`GFX_ERR_PROTOCOL;faulted<=1;state<=S_IDLE;
      end else if(beats!=2'd3)begin
       // Still collecting beats 0..2.
      end else if(!rlast)begin
       fault_pulse<=1;fault_code<=`GFX_ERR_PROTOCOL;fault_code_latched<=`GFX_ERR_PROTOCOL;faulted<=1;state<=S_IDLE;
      end else if(validation_error!=0)begin
       fault_pulse<=1;fault_code<=validation_error;fault_code_latched<=validation_error;faulted<=1;state<=S_IDLE;
      end else state<=S_PUSH;
     end
     S_PUSH:if(push_ready)begin fetched<=fetched+1'b1;state<=S_IDLE;end
     default:state<=S_IDLE;
    endcase
    if(axi_wait)begin
     if(wait_cycles<TIMEOUT_CYCLES)wait_cycles<=wait_cycles+1'b1;
     else begin
      fault_pulse<=1;fault_code<=`GFX_ERR_TIMEOUT;fault_code_latched<=`GFX_ERR_TIMEOUT;faulted<=1;state<=S_IDLE;
     end
    end
   end
  end
 end
endmodule
