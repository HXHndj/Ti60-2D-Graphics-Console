// Joint pixel tile limit; source and destination use their own aligned address
// and beat count. No 4KB crossing, no more than 16 beats on either stream.
module gfx_burst_plan(input wire [31:0] src,dst,remaining,input wire use_src,
 output reg [6:0] pixels,output wire [31:0] src_aligned,dst_aligned,
 output wire [4:0] src_beats,dst_beats);
 reg [31:0] n,t;
 assign src_aligned={src[31:4],4'b0};assign dst_aligned={dst[31:4],4'b0};
 assign src_beats=({1'b0,pixels}+src[3:2]+3)>>2;
 assign dst_beats=({1'b0,pixels}+dst[3:2]+3)>>2;
 always @*begin
  n=remaining;
  t=64-dst[3:2];if(n>t)n=t;
  t=(4096-{20'b0,dst[11:0]})>>2;if(n>t)n=t;
  if(use_src)begin
   t=64-src[3:2];if(n>t)n=t;
   t=(4096-{20'b0,src[11:0]})>>2;if(n>t)n=t;
  end
  pixels=n[6:0];
 end
endmodule
