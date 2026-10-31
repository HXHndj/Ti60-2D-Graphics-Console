module display_timing #(
    parameter H_ACTIVE=640,H_FRONT=16,H_SYNC=96,H_BACK=48,
    parameter V_ACTIVE=480,V_FRONT=10,V_SYNC=2,V_BACK=33,
    parameter ALIGN_VSYNC=0
)(input wire clk,rst_n, output reg [11:0] x,y,
  output wire de,hsync_n,vsync_n,frame_start,vblank_start);
    localparam HT=H_ACTIVE+H_FRONT+H_SYNC+H_BACK;
    localparam VT=V_ACTIVE+V_FRONT+V_SYNC+V_BACK;
    assign de=(x<H_ACTIVE && y<V_ACTIVE);
    assign hsync_n=!(x>=H_ACTIVE+H_FRONT && x<H_ACTIVE+H_FRONT+H_SYNC);
    reg aligned_vs;
    // x=0 is the first active pixel, not the start of HS. In aligned mode,
    // VS changes at the HS leading edge preceding the corresponding line.
    // This preserves V_BACK whole lines plus H_SYNC+H_BACK before active DE.
    always @(posedge clk) begin
        if (!rst_n) aligned_vs<=1'b1;
        else if(x==H_ACTIVE+H_FRONT-1)
            aligned_vs<=!(y>=V_ACTIVE+V_FRONT-1 && y<V_ACTIVE+V_FRONT+V_SYNC-1);
    end
    assign vsync_n=ALIGN_VSYNC?aligned_vs:!(y>=V_ACTIVE+V_FRONT && y<V_ACTIVE+V_FRONT+V_SYNC);
    assign frame_start=(x==0 && y==0);
    assign vblank_start=(x==0 && y==V_ACTIVE);
    always @(posedge clk) begin
        if (!rst_n) begin x<=0;y<=0;end
        else if(x==HT-1) begin x<=0;y<=(y==VT-1)?0:y+1'b1;end
        else x<=x+1'b1;
    end
endmodule
