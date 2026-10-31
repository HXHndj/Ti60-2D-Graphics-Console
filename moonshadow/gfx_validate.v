`include "gfx_abi.vh"
// Whole command check before enqueue; 64-bit extended spans prevent aliasing.
module gfx_validate(input wire [`GFX_CMD_BITS-1:0] cmd,
 input wire [31:0] pool_start,pool_end,protect0_start,protect0_end,protect1_start,protect1_end,
 output reg [7:0] error);
    wire [31:0] op=cmd[0+:32],src=cmd[32+:32],dst=cmd[64+:32];
    wire [31:0] w=cmd[96+:32],h=cmd[128+:32],ss=cmd[160+:32],ds=cmd[192+:32];
    wire [31:0] a=cmd[256+:32],k=cmd[288+:32];
    wire tile_dirty=a[31],tile_list=&a[31:30];
    wire [31:0] tile_list_addr={2'b0,a[29:0]};
    wire [15:0] tile_list_count=k[15:0];
    wire [16:0] tile_list_beats=({1'b0,tile_list_count}+17'd3)>>2;
    wire [32:0] tile_list_end={1'b0,tile_list_addr}+{12'b0,tile_list_beats,4'b0};
    wire [15:0] dirty_x=a[15:0],dirty_w=k[15:0],dirty_h=k[31:16];
    wire [14:0] dirty_y=a[30:16];
    wire [16:0] dirty_right={1'b0,dirty_x}+{1'b0,dirty_w};
    wire [16:0] dirty_bottom={2'b0,dirty_y}+{1'b0,dirty_h};
    // Nonempty legal dimensions are <=4096. Reject larger values below before
    // using these spans. 13x32 products plus a 32-bit address fit in 45 bits;
    // avoid the 64-bit h-1 borrow chain while still detecting 32-bit overflow.
    wire [12:0] hm1=h[12:0]-13'd1;
    wire [44:0] se={13'b0,src}+hm1*{13'b0,ss}+{30'b0,w[12:0],2'b0};
    wire [44:0] de={13'b0,dst}+hm1*{13'b0,ds}+{30'b0,w[12:0],2'b0};
    wire [44:0] se_pad=se+45'd15,de_pad=de+45'd15;
    wire [44:0] sr={se_pad[44:4],4'b0},dr={de_pad[44:4],4'b0};
    wire [44:0] tile_desc_end={13'b0,src}+45'd256;
    wire [44:0] list_end={13'b0,src}+({13'b0,a}<<4);
    always @* begin
        error=0;
        if(op==`GFX_OP_TILE4)begin
         if(w==0||h==0||w>4096||h>4096)error=`GFX_ERR_SIZE;
         else if(tile_list&&(!tile_list_count||tile_list_count>256||k[31:16]!=0||
          tile_list_addr[3:0]!=0||tile_list_addr<pool_start||tile_list_end>{1'b0,pool_end}))error=`GFX_ERR_SIZE;
         else if(tile_dirty&&!tile_list&&(dirty_w==0||dirty_h==0||dirty_x[3:0]!=0||dirty_y[3:0]!=0||
          dirty_right>{1'b0,w[15:0]}||dirty_bottom>{1'b0,h[15:0]}||
          (dirty_w[3:0]!=0&&dirty_right!={1'b0,w[15:0]})||
          (dirty_h[3:0]!=0&&dirty_bottom!={1'b0,h[15:0]})))error=`GFX_ERR_SIZE;
         else if(!tile_dirty&&(a!=0||k!=0))error=`GFX_ERR_OPCODE;
         else if(src[3:0]!=0||dst[1:0]!=0)error=`GFX_ERR_ALIGN;
         else if(ss<1||ss>8||ds[5:0]!=0||{32'b0,ds}<({32'b0,w}<<2))error=`GFX_ERR_STRIDE;
         else if(src<pool_start||tile_desc_end>{32'b0,pool_end}||{dst[31:4],4'b0}<pool_start||dr>{32'b0,pool_end})error=`GFX_ERR_RANGE;
         else if({32'b0,src}<de&&{32'b0,dst}<tile_desc_end)error=`GFX_ERR_OVERLAP;
         else if((dst<protect0_end&&{32'b0,protect0_start}<de)||(dst<protect1_end&&{32'b0,protect1_start}<de))error=`GFX_ERR_PROTECTED;
        end else if(op==`GFX_OP_FILL_LIST)begin
         if(w!=1024||h!=600||a==0||a>256)error=`GFX_ERR_SIZE;
         else if(src[3:0]!=0||dst[1:0]!=0)error=`GFX_ERR_ALIGN;
         else if(ss!=16||ds!=4096||cmd[224+:32]!=0||k!=0)error=`GFX_ERR_STRIDE;
         else if(src<pool_start||list_end>{32'b0,pool_end}||{dst[31:4],4'b0}<pool_start||dr>{32'b0,pool_end})error=`GFX_ERR_RANGE;
         else if({32'b0,src}<de&&{32'b0,dst}<list_end)error=`GFX_ERR_OVERLAP;
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
