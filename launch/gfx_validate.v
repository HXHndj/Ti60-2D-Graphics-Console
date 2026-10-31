`include "gfx_abi.vh"
// Whole command check before enqueue; 64-bit extended spans prevent aliasing.
module gfx_validate(input wire [`GFX_CMD_BITS-1:0] cmd,
 input wire [31:0] pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,
 output reg [7:0] error);
    wire [31:0] op=cmd[0+:32],src=cmd[32+:32],dst=cmd[64+:32];
    wire [31:0] w=cmd[96+:32],h=cmd[128+:32],ss=cmd[160+:32],ds=cmd[192+:32];
    wire [31:0] lc=cmd[224+:32],a=cmd[256+:32],k=cmd[288+:32];
    // Nonempty legal dimensions are <=4096. Reject larger values below before
    // using these spans. 13x32 products plus a 32-bit address fit in 45 bits;
    // avoid the 64-bit h-1 borrow chain while still detecting 32-bit overflow.
    wire [12:0] hm1=h[12:0]-13'd1;
    wire [44:0] se={13'b0,src}+hm1*{13'b0,ss}+{30'b0,w[12:0],2'b0};
    wire [44:0] de={13'b0,dst}+hm1*{13'b0,ds}+{30'b0,w[12:0],2'b0};
    wire [44:0] se_pad=se+45'd15,de_pad=de+45'd15;
    wire [44:0] sr={se_pad[44:4],4'b0},dr={de_pad[44:4],4'b0};
    wire [44:0] tile_desc_end={13'b0,src}+45'd256;
    always @* begin
        error=0;
        if(op==`GFX_OP_BUCKET)begin
         // INIT_FB skeleton: recipe base (alpha word) and the key word stay
         // zero. Descriptor, directory and list spans are frame data, so only
         // their bases and the whole destination frame are range-checked
         // here; the firmware builder owns the in-table bounds.
         if(k!=0)error=`GFX_ERR_OPCODE;
         else if(w==0||h==0||w>4096||h>4096||w[4:0]!=0)error=`GFX_ERR_SIZE;
         else if(src[3:0]!=0||ss[3:0]!=0||dst[3:0]!=0||lc[3:0]!=0)error=`GFX_ERR_ALIGN;
         else if(ds[5:0]!=0||{32'b0,ds}<({32'b0,w}<<2))error=`GFX_ERR_STRIDE;
         else if({src[31:4],4'b0}<pool_start||src>{32'b0,pool_end}||
                 {ss[31:4],4'b0}<pool_start||ss>{32'b0,pool_end}||
                 {lc[31:4],4'b0}<pool_start||lc>{32'b0,pool_end}||
                 (a!=0&&(a[3:0]!=0||{a[31:4],4'b0}<pool_start||a>{32'b0,pool_end}))||
                 {dst[31:4],4'b0}<pool_start||dr>{32'b0,pool_end})error=`GFX_ERR_RANGE;
         else if((dst<protect0_end&&{32'b0,protect0_start}<de)||(dst<protect1_end&&{32'b0,protect1_start}<de))error=`GFX_ERR_PROTECTED;
        end else if(op==`GFX_OP_TILE4)begin
         if(w==0||h==0||w>4096||h>4096)error=`GFX_ERR_SIZE;
         else if(src[3:0]!=0||dst[1:0]!=0)error=`GFX_ERR_ALIGN;
         else if(ss<1||ss>8||ds[5:0]!=0||{32'b0,ds}<({32'b0,w}<<2))error=`GFX_ERR_STRIDE;
         else if(src<pool_start||tile_desc_end>{32'b0,pool_end}||{dst[31:4],4'b0}<pool_start||dr>{32'b0,pool_end})error=`GFX_ERR_RANGE;
         else if({32'b0,src}<de&&{32'b0,dst}<tile_desc_end)error=`GFX_ERR_OVERLAP;
         else if((dst<protect0_end&&{32'b0,protect0_start}<de)||(dst<protect1_end&&{32'b0,protect1_start}<de))error=`GFX_ERR_PROTECTED;
        end else if((op & 32'hfffffcfc)!=0 || op[1:0]==0 ||
           (op[1:0]==1 && op!=1) || (op[1:0]!=3 && op[9]) || a>255 || k>24'hffffff) error=`GFX_ERR_OPCODE;
        else if(w==0 || h==0) error=0;
        else if(w>4096 || h>4096) error=`GFX_ERR_SIZE;
        else if(dst[1:0]!=0 || (op[1:0]!=1 && src[1:0]!=0)) error=`GFX_ERR_ALIGN;
        else if(ds[5:0]!=0 || {32'b0,ds}<({32'b0,w}<<2) ||
          (op[1:0]!=1 && (ss[5:0]!=0 || {32'b0,ss}<({32'b0,w}<<2)))) error=`GFX_ERR_STRIDE;
        else if({dst[31:4],4'b0}<pool_start || dr>{32'b0,pool_end} ||
          (op[1:0]!=1 && ({src[31:4],4'b0}<pool_start || sr>{32'b0,pool_end}))) error=`GFX_ERR_RANGE;
        else if(op[1:0]!=1 && {32'b0,src}<de && {32'b0,dst}<se) error=`GFX_ERR_OVERLAP;
        else if((dst<protect0_end && {32'b0,protect0_start}<de) ||
                (dst<protect1_end && {32'b0,protect1_start}<de)) error=`GFX_ERR_PROTECTED;
    end
endmodule
