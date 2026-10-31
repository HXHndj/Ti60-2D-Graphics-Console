// Synchronous ready/valid FIFO; arbitrary depth including 1. No unaccepted access.
module gfx_fifo #(parameter WIDTH=32, DEPTH=16, PW=(DEPTH<2?1:$clog2(DEPTH)), REGISTER_STORAGE=0) (
    input wire clk, rst_n, clear,
    input wire in_valid, output wire in_ready, input wire [WIDTH-1:0] in_data,
    output wire out_valid, input wire out_ready, output wire [WIDTH-1:0] out_data,
    output reg [PW:0] count
);
    reg [PW-1:0] wp, rp;
    assign in_ready = count < DEPTH;
    assign out_valid = count != 0;
    wire push = in_valid && in_ready;
    wire pop = out_valid && out_ready;
    generate if (REGISTER_STORAGE) begin:words
        // Wide, shallow command queues otherwise waste many RAM10 blocks.
        // Explicit independently enabled words keep their storage in FFs.
        wire [DEPTH*WIDTH-1:0] data;
        genvar i;
        for(i=0;i<DEPTH;i=i+1)begin:word_reg
            reg [WIDTH-1:0] value;
            always @(posedge clk)if(rst_n&&!clear&&push&&wp==i)value<=in_data;
            assign data[i*WIDTH+:WIDTH]=value;
        end
        assign out_data=data[rp*WIDTH+:WIDTH];
    end else begin:ram
        reg [WIDTH-1:0] mem [0:DEPTH-1];
        always @(posedge clk)if(rst_n&&!clear&&push)mem[wp]<=in_data;
        assign out_data=mem[rp];
    end endgenerate
    always @(posedge clk) begin
        if (!rst_n || clear) begin wp<=0; rp<=0; count<=0; end
        else begin
            if (push) wp<=(wp==DEPTH-1)?0:wp+1'b1;
            if (pop) rp <= (rp==DEPTH-1)?0:rp+1'b1;
            case ({push,pop})
                2'b10: count<=count+1'b1;
                2'b01: count<=count-1'b1;
                default: count<=count;
            endcase
        end
    end
endmodule
