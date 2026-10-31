// One-shot bridge from the existing two-write APB protocol to CONFIG_CTRL0.
// AN010 v2.5 p7: CBSEL is clocked under ENA; CONFIG is asynchronous.
// The source freezes the complete slot before raising its sticky request.
// Two request synchronizers and the physical <=one-cfg-period bundled-data
// bound ensure the slot settles before destination capture. No bitwise slot
// synchronization (which could tear 01->10); no dependence on firmware delay.
module mirror_reconfig(
 input wire user_clk,user_rst_n,cfg_clk,cfg_arst_n,
 input wire write_valid,input wire [2:0] write_data,
 input wire cfg_error,
 output reg [1:0] staged_slot,output reg requested,
 output wire error_user,
 output reg [1:0] cfg_cbsel,output reg cfg_ena,cfg_config
);
 reg [1:0] slot_hold;
 (* async_reg="true" *) reg error_meta,error_sync;
 (* async_reg="true" *) reg request_meta,request_sync;
 wire cfg_rst_n;
 reset_sync cfg_reset(cfg_clk,cfg_arst_n,cfg_rst_n);
 assign error_user=error_sync;
 always @(posedge user_clk)begin
  if(!user_rst_n)begin
   staged_slot<=0;slot_hold<=0;requested<=0;error_meta<=0;error_sync<=0;
  end else begin
   error_meta<=cfg_error;error_sync<=error_meta;
   if(write_valid&&!requested)begin
    if(!write_data[2])staged_slot<=write_data[1:0];
    else begin slot_hold<=staged_slot;requested<=1;end
   end
  end
 end
 // Clear immediately with system reset, release only in the config domain.
 always @(posedge cfg_clk or negedge cfg_rst_n)begin
  if(!cfg_rst_n)begin request_meta<=0;request_sync<=0;end
  else begin request_meta<=requested;request_sync<=request_meta;end
 end
 localparam IDLE=0,SELECTED=1,CAPTURE=2,ARMED=3,TRIGGERED=4;
 reg [2:0] state;
 always @(posedge cfg_clk or negedge cfg_rst_n)begin
  if(!cfg_rst_n)begin cfg_cbsel<=0;cfg_ena<=0;cfg_config<=0;state<=IDLE;end
  else case(state)
   IDLE:if(request_sync)begin cfg_cbsel<=slot_hold;state<=SELECTED;end
   // Leave a full config cycle after CBSEL changes before enabling capture.
   SELECTED:begin cfg_ena<=1;state<=CAPTURE;end
   // CONFIG_CTRL0 sees ENA=1 at this edge and latches the stable selector.
   CAPTURE:state<=ARMED;
   // Trigger one complete cfg period after that actual capture edge.
   ARMED:begin cfg_config<=1;state<=TRIGGERED;end
   TRIGGERED:begin end // All outputs and the slot remain frozen until reset.
   default:state<=IDLE;
  endcase
 end
endmodule
