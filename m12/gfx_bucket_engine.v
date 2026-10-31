`include "gfx_abi.vh"
// R6 bucket composite engine. One command walks a 32x32 screen bucket grid.
// For each bucket whose reference list is non-empty it reads back the
// framebuffer work block (INIT_FB), blends the bucket's sprite references in
// original draw order through the shared four-lane pixel pipe, and writes the
// block back once. Empty buckets are skipped untouched. Sprite sources stream
// through the shared gfx_source_reader; the bucket directory, reference
// lists, and 32-byte sprite descriptors live in DDR and are read by an
// internal control DMA. Per-pixel semantics (integer rounding, Key keeps the
// target, A=0 still writes back, non-Key A forced to FF) are the legacy
// engine's, because the same gfx_pixel_pipe lanes run over the same
// sequential per-pixel stream that per-sprite legacy commands produce.
module gfx_bucket_engine #(parameter TIMEOUT_CYCLES=10000000,LIST_MAX=64,SPRITE_MAX=4096,SHARED_SOURCE_ROW=0,SHARED_PIXEL_PIPE=0,SPRITE_ROW_PREFETCH=0,RCOPY_ROW_PREFETCH=0,SOURCE_HALF_TRIM=0)(
 input wire clk,rst_n,input wire cmd_valid,output wire cmd_ready,input wire [`GFX_CMD_BITS-1:0] cmd,
 output reg done,output reg [31:0] done_seq,output reg [7:0] fault,output reg [31:0] fault_addr,
 output wire busy,output wire recover_safe,
 output wire [31:0] araddr,output wire [7:0] arlen,output wire [2:0] arsize,output wire [1:0] arburst,
 output wire arvalid,input wire arready,input wire [127:0] rdata,input wire [1:0] rresp,input wire rlast,rvalid,output wire rready,
 // Shared source reader, same contract as gfx_engine.
 output wire src_req_valid,input wire src_req_ready,
 output wire [31:0] src_req_addr,output wire [4:0] src_req_beats,
 input wire [127:0] rd_data,input wire rd_valid,input wire rd_done,input wire [7:0] rd_error,
 input wire src_busy,input wire cache_serving,
 input wire [31:0] src_araddr,input wire [7:0] src_arlen,input wire [2:0] src_arsize,
 input wire [1:0] src_arburst,input wire src_arvalid,output wire src_arready,
 output wire src_rbeat_valid,input wire src_rready,
 input wire display_urgent,input wire cache_enable,cache_flush,
 // M3b-1 shared group backend client interface (see gfx_group_backend).
 // Bank layout is linear: {row[4:0],half,word[2:0]} - row pitch 16 words,
 // the group's left bucket in words 0-7 of each row, right in 8-15.
 output wire grp_begin,input wire grp_ready,
 output wire [8:0] fill_waddr,output wire [127:0] fill_wdata,output wire fill_we,
 output wire [8:0] fill_raddr,input wire [127:0] fill_rdata,
 output wire grp_commit,
 output wire [31:0] desc_base,output wire [31:0] desc_stride,
 output wire [5:0] desc_rows,output wire [4:0] desc_beats_m1,
 output wire [7:0] desc_width,output wire desc_pitch32,
 output wire grp_flush,
 input wire grp_idle,input wire [7:0] grp_error,input wire [31:0] grp_error_addr,
 output wire source_row_wr_en,output wire [3:0] source_row_wr_addr,
 output wire [127:0] source_row_wr_data,output wire [3:0] source_row_rd_addr,
 input wire [127:0] source_row_rd_data,
 output wire pixel_rst_n,pixel_in_valid,pixel_out_ready,pixel_in_last,
 output wire [127:0] pixel_src,pixel_dst,
 output wire [31:0] pixel_color,pixel_op,
 output wire [7:0] pixel_alpha,output wire [23:0] pixel_key,
 input wire [3:0] pixel_ready,pixel_valid,pixel_write,pixel_last,
 input wire [127:0] pixel_result
);
 localparam IDLE=0,DIR_REQ=1,DIR_WAIT=2,LIST_REQ=3,LIST_WAIT=4,WB_LOAD_REQ=5,WB_LOAD_WAIT=6,
  DESC_REQ=7,DESC_WAIT=8,ROW_SRC_REQ=9,ROW_SRC_WAIT=10,BLEND=11,WB_STORE_REQ=12,WB_STORE_RUN=13,
  WB_STORE_WAIT=14,NEXT_BUCKET=15,FINISH=16,FAULT_DRAIN=17,
  // Recipe-init phase (alpha word = recipe package base, 0 = INIT_FB).
  RHDR_REQ=18,RHDR_WAIT=19,RIDX_REQ=20,RIDX_WAIT=21,RLIST_REQ=22,RLIST_WAIT=23,
  ROP_REQ=24,ROP_WAIT=25,RFILL_FAST=26,RWM_SET=27,RWM_WRITE=28,RCOPY_REQ=29,RCOPY_WAIT=30,
  RBASE=31,
  // REPEAT support: texture row loads, post-load dispatch, fast full-width
  // generation. State space is 6 bits now.
  RTEX_REQ=32,RTEX_WAIT=33,RTEX_DONE=34,RREP_FAST=35,
  // Group write-back commit point: production hands the filled bank to the
  // shared group backend (gfx_group_backend) and moves on; the backend's
  // drain streams the bank out overlapped with the next group's production.
  GRP_COMMIT=36,RCOPY_PRIME=37,RCOPY_WINDOW=38,RCOPY_PWAIT=39;
 reg [5:0] state;
 wire fault_pending;
 reg empty_load;
 reg [31:0] src,dst,width,height,ss,ds,color,alpha,seq;
 reg recipe_mode;reg [31:0] r_idx_off,r_base_color;
 reg [15:0] rseg;reg [6:0] rcount,rop_idx;
 reg [7:0] ro_kind;reg [31:0] ro_a,ro_ss,ro_xy,ro_wh;
 reg [11:0] rix0,riy0,riy1;reg rfull;
 reg [11:0] rcursor;reg [3:0] rbeats;reg [1:0] cbase;reg copy_negative;
 // REPEAT: expanded-pattern cache. The asset holds the 16x3-expanded source
 // (16 distinct rows x 48 px, 256B stride), cached in a 1R1W synchronous RAM
 // so nothing wider than the RAM port is built in logic. Generation is a pure
 // address stream: work words are phase-word aligned, so each maps to exactly
 // one cached pattern word. Provenance tag skips reloads; row/word/phase
 // registers advance by increment-wrap only (no dividers).
 reg tcache_valid;reg [31:0] tcache_base;
 reg [3:0] tex_row;reg [5:0] rpx0,rpy;reg [3:0] tword,trow;reg [5:0] bmod48,rowmod48;
 wire [127:0] texp_out;reg [127:0] texp_wdata;reg [7:0] texp_waddr;reg texp_we;wire [7:0] texp_raddr;
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(8),.OUTPUT_REG("TRUE")) texp_ram(
  .wdata(texp_wdata),.waddr(texp_waddr),.raddr(texp_raddr),.we(texp_we),
  .wclk(clk),.re(1'b1),.rclk(clk),.rdata(texp_out));
 reg [2:0] rwm_w;reg [127:0] rwm_word;reg [3:0] rwm_mask;
 reg [8:0] rfill_cnt,rfill_end,rwm_addr;
 reg [2:0] rwf,rwl;reg [6:0] rlo,rhi;
 // Row-address accumulators: per-row 12..17b x 32b multiply-add trees are
 // replaced by per-op 5b x 32b initialisers plus += stride advances.
 // frame_base tracks dst+by0*ds incrementally (band advance adds 32*ds).
 reg [31:0] frame_base,load_row_addr,src_row_addr,rcopy_row_addr;
 reg [13:0] bucket,bucket_total;
 reg [7:0] grid_cols,grid_rows,bucket_col,bucket_row;
 reg [11:0] bx0,by0;
 reg [15:0] dir_offset,dir_count;
 // List storage shape is a P0 correctness constraint, not style. Two mapper
 // hazards are banned outright: (a) the old 64x16 unpacked array wrote eight
 // dynamic addresses per beat and mis-mapped (48/64 items read 0); (b) an
 // 8x128 unpacked array maps to EFX_RAM10 with a REGISTERED read plus bypass
 // retiming (observed 2026-10-12), changing the async-read timing contract
 // the FSM depends on. So the beats are eight plain 128-bit registers -
 // no unpacked array exists for RAM inference - written one per DMA beat
 // through an explicit case, read combinationally through explicit muxes.
 // Fixed depth 8 == LIST_MAX/8 (LIST_MAX must stay 64). Like the old array,
 // beats are never cleared: every list load rewrites the beats its count
 // spans, stale tails stay unreachable.
 reg [127:0] list_store_0,list_store_1,list_store_2,list_store_3,
  list_store_4,list_store_5,list_store_6,list_store_7;
 reg [6:0] ref_idx;
 reg [31:0] d_src,d_x,d_y,d_w,d_ss,d_ak;
 reg source_upper_half;
 reg [4:0] source_beats;
 reg d_solid,d_pad24;
 reg [127:0] desc_beat0,desc_beat1;
 reg [11:0] ix0,ix1,iy1;
 reg [6:0] lo,hi;
 reg [11:0] row_cursor;
 reg [4:0] wr_row;
 // Out-side row tracking: with continuous row feed the next row's inputs
 // enter the pipe while the previous row's outputs still drain, so the
 // work-block write row follows the output stream, not wr_row.
 reg [4:0] out_row;
 reg [5:0] out_rows_left;
 reg ref_drain_done;
 // Next-reference descriptor prefetch: the ctrl DMA sits idle while a
 // reference's last row blends, so the following descriptor's two-beat
 // fetch overlaps that drain instead of serialising after it.
 reg [127:0] desc_pre_beat0,desc_pre_beat1;
 reg desc_pre_pending,desc_pre_valid;
 reg desc_pre_cnt;
 // Atlas row replay cache: 256 slots, each one 128B source row region (8
 // words) plus a tag entry {full region address, per-granule filled bits}.
 // The index hashes address bits [14:7]; collisions simply miss (full-tag
 // compare). A hit skips the source-reader roundtrip - the measured
 // per-sprite floor - and the blend window reads the cache RAM with the
 // same presented-address timing as the row RAMs. Misses fill
 // write-through; a fill ORs its granule bit into a same-region slot so
 // mixed-trim regions stay conservative. Reset and the driver's source-cache
 // flush sweep tags before cmd_ready opens, including CPU source rewrites.
 reg [26:0] rowc_fill_old;
 reg [9:0] rowc_clear;
 reg rowc_row;
 reg rowc_cold_check;
 reg rowc_cold_eval;
 reg pf_check;
 reg pf_issue;
 reg [31:0] pf_req_addr; // latched at issue: a row advance must not retarget the held request
 reg pf_in_cache;
 reg rowc_fill_armed;
 reg rowc_fill_is_pf;
 reg [24:0] rowc_fill_tag_q;
 reg [7:0] rowc_fill_idx,rowc_idx_cur,rowc_idx_next;
 reg [2:0] wfirst,wlastidx;
 reg [5:0] wb_row,rows_valid;
 reg [2:0] ld_cnt;reg [3:0] rx_cnt;
 // 32x32 ARGB work block, one 128-bit word per four pixels. The block lives
 // in the shared group backend's fill bank; production addresses are the
 // original 8-bit work addresses {row[4:0],word[2:0]}, linearised into the
 // bank as {row,half,word} (row pitch 16 words) by prefixing the current
 // bucket's column parity at bit 3. Reads come back through fill_rdata
 // with the same synchronous-RAM latency the internal bank had.
 reg [8:0] wr_addr;
 wire [127:0] w_rdata;
 reg [8:0] wk_waddr;reg [127:0] wk_wdata;reg wk_we;
 wire [8:0] wk_raddr;
 assign fill_waddr={wk_waddr[7:3],bucket_col[0],wk_waddr[2:0]};
 assign fill_raddr={wk_raddr[7:3],bucket_col[0],wk_raddr[2:0]};
 assign fill_we=wk_we&&!fault_pending;
 assign fill_wdata=wk_wdata;
 assign w_rdata=fill_rdata;
 // Sprite/source row staging: see the srow_ram instance below (declared
 // after the DMA signals it references).
 // Blend feed/output tracking and the three-stage background delay line that
 // keeps each output word aligned with the work word it must preserve.
 reg [3:0] issue_w,out_cnt;
 reg [127:0] bd1,bd2,bd3;
 reg [127:0] ctrl_beat;
 wire [31:0] dir_word=ctrl_beat[32*bucket[1:0]+:32];
 wire [31:0] ridx_word=ctrl_beat[32*bucket[1:0]+:32];
 wire [15:0] ro_x=ro_xy[15:0],ro_y=ro_xy[31:16];
 wire [15:0] ro_w=ro_wh[15:0],ro_h=ro_wh[31:16];
 wire [31:0] rhdr_color=ctrl_beat[95:64];
 reg [3:0] ctrl_beat_cnt;
 // Command decode and grid geometry from the raw command word.
 wire [31:0] c_op=cmd[0+:32],c_w=cmd[96+:32],c_h=cmd[128+:32],c_alpha=cmd[256+:32];
 wire [7:0] c_cols=c_w[12:5];
 wire [7:0] c_rows=(c_h[12:0]+13'd31)>>5;
 wire [7:0] cols=grid_cols;
 wire [11:0] bx1=bx0+12'd31;
 wire [12:0] by1_ext={1'b0,by0}+13'd31;
 wire [12:0] height13={1'b0,height[12:0]};
 wire [11:0] by1=(by1_ext>=height13)?{1'b0,height13-13'd1}:by1_ext[11:0];
 wire [12:0] rows_here_ext=height13-{1'b0,by0};
 wire [5:0] rows_here=(rows_here_ext>=13'd32)?6'd32:rows_here_ext[5:0];
// Width is bucket aligned, so every work row is a full 32 px (8 beats); the
 // vertical remainder only limits how many rows the bucket owns.
 wire [8:0] row_beats=9'd8;
 wire [31:0] wb_row_addr=dst+({1'b0,by0}+{7'b0,wb_row})*ds+({1'b0,bx0}<<2);
 // Group geometry: the group's framebuffer byte base uses the left bucket's
 // even column; a group closes on its right bucket (odd column) or on the
 // final tail column of an odd-width grid. post_prod is where every
 // bucket-production completion goes: commit the bank (right/tail bucket)
 // or walk to the right bucket.
 wire [31:0] grp_base_addr=frame_base+({23'b0,bucket_col[7:1],1'b0}<<7);
 wire grp_commit_now=bucket_col[0]||({1'b0,bucket_col}+9'd1>={1'b0,grid_cols});
 wire [5:0] post_prod=grp_commit_now?GRP_COMMIT:NEXT_BUCKET;
 // Group protocol with the shared backend: enter a group's first bucket
 // (the EVEN column - groups are column pairs 2c,2c+1) only when the
 // backend can allocate a fill bank, and mark the allocation then. A level
 // begin is safe: the backend latches it once and the DIR_REQ stall holds
 // the engine here until the previous bank's drain has released it.
 wire grp_enter=state==DIR_REQ&&!bucket_col[0]&&!fault_pending;
 assign grp_begin=grp_enter;
 // Commit is the one-cycle GRP_COMMIT state. A fault can abandon a group
 // mid-fill (never committed); the command owner must hand the shared
 // backend back clean, so flush the leftover fill state when FAULT_DRAIN
 // has settled and defensively on command accept.
 assign grp_commit=state==GRP_COMMIT&&!fault_pending;
 wire dir_bank_stall=grp_enter&&!grp_ready;
 // Commit descriptor (valid during the one-cycle GRP_COMMIT state).
 assign desc_base=grp_base_addr;
 assign desc_stride=ds;
 assign desc_rows=rows_valid[5:0];
 assign desc_beats_m1=bucket_col[0]?5'd15:5'd7;
 assign desc_width=bucket_col[0]?8'd64:8'd32;
 assign desc_pitch32=1'b0;
 // Per-reference decode straight from the captured descriptor beats. During
 // BLEND a valid prefetched next-reference descriptor reroutes the decode
 // wires to its beats: mid-blend nothing consumes them, and the exit decode
 // (the only BLEND-time consumer) wants exactly the prefetched reference.
 wire desc_use_pre=desc_pre_valid&&state==BLEND;
 wire [127:0] db0_sel=desc_use_pre?desc_pre_beat0:desc_beat0;
 wire [127:0] db1_sel=desc_use_pre?desc_pre_beat1:desc_beat1;
 wire [31:0] v_x=db0_sel[95:64],v_y=db0_sel[127:96];
 wire [31:0] v_w=db1_sel[31:0],v_h=db1_sel[63:32],v_ss=db1_sel[95:64];
 // Legal sprite widths/heights are 1..32 (v_bad_desc is checked first),
 // and the command validator limits the canvas to 4096. Positions with
 // high bits set cannot intersect any bucket. Keep their rejection exact,
 // but use 13-bit inclusive endpoints instead of cascaded 33-bit add/sub.
 wire [5:0] v_wm1=v_w[5:0]-6'd1,v_hm1=v_h[5:0]-6'd1;
 wire [12:0] v_lastx={1'b0,v_x[11:0]}+{7'b0,v_wm1};
 wire [12:0] v_lasty={1'b0,v_y[11:0]}+{7'b0,v_hm1};
 wire v_empty=(|v_x[31:12])||(|v_y[31:12])||
  (v_x[11:0]>bx1)||(v_y[11:0]>by1)||(v_lastx<bx0)||(v_lasty<by0);
 wire [11:0] v_ix0=(v_x[11:0]<bx0)?bx0:v_x[11:0];
 wire [11:0] v_iy0=(v_y[11:0]<by0)?by0:v_y[11:0];
 wire [11:0] v_ix1=(v_lastx>{1'b0,bx1})?bx1:v_lastx[11:0];
 wire [11:0] v_iy1=(v_lasty>{1'b0,by1})?by1:v_lasty[11:0];
 // Async list reads, both consumers: beat word by index[5:3] through an
 // explicit mux, 16b lane by index[2:0]. No array read anywhere - see the
 // list_store shape note above.
 reg [127:0] rop_beat,ref_beat;
 always @*begin
  case(rop_idx[5:3])
   3'd0:rop_beat=list_store_0;3'd1:rop_beat=list_store_1;3'd2:rop_beat=list_store_2;
   3'd3:rop_beat=list_store_3;3'd4:rop_beat=list_store_4;3'd5:rop_beat=list_store_5;
   3'd6:rop_beat=list_store_6;default:rop_beat=list_store_7;
  endcase
 end
 always @*begin
  case(ref_idx[5:3])
   3'd0:ref_beat=list_store_0;3'd1:ref_beat=list_store_1;3'd2:ref_beat=list_store_2;
   3'd3:ref_beat=list_store_3;3'd4:ref_beat=list_store_4;3'd5:ref_beat=list_store_5;
   3'd6:ref_beat=list_store_6;default:ref_beat=list_store_7;
  endcase
 end
 wire [15:0] rop_base=rop_beat[{rop_idx[2:0],4'b0}+:16];
 wire [15:0] ref_item=ref_beat[{ref_idx[2:0],4'b0}+:16];
 // Next reference's list entry (combinational, same store regs) for the
 // descriptor prefetch and its validation.
 wire [6:0] ref_idx_n=ref_idx+7'd1;
 reg [127:0] ref_beat_n;
 always @*begin
  case(ref_idx_n[5:3])
   3'd0:ref_beat_n=list_store_0;3'd1:ref_beat_n=list_store_1;3'd2:ref_beat_n=list_store_2;
   3'd3:ref_beat_n=list_store_3;3'd4:ref_beat_n=list_store_4;3'd5:ref_beat_n=list_store_5;
   3'd6:ref_beat_n=list_store_6;default:ref_beat_n=list_store_7;
  endcase
 end
 wire [15:0] ref_item_n=ref_beat_n[{ref_idx_n[2:0],4'b0}+:16];
 wire [15:0] ref_item_sel=desc_use_pre?ref_item_n:ref_item;
 // syn_keep preserves the name in the mapped netlist so the list-readout
 // audit can cone-check the only consumer of ref_item bits 15:10.
 // Descriptor control word: bit0 key, bit1 pixel alpha, bit2 solid fill.
 // Solid sprites carry their opaque color in the source word (no texture
 // fetch), so the source 16B-alignment check only applies to textured ones.
 (* syn_keep = 1 *)
 // Descriptor bit3 opts into a fully allocated, 128B-aligned padded row.
 // Width remains24: only the source transfer expands to8 beats. Other legal
 // descriptors retain their old transfer and may not overread their allocation.
 wire source_pad24=db0_sel[3]&&!db0_sel[2]&&v_w==24&&db0_sel[38:32]==0&&v_ss[6:0]==0&&v_ss>=128;
 wire [4:0] src_row_beats=source_pad24?5'd8:(({v_w[5:0],2'b00}+7'd15)>>4);
 wire v_bad_desc=(db0_sel[31:4]!=0)||(db0_sel[3]&&!source_pad24)||(!db0_sel[2]&&(db0_sel[63:32]&32'hf)!=0)||
  (v_w==0)||(v_w>32)||(v_h==0)||(v_h>32)||(v_ss[5:0]!=0)||(v_ss<({v_w[5:0],2'b00}))||
  (ref_item_sel>=SPRITE_MAX);
 wire [31:0] desc_op=db0_sel[2]?32'd1:(32'd3|(db0_sel[0]?`GFX_FLAG_KEY:32'd0)|(db0_sel[1]?`GFX_FLAG_PIXEL_ALPHA:32'd0));
 // Read only the aligned 64B half containing the bucket's entire source
 // intersection. Keep its original logical RAM word positions (4..7 for
 // the upper half), so clipping, splice alignment and lane masks are unchanged.
 // Restrict this fast path to aligned full-size rows; all other descriptors
 // retain the original transfer, including narrow/non-cacheable sources.
 wire [5:0] source_clip_lo=v_ix0[5:0]-v_x[5:0];
 wire [5:0] source_clip_hi=v_ix1[5:0]-v_x[5:0];
 wire source_can_trim=SOURCE_HALF_TRIM&&v_w==32&&db0_sel[38:32]==0&&db1_sel[70:64]==0;
 wire source_trim_upper=source_can_trim&&source_clip_lo>=16;
 wire source_trim_lower=source_can_trim&&source_clip_hi<16;
 // Control DMA request per phase (address is 16B aligned by construction).
 // The BLEND-phase entry is the next reference's descriptor prefetch: it
 // fires on the last feed token of a reference's last row, when the ctrl
 // port is otherwise idle until the state exits.
 wire desc_prearm=state==BLEND&&!fault_pending&&row_cursor==iy1&&
  ({1'b0,ref_idx}+7'd1<{1'b0,dir_count})&&!desc_pre_pending&&!desc_pre_valid&&
  (issue_w==({1'b0,wlastidx}+4'd1));
 reg [31:0] ctrl_addr;
 reg [8:0] ctrl_beats;
 wire tex_need=!(tcache_valid&&(tcache_base==ro_a));
 wire [4:0] rl_beats=(rcount+4'd7)>>3;
 wire [3:0] list_beats=({dir_count[6:0],1'b0}+5'd15)>>4;
 always @*begin
  ctrl_addr=ss+{bucket[13:2],4'b0000};ctrl_beats=9'd1;
  if(desc_prearm)begin ctrl_addr=src+{ref_item_n[11:0],5'b00000};ctrl_beats=9'd2;end
  else if(state==LIST_REQ)begin ctrl_addr=color+{dir_offset[15:0],8'b00000000};ctrl_beats={5'b0,list_beats};end
  else if(state==DESC_REQ)begin ctrl_addr=src+{ref_item[11:0],5'b00000};ctrl_beats=9'd2;end
  else if(state==RHDR_REQ)begin ctrl_addr=alpha;ctrl_beats=9'd1;end
  else if(state==RIDX_REQ)begin ctrl_addr=alpha+r_idx_off+{bucket[13:2],4'b0000};ctrl_beats=9'd1;end
  // Lists sit past the largest possible index directory (1024 buckets x16B)
  // so dir and lists can never overlap at production grid sizes.
  else if(state==RLIST_REQ)begin ctrl_addr=alpha+r_idx_off+32'd16384+{rseg[15:0],8'b00000000};ctrl_beats={4'b0,rl_beats};end
  else if(state==ROP_REQ)begin ctrl_addr=alpha+32'd16+{rop_base[9:0],5'b00000};ctrl_beats=9'd2;end
  else if(state==RCOPY_REQ)begin ctrl_addr=rcopy_row_addr;ctrl_beats={5'b0,rbeats};end
  else if(state==RTEX_REQ&&tex_need)begin ctrl_addr=ro_a+{tex_row,8'b0};ctrl_beats=9'd12;end
 end
 wire ctrl_req_ready,ctrl_done,ctrl_busy,ctrl_valid;wire [7:0] ctrl_error;wire [127:0] ctrl_data;
 wire [31:0] ctrl_araddr;wire [7:0] ctrl_arlen;wire [2:0] ctrl_arsize;wire [1:0] ctrl_arburst;
 wire ctrl_arvalid,ctrl_rready;
 wire wb_req_ready,wb_done,wb_busy,wb_valid;wire [7:0] wb_error;wire [127:0] wb_data;
 wire [31:0] wb_araddr;wire [7:0] wb_arlen;wire [2:0] wb_arsize;wire [1:0] wb_arburst;
 wire wb_arvalid,wb_rready;
 // RCOPY_ROW_PREFETCH (m12 candidate, default off): recipe COPY rows
 // ping-pong between the shared row RAM (row 0, control DMA, unchanged) and
 // the prefetch RAM. While row n splices into the work bank, the WB-load DMA
 // - idle through the whole recipe phase - streams row n+1 into the other
 // bank, hiding the per-row control-DMA turnaround under the RWM window. The
 // sprite prefetch and this stage never overlap: recipes finish before the
 // first sprite reference's source request, and all rcopy state is per-op.
 // Declaration order is load-bearing for the same toolchain reason as the
 // prefetch RAM below: an earlier use turns these into 1-bit implicit nets.
 reg rc_bank;      // 1 = the row being spliced lives in the prefetch RAM
 reg wb_pend;      // WB-load DMA is fetching the next COPY row
 reg rc_pf_bank;   // destination bank of the in-flight row prefetch
 reg [3:0] rc_wbeat;
 // Stop launching producer work as soon as any in-flight read or shared
 // writer reports a fault. The DMAs remain enabled to finish accepted AXI.
 assign fault_pending=(fault!=0)||((ctrl_busy||ctrl_done)&&ctrl_error!=0)||
  ((wb_busy||wb_done)&&wb_error!=0)||((src_busy||rd_done)&&rd_error!=0)||(grp_error!=0);
 // M3b-1: the engine's AXI write side is gone; the shared group backend
 // owns the writer and reports its errors back through grp_error.
 wire ctrl_req=(state==DIR_REQ&&!dir_bank_stall)||state==LIST_REQ||state==DESC_REQ||state==RHDR_REQ||
  state==RIDX_REQ||state==RLIST_REQ||state==ROP_REQ||state==RCOPY_REQ||
  (state==RTEX_REQ&&tex_need)||desc_prearm;
 // Engine DMAs never run concurrently (the FSM issues one request at a time);
 // while either owns the read port the shared reader is held off, mirroring
 // the legacy background/source arbitration.
 wire engine_read_active=ctrl_busy||wb_busy;
 wire read_goes_ctrl=engine_read_active&&!wb_busy;
 wire read_goes_wb=engine_read_active&&wb_busy;
 gfx_read_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.FIFO_DEPTH(2),.FIFO_REGISTERS(1)) ctrl_dma(
  clk,rst_n,ctrl_req&&ctrl_req_ready&&!fault_pending,ctrl_req_ready,ctrl_addr,ctrl_beats,
  ctrl_araddr,ctrl_arlen,ctrl_arsize,ctrl_arburst,ctrl_arvalid,arready&&read_goes_ctrl,rdata,rresp,rlast,rvalid&&read_goes_ctrl,ctrl_rready,
  ctrl_data,ctrl_valid,1'b1,ctrl_done,ctrl_error,ctrl_busy);
 // Recipe-COPY row prefetch request: armed at each spliced row's PRIME (its
 // data is fully consumed by then) on the WB-load DMA, which is idle through
 // the whole recipe phase. First op rows and every fallback path keep the
 // original control-DMA RCOPY_REQ/WAIT flow unchanged.
 wire rc_pf_fire=RCOPY_ROW_PREFETCH&&state==RCOPY_PRIME&&ro_kind==1&&rcursor!=riy1&&
  !wb_pend&&!fault_pending;
 gfx_read_dma #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.FIFO_DEPTH(2),.FIFO_REGISTERS(1)) wb_reader(
  clk,rst_n,((state==WB_LOAD_REQ&&!fault_pending)||rc_pf_fire)&&wb_req_ready,wb_req_ready,
  rc_pf_fire?(rcopy_row_addr+ro_ss):load_row_addr,rc_pf_fire?{5'b0,rbeats}:row_beats,
  wb_araddr,wb_arlen,wb_arsize,wb_arburst,wb_arvalid,arready&&read_goes_wb,rdata,rresp,rlast,rvalid&&read_goes_wb,wb_rready,
  wb_data,wb_valid,1'b1,wb_done,wb_error,wb_busy);
 // M3b-1: the skid, drain FSM and writer live in gfx_group_backend. The
 // engine only drives the commit descriptor and observes the backend.
 // Sprite/source row staging as a 16x128b synchronous RAM (1 RAM10): the
 // old 10x128b unpacked array with asynchronous lane reads mapped to logic
 // (SYN-0657). BLEND's read address is a pure function of issue_w - the old
 // array index was spx[4:2] of a mod-128 wrapped offset, which is NOT a
 // linear +1/cycle stream when the sprite starts left of the bucket, so a
 // steering register cannot reproduce it. The window is {srow_prev,
 // srow_rdata} = the last two addressed words; the per-lane base is the
 // pipelined address srow_raddr_d. The recipe-copy splice is word-aligned
 // by construction (its x positions snap to 16B), so all four lanes read
 // the same word from a small steering register with a static shift.
 reg [127:0] srow_prev;
 wire [127:0] srow_rdata_raw,srow_local_data;
 wire [127:0] srow_rdata;
 wire srow_we_ctrl=state==RCOPY_WAIT&&ctrl_valid;
 wire srow_we_src=rd_valid;
 wire [5:0] srow_px0=({2'b0,issue_w}+4'd1)<<2;
 // The sprite-to-bucket x difference is taken mod 64 and sign-extended:
 // 7-bit slices would carry a mod-128 phantom (bx0>=64 buckets truncate)
 // and break the clamp. True difference is within +-31 on the intersect.
 wire [5:0] srow_dxy=d_x[5:0]-bx0[5:0];
 wire [7:0] srow_dxsext={{2{srow_dxy[5]}},srow_dxy};
 wire [7:0] srow_tofs={2'b0,srow_px0}-srow_dxsext;
 wire [3:0] srow_blend_raddr=srow_tofs[7]?4'd0:srow_tofs[5:2];
 // Outside BLEND (the row-load wait) present the first word's address so
 // the RAM pipeline is primed when consumption starts; issue_w is stale
 // there and would stream garbage into srow_prev.
 wire [7:0] srow_wait_tofs={2'b0,{1'b0,wfirst,2'b00}}-srow_dxsext;
 wire [3:0] srow_wait_raddr=srow_wait_tofs[7]?4'd0:srow_wait_tofs[5:2];
 // SPRITE_ROW_PREFETCH (m12 candidate, default off): while row n of a
 // textured reference blends, the source reader fetches row n+1 into a
 // private 16-word RAM, hiding the per-row cache-hit fetch latency (~22
 // cycles measured at 1024x600/540 sprites) under the ~17-cycle blend
 // issue window. Rows ping-pong: even rows stream through the shared row
 // RAM as before, odd rows land in the prefetch RAM; the left-shifted
 // sprite window really does present logical addresses 8..15 in the shared
 // RAM, so no free half exists there to double-buffer in. Both RAMs see
 // the same read address every cycle; a one-cycle-delayed select picks
 // which output feeds the blend window, matching the RAM's own one-cycle
 // read latency. All prefetch state is per-reference: no prefetch arms on
 // a reference's last row, so nothing survives into the next reference.
 reg pf_row_ram;   // 1 = the row being blended lives in the prefetch RAM
 reg pf_fetch_ram; // destination RAM of the armed/in-flight prefetch
 reg pf_pending;   // prefetch accepted, beats still in flight
 reg pf_ready;     // prefetch RAM holds the next row, blend may advance now
 // Prime window: after the final feed word issue_w parks at wlastidx+2 for
 // the three-cycle pipe drain; presenting the next row's first-word address
 // then reproduces the ROW_SRC_WAIT priming without leaving BLEND.
 wire pf_prime_now=state==BLEND&&(issue_w==({1'b0,wlastidx}+4'd2));
 wire pf_arm_ok=pf_pending||pf_ready;
 // Continuous row feed: at the last feed token of a non-final row whose
 // prefetch already landed, wrap straight through one pass-through token
 // instead of parking for the pipe drain. The pass token re-primes both
 // the srow window and the work-read address for the next row exactly as
 // the parked prime did, just without dead feed cycles; the out side
 // tracks rows independently through out_row/out_rows_left.
 wire cont_advance=SPRITE_ROW_PREFETCH&&state==BLEND&&!d_solid&&!fault_pending&&
  pf_ready&&row_cursor!=iy1&&(issue_w==({1'b0,wlastidx}+4'd1));
 // Read-target select, evaluated at address-presentation time: the row
 // currently blending (pf_row_ram), the row being primed for once its
 // prefetch exists (pf_fetch_ram), a late prefetch waited out in
 // ROW_SRC_WAIT (pf_fetch_ram again), or the continuous handoff (next
 // row's bank). On the handoff cycle the port that serves the next row
 // presents its first-word address, so that bank's data must be selected
 // on the following cycle regardless of which bank blended before.
 wire pf_sel_now=(state==BLEND&&cont_advance)?pf_fetch_ram:
  (state==BLEND&&!pf_prime_now&&pf_row_ram)||
  (state==BLEND&&pf_prime_now&&pf_arm_ok&&pf_fetch_ram)||
  (state==ROW_SRC_WAIT&&pf_pending&&pf_fetch_ram);
 reg pf_out_sel_q;
 always @(posedge clk)begin
  if(!rst_n)pf_out_sel_q<=1'b0;
  else pf_out_sel_q<=SPRITE_ROW_PREFETCH&&pf_sel_now;
 end
 wire pf_we_spr=SPRITE_ROW_PREFETCH&&srow_we_src&&pf_pending&&pf_fetch_ram;
 wire pf_we_rc=RCOPY_ROW_PREFETCH&&wb_pend&&rc_pf_bank&&wb_valid;
 wire pf_we=pf_we_spr||pf_we_rc;
 reg [3:0] srow_rc_raddr;
 // During the drain-prime window the next row's first-word address takes
 // over the read port; every consumed word of the finished row was already
 // latched into the pipe at its feed cycle, so retargeting is safe. The
 // continuous handoff presents the same first-word address one cycle
 // earlier - the window base registers (srow_raddr_d/d2) track this port,
 // so both RAMs must see the identical stream and pf_out_sel picks which
 // bank's output feeds the window on the following cycles.
 wire [3:0] srow_prime_addr=(pf_prime_now||cont_advance)?srow_wait_raddr:srow_blend_raddr;
 wire [3:0] srow_raddr_now=state==BLEND?srow_prime_addr:
  (state==ROW_SRC_WAIT||state==ROW_SRC_REQ)?srow_wait_raddr:srow_rc_raddr;
 // The prefetch RAM lives beside the shared one, addressed by the same read
 // stream. Declaration order is load-bearing: this toolchain turns a use
 // before the declaration into a 1-bit implicit net and the upper address
 // bits float (observed as Z in simulation).
 wire [127:0] pf_rdata;
 wire [3:0] sprite_row_waddr=rx_cnt[3:0]+(source_upper_half?4'd4:4'd0);
 generate if(SPRITE_ROW_PREFETCH||RCOPY_ROW_PREFETCH)begin:pf_stage
  gfx_source_row_ram pf_rows(clk,rst_n,pf_we,wb_pend?rc_wbeat:sprite_row_waddr,
   wb_pend?wb_data:rd_data,
   srow_raddr_now,pf_rdata);
 end else begin:pf_absent
  assign pf_rdata=128'b0;
 end endgenerate
 // Region keys and window mapping (declared here: they read wires that the
 // row/prefetch sections above define). Position p maps to region word
 // p[2:0]; positions >=8 are the +1-lookahead artifacts whose lanes are
 // never active, so their aliasing inside the region is harmless.
 wire [31:0] rowc_next_addr=src_row_addr+d_ss;
 wire [7:0] rowc_idx_cold=src_row_addr[14:7];
 wire [7:0] rowc_idx_pf=rowc_next_addr[14:7];
 wire [24:0] rowc_tag_cold=src_row_addr[31:7];
 wire [24:0] rowc_tag_pf=rowc_next_addr[31:7];
 wire [26:0] rowc_tag_ram_q;   // tag RAM output (instance below)
 wire [26:0] rowc_tag_q=rowc_tag_ram_q;
 // The replay window is region-relative. Only full-size aligned rows (or
 // their aligned TRIM halves) have that mapping; other legal sources retain
 // the ordinary reader path and must never publish a partial region as full.
 wire rowc_eligible=cache_enable&&!cache_flush&&(d_w==32||d_pad24)&&d_src[6:0]==0&&d_ss[6:0]==0;
 wire rowc_need_pair=(source_beats>5'd4);
 wire rowc_hit_cold=rowc_eligible&&rowc_cold_eval&&(rowc_tag_q[26:2]==rowc_tag_cold)&&
  ((rowc_need_pair)?(&rowc_tag_q[1:0]):(source_upper_half?rowc_tag_q[1]:rowc_tag_q[0]));
 wire rowc_hit_pf=rowc_eligible&&pf_check&&(rowc_tag_q[26:2]==rowc_tag_pf)&&
  ((rowc_need_pair)?(&rowc_tag_q[1:0]):(source_upper_half?rowc_tag_q[1]:rowc_tag_q[0]));
 wire rowc_prime=(cont_advance||pf_prime_now);
 wire [7:0] rowc_rd_region=rowc_prime?rowc_idx_next:
  (state==ROW_SRC_REQ||state==ROW_SRC_WAIT)?rowc_idx_cold:rowc_idx_cur;
 wire [10:0] rowc_rd_addr={rowc_rd_region,srow_raddr_now[2:0]};
 wire [10:0] rowc_waddr={rowc_fill_idx,(source_upper_half?3'd4:3'd0)+rx_cnt[2:0]};
 wire rowc_we=rd_valid&&rowc_fill_armed;
 wire [127:0] rowc_rdata;
 // One extra read port with the same one-cycle OUTPUT_REG latency as the
 // row RAMs, so the window mux is timing-equal.
 simple_dual_port_ram #(.DATA_WIDTH(128),.ADDR_WIDTH(11),.OUTPUT_REG("TRUE")) rowc_ram(
  .wdata(rd_data),.waddr(rowc_waddr),.raddr(rowc_rd_addr),.we(rowc_we),.wclk(clk),
  .re(1'b1),.rclk(clk),.rdata(rowc_rdata));
 // Recipe-COPY output select is combinational on the live bank register: the
 // splice consumes srow_rdata/srow_prev from PRIME's first cycle, so the
 // one-cycle-delayed sprite select register cannot serve this path. kind 0/2
 // RWM words never read the source row, so gating on ro_kind only narrows it.
 wire rcopy_series=state==RCOPY_PRIME||state==RCOPY_WINDOW||state==RWM_SET||
  state==RWM_WRITE||state==RCOPY_PWAIT;
 wire pf_sel_rc=RCOPY_ROW_PREFETCH&&rc_bank&&ro_kind==1&&rcopy_series;
 assign srow_rdata=rowc_row?rowc_rdata:
  (pf_sel_rc?pf_rdata:(SPRITE_ROW_PREFETCH&&pf_out_sel_q?pf_rdata:srow_rdata_raw));
 reg [3:0] srow_raddr_d,srow_raddr_d2;
 always @(posedge clk)begin
  if(!rst_n)begin srow_raddr_d<=4'd0;srow_raddr_d2<=4'd0;end
  else begin srow_raddr_d<=srow_raddr_now;srow_raddr_d2<=srow_raddr_d;end
 end
 wire srow_we_rc=RCOPY_ROW_PREFETCH&&wb_pend&&!rc_pf_bank&&wb_valid;
 assign source_row_wr_en=srow_we_ctrl||(srow_we_src&&!pf_we_spr)||srow_we_rc;
 assign source_row_wr_addr=srow_we_ctrl?ctrl_beat_cnt[3:0]:srow_we_rc?rc_wbeat:sprite_row_waddr;
 assign source_row_wr_data=srow_we_ctrl?ctrl_data:srow_we_rc?wb_data:rd_data;
 assign source_row_rd_addr=srow_raddr_now;
 assign srow_rdata_raw=SHARED_SOURCE_ROW?source_row_rd_data:srow_local_data;
 generate if(SHARED_SOURCE_ROW==0)begin:local_source_row_ram
  gfx_source_row_ram local_rows(clk,rst_n,source_row_wr_en,source_row_wr_addr,
   source_row_wr_data,source_row_rd_addr,srow_local_data);
 end else begin:external_source_row_ram
  assign srow_local_data=128'b0;
 end endgenerate
 assign araddr=engine_read_active?(wb_busy?wb_araddr:ctrl_araddr):src_araddr;
 assign arlen=engine_read_active?(wb_busy?wb_arlen:ctrl_arlen):src_arlen;
 assign arsize=engine_read_active?(wb_busy?wb_arsize:ctrl_arsize):src_arsize;
 assign arburst=engine_read_active?(wb_busy?wb_arburst:ctrl_arburst):src_arburst;
 assign arvalid=engine_read_active?(wb_busy?wb_arvalid:ctrl_arvalid):src_arvalid;
 assign rready=engine_read_active?(wb_busy?wb_rready:ctrl_rready):src_rready;
 assign src_arready=arready&&!engine_read_active;
 assign src_rbeat_valid=rvalid&&!engine_read_active;
 // Prefetch request: armed during a non-final textured row's blend. The
 // shared reader is idle then (the current row's fetch completed before
 // BLEND) and the ctrl DMA is idle between references, so the request never
 // collides with either owner of the read port.
 wire pf_arm=SPRITE_ROW_PREFETCH&&state==BLEND&&!d_solid&&!fault_pending&&
  !pf_pending&&!pf_ready&&!pf_check&&!pf_issue&&row_cursor!=iy1;
 // Prefetch arms resolve through the replay cache first: one tag-lookup
 // cycle, then either an instant ready (hit) or the reader request (miss).
 assign src_req_valid=(state==ROW_SRC_REQ&&!d_solid&&!fault_pending&&!rowc_cold_check&&!rowc_cold_eval)||pf_issue;
 assign src_req_addr=pf_issue?pf_req_addr:src_row_addr;
 wire [7:0] rowc_tag_raddr=pf_arm?rowc_idx_pf:
  (state==ROW_SRC_REQ)?rowc_idx_cold:8'd0;
 // Tag RAM: the array form mapped to LUT logic (XLR 103%); one RAM10 block
 // serves the same single-sync-read/single-write contract. Instantiated
 // after rowc_tag_raddr's declaration - port connections to later-declared
 // wires create implicit nets.
 wire rowc_tag_we=(rowc_clear<10'd256)||(rd_done&&rowc_fill_armed);
 wire [7:0] rowc_tag_waddr=(rowc_clear<10'd256)?rowc_clear[7:0]:rowc_fill_idx;
 wire [26:0] rowc_tag_wdata=(rowc_clear<10'd256)?27'd0:
  {rowc_fill_tag_q,
   ((rowc_fill_old[26:2]==rowc_fill_tag_q)&&rowc_fill_old[1])||source_upper_half||rowc_need_pair,
   ((rowc_fill_old[26:2]==rowc_fill_tag_q)&&rowc_fill_old[0])||!source_upper_half};
 simple_dual_port_ram #(.DATA_WIDTH(27),.ADDR_WIDTH(8),.OUTPUT_REG("TRUE")) rowc_tag_ram(
  .wdata(rowc_tag_wdata),.waddr(rowc_tag_waddr),.raddr(rowc_tag_raddr),.we(rowc_tag_we),.wclk(clk),
  .re(1'b1),.rclk(clk),.rdata(rowc_tag_ram_q));
 assign src_req_beats=source_beats;
 assign cmd_ready=state==IDLE&&rowc_clear>=10'd256&&!cache_flush;
 assign busy=state!=IDLE;
 wire grp_flush_now=state==FAULT_DRAIN&&!ctrl_busy&&!wb_busy&&!src_busy&&grp_idle;
 assign grp_flush=grp_flush_now||(state==IDLE&&cmd_valid);
 assign recover_safe=!busy&&!ctrl_busy&&!wb_busy&&!src_busy&&grp_idle;
 // Sprite pixel offset of the bucket's first column; |value|<=31 on intersect.
 wire [6:0] base_off=bx0[6:0]-d_x[6:0];
 // Four pixel lanes in lockstep; identical control keeps them aligned.
 wire [3:0] lane_ready,lane_valid,lane_write,lane_last;
 wire [127:0] blended_lanes;
 wire [3:0] feed_word0=issue_w-4'd1;
 wire blend_in_valid=state==BLEND&&(issue_w>{1'b0,wfirst})&&(issue_w<=({1'b0,wlastidx}+4'd1));
 wire blend_in_last=issue_w==({1'b0,wlastidx}+4'd1);
 reg [3:0] lane_active;
 reg [31:0] lane_src[0:3];
 integer p;reg [5:0] px;reg [6:0] spx;reg [2:0] srow_off;
 always @*begin
  for(p=0;p<4;p=p+1)begin
   px=feed_word0*4+p;
   lane_active[p]=(px>=lo)&&(px<=hi);
   spx=lane_active[p]?(px+base_off):7'd0;
   // The window base word is the address whose data sits in srow_prev,
   // which is the read address from two cycles back (srow_prev = rdata
   // delayed once, rdata = ram[raddr] delayed once); offset 0..7. Inactive
   // lanes pick an arbitrary slot; their pipe output is gated by out_act.
   srow_off={1'b0,spx[5:0]}-{srow_raddr_d2,2'b00};
   lane_src[p]=srow_off[2]?srow_rdata[{srow_off[1:0],5'b0}+:32]
    :srow_prev[{srow_off[1:0],5'b0}+:32];
  end
 end
 genvar lane;
 assign pixel_rst_n=rst_n&&state!=FAULT_DRAIN;
 assign pixel_in_valid=blend_in_valid;assign pixel_out_ready=1'b1;
 assign pixel_in_last=blend_in_last;assign pixel_dst=w_rdata;
 assign pixel_color=d_solid?d_src:32'd0;assign pixel_op=desc_op;
 assign pixel_alpha=d_ak[7:0];assign pixel_key=d_ak[31:8];
 generate if(SHARED_PIXEL_PIPE)begin:shared_pixels
  assign lane_ready=pixel_ready;assign lane_valid=pixel_valid;
  assign lane_write=pixel_write;assign lane_last=pixel_last;
  assign blended_lanes=pixel_result;
 end endgenerate
 generate for(lane=0;lane<4;lane=lane+1)begin:blend_lane
  assign pixel_src[lane*32+:32]=lane_src[lane];
  if(!SHARED_PIXEL_PIPE)begin:local_pixels
  gfx_pixel_pipe pipeline(clk,rst_n&&state!=FAULT_DRAIN,blend_in_valid,lane_ready[lane],lane_src[lane],w_rdata[lane*32+:32],pixel_color,desc_op,d_ak[7:0],d_ak[31:8],blend_in_last,
   lane_valid[lane],1'b1,blended_lanes[lane*32+:32],lane_write[lane],lane_last[lane]);
  end
 end endgenerate
 // Output-side word index and per-lane activity for the work-block merge.
 wire [3:0] out_word={1'b0,wfirst}+out_cnt;
 reg [3:0] out_act;
 always @*begin
  out_act=0;
  out_act[0]=((out_word*4)>=lo)&&((out_word*4)<=hi);
  out_act[1]=((out_word*4+1)>=lo)&&((out_word*4+1)<=hi);
  out_act[2]=((out_word*4+2)>=lo)&&((out_word*4+2)<=hi);
  out_act[3]=((out_word*4+3)>=lo)&&((out_word*4+3)<=hi);
 end
 wire [127:0] merged={(lane_write[3]&&out_act[3])?blended_lanes[96+:32]:bd3[96+:32],
  (lane_write[2]&&out_act[2])?blended_lanes[64+:32]:bd3[64+:32],
  (lane_write[1]&&out_act[1])?blended_lanes[32+:32]:bd3[32+:32],
  (lane_write[0]&&out_act[0])?blended_lanes[0+:32]:bd3[0+:32]};
 // Copy-op word splice: all four lanes read the same window word from the
 // synchronous RAM (recipe ops are 16B-aligned, csw[1:0]=lane), a static
 // per-lane shift; A is forced FF on write, matching the legacy COPY rule.
 // (Scratch names live at module scope: block-local declarations are
 // Verilog-2001 illegal.)
 reg [127:0] rcopy_splice;
 integer cp;reg [31:0] ctmp;reg [2:0] copy_slot;
 always @*begin
  for(cp=0;cp<4;cp=cp+1)begin
   copy_slot={1'b0,cp[1:0]}+{1'b0,cbase};
   ctmp=copy_slot[2]?srow_rdata[{copy_slot[1:0],5'b0}+:32]:srow_prev[{copy_slot[1:0],5'b0}+:32];
   rcopy_splice[cp*32+:32]={8'hff,ctmp[23:0]};
  end
 end

 wire pout_valid=lane_valid[0];
 wire [3:0] words_total={1'b0,wlastidx}-{1'b0,wfirst}+4'd1;
 wire row_blend_done=pout_valid&&(out_cnt==words_total-4'd1);
 wire [31:0] v_ak_full=db1_sel[127:96];
 // Recipe-op decode straight from the captured op beats: kind, color-or-src,
 // packed x/y/w/h, src stride. Reserved fields must stay zero in v1.
 wire [7:0] v_kind=desc_beat0[7:0];
 wire [31:0] v_ra=desc_beat0[63:32];
 wire [15:0] v_rx=desc_beat0[79:64],v_ry=desc_beat0[95:80];
 wire [15:0] v_rw=desc_beat0[111:96],v_rh=desc_beat0[127:112];
 wire [32:0] v_rxs={1'b0,{16'b0,v_rx}},v_rys={1'b0,{16'b0,v_ry}};
 wire [32:0] v_rxe=v_rxs+{1'b0,{16'b0,v_rw}},v_rye=v_rys+{1'b0,{16'b0,v_rh}};
 wire [5:0] v_phx=desc_beat0[13:8],v_phy=desc_beat0[19:14];
 wire v_rbad=(desc_beat0[31:20]!=0)||(v_kind>2)||(desc_beat1[127:32]!=0)||
  (v_kind==1&&((v_ra[1:0]!=0)||(desc_beat1[3:0]!=0)))||
  (v_kind==2&&((v_ra[7:0]!=0)||(desc_beat1[31:0]!=0)||(v_phx[1:0]!=0)));
 wire v_rempty=(v_rxs>{1'b0,bx1})||(v_rys>{1'b0,by1})||(v_rxe<=bx0)||(v_rye<=by0);
 wire [31:0] v_rix0=(v_rx<bx0)?bx0:{16'b0,v_rx};
 wire [31:0] v_riy0=(v_ry<by0)?by0:{16'b0,v_ry};
 wire [31:0] v_rix1=(v_rxe-1>{1'b0,bx1})?bx1:v_rxe-1;
 wire [31:0] v_riy1=(v_rye-1>{1'b0,by1})?by1:v_rye-1;
 wire v_rfull=(v_kind==0)&&(v_rx<=bx0)&&(v_rxe>bx1);
 wire v_rfull_rep=(v_kind==2)&&(v_rx<=bx0)&&(v_rxe>bx1);
 wire [31:0] v_coff4=(v_rix0-{16'b0,v_rx})<<2;
 // Recipe COPY initialiser: one multiply per op plus a += ro_ss advance per
 // row replaces the 12b x 32b multiply-add that ran every row. The y
 // difference must wrap in 16-bit arithmetic before widening (recipe ops may
 // be up to 64K rows tall - the sky COPY spans 600 - so unlike sprites the
 // offset is NOT bounded by the 32px bucket height). Difference offsets are
 // non-negative on the intersect path.
 wire [15:0] v_riy0_dy16=v_riy0[15:0]-v_ry[15:0];
 wire [31:0] rcopy_first_pixel=v_ra+{16'b0,v_riy0_dy16}*desc_beat1[31:0]+v_coff4;
 wire [31:0] rcopy_init={rcopy_first_pixel[31:4],4'b0000};
 wire [4:0] v_cbeats=(rcopy_first_pixel[3:0]+((v_rix1-v_rix0+1)<<2)+5'd15)>>4;
 // REPEAT decode: pattern phases are firmware-precomputed mod 48 (fields in
 // the op's reserved bits); the bucket-local start phase then follows from
 // the running bucket/row origin phases - increment-wrap only, no divider.
 wire [6:0] rp_a={1'b0,bmod48}+{rlo[6:2],2'b00};
 wire [6:0] rp_b=(rp_a>=7'd48)?(rp_a-7'd48):rp_a;
 wire [6:0] p0x_calc=(rp_b>={1'b0,v_phx})?(rp_b-{1'b0,v_phx}):(rp_b+7'd48-{1'b0,v_phx});
 wire [11:0] rq_off=riy0-by0;
 wire [6:0] rq_a={1'b0,rowmod48}+rq_off;
 wire [6:0] rq_b=(rq_a>=7'd48)?(rq_a-7'd48):rq_a;
 wire [6:0] p0y_calc=(rq_b>={1'b0,v_phy})?(rq_b-{1'b0,v_phy}):(rq_b+7'd48-{1'b0,v_phy});
 // Generated word = the cached expanded pattern word with A forced FF on all
 // four lanes (fixed wiring; the asset alpha is a don't-care).
 wire [127:0] rep_word={{8'hff,texp_out[119:96]},{8'hff,texp_out[87:64]},{8'hff,texp_out[55:32]},{8'hff,texp_out[23:0]}};
 // Address stream: texel row of the current row phase, and the RAM read
 // address, which leads the consumed word by one cycle (sync read). RTEX_DONE
 // presents the first word combinationally so both fast and RMW paths consume
 // it on their first cycle.
 wire [5:0] rpy_next=(rpy==6'd47)?6'd0:(rpy+6'd1);
 wire [3:0] rty_next=(rpy_next*14'd43)>>7;
 // Horizontal wrap stays on the same expanded row; only a work-row boundary
 // moves to the next pattern row at the window's base word.
 wire [7:0] taddr_adv=(tword==4'd11)?{trow,4'd0}:{trow,tword+4'd1};
 wire [3:0] p0_row=(p0y_calc*14'd43)>>7;
 wire [7:0] taddr_first={p0_row,p0x_calc[5:2]};
 wire [7:0] taddr_rowadv={rty_next,rpx0[5:2]};
 wire rwm_row_end=state==RWM_WRITE&&rwm_w>=rwl&&rcursor!=riy1;
 wire rep_row_end=state==RREP_FAST&&rfill_cnt[2:0]==3'b111&&rfill_cnt<rfill_end;
 assign texp_raddr=(state==RTEX_DONE)?taddr_first:((rwm_row_end||rep_row_end)?taddr_rowadv:taddr_adv);
 always @(posedge clk)begin
  if(!rst_n)begin
   state<=IDLE;done<=0;done_seq<=0;fault<=0;fault_addr<=0;
   src<=0;dst<=0;width<=0;height<=0;ss<=0;ds<=0;color<=0;alpha<=0;
   bucket<=0;bucket_total<=0;grid_cols<=0;grid_rows<=0;bucket_col<=0;bucket_row<=0;
   bx0<=0;by0<=0;dir_offset<=0;dir_count<=0;ref_idx<=0;
   d_src<=0;d_x<=0;d_y<=0;d_w<=0;d_ss<=0;d_ak<=0;d_solid<=0;d_pad24<=0;
   source_upper_half<=0;source_beats<=0;
   desc_beat0<=0;desc_beat1<=0;
   ix0<=0;ix1<=0;iy1<=0;lo<=0;hi<=0;row_cursor<=0;wr_row<=0;
   wfirst<=0;wlastidx<=0;wb_row<=0;rows_valid<=0;ld_cnt<=0;rx_cnt<=0;
   wr_addr<=0;issue_w<=0;out_cnt<=0;bd1<=0;bd2<=0;bd3<=0;
   ctrl_beat<=0;ctrl_beat_cnt<=0;
   recipe_mode<=0;r_idx_off<=0;r_base_color<=0;rseg<=0;rcount<=0;rop_idx<=0;
   ro_kind<=0;ro_a<=0;ro_ss<=0;ro_xy<=0;ro_wh<=0;
   rix0<=0;riy0<=0;riy1<=0;rfill_cnt<=0;rfill_end<=0;rwm_addr<=0;rwf<=0;rwl<=0;
   rlo<=0;rhi<=0;rcursor<=0;rbeats<=0;cbase<=0;copy_negative<=0;rwm_w<=0;rwm_mask<=0;rwm_word<=0;
   tcache_valid<=0;tcache_base<=0;tex_row<=0;rpx0<=0;rpy<=0;tword<=0;trow<=0;bmod48<=0;rowmod48<=0;texp_we<=0;texp_waddr<=0;texp_wdata<=0;
   empty_load<=0;
   frame_base<=0;load_row_addr<=0;src_row_addr<=0;rcopy_row_addr<=0;
   srow_rc_raddr<=0;srow_prev<=0;
   pf_row_ram<=0;pf_fetch_ram<=0;pf_pending<=0;pf_ready<=0;
   rc_bank<=0;wb_pend<=0;rc_pf_bank<=0;rc_wbeat<=0;
   out_row<=0;out_rows_left<=0;ref_drain_done<=0;
   desc_pre_beat0<=0;desc_pre_beat1<=0;desc_pre_pending<=0;desc_pre_valid<=0;desc_pre_cnt<=0;
   rowc_clear<=0;rowc_row<=0;rowc_cold_check<=0;rowc_cold_eval<=0;pf_check<=0;pf_issue<=0;pf_req_addr<=0;
   pf_in_cache<=0;rowc_fill_armed<=0;rowc_fill_is_pf<=0;rowc_fill_tag_q<=0;
   rowc_fill_idx<=0;rowc_idx_cur<=0;rowc_idx_next<=0;rowc_fill_old<=27'd0;
  end else begin
   done<=0;
   // Expose faults immediately; the DMAs keep draining accepted work.
   if(fault==0)begin
     if((ctrl_busy||ctrl_done)&&ctrl_error!=0)begin fault<=ctrl_error;fault_addr<=ctrl_addr;end
     else if((wb_busy||wb_done)&&wb_error!=0)begin
      fault<=wb_error;
      // A WB-load error during the recipe phase is a COPY-row prefetch;
      // rcopy_row_addr+ro_ss is exactly the address it was armed with.
      fault_addr<=wb_pend?(rcopy_row_addr+ro_ss):wb_row_addr;end
    else if(grp_error!=0)begin fault<=grp_error;fault_addr<=grp_error_addr;end
     else if((src_busy||rd_done)&&rd_error!=0)begin fault<=rd_error;fault_addr<=src_req_addr;end
   end
   // Control captures.
   if(ctrl_valid)begin ctrl_beat<=ctrl_data;ctrl_beat_cnt<=ctrl_beat_cnt+1'b1;end
   // Prefetch bookkeeping: accept marks the destination bank and restarts
   // the beat counter (ROW_SRC_REQ is skipped, so its rx_cnt reset must be
   // replayed here or the beats land at 8..15), completion outside the
   // ROW_SRC_WAIT consumer publishes the ready flag.
   if(src_req_valid&&src_req_ready&&pf_arm)begin
    pf_pending<=1'b1;pf_fetch_ram<=~pf_row_ram;rx_cnt<=4'd0;
   end
   if(rd_done&&pf_pending)begin
    pf_pending<=1'b0;
    if(state!=ROW_SRC_WAIT)pf_ready<=1'b1;
   end
   // Recipe-COPY prefetch bookkeeping: the armed WB-load DMA fetch lands in
   // the opposite row bank; completion at or before the RWM row tail lets the
   // tail skip RCOPY_REQ/WAIT and splice straight out of that bank.
   if(rc_pf_fire&&wb_req_ready)begin
    wb_pend<=1'b1;rc_pf_bank<=~rc_bank;rc_wbeat<=4'd0;
   end
   if(wb_valid&&wb_pend)rc_wbeat<=rc_wbeat+4'd1;
   if(wb_done&&wb_pend)wb_pend<=1'b0;
   // One list beat per accepted DMA word into the addressed register. The
   // low three counter bits are the whole address: count<=LIST_MAX bounds
   // every list load to one 8-beat pass and each REQ phase restarts the
   // counter at 0.
   if((state==LIST_WAIT||state==RLIST_WAIT)&&ctrl_valid)begin
    case(ctrl_beat_cnt[2:0])
     3'd0:list_store_0<=ctrl_data;3'd1:list_store_1<=ctrl_data;3'd2:list_store_2<=ctrl_data;
     3'd3:list_store_3<=ctrl_data;3'd4:list_store_4<=ctrl_data;3'd5:list_store_5<=ctrl_data;
     3'd6:list_store_6<=ctrl_data;default:list_store_7<=ctrl_data;
    endcase
   end
   if((state==DESC_WAIT||state==ROP_WAIT)&&ctrl_valid)begin
    if(ctrl_beat_cnt[0])desc_beat1<=ctrl_data;else desc_beat0<=ctrl_data;
   end
   // Next-reference descriptor prefetch capture. Beats and completion are
   // tracked separately from the main FSM's descriptor latch; the request
   // was accepted on the desc_prearm cycle.
   if(desc_pre_pending)begin
    if(ctrl_valid)begin
     if(desc_pre_cnt)desc_pre_beat1<=ctrl_data;else desc_pre_beat0<=ctrl_data;
     desc_pre_cnt<=desc_pre_cnt+1'b1;
    end
    if(ctrl_done)begin desc_pre_pending<=1'b0;desc_pre_valid<=1'b1;end
   end else if(desc_prearm&&ctrl_req_ready&&!fault_pending)begin
    desc_pre_pending<=1'b1;desc_pre_cnt<=1'b0;
   end
   // Replay-cache tag array (sync read, single-driver writes) and both
   // lookup flows. The boot sweep clears tags before cmd_ready opens.
   if(cache_flush)begin
    rowc_clear<=0;rowc_fill_armed<=1'b0;
   end else if(rowc_clear<10'd256)begin
    rowc_clear<=rowc_clear+1'b1;
   end else begin
    if(pf_arm)begin
     pf_check<=1'b1;rowc_idx_next<=rowc_idx_pf;
    end else if(pf_check)begin
     pf_check<=1'b0;
     if(rowc_hit_pf)begin pf_ready<=1'b1;pf_in_cache<=1'b1;end
     else begin pf_issue<=1'b1;pf_pending<=1'b1;pf_fetch_ram<=~pf_row_ram;rx_cnt<=4'd0;
      pf_req_addr<=src_row_addr+d_ss;
      rowc_fill_armed<=rowc_eligible;rowc_fill_idx<=rowc_idx_pf;rowc_fill_old<=rowc_tag_q;
      rowc_fill_is_pf<=1'b1;rowc_fill_tag_q<=rowc_tag_pf;end
    end
    if(pf_issue&&src_req_ready&&!fault_pending)pf_issue<=1'b0;
    if(rd_done)rowc_fill_armed<=1'b0;
   end
   // Sprite row capture: beats land in the srow RAM via its write port.
   if(state==RTEX_WAIT&&ctrl_valid)begin
    texp_we<=1'b1;texp_waddr<={tex_row,ctrl_beat_cnt[3:0]};texp_wdata<=ctrl_data;
   end else texp_we<=1'b0;
   // Work-block load from the framebuffer.
   // Work-block load from the framebuffer.
   if(state==WB_LOAD_WAIT&&wb_valid)ld_cnt<=ld_cnt+1'b1;
   // Sprite row capture: beats land in the srow RAM via its write port. The
   // BLEND read address is combinational from issue_w; only the pipe drain
   // still needs srow_prev refreshed every BLEND cycle.
   if(rd_valid)rx_cnt<=rx_cnt+1'b1;
   if(state==BLEND)srow_prev<=srow_rdata;
 // Blend feed: one work word per cycle, background delayed to align with
 // the three-stage pipe output.
   if(state==BLEND)begin
    bd1<=w_rdata;bd2<=bd1;bd3<=bd2;
    if(issue_w<={1'b0,wlastidx})begin
     wr_addr<={1'b0,wr_row,3'b000}+{4'b0,issue_w}+9'd1;
    end
    if(cont_advance)begin
     /* Continuous wrap to the next row: issue_w takes the one pass-through
        token (wfirst, no pipe input) whose address presentation re-primes
        the srow window; the work-read base moves to the next row now so
        the first valid token's background is already on the RAM output. */
     issue_w<={1'b0,wfirst};
     row_cursor<=row_cursor+1'b1;wr_row<=wr_row+5'd1;
     src_row_addr<=src_row_addr+d_ss;
     wr_addr<={1'b0,wr_row+5'd1,3'b000}+{6'b0,wfirst};
     pf_row_ram<=pf_fetch_ram;pf_ready<=1'b0;
     rowc_row<=pf_in_cache;rowc_idx_cur<=rowc_idx_next;pf_in_cache<=1'b0;
    end
    // Retire the final feed token too. Otherwise issue_w parks at last+1
    // with in_valid still high throughout the drain, injecting duplicate
    // outputs into the next solid row (textured DMA used to hide the bug).
    else if(issue_w<=({1'b0,wlastidx}+4'd1))issue_w<=issue_w+4'd1;
    if(pout_valid)begin
     if(out_cnt==words_total-4'd1)begin
      out_cnt<=4'd0;out_row<=out_row+5'd1;
      if(out_rows_left!=6'd0)out_rows_left<=out_rows_left-6'd1;
     end else out_cnt<=out_cnt+4'd1;
    end
   end
   case(state)
    IDLE:if(cmd_valid&&cmd_ready)begin
     src<=cmd[32+:32];dst<=cmd[64+:32];width<=c_w;height<=c_h;
     ss<=cmd[160+:32];ds<=cmd[192+:32];color<=cmd[224+:32];alpha<=c_alpha;seq<=cmd[320+:32];done_seq<=cmd[320+:32];
     bucket<=0;bucket_col<=0;bucket_row<=0;bx0<=0;by0<=0;fault<=0;fault_addr<=0;
     ref_drain_done<=0;desc_pre_pending<=0;desc_pre_valid<=0;
     rowc_row<=1'b0;rowc_cold_check<=1'b0;rowc_cold_eval<=1'b0;pf_check<=1'b0;pf_issue<=1'b0;
     pf_in_cache<=1'b0;rowc_fill_armed<=1'b0;rowc_fill_is_pf<=1'b0;rowc_fill_tag_q<=25'd0;
     dir_count<=0;dir_offset<=0;
     bmod48<=0;rowmod48<=0;tcache_valid<=0;
     empty_load<=0;
     pf_pending<=0;pf_ready<=0;pf_row_ram<=0;pf_fetch_ram<=0;
     wb_pend<=0;rc_bank<=0;
     frame_base<=cmd[64+:32];
     grid_cols<=c_cols;grid_rows<=c_rows;bucket_total<=c_cols*c_rows;
     recipe_mode<=(c_alpha!=0);
     if(c_op!=`GFX_OP_BUCKET)begin fault<=`GFX_ERR_OPCODE;fault_addr<=cmd[32+:32];state<=FINISH;end
     else if(c_alpha!=0&&c_alpha[3:0]!=0)begin fault<=`GFX_ERR_ALIGN;fault_addr<=c_alpha;state<=FINISH;end
     else if(c_w==0||c_h==0||c_w[4:0]!=0)begin fault<=`GFX_ERR_SIZE;fault_addr<=cmd[64+:32];state<=FINISH;end
     // Group rows are one 256B burst: dst and ds must be 256B aligned or the
     // 16-beat AW could cross a 4KiB page. Mode 0 keeps the original contract.
     else if(cmd[199:192]!=8'd0||cmd[71:64]!=8'd0)begin fault<=`GFX_ERR_ALIGN;fault_addr<=cmd[64+:32];state<=FINISH;end
     else state<=(c_alpha!=0)?RHDR_REQ:DIR_REQ;
    end
    DIR_REQ:if(!dir_bank_stall&&ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=DIR_WAIT;end
    // Directory word: {segment[31:16], count[15:0]}; a segment number is a
    // 128-entry (256-byte) aligned block, so burst legality is inherent.
    DIR_WAIT:if(ctrl_done)begin
     if(dir_word[15:0]>LIST_MAX)begin fault<=`GFX_ERR_SIZE;fault_addr<=ss+{bucket[13:2],4'b0000};state<=FAULT_DRAIN;end
     else begin
      // Always refresh both fields: an empty bucket must clear anything an
      // earlier command left behind, or the post-recipe completion would
      // phantom-issue a sprite phase that was never requested.
      dir_count<=dir_word[15:0];
      dir_offset<=dir_word[31:16];
      if(dir_word[15:0]==0)begin
       if(recipe_mode)state<=RIDX_REQ;
       // An empty INIT_FB bucket still fills its bank half from the
       // framebuffer so the 64px drain row stays byte-identical.
       else begin rows_valid<=rows_here;wb_row<=0;empty_load<=1;
        load_row_addr<=frame_base+({1'b0,bx0}<<2);state<=WB_LOAD_REQ;end
      end
      else begin
       ctrl_beat_cnt<=0;
       // Recipes are the bucket background: they run before the sprite list
       // in recipe mode, whatever the sprite count is.
       state<=recipe_mode?RIDX_REQ:LIST_REQ;
      end
     end
    end
    LIST_REQ:if(ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=LIST_WAIT;end
    LIST_WAIT:if(ctrl_done)begin ref_idx<=0;wb_row<=0;ld_cnt<=0;rows_valid<=rows_here;desc_pre_valid<=1'b0;
     load_row_addr<=frame_base+({1'b0,bx0}<<2);state<=recipe_mode?DESC_REQ:WB_LOAD_REQ;end
    WB_LOAD_REQ:if(wb_req_ready)begin ld_cnt<=0;state<=WB_LOAD_WAIT;end
    WB_LOAD_WAIT:if(wb_done)begin
     if(wb_row+1>=rows_valid)begin
      if(empty_load)begin empty_load<=0;state<=post_prod;end
      else begin ref_idx<=0;state<=DESC_REQ;end
     end
     else begin wb_row<=wb_row+1'b1;load_row_addr<=load_row_addr+ds;state<=WB_LOAD_REQ;end
    end
    DESC_REQ:if(ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=DESC_WAIT;end
    DESC_WAIT:if(ctrl_done)begin
     if(v_bad_desc)begin fault<=`GFX_ERR_OPCODE;fault_addr<=src+{ref_item[11:0],5'b00000};state<=FAULT_DRAIN;end
     else if(v_empty)begin
      if(ref_idx+1>=dir_count)begin wb_row<=0;state<=post_prod;end
      else begin ref_idx<=ref_idx+1'b1;state<=DESC_REQ;end
     end
     else begin
      d_src<=db0_sel[63:32];d_x<=v_x;d_y<=v_y;d_w<=v_w;d_ss<=v_ss;d_ak<=v_ak_full;
      d_solid<=desc_beat0[2];d_pad24<=source_pad24;
      source_upper_half<=source_trim_upper;
      source_beats<=(source_trim_upper||source_trim_lower)?5'd4:src_row_beats;
      // 6-bit wrap difference (see rcopy_init note), then one small multiply.
      src_row_addr<=desc_beat0[63:32]+{26'b0,v_iy0[5:0]-v_y[5:0]}*desc_beat1[95:64]+(source_trim_upper?32'd64:32'd0);
      ix0<=v_ix0[11:0];ix1<=v_ix1[11:0];iy1<=v_iy1[11:0];
      lo<=v_ix0[6:0]-bx0[6:0];hi<=v_ix1[6:0]-bx0[6:0];
      wfirst<=(v_ix0[6:0]-bx0[6:0])>>2;wlastidx<=(v_ix1[6:0]-bx0[6:0])>>2;
      row_cursor<=v_iy0[11:0];wr_row<=v_iy0[11:0]-by0;
      out_row<=v_iy0[11:0]-by0;out_rows_left<=v_iy1[11:0]-v_iy0[11:0]+6'd1;
      rowc_cold_check<=1'b1;rowc_cold_eval<=1'b0;rowc_row<=1'b0;
      // Both paths initialize the feed from the latched row/word bounds.
      state<=ROW_SRC_REQ;
     end
    end
    ROW_SRC_REQ:if(d_solid)begin
     wr_addr<={1'b0,wr_row,3'b000}+{6'b0,wfirst};
     issue_w<={1'b0,wfirst};out_cnt<=0;state<=BLEND;rowc_cold_check<=1'b0;rowc_cold_eval<=1'b0;
    end else if(rowc_hit_cold)begin
     /* Replay-cache hit: no fetch at all, straight into the blend with the
        window reading the cache (the tag address was presented one cycle
        earlier, so the first word's data is on the RAM output now). */
     wr_addr<={1'b0,wr_row,3'b000}+{6'b0,wfirst};
     issue_w<={1'b0,wfirst};out_cnt<=0;
     rowc_row<=1'b1;rowc_idx_cur<=rowc_idx_cold;rowc_cold_eval<=1'b0;
     pf_row_ram<=1'b0;state<=BLEND;
    end else if(rowc_cold_check)begin
     rowc_cold_check<=1'b0;rowc_cold_eval<=1'b1;
    end else if(rowc_cold_eval)begin
     rowc_cold_eval<=1'b0;
    end else if(src_req_ready)begin rx_cnt<=0;state<=ROW_SRC_WAIT;
     // A fetch starting here always targets the shared row RAM (reference
     // row 0, or the no-prefetch fallback); pf_row_ram follows it so the
     // blend output mux cannot read a stale prefetch bank.
     rowc_fill_armed<=rowc_eligible;rowc_fill_idx<=rowc_idx_cold;rowc_fill_old<=rowc_tag_q;
     rowc_fill_is_pf<=1'b0;rowc_fill_tag_q<=rowc_tag_cold;
     pf_row_ram<=1'b0;end
    ROW_SRC_WAIT:if(rd_done)begin
     wr_addr<={1'b0,wr_row,3'b000}+{6'b0,wfirst};
     issue_w<={1'b0,wfirst};out_cnt<=0;rowc_row<=1'b0;
     // Waiting out a late prefetch: its beats landed in pf_fetch_ram.
     if(pf_pending)pf_row_ram<=pf_fetch_ram;
     state<=BLEND;
    end
    BLEND:begin
     /* Reference completion is out-side driven: the final row's completion
        is the cycle whose pre-edge out_rows_left reads one (the wrap to
        zero lands on this very edge), so the exit fires combinationally on
        that cycle. A prefetched next-reference descriptor is decoded in
        place (the v_* wires select the prefetch beats during BLEND); if
        its fetch is still in flight the drain-done latch holds the exit
        until it lands. */
     if(row_blend_done&&out_rows_left==6'd1)ref_drain_done<=1'b1;
     if(((row_blend_done&&out_rows_left==6'd1)||ref_drain_done)&&!desc_pre_pending)begin
      ref_drain_done<=1'b0;rowc_row<=1'b0;
      if(v_bad_desc)begin fault<=`GFX_ERR_OPCODE;fault_addr<=src+{ref_item_sel[11:0],5'b00000};state<=FAULT_DRAIN;end
      else if(v_empty)begin
       desc_pre_valid<=1'b0;
       if(ref_idx+1>=dir_count)begin wb_row<=0;state<=post_prod;end
       else begin ref_idx<=ref_idx+1'b1;state<=DESC_REQ;end
      end
      else if(desc_pre_valid)begin
       desc_beat0<=desc_pre_beat0;desc_beat1<=desc_pre_beat1;
       ref_idx<=ref_idx+1'b1;desc_pre_valid<=1'b0;
       d_src<=db0_sel[63:32];d_x<=v_x;d_y<=v_y;d_w<=v_w;d_ss<=v_ss;d_ak<=v_ak_full;
       d_solid<=db0_sel[2];d_pad24<=source_pad24;
       source_upper_half<=source_trim_upper;
       source_beats<=(source_trim_upper||source_trim_lower)?5'd4:src_row_beats;
       src_row_addr<=db0_sel[63:32]+{26'b0,v_iy0[5:0]-v_y[5:0]}*v_ss+(source_trim_upper?32'd64:32'd0);
       ix0<=v_ix0[11:0];ix1<=v_ix1[11:0];iy1<=v_iy1[11:0];
       lo<=v_ix0[6:0]-bx0[6:0];hi<=v_ix1[6:0]-bx0[6:0];
       wfirst<=(v_ix0[6:0]-bx0[6:0])>>2;wlastidx<=(v_ix1[6:0]-bx0[6:0])>>2;
       row_cursor<=v_iy0[11:0];wr_row<=v_iy0[11:0]-by0;
       out_row<=v_iy0[11:0]-by0;out_rows_left<=v_iy1[11:0]-v_iy0[11:0]+6'd1;
       rowc_cold_check<=1'b1;rowc_cold_eval<=1'b0;rowc_row<=1'b0;
       state<=ROW_SRC_REQ;
      end
      else begin
       if(ref_idx+1>=dir_count)begin wb_row<=0;state<=post_prod;end
       else begin ref_idx<=ref_idx+1'b1;state<=DESC_REQ;end
      end
     end
     else if(row_blend_done&&out_rows_left!=6'd1&&(issue_w==({1'b0,wlastidx}+4'd2)))begin
      /* Parked advance (no continuous handoff): the feed has drained and
         the priming window has run; identical to the pre-pipeline flow.
         The final row (pre-edge count one) is excluded - the exit above
         owns it. */
      if(d_solid)begin
       // Re-arm via ROW_SRC_REQ without issuing a source transaction.
       row_cursor<=row_cursor+1'b1;wr_row<=wr_row+1'b1;
       state<=ROW_SRC_REQ;
      end else if(SPRITE_ROW_PREFETCH&&(pf_ready||(rd_done&&pf_pending)))begin
       // Fast row advance: the prefetch bank already holds the next row and
       // the drain-prime window has presented its first-word address, so
       // blend continues without leaving the state. rd_done landing on this
       // very cycle counts too - waiting for another pulse would deadlock.
       row_cursor<=row_cursor+1'b1;wr_row<=wr_row+1'b1;src_row_addr<=src_row_addr+d_ss;
       wr_addr<={1'b0,wr_row+5'd1,3'b000}+{6'b0,wfirst};
       issue_w<={1'b0,wfirst};
       pf_row_ram<=pf_fetch_ram;pf_ready<=1'b0;
       rowc_row<=pf_in_cache;rowc_idx_cur<=rowc_idx_next;pf_in_cache<=1'b0;
      end else begin
       row_cursor<=row_cursor+1'b1;wr_row<=wr_row+1'b1;src_row_addr<=src_row_addr+d_ss;
       // In-flight prefetch (slow fetch): wait it out where the priming
       // address stays presented; without one, re-arm the old fetch path.
       state<=pf_pending?ROW_SRC_WAIT:ROW_SRC_REQ;
      end
     end
    end
    RHDR_REQ:if(ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=RHDR_WAIT;end
    RHDR_WAIT:if(ctrl_done)begin
     if((ctrl_beat[15:0]!=1&&ctrl_beat[15:0]!=2)||ctrl_beat[127:96]!=0||rhdr_color[3:0]!=0)begin fault<=`GFX_ERR_OPCODE;fault_addr<=alpha;state<=FAULT_DRAIN;end
     else begin r_idx_off<=ctrl_beat[63:32];r_base_color<=ctrl_beat[95:64];rows_valid<=rows_here;state<=DIR_REQ;end
    end
    RIDX_REQ:if(ctrl_req_ready)begin ctrl_beat_cnt<=0;rows_valid<=rows_here;state<=RIDX_WAIT;end
    RIDX_WAIT:if(ctrl_done)begin
     if(ridx_word[15:0]>64)begin fault<=`GFX_ERR_SIZE;fault_addr<=alpha+r_idx_off;state<=FAULT_DRAIN;end
     else if(ridx_word[15:0]==0)begin rfill_cnt<=0;state<=RBASE;end
     else begin rseg<=ridx_word[31:16];rcount<=ridx_word[6:0];ctrl_beat_cnt<=0;state<=RLIST_REQ;end
    end
    RLIST_REQ:if(ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=RLIST_WAIT;end
    RLIST_WAIT:if(ctrl_done)begin rop_idx<=0;ctrl_beat_cnt<=0;state<=ROP_REQ;end
    ROP_REQ:if(ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=ROP_WAIT;end
    ROP_WAIT:if(ctrl_done)begin
     if(v_rbad)begin fault<=`GFX_ERR_OPCODE;fault_addr<=alpha+32'd16+{rop_base[9:0],5'b00000};state<=FAULT_DRAIN;end
     else if(v_rempty)begin
      if(rop_idx+1>=rcount)begin ref_idx<=0;wb_row<=0;state<=dir_count!=0?LIST_REQ:post_prod;end
      else begin rop_idx<=rop_idx+1'b1;ctrl_beat_cnt<=0;state<=ROP_REQ;end
     end
     else begin
      ro_kind<=v_kind;ro_a<=v_ra;ro_ss<=desc_beat1[31:0];ro_xy<=desc_beat0[127:64];ro_wh<=desc_beat0[127:96];
      rix0<=v_rix0[11:0];riy0<=v_riy0[11:0];riy1<=v_riy1[11:0];
      rlo<=v_rix0[6:0]-bx0[6:0];rhi<=v_rix1[6:0]-bx0[6:0];
      rcursor<=v_riy0[11:0];
      if(v_kind==0)begin
       if(v_rfull)begin
        rfill_cnt<={v_riy0[11:0]-by0,3'b000};
        rfill_end<={v_riy1[11:0]-by0,3'b000}+9'd7;
        state<=RFILL_FAST;
       end else begin
        rwf<=(v_rix0[6:0]-bx0[6:0])>>2;rwl<=(v_rix1[6:0]-bx0[6:0])>>2;
        rwm_w<=(v_rix0[6:0]-bx0[6:0])>>2;state<=RWM_SET;
       end
      end else if(v_kind==1)begin
       if(v_cbeats>10)begin fault<=`GFX_ERR_SIZE;fault_addr<=alpha+32'd16+{rop_base[9:0],5'b00000};state<=FAULT_DRAIN;end
       else begin
        cbase<=rcopy_first_pixel[3:2]-v_rix0[1:0];
        copy_negative<=rcopy_first_pixel[3:2]<v_rix0[1:0];
        rbeats<=v_cbeats[4:0];rcopy_row_addr<=rcopy_init;
        rwf<=(v_rix0[6:0]-bx0[6:0])>>2;rwl<=(v_rix1[6:0]-bx0[6:0])>>2;
        srow_rc_raddr<=0;
        rc_bank<=1'b0;
        state<=RCOPY_REQ;
       end
      end else begin
       tex_row<=4'd0;state<=RTEX_REQ;
      end
     end
    end
    RFILL_FAST:begin
     if(rfill_cnt>=rfill_end)begin
      if(rop_idx+1>=rcount)begin ref_idx<=0;wb_row<=0;state<=dir_count!=0?LIST_REQ:post_prod;end
      else begin rop_idx<=rop_idx+1'b1;ctrl_beat_cnt<=0;state<=ROP_REQ;end
     end else rfill_cnt<=rfill_cnt+9'd1;
    end
    RWM_SET:begin
     rwm_addr<={{rcursor-by0},3'b000}+{6'b0,rwm_w};
     // The merge source comes from the RAM read port: this cycle's address
     // presents the RWM word, whose data is on w_rdata during RWM_WRITE.
     wr_addr<={{rcursor-by0},3'b000}+{6'b0,rwm_w};
     // The splice reads srow_rdata during RWM_SET; the word's address was
     // presented during the previous cycle. Advance here so the next word's
     // data streams during RWM_WRITE and is on rdata at the next RWM_SET.
     srow_rc_raddr<=srow_rc_raddr+4'd1;
     if(ro_kind==1)srow_prev<=srow_rdata;
     rwm_mask<={(rwm_w*4+3>=rlo)&&(rwm_w*4+3<=rhi),(rwm_w*4+2>=rlo)&&(rwm_w*4+2<=rhi),
                (rwm_w*4+1>=rlo)&&(rwm_w*4+1<=rhi),(rwm_w*4>=rlo)&&(rwm_w*4<=rhi)};
     if(ro_kind==0)rwm_word<={{8'hff,ro_a[23:0]},{8'hff,ro_a[23:0]},{8'hff,ro_a[23:0]},{8'hff,ro_a[23:0]}};
     else if(ro_kind==2)rwm_word<=rep_word;
     else rwm_word<=rcopy_splice;
     state<=RWM_WRITE;
    end
    RWM_WRITE:begin
     if(rwm_w>=rwl)begin
      if(rcursor==riy1)begin
       if(rop_idx+1>=rcount)begin ref_idx<=0;wb_row<=0;state<=dir_count!=0?LIST_REQ:post_prod;end
       else begin rop_idx<=rop_idx+1'b1;ctrl_beat_cnt<=0;state<=ROP_REQ;end
      end else begin
       rcursor<=rcursor+1'b1;rcopy_row_addr<=rcopy_row_addr+ro_ss;
       if(ro_kind==1)begin
        srow_rc_raddr<=0;
        // Prefetch path: flip to the bank the WB-load DMA filled and splice
        // straight on; only the legacy flow re-issues the control DMA per
        // row. PWAIT always runs at least one cycle even when the prefetch
        // already finished: its tail sets the read address to 0, and the
        // synchronous RAM needs that cycle before PRIME can latch word 0
        // into srow_prev (RCOPY_WAIT used to provide this warm-up).
        if(!RCOPY_ROW_PREFETCH)state<=RCOPY_REQ;
        else begin
         rc_bank<=~rc_bank;
         state<=RCOPY_PWAIT;
        end
       end
       else begin
        rwm_w<=rwf;state<=RWM_SET;
        if(ro_kind==2)begin
         tword<=rpx0[5:2];trow<=rty_next;rpy<=rpy_next;
        end
       end
      end
     end else begin
      rwm_w<=rwm_w+1'b1;state<=RWM_SET;
      if(ro_kind==2)tword<=(tword==4'd11)?4'd0:(tword+4'd1);
     end
    end
    RCOPY_REQ:if(ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=RCOPY_WAIT;end
    RCOPY_WAIT:if(ctrl_done)begin rwm_w<=rwf;state<=RCOPY_PRIME;end
    RCOPY_PRIME:begin
     srow_prev<=srow_rdata;srow_rc_raddr<=copy_negative?4'd0:4'd1;
     state<=RCOPY_WINDOW;
    end
    // Wait out a prefetch that outlived the row's RWM window. The read port
    // already presents address 0 to the target bank (bank flipped at the row
    // tail), so RAM word 0 streams out the cycle the last beat lands. The
    // rwm_w reset here replays RCOPY_WAIT's tail: the fast path bypasses it,
    // and without it RWM_WRITE sees rwm_w>=rwl and retires the row unfilled.
    RCOPY_PWAIT:if(!wb_pend||wb_done)begin rwm_w<=rwf;state<=RCOPY_PRIME;end
    RCOPY_WINDOW:state<=RWM_SET;
    // REPEAT texture load: sixteen 64B row bursts; a provenance-tag cache hit
    // skips the DMA entirely and goes straight to dispatch.
    RTEX_REQ:if(tex_need)begin
     if(ctrl_req_ready)begin ctrl_beat_cnt<=0;state<=RTEX_WAIT;end
    end else state<=RTEX_DONE;
    RTEX_WAIT:if(ctrl_done)begin
     if(tex_row==4'd15)begin tcache_valid<=1'b1;tcache_base<=ro_a;state<=RTEX_DONE;end
     else begin tex_row<=tex_row+1'b1;state<=RTEX_REQ;end
    end
    RTEX_DONE:begin
     rpx0<=p0x_calc[5:0];
     rpy<=p0y_calc[5:0];
     trow<=p0_row;tword<=p0x_calc[5:2];
     if(v_rfull_rep)begin
      rfill_cnt<={riy0-by0,3'b000};
      rfill_end<={riy1-by0,3'b000}+9'd7;
      state<=RREP_FAST;
     end else begin
      rwf<=rlo[4:2];rwl<=rhi[4:2];rwm_w<=rlo[4:2];state<=RWM_SET;
     end
    end
    RREP_FAST:begin
     if(rfill_cnt>=rfill_end)begin
      if(rop_idx+1>=rcount)begin ref_idx<=0;wb_row<=0;state<=dir_count!=0?LIST_REQ:post_prod;end
      else begin rop_idx<=rop_idx+1'b1;ctrl_beat_cnt<=0;state<=ROP_REQ;end
     end else if(rfill_cnt[2:0]==3'b111)begin
      tword<=rpx0[5:2];trow<=rty_next;rpy<=rpy_next;
      rfill_cnt<=rfill_cnt+9'd1;
     end else begin
      tword<=(tword==4'd11)?4'd0:(tword+4'd1);
      rfill_cnt<=rfill_cnt+9'd1;
     end
    end
    RBASE:begin
     if(rfill_cnt>=9'd255)begin
      if(dir_count!=0)begin ref_idx<=0;ctrl_beat_cnt<=0;state<=LIST_REQ;end
      else begin wb_row<=0;state<=post_prod;end
     end else rfill_cnt<=rfill_cnt+9'd1;
    end
    // Group commit (GROUP_WB): latch the group's framebuffer geometry into
    // One cycle: hand the filled bank to the shared backend. The commit
    // descriptor wires are valid from this group's live geometry before
    // NEXT_BUCKET advances it.
    GRP_COMMIT:state<=NEXT_BUCKET;
    NEXT_BUCKET:begin rowc_row<=1'b0;
     if(bucket+1>=bucket_total)state<=FINISH;
     else if(bucket_col+1>=grid_cols)begin
      bucket<=bucket+1'b1;bucket_col<=0;bx0<=0;by0<={bucket_row,5'b00000}+32;
      bucket_row<=bucket_row+1'b1;frame_base<=frame_base+{ds[26:0],5'b0};state<=DIR_REQ;
      bmod48<=0;
      rowmod48<=(rowmod48>=6'd16)?(rowmod48-6'd16):(rowmod48+6'd32);
     end else begin
      bucket<=bucket+1'b1;bucket_col<=bucket_col+1'b1;bx0<=bx0+32;state<=DIR_REQ;
      bmod48<=(bmod48>=6'd16)?(bmod48-6'd16):(bmod48+6'd32);
     end
    end
    // Command ownership: stay busy until the backend has fully drained
    // every committed bank; the next command must not touch the shared
    // resources early.
    FINISH:if(grp_idle)begin done<=1;done_seq<=seq;state<=IDLE;end
    FAULT_DRAIN:if(!ctrl_busy&&!wb_busy&&!src_busy&&grp_idle)state<=FINISH;
    default:state<=IDLE;
   endcase
   if(fault_pending&&state!=IDLE&&state!=FINISH&&state!=FAULT_DRAIN)state<=FAULT_DRAIN;
  end
 end
 // Work-port read address: the RWM merge presents its word during RWM_SET so
 // the synchronous RAM returns it on w_rdata exactly when RWM_WRITE consumes
 // it; every other phase streams wr_addr, which reproduces the previous
 // continuous-sampler timing (address register + RAM output register).
 assign wk_raddr=(state==RWM_SET)?({{rcursor-by0},3'b000}+{6'b0,rwm_w}):wr_addr;

 // Work-block write port: phases are exclusive, so a state mux drives one
 // address/data pair into the RAM's single write port.
 always @*begin
  wk_we=1'b0;wk_waddr=9'd0;wk_wdata=128'd0;
  case(state)
   WB_LOAD_WAIT:if(wb_valid)begin wk_we=1'b1;wk_waddr={1'b0,wb_row,3'b000}+{5'b0,ld_cnt};wk_wdata=wb_data;end
   BLEND:if(pout_valid)begin wk_we=1'b1;wk_waddr={1'b0,out_row,3'b000}+{4'b0,out_word};wk_wdata=merged;end
   RFILL_FAST:begin wk_we=1'b1;wk_waddr=rfill_cnt;wk_wdata={{8'hff,ro_a[23:0]},{8'hff,ro_a[23:0]},{8'hff,ro_a[23:0]},{8'hff,ro_a[23:0]}};end
   RBASE:begin wk_we=1'b1;wk_waddr=rfill_cnt;wk_wdata={{8'hff,r_base_color[23:0]},{8'hff,r_base_color[23:0]},{8'hff,r_base_color[23:0]},{8'hff,r_base_color[23:0]}};end
   RWM_WRITE:begin wk_we=1'b1;wk_waddr=rwm_addr;
    wk_wdata={rwm_mask[3]?rwm_word[127:96]:w_rdata[127:96],
              rwm_mask[2]?rwm_word[95:64]:w_rdata[95:64],
              rwm_mask[1]?rwm_word[63:32]:w_rdata[63:32],
              rwm_mask[0]?rwm_word[31:0]:w_rdata[31:0]};end
   RREP_FAST:begin wk_we=1'b1;wk_waddr=rfill_cnt;wk_wdata=rep_word;end
   default:begin end
  endcase
 end
endmodule
