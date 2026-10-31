// Three global-enable stages, one pixel/cycle. Backpressure freezes data and tags.
module gfx_pixel_pipe (
    input wire clk,rst_n, input wire in_valid,output wire in_ready,
    input wire [31:0] src,dst,color,op_flags,
    input wire [7:0] global_alpha,input wire [23:0] key_rgb,input wire in_last,
    output wire out_valid,input wire out_ready,
    output reg [31:0] out_pixel,output reg out_write,output reg out_last
);
    reg [2:0] valid;
    wire ce = !valid[2] || out_ready;
    assign in_ready=ce; assign out_valid=valid[2];
    reg [31:0] s0,d0,c0;reg [7:0] a0;reg [1:0] op0;
    reg k0,l0,k1,l1; reg [31:0] direct1;reg [1:0] op1;
    reg [16:0] r1,g1,b1;
    // Exact floor(x/255) for 0 <= x <= 65152 (tested exhaustively).
    function [7:0] div255;
        input [16:0] x;
        reg [17:0] t;
        begin t={1'b0,x}+18'd1+({1'b0,x}>>8); div255=t>>8; end
    endfunction
    wire [15:0] amul=src[31:24]*global_alpha;
    always @(posedge clk) begin
        if(!rst_n) begin valid<=0;out_pixel<=0;out_write<=0;out_last<=0;end
        else if(ce) begin
            valid<={valid[1:0],in_valid};
            s0<=src;d0<=dst;c0<=color;op0<=op_flags[1:0];l0<=in_last;
            k0<=(op_flags[1:0]!=1) && op_flags[8] && src[23:0]==key_rgb;
            a0<=op_flags[9]?div255({1'b0,amul}+17'd127):global_alpha;
            r1<=s0[23:16]*a0+d0[23:16]*(8'd255-a0)+17'd127;
            g1<=s0[15:8]*a0+d0[15:8]*(8'd255-a0)+17'd127;
            b1<=s0[7:0]*a0+d0[7:0]*(8'd255-a0)+17'd127;
            direct1<=(op0==1)?{8'hff,c0[23:0]}:{8'hff,s0[23:0]};
            op1<=op0;k1<=k0;l1<=l0;
            out_pixel<=(op1==3)?{8'hff,div255(r1),div255(g1),div255(b1)}:direct1;
            out_write<=!k1;out_last<=l1;
        end
    end
endmodule
