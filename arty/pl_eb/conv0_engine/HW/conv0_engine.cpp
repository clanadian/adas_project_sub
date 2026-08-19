#include "conv0_engine.h"
#include <cassert>

// ---------------------------------------------------------------------------
// Activation math - COPIED VERBATIM from conv_engine.cpp, deliberately.
//
// These three functions are already bit-exact against the golden model on all
// 13 real conv layers (RTL_HANDOFF_KO.md section 5's exact
// saturate_int8(round_shift(acc * multiplier, right_shift)) formula, with
// LeakyReLU's exact 13/128 slope applied to the RAW accumulator before
// requantization). Re-deriving them here instead of copying is precisely how
// this project has produced silent mismatches before - see conv_engine.cpp's
// own comments on the split-arithmetic rounding bug.
//
// If conv_engine.cpp's versions ever change, change these identically.
// ---------------------------------------------------------------------------
static act_t saturate(ap_int<64> v) {
    if (v > 127)  return (act_t)127;
    if (v < -128) return (act_t)-128;
    return (act_t)v;
}

// round(x / 2^s), ties away from zero. NOT a plain arithmetic shift: two's-
// complement `>>` rounds negatives toward -infinity, not away from zero.
static ap_int<64> round_shift(ap_int<64> x, ap_uint<6> s) {
#pragma HLS INLINE
    if (s == 0) return x;
    ap_int<64> half = (ap_int<64>)1 << (s - 1);
    // Explicit if/else, not a ternary - ap_int's bit growth widens
    // `x + half` to ap_int<65> on one branch, which cannot be unified with
    // the other branch's ap_int<64> inside a single `?:` (a real build error
    // against Vitis's standalone ap_int.h, not a style preference).
    if (x >= 0) {
        return (ap_int<64>)((x + half) >> s);
    } else {
        return (ap_int<64>)(-(((-x) + half) >> s));
    }
}

// INLINE is correct HERE for the same reason it is in conv_engine.cpp today:
// the only caller is a PIPELINE'd loop, not an UNROLL'd one, so this produces
// ONE spatial copy of the runtime-width barrel shift per pipeline, not one
// per lane. The 20-copy LUT explosion (89% -> 105%) that history records
// happened under an UNROLL. If a future change puts this under an UNROLL'd
// loop, re-read conv_engine.cpp's comment before keeping the INLINE.
static act_t apply_activation(
    accum_t acc, bool leaky_relu_enable,
    int32_t requant_multiplier, unsigned requant_shift
) {
#pragma HLS INLINE
    accum_t post_leaky = acc;
    if (leaky_relu_enable && acc < 0) {
        post_leaky = (accum_t)round_shift((ap_int<64>)acc * 13, 7);
    }
    ap_int<64> scaled = (ap_int<64>)post_leaky * (ap_int<64>)requant_multiplier;
    return saturate(round_shift(scaled, requant_shift));
}

