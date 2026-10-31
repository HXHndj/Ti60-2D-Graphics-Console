// Synchronous ready/valid FIFO; arbitrary depth including 1. No unaccepted access.
module gfx_fifo #(parameter WIDTH=32, DEPTH=16, PW=(DEPTH<2?1:$clog2(DEPTH)), REGISTER_STORAGE=0, SYNC_RAM=0) (
    input wire clk, rst_n, clear,
    input wire in_valid, output wire in_ready, input wire [WIDTH-1:0] in_data,
    output wire out_valid, input wire out_ready, output wire [WIDTH-1:0] out_data,
    output reg [PW:0] count
);
    generate if (SYNC_RAM) begin:sync_ram_fifo
        if (DEPTH == 1) begin:single_entry
            // A one-entry FIFO has no useful memory queue; use one elastic
            // output register with same-cycle replacement when it is popped.
            reg [WIDTH-1:0] head_data;
            wire push, pop;
            assign in_ready = !clear && ((count < DEPTH) || pop);
            assign out_valid = !clear && count != 0;
            assign out_data = head_data;
            assign push = in_valid && in_ready;
            assign pop = out_valid && out_ready;
            always @(posedge clk) begin
                if (!rst_n || clear) count <= 0;
                else begin
                    if (push) head_data <= in_data;
                    case ({push,pop})
                        2'b10: count <= count + 1'b1;
                        2'b01: count <= count - 1'b1;
                        default: count <= count;
                    endcase
                end
            end
        end else begin:ring_buffer
            // Keep the oldest item in an output register. The remaining
            // DEPTH-1 entries form a one-slot-empty circular RAM queue. A
            // synchronous read is launched on each accepted output pop, so
            // RAM data replaces the current head immediately after the edge.
            // The reserved ring slot guarantees read/write addresses differ
            // even while both ports are active; total capacity remains DEPTH.
            reg [WIDTH-1:0] mem [0:DEPTH-1];
            reg [WIDTH-1:0] head_data;
            reg [WIDTH-1:0] ram_q;
            reg head_from_ram;
            reg [PW-1:0] wp, rp;
            reg [PW:0] mem_count;
            wire push, pop, mem_push, mem_pop, direct_load;

            assign in_ready = !clear && ((count < DEPTH) || pop);
            assign out_valid = !clear && count != 0;
            assign out_data = head_from_ram ? ram_q : head_data;
            assign push = in_valid && in_ready;
            assign pop = out_valid && out_ready;

            // An empty FIFO load and a pop of the sole buffered item are
            // handled directly by the head register. All other accepted
            // inputs enter the RAM ring.
            assign direct_load = push && ((count == 0) || (pop && mem_count == 0));
            assign mem_push = push && !direct_load;
            assign mem_pop = pop && (mem_count != 0);

            // Separate synchronous read and write ports. RAM contents are
            // deliberately not reset; clear only invalidates pointers/counts.
            always @(posedge clk)
                if (rst_n && !clear && mem_push) mem[wp] <= in_data;
            always @(posedge clk)
                if (rst_n && !clear && mem_pop) ram_q <= mem[rp];

            always @(posedge clk) begin
                if (!rst_n || clear) begin
                    wp <= 0;
                    rp <= 0;
                    mem_count <= 0;
                    count <= 0;
                    head_from_ram <= 0;
                end else begin
                    if (direct_load) begin
                        head_data <= in_data;
                        head_from_ram <= 0;
                    end else if (mem_pop) begin
                        // ram_q is updated by the synchronous read at this
                        // same edge and becomes the visible head after it.
                        head_from_ram <= 1;
                    end else if (pop) begin
                        head_from_ram <= 0;
                    end

                    if (mem_push) wp <= (wp == DEPTH-1) ? 0 : wp + 1'b1;
                    if (mem_pop) rp <= (rp == DEPTH-1) ? 0 : rp + 1'b1;
                    case ({mem_push,mem_pop})
                        2'b10: mem_count <= mem_count + 1'b1;
                        2'b01: mem_count <= mem_count - 1'b1;
                        default: mem_count <= mem_count;
                    endcase
                    case ({push,pop})
                        2'b10: count <= count + 1'b1;
                        2'b01: count <= count - 1'b1;
                        default: count <= count;
                    endcase
                end
            end
        end
    end else begin:legacy_fifo
        reg [PW-1:0] wp, rp;
        assign in_ready = count < DEPTH;
        assign out_valid = count != 0;
        wire push = in_valid && in_ready;
        wire pop = out_valid && out_ready;
        if (REGISTER_STORAGE) begin:words
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
        end
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
    end endgenerate
endmodule