// ---------------------------------------------------------------------------
// conv0_engine
//
// Structure (see conv0_engine.h for why these shapes):
//
//   prologue : load input rows 0..K-1 into row slots 0..K-1
//   ROW r    : THREE pipelined phases, each II=1, each with at most one
//              m_axi port and no branch around its access:
//                1. READ_ROW  - input row r+K into slot (r+K)%ROW_SLOTS, the
//                   one slot NOT read this row (the whole reason ROW_SLOTS is
//                   K+1 and not K).
//                2. COL_GROUP - the whole output row into the on-chip
//                   out_row, reading slots (r+ky)%ROW_SLOTS.
//                3. WRITE_ROW - out_row to DDR as one burst.
//              COL_GROUP runs over all img_w input columns, not the out_w
//              output ones: a row is img_w wide (out_w + K - 1), and the last
//              K-1 columns emit nothing. That used to be a separate trailing
//              loop; TROUBLESHOOTING §34 is why it is not.
//
// THE ONE RULE THIS FILE KEEPS RELEARNING: an m_axi access wants its own
// pipelined loop, unconditional, unit-stride, one port, nothing else in the
// body. Every performance problem this engine has had was a violation of it -
// the ofmap store buried in a conditional (never bursted, 34.1 cyc/pos), and
// the 3-byte prefetch buried in COL_GROUP (3 requests on one port, pinned
// II=3). Both fixes were the same move: pull the access into its own loop.
//
// WHY THE ROW BUFFER EXISTS (2026-08-06)
// --------------------------------------
// It is the fix for this engine's first cosim: 5,030,205 cycles = 34.1
// cycles/position against a 4 cycles/position model. The csynth burst table
// named the cause directly - the `ofmap` write has never once inferred a
// burst, in either loop structure this file has had:
//
//     pre-§34 : ofmap write | Fail | Could not analyze pattern
//     post-§34: ofmap write | Fail | Access store is in the conditional branch
//     ifmap read (COL_GROUP)| Inferred | variable      <- reads were always fine
//
// (The §34 `if (emit)` changed the REASON reported, not the outcome. It did
// not cause this; the write was already failing before it existed.)
//
// The 4-cycle model assumed 16 int8 out = 4 beats on a 32-bit port = 4 cycles.
// Four beats cost four cycles only INSIDE a burst. Without inference each beat
// is a full AW+W+B transaction, so a position cost 4 write transactions plus
// ~3 read beats - which is where 34.1 came from. This engine was never
// write-BANDWIDTH bound; it was write-TRANSACTION bound.
//
// So the DDR writes move out of the per-position loop entirely. Both of the
// burst blockers go away at once: the store leaves the conditional branch, and
// 2,048 scattered transactions per row become one unit-stride burst. This is
// conv_engine.cpp's FINISH_WR phase-separation technique (TROUBLESHOOTING §25)
// applied at row granularity instead of at lane granularity - and run_hls.tcl's
// own header already named that technique as the remedy for exactly this
// symptom, before it was measured.
// ---------------------------------------------------------------------------
void conv0_engine(
    const act_t    *ifmap,
    const weight_t *weights,
    const bias_t   *bias,
    pack4_t        *ofmap,
    uint16_t img_h, uint16_t img_w,
    int32_t requant_multiplier, uint8_t requant_shift, uint8_t leaky_relu_enable
) {
// NO max_widen_bitwidth HERE, AND THAT IS A MEASURED RESULT, NOT AN OMISSION.
//
// Every ifmap access reports "Widen Fail". The obvious read is that widening
// is merely switched off ("max_widen_bitwidth threshold of 0" - 0 is the
// default and means disabled), and that turning it on would merge the IN_CH=3
// per-column byte loads into one 32-bit load, taking COL_GROUP off II=3
// (HLS 200-885 names RD_BUS_load_5_req/_6_req as the II violation).
//
// Tried 2026-08-06 with `max_widen_bitwidth=32`. The pragma engages - the
// message changes to "Could not widen since type i8 size is greater than or
// equal to alignment 1(bytes)" - but widening still does not happen, and
// latency/II/resources come back BIT-IDENTICAL (2,381,986 cycles, COL_GROUP
// II=3, LUT 20,888). Reverted, because a pragma that provably changes no RTL
// is worse than a comment saying so.
//
// The second message is the informative one: the blocker is ALIGNMENT, not
// the threshold. `ifmap` is `ap_int<8>*`, so HLS can only assume 1-byte
// alignment, and it will not widen a load it cannot prove is aligned. No
// pragma fixes that - the POINTER TYPE has to change. Which is precisely why
// the read-side fix is the pack4/RGBX layout question and not a knob: making
// `ifmap` a `pack4_t*` is what buys 4-byte alignment, one request per column,
// and COL_GROUP at II=1. See the WRITE_ROW cost model for the options.
// ARTY CLASSIFIER FORK (2026-08-18): depth 는 **리터럴이 아니라 식**이어야 한다.
// 원본은 447180 (= 514*290*3, YOLO 레이어 0) 이 박혀 있었고, MAX_IMG_W 를
// 514 -> 66 으로 줄여도 **따라오지 않았다**. depth 는 하드웨어를 안 바꾸지만
// cosim 의 BFM 버퍼 크기를 정하므로, 테스트벤치가 그보다 작은 호스트 배열을
// 넘기면 ENTER_WRAPC 에서 SIGSEGV 다 - 2026-08-18 에 실제로 그렇게 죽었다.
// 형제 엔진(conv_engine)은 처음부터 식으로 써서 이 문제가 없었다.
#pragma HLS INTERFACE m_axi     port=ifmap   offset=slave bundle=RD_BUS depth=MAX_IMG_W*MAX_IMG_W*IN_CH
#pragma HLS INTERFACE m_axi     port=weights offset=slave bundle=RD_BUS depth=432
#pragma HLS INTERFACE m_axi     port=bias    offset=slave bundle=RD_BUS depth=16
// max_write_burst_length=256: WORTH ZERO IN COSIM. MEASURED, NOT ASSUMED.
//
// WRITE_ROW hands the tool a 2,048-word unit-stride burst, but the default cap
// is 16 beats, and the burst report notes that requests "might be further
// partitioned into multiple requests during RTL generation based on
// max_read_burst_length or max_write_burst_length" - so a row was being issued
// as 128 separate 16-beat bursts, each paying its own AW and B. That looked
// like an exact fit for the residual gap between csynth (8,253 cycles/row) and
// the first row-buffered cosim (10,616/row): 128 x ~18 ~= 2,300.
//
// IT IS NOT. Raising it to 256 (confirmed applied - the M_AXI table reports
// Max Write Burst Length 256) produced a cosim of 90,039 cycles against the
// default's 90,039 cycles. Identical to the cycle. The residual gap is NOT
// write-burst fragmentation; do not spend time on that theory again.
//
// KEPT ANYWAY, with the reason stated so nobody later reads this as a measured
// win: cosim drives an idealized AXI model, not DDR. 8 AW transactions per row
// instead of 128 cannot be worse on the real HP port, where page misses and
// refresh make per-transaction cost real, and it costs 11 LUT. But it is
// speculative for hardware and zero for every number in this file.
//
// 256 is the AXI4 maximum (AWLEN is 8 bits). num_write_outstanding is dropped
// to 4 from its default 16 deliberately: HLS sizes an internal write buffer at
// burst_length x outstanding x 32 bits, so leaving it at 16 would ask for
// 16 KB of BRAM to buy nothing - this is ONE sequential stream.
//
// No block-design impact: this changes the AWLEN values the master issues and
// the size of an internal buffer, not the port geometry. Same reasoning
// pack4.h's "WHAT THIS DOES NOT CHANGE" uses for the data width.
// depth: 원본은 589824 (= 512*288*16/4) 리터럴. 위 ifmap 과 같은 이유로 식으로.
#pragma HLS INTERFACE m_axi     port=ofmap   offset=slave bundle=WR_BUS depth=MAX_IMG_W*MAX_IMG_W*OUT_CH/PACK4_LANES \
    max_write_burst_length=256 num_write_outstanding=4
#pragma HLS INTERFACE s_axilite port=ifmap   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=weights bundle=CTRL
#pragma HLS INTERFACE s_axilite port=bias    bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ofmap   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=img_h   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=img_w   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=requant_multiplier bundle=CTRL
#pragma HLS INTERFACE s_axilite port=requant_shift      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=leaky_relu_enable  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL

#ifndef __SYNTHESIS__
    // Simulation-only guards, same convention as conv_engine.cpp: misuse
    // fails loudly in C-sim instead of silently indexing out of bounds.
    // Compiled out entirely in synthesis - zero hardware cost, and therefore
    // zero protection on the board, so SW must still not misconfigure this.
    assert(img_w <= MAX_IMG_W && "img_w exceeds the row buffer width");
    assert(img_w >= K && img_h >= K && "image smaller than the kernel");
    assert(requant_shift <= 48 && "requant_shift implausibly large for round_shift's ap_int<64>");
#endif

    const unsigned out_h = img_h - K + 1;
    const unsigned out_w = img_w - K + 1;

    // Weights (432 B) and bias (64 B) are loaded ONCE, not per output tile -
    // conv_engine re-reads them per oc_tile because it has many; this engine
    // has exactly one. Fully partitioned so the MAC array can read every tap
    // in the same cycle.
    weight_t wbuf[OUT_CH][IN_CH][K][K];
    bias_t   bbuf[OUT_CH];
#pragma HLS ARRAY_PARTITION variable=wbuf complete dim=0
#pragma HLS ARRAY_PARTITION variable=bbuf complete dim=0

LOAD_W:
    for (unsigned oc = 0; oc < OUT_CH; oc++) {
        for (unsigned ic = 0; ic < IN_CH; ic++) {
            for (unsigned ky = 0; ky < K; ky++) {
                for (unsigned kx = 0; kx < K; kx++) {
#pragma HLS PIPELINE II=1
                    wbuf[oc][ic][ky][kx] =
                        weights[((oc * IN_CH + ic) * K + ky) * K + kx];
                }
            }
        }
    }
LOAD_B:
    for (unsigned oc = 0; oc < OUT_CH; oc++) {
#pragma HLS PIPELINE II=1
        bbuf[oc] = bias[oc];
    }

    // Row buffer. dim=1 (slot) and dim=3 (channel) fully partitioned so one
    // position's whole KxKxIN_CH window is readable in a cycle; dim=2 (the
    // 514-wide column axis) stays in BRAM, which is where the memory actually
    // lives.
    static act_t row_buf[ROW_SLOTS][MAX_IMG_W][IN_CH];
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=3
// Column axis banked by K so that c, c+1, c+2 are in different
// banks - the COL_GROUP loop reads all three every cycle, and one
// BRAM has only 2 ports.
#pragma HLS ARRAY_PARTITION variable=row_buf cyclic factor=K dim=2
#pragma HLS BIND_STORAGE variable=row_buf type=RAM_2P impl=BRAM

    // Output row staging buffer - see the WHY THE ROW BUFFER EXISTS block at
    // the top of this file. One output row of PACKED words; COL_GROUP fills it
    // one word per iteration, WRITE_ROW drains it to DDR in one burst.
    //
    // NOT partitioned. Both loops touch exactly one word per cycle at
    // consecutive indices, so a single 2-port BRAM is already enough, and
    // partitioning would only obstruct the wide sequential read WRITE_ROW
    // wants. BRAM, never URAM, for the same reason row_buf gives in
    // conv0_engine.h: conv_engine holds URAM at 64/64 deliberately.
    static pack4_t out_row[MAX_OUT_ROW_WORDS];
#pragma HLS BIND_STORAGE variable=out_row type=RAM_2P impl=BRAM

PROLOGUE_ROWS:
    for (unsigned rr = 0; rr < K; rr++) {
    PROLOGUE_COLS:
        for (unsigned c = 0; c < img_w; c++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IMG_W
            for (unsigned ic = 0; ic < IN_CH; ic++) {
#pragma HLS UNROLL
                row_buf[rr][c][ic] = ifmap[((unsigned)rr * img_w + c) * IN_CH + ic];
            }
        }
    }

    // Words of `ofmap` per output row, and an EXPLICIT running base for them.
    //
    // Written as an accumulator rather than as `r * row_words` inside
    // WRITE_ROW on purpose. §34 (below) is this project's record of what HLS
    // does to a strength-reduced m_axi index containing a subtraction: it
    // evaluated `img_w*12 - 6` in THIRTY-THREE bits and zero-extended all 33
    // into the 64-bit address, issuing every access at base + 4 GiB - a bug
    // C-sim structurally cannot see, and one an `ap_uint<32>` cast does not
    // fix. `row_words` does contain a subtraction (out_w = img_w - K + 1), but
    // it is evaluated ONCE here, outside every loop, where there is no
    // induction variable for the tool to fold it into; WRITE_ROW then sees a
    // plain accumulator with no arithmetic in the index at all.
    const unsigned row_words  = out_w * (OUT_CH / PACK4_LANES);
    unsigned       ofmap_base = 0;

    // Same treatment for READ_ROW's `ifmap` index, and for the same §34
    // reason - except this one is even safer: `ifmap_row_bytes` has no
    // subtraction in it at all (K and IN_CH are compile-time constants), so
    // there is no negative constant for HLS to widen into a 33-bit address.
    // The first row READ_ROW fetches is row K, the prologue having already
    // loaded rows 0..K-1.
    const unsigned ifmap_row_bytes = img_w * IN_CH;
    unsigned       prefetch_base   = K * ifmap_row_bytes;

ROW_LOOP:
    for (unsigned r = 0; r < out_h; r++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=288
        const unsigned prefetch_row  = r + K;              // row to bring in
        const unsigned prefetch_slot = prefetch_row % ROW_SLOTS;
        const bool     do_prefetch   = (prefetch_row < img_h);

        // ------------------------------------------------------------------
        // PHASE 1 of 3: bring the next input row on chip.
        //
        // WHY THIS IS ITS OWN LOOP (2026-08-06). It used to be a 3-iteration
        // UNROLL'd `PREFETCH` block inside COL_GROUP, and that is what pinned
        // COL_GROUP at II=3:
        //
        //   [HLS 200-885] COL_GROUP: Unable to schedule bus request operation
        //   'RD_BUS_load_5_req' on port 'RD_BUS' due to limited memory ports
        //   (II = 1)                                    [and '_6_req', II = 2]
        //
        // One AXI port issues ONE request per cycle, and IN_CH=3 bytes per
        // column is 3 requests - so COL_GROUP could never beat II=3 while it
        // owned them, no matter what the MAC array did. Note this is a
        // REQUEST-COUNT limit, not a burst limit: these reads were already
        // reported "Inferred | variable", and confirming that,
        // `max_widen_bitwidth=32` (which would have merged the 3 byte loads
        // into one word load) changes NOTHING - see the ifmap pragma for the
        // alignment reason.
        //
        // Splitting them out does not make the reads themselves faster: 1,542
        // byte-beats per row cost 1,542 cycles either way. What it buys is
        // COL_GROUP dropping to II=1, because it is then left with no m_axi
        // access at all - 6,192 cycles/row -> 2,056.
        //
        // SAFE AGAINST THE COMPUTE that follows, for the reason ROW_SLOTS is
        // K+1 and not K: `prefetch_slot` is (r+K)%ROW_SLOTS, and COL_GROUP
        // reads slots r, r+1, r+2 (mod ROW_SLOTS). The slot written here is
        // the one slot not read this row, so running the whole prefetch
        // BEFORE the compute rather than interleaved with it changes timing
        // only - never a value. (Ordered read -> compute -> write also so the
        // three phases are already in pipeline order should they later go
        // under DATAFLOW.)
        //
        // `col`/`ic` are carried as explicit counters instead of `n / IN_CH`
        // and `n % IN_CH`: IN_CH is 3, so those would be a real divide/modulo
        // rather than the shift-and-mask a power of two would give.
        if (do_prefetch) {
            unsigned col = 0, ic = 0;
        READ_ROW:
            for (unsigned n = 0; n < img_w * IN_CH; n++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=3 max=1542
                row_buf[prefetch_slot][col][ic] = ifmap[prefetch_base + n];
                if (ic == IN_CH - 1) { ic = 0; col++; } else { ic++; }
            }
        }

            // (c, g) FLATTENED INTO ONE PIPELINED LOOP (2026-08-05, 3rd
            // structure - see TROUBLESHOOTING §33 for the first two and why
            // each failed).
            //
            //   1st: COL_LOOP `PIPELINE II=4`. Hit II=4, but a PIPELINE on an
            //        outer loop fully unrolls every inner loop, so the
            //        4-iteration write loop became 16 spatial activation
            //        copies -> 336 DSP.
            //   2nd: COL_LOOP unpipelined + inner OC_GROUP `II=1`. Got the
            //        resources right (87 DSP) but the per-position regions
            //        then ran sequentially -> 31 cycles/position, 4.58M total.
            //   3rd (this): iterate over (column, output-group) as ONE loop at
            //        II=1. 4 iterations per column = the same 4 AXI write
            //        beats, but only ONE group's hardware exists (108 MACs,
            //        4 activations) and it is time-multiplexed.
            //
            // row_buf gains a cyclic(K) partition on its COLUMN axis below so
            // that columns c, c+1, c+2 land in different banks - otherwise
            // this loop needs 3 reads per cycle from one BRAM (2 ports) and
            // the tool is forced to II=2.
            //
            // THE LOOP SPANS img_w COLUMNS, NOT out_w - and that is what
            // absorbed the old EPILOGUE_COLS loop. See TROUBLESHOOTING §34.
            //
            // The last K-1 columns produce no output; they exist only so that
            // the prefetch below covers the whole input row. Written instead
            // as a separate trailing loop over `c = out_w .. img_w-1`, HLS
            // strength-reduces that loop's ifmap index to `img_w*12 - 6`,
            // evaluates it in THIRTY-THREE bits (as `x + 2^32 - 6`, modelling
            // the 32-bit wrap explicitly) and then ZERO-extends all 33 bits
            // into the 64-bit m_axi address:
            //
            //   assign add_ln168_2  = (zext_ln168_2 + 33'd4294967290);  // -6
            //   assign zext_ln168_5 = add_ln168_2;                      // 33->64
            //
            // so every epilogue read is issued at ifmap + 4 GiB. C-sim cannot
            // see it (the C value is a correct 32-bit unsigned; only the RTL
            // address is wrong - the same class of bug as §33-a's `act_t*
            // ofmap`), and an explicit `ap_uint<32>` truncation does NOT fix
            // it: HLS folds the cast away and re-derives the 33-bit form.
            // Folding the columns into this loop removes the expression
            // instead, reusing the base the prefetch already uses - an
            // induction variable (`phi_mul + img_w*9`) with no subtraction in
            // it, which cosim has executed 512x per row without complaint.
        COL_GROUP:
            for (unsigned i = 0; i < img_w * (OUT_CH / PACK4_LANES); i++) {
// ZYBO PORT - EXPERIMENT D5 (2026-08-14): halving this engine's MAC hardware
// was tried and is a MEASURED REJECTION. Do not re-try it.
//
// Tried: `PIPELINE II=2` + `ALLOCATION operation instances=mul limit=54`,
// i.e. share 54 multipliers across the 108 MACs instead of building all 108.
// Motivation: on XC7Z020 this engine synthesises to 13,223 LUT - 24.9% of the
// device - for a layer worth only ~9% of the frame, the worst
// area-per-frame-share ratio in the design, and the system was sitting at
// LUT 53,276 / 53,200 = 100.14%.
//
// MEASURED (csim still bit-exact against REAL_LAYERS[0]):
//     LUT     22,357 -> 25,025   (+2,668, WORSE - and LUT is the constraint)
//     DSP         93 -> 33       (-60, a resource we already have spare)
//     cycles 1,655,365 -> 2,838,469  (1.71x slower)
// Baseline report preserved in ../syn_report_baseline_2026-08-14/.
//
// WHY IT BACKFIRED - the part worth remembering: these 108 MACs were never
// in LUTs. They were in DSPs (93 of them). Sharing multipliers does not
// delete LUT logic, it ADDS it - the muxes that steer 108 operand pairs
// through 54 multipliers are built out of LUTs. So the trade spent the
// resource that is scarce (LUT) to save the one that is spare (DSP), and
// paid 1.71x on layer 0 for the privilege.
//
// GENERAL RULE this is an instance of: before sharing a compute resource to
// save area, check WHICH primitive that compute actually occupies. Sharing
// only helps when the shared thing is made of the resource you are short of.
//
// Levers considered and their status, so nobody re-derives this list:
//   - conv_engine PE_OC/TR: carries ~81% of the frame. TR 8->4 caps the
//     engine at 64 MAC/cycle and pushes the frame to ~40.9 M cycles - a far
//     worse trade than anything here.
//   - SmartConnect (8,370 LUT): splitting it was measured WORSE (v2:
//     10,682) - per-instance fixed cost dominates.
//   - Address-arithmetic DSPs: already harvested in maxpool/upsample/
//     route_concat (-10 DSP). DSP is no longer binding; LUT is.
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=4 max=2056
                const unsigned c = i / (OUT_CH / PACK4_LANES);
                const unsigned g = i % (OUT_CH / PACK4_LANES);

                // The tail columns produce no OUTPUT (they exist only to carry
                // the prefetch across the full input row), though they do
                // still run the MACs - into out_row slots WRITE_ROW never
                // reads. `cw` keeps their window reads inside row_buf: a raw
                // `c + kx` would reach img_w + 1 = 515 against a 514-wide
                // buffer.
                //
                // `emit` no longer gates a store - see out_row below. It now
                // only clamps the window, which is why it can be a mux on an
                // index rather than a branch around an m_axi access.
                const bool     emit = (c < out_w);
                const unsigned cw   = emit ? c : 0;

                accum_t acc[PACK4_LANES];
#pragma HLS ARRAY_PARTITION variable=acc complete dim=0
            INIT_ACC:
                for (unsigned lane = 0; lane < PACK4_LANES; lane++) {
#pragma HLS UNROLL
                    acc[lane] = bbuf[g * PACK4_LANES + lane];
                }

                // 108 MACs. Operands NOT pre-cast - §33-a: casting two int8
                // values up before multiplying forces a 32x32 multiply that
                // spills across four DSP48E2s.
            MAC_KY:
                for (unsigned ky = 0; ky < K; ky++) {
#pragma HLS UNROLL
                    const unsigned slot = (r + ky) % ROW_SLOTS;
                MAC_KX:
                    for (unsigned kx = 0; kx < K; kx++) {
#pragma HLS UNROLL
                    MAC_IC:
                        for (unsigned ic = 0; ic < IN_CH; ic++) {
#pragma HLS UNROLL
                            const act_t v = row_buf[slot][cw + kx][ic];
                        MAC_LANE:
                            for (unsigned lane = 0; lane < PACK4_LANES; lane++) {
#pragma HLS UNROLL
                                acc[lane] += v * wbuf[g * PACK4_LANES + lane][ic][ky][kx];
                            }
                        }
                    }
                }

                // 4 activation units - the whole point of this structure.
                pack4_t word = 0;
            FINISH_PACK:
                for (unsigned lane = 0; lane < PACK4_LANES; lane++) {
#pragma HLS UNROLL
                    pack4_set(word, lane,
                              apply_activation(acc[lane], leaky_relu_enable,
                                               requant_multiplier, requant_shift));
                }
                // Unconditional, and to BRAM rather than DDR. The index is
                // just `i`: (c * groups + g) with c = i/groups, g = i%groups
                // IS i, so this is a unit-stride fill. The tail columns
                // (c >= out_w) do land here, writing the garbage their cw=0
                // window produced into slots WRITE_ROW never reads - that is
                // what MAX_OUT_ROW_WORDS is sized for, and it is what buys the
                // store its way out of the conditional branch the burst table
                // complained about.
                out_row[i] = word;
            }

        // The whole output row, to DDR, in one shot. Everything the burst
        // analyzer needs is now true of this loop and was true of none of its
        // predecessors: the store is unconditional, the address is a bare
        // induction variable at unit stride, and the loop body does nothing
        // else - no read on the same bundle, no branch, no arithmetic on the
        // index. Same shape as COL_GROUP's ifmap read, which the burst table
        // has always reported as "Inferred | variable".
        //
        // Byte-for-byte identical output to the per-position store it
        // replaces, in the identical order - only the timing moves. The gate
        // for that is automatic: conv0_engine_tb.cpp compares all 2,359,296
        // values against REAL_LAYERS[0].
        //
        // COST MODEL of the finished 3-phase structure, from csynth (2024.2,
        // 2026-08-06). This supersedes the 4-cycles/position model in
        // conv0_engine.h, which was never achievable as written:
        //
        //     READ_ROW    II=1, 1,578 cycles/row   (img_w * IN_CH byte-beats)
        //     COL_GROUP   II=1, 2,082 cycles/row   (img_w * OUT_CH/4 groups)
        //     WRITE_ROW   II=1, 2,051 cycles/row   (out_w * OUT_CH/4 words)
        //     ROW_LOOP          5,722 cycles/row  = 11.2 cycles/position
        //
        // All three phases are at II=1 and every m_axi access is a burst, so
        // no phase can be made shorter without changing WHAT it moves:
        //   - WRITE_ROW is at the hard bound. 16 int8 out per position on a
        //     32-bit port IS 4 beats; only a wider port beats it, and §29-d /
        //     conv0_engine.h reject that.
        //   - COL_GROUP is at the compute bound this engine was sized for:
        //     4 groups/position x 108 MACs.
        //   - READ_ROW moves 3 bytes/column as 3 byte-beats. THIS is the one
        //     phase with slack - a 32-bit port could carry those 3 bytes in
        //     one beat, taking 1,578 -> ~530. It needs `ifmap` to become a
        //     `pack4_t*` (4-byte alignment), which layer 0 alone cannot do
        //     because IN_CH=3 does not divide 4. See the ifmap pragma for why
        //     no pragma reaches this, and RGBX input padding for the fix that
        //     does - it is a SW data-contract change, not a HW one.
        //
        // WHAT IS LEFT AFTER THAT is overlap. The three phases run BACK TO
        // BACK, so the row costs their SUM; under #pragma HLS DATAFLOW with a
        // ping-pong out_row it would cost their MAX (~2,082 = 4.1
        // cycles/position, which is the original target reached honestly).
        // DATAFLOW is the biggest single remaining lever AND the riskiest:
        // tried once in this project and reverted (RESOURCE_BUDGET.md §5),
        // where it hard-errored on m_axi bundle sharing - which this engine
        // still has, with ifmap/weights/bias all on RD_BUS.
    WRITE_ROW:
        for (unsigned n = 0; n < row_words; n++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=4 max=2048
            ofmap[ofmap_base + n] = out_row[n];
        }
        ofmap_base    += row_words;
        prefetch_base += ifmap_row_bytes;
    }
}
