#include "maxpool_engine.h"
#include <cassert>

void maxpool_engine(
    const pack4_t *ifmap,
    pack4_t       *ofmap,
    uint16_t img_h, uint16_t img_w, uint16_t ch,
    uint8_t  stride,
    uint8_t  pad_right, uint8_t pad_bottom
) {
    // depth= is now in 32-bit WORDS, not int8 elements - the total byte
    // footprint each port can address is unchanged (MAX_CH_WORDS is
    // MAX_CH/4), it is only the unit of the pointer that changed.
#pragma HLS INTERFACE m_axi port=ifmap offset=slave bundle=RD_BUS depth=MAX_IMG_W*MAX_IMG_W*MAX_CH_WORDS
#pragma HLS INTERFACE m_axi port=ofmap offset=slave bundle=WR_BUS depth=MAX_IMG_W*MAX_IMG_W*MAX_CH_WORDS

#pragma HLS INTERFACE s_axilite port=ifmap      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ofmap      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=img_h      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=img_w      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ch         bundle=CTRL
#pragma HLS INTERFACE s_axilite port=stride     bundle=CTRL
#pragma HLS INTERFACE s_axilite port=pad_right  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=pad_bottom bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return     bundle=CTRL

#ifndef __SYNTHESIS__
    assert(img_h <= MAX_IMG_W && "img_h exceeds MAX_IMG_W - see maxpool_engine.h");
    assert(img_w <= MAX_IMG_W && "img_w exceeds MAX_IMG_W - see maxpool_engine.h");
    assert(ch <= MAX_CH && "ch exceeds MAX_CH - see maxpool_engine.h");
    // New in the pack4 variant: every pixel must occupy a whole number of
    // 32-bit words, so the next pixel's channel 0 always starts on a word
    // boundary and a whole-word store can never clobber a neighbour. Every
    // real maxpool layer has ch in {16,32,64,128,256,512}. See pack4.h for
    // why a remainder tail is deliberately not supported.
    assert((ch % PACK4_LANES) == 0 &&
           "ch must be a multiple of 4 in the pack4 variant - see pack4.h");
    assert((stride == 1 || stride == 2) && "stride must be 1 or 2 - see RTL_HANDOFF_KO.md section 6");
    assert(pad_right <= 1 && pad_bottom <= 1 && "pad_right/pad_bottom must each be 0 or 1");
    // RTL_HANDOFF_KO.md section 7 only ever pairs stride=1 with padding
    // (layer 11) and stride=2 with no padding (every other maxpool layer) -
    // asserting this combination catches a misprogrammed layer descriptor
    // loudly in sim rather than silently computing the wrong output shape.
    assert(!(stride == 2 && (pad_right != 0 || pad_bottom != 0)) &&
           "stride=2 must not be combined with padding - see RTL_HANDOFF_KO.md section 6/7");
    // NOT asserted: stride==2 with an odd img_h/img_w. Every real stride=2
    // layer happens to be even (RTL_HANDOFF_KO.md section 7), but an odd
    // dimension is not an invalid config - out_h/out_w's integer division
    // below just drops the last row/column, well-defined truncation
    // behavior, not undefined/garbage output. See the testbench's
    // MP-C config, which deliberately exercises exactly this case.
    // (Odd SPATIAL dims stay supported; only odd CHANNEL counts do not.)
#else
    (void)0;
#endif

    const unsigned out_h = (img_h + (unsigned)pad_bottom - 2u) / stride + 1u;
    const unsigned out_w = (img_w + (unsigned)pad_right  - 2u) / stride + 1u;

    // Channels per pixel expressed in beats. Every loop below counts in
    // these units, which is the entire speedup: the trip count of each
    // inner loop drops by 4x while II stays at 1.
    const unsigned ch_words = (unsigned)ch / PACK4_LANES;

    // -----------------------------------------------------------------
    // Row-buffer path (2026-08-09). Why it exists:
    //
    // The per-position path below runs SIX pipelined loops for every output
    // position (INIT + 4 window passes + WRITE), and each one pays its own
    // fill/drain. Measured across the two extremes of the network:
    //
    //     cycles/position ~= 6 * (ch_words + 12)
    //     idx0 (ch_words=4)   predicted  96  measured  97.1
    //     idx5 (ch_words=128) predicted 840  measured 843.8
    //
    // So the cost is dominated by loop entry/exit, not by the compares -
    // the same pathology conv0_engine was built to fix for layer 0. This
    // path loads two input rows as bursts, folds the whole 2x2 window into
    // ONE pipelined loop over (c_out, w), and writes the output row as a
    // burst: one fill/drain per row instead of six per position.
    //
    // Restricted to stride==2 on purpose. stride==1 (layer 11, the only one)
    // overlaps rows and needs a different buffer discipline; it is 0.5% of
    // the frame, so it keeps the original path. See
    // doc/02_plans/2026-08-09_next-lever-maxpool.md.
    //
    // ⚠️ Do NOT "simplify" this by reading the four window pixels straight
    // from m_axi in one loop. That is what makes it fast here - the reads
    // come from BRAM. Going back to m_axi interleaves four addresses and
    // kills burst inference, which is exactly how conv0_engine's ofmap
    // ended up at 34.1 cycles/position before its own row buffer.
    const unsigned row_words     = (unsigned)img_w * ch_words;
    const unsigned out_row_words = out_w * ch_words;
// ZYBO PORT - EXPERIMENT D2 (2026-08-14): forcing these two onto fabric was
// tried, TWICE, and REJECTED both times. Do not re-try it. Reports kept in
// ../syn_report_d2_nolatency/ next to the ../syn_report_baseline_2026-08-14/
// they should be read against.
//
// Motivation was sound: max() contains no multiply, so all 12 of this
// engine's DSPs are address arithmetic, and the 5-engine total came in at
// 221 against XC7Z020's 220. Address multiplies are the cheap place to give
// DSPs back - conv_engine's MAC_REDUCE (64) and conv0_engine's 108-MAC
// array (87) are not.
//
//   D2  `impl=fabric`             DSP 12->10  LUT 6,412->7,083  Slack -0.09 -> -2.60
//   D2b `impl=fabric latency=3`   DSP 12->10  LUT 6,412->7,097  Slack -0.09 -> -2.60
//
// Both bought the -2 DSP and both cost the clock: estimated period 7.39 ns
// -> 9.897 ns, i.e. this engine's Fmax fell ~135 MHz -> ~101 MHz, which
// would have made maxpool the SYSTEM critical path (conv_engine estimates
// 130.57 MHz). Paying ~25% of the clock for 2 DSP is a bad trade.
//
// D2b also disproves the obvious fix. `latency=3` DID take effect - total
// latency moved 224,400,423 -> 224,400,426, exactly +3 - and timing did not
// improve by one picosecond. So the critical path is NOT the multiply's own
// combinational delay; forcing fabric reorganises which operations share
// which instances, and the damage is downstream of that. "Register the
// multiply" does not recover it.
//
// AND THE WHOLE THING IS UNNECESSARY, which is the real lesson. This
// engine's DSP table reports `Impl: auto` for all 12 and `dsp_slice` for
// ZERO of them: HLS emits a plain `*` in the Verilog and VIVADO decides
// DSP-vs-LUT at synthesis. When DSPs run short Vivado demotes multipliers
// to fabric by itself - the same trade done here by hand, except Vivado
// picks which ones using real post-scheduling timing instead of guessing.
// D2 guessed, and guessed into the critical path.
//
// So: 221/220 is a projection, not a hard requirement. Ship it and let the
// Vivado build resolve it. If Vivado genuinely cannot fit, come back here -
// but bind the multiplies that are NOT on the critical path, and read
// ../README.md section 4 first.

    if (stride == 2 && row_words <= MAX_ROW_WORDS) {
        static pack4_t row0[MAX_ROW_WORDS];
        static pack4_t row1[MAX_ROW_WORDS];
#pragma HLS BIND_STORAGE variable=row0 type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=row1 type=ram_2p impl=bram
        // W7 (2026-08-19): `orow` is gone. RB_COMPUTE used to stage the output
        // row here and RB_WRITE copied it to ofmap - one extra loop REGION and
        // one extra pass of `out_row_words` iterations per output row, for a
        // buffer nothing else read. Measured cost of that pass: the three pool
        // shapes all fit `6 x (out_w x ch_words) + ~88` per output row, and
        // the write pass was one of those six. RB_COMPUTE now stores straight
        // to ofmap; the address is `obase + idx`, still affine in the loop
        // counter, which is the property the burst inference needs (see the
        // opack note in conv_engine.cpp for what happens when it is lost).

        // ZYBO PORT (2026-08-14, EXPERIMENT D3): running bases instead of
        // `in_r0 * row_words` / `r_out * out_row_words`. All three advance by a
        // fixed stride once per output row and RB_ROW_LOOP is a plain in-order
        // scan, so they accumulate instead of multiplying - THREE runtime
        // multiplies deleted.
        //
        // THIS IS THE RIGHT VERSION OF D2. D2 (rejected, see the block at the
        // top of this file) tried to give DSPs back with
        // `BIND_OP impl=fabric`, which KEEPS the multiply and merely rebuilds
        // it out of LUTs - it bought -2 DSP and cost ~25% of the clock
        // (slack -0.09 -> -2.60). Strength reduction DELETES the multiply, so
        // it cannot lengthen a path; the only thing it adds is an adder.
        //
        // Why it is needed: XC7Z020 has 220 DSPs and v1 Vivado synthesis came
        // back at 226. maxpool's arithmetic is max() - every one of its 12
        // DSPs is address arithmetic, which makes it the cheapest place to
        // give some back.
        //
        // Correctness rests on the in-order scan with no early exit. csim
        // (7 configs incl. the odd-dimension MP-C case + the real layers) is
        // the check; a stride bug lands every row after the first in the
        // wrong place, which is not subtle.
        unsigned base0 = 0;   // in_r0 * row_words,  in_r0 = 2*r_out
        unsigned obase = 0;   // r_out * out_row_words
    RB_ROW_LOOP:
        for (unsigned r_out = 0; r_out < out_h; r_out++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IMG_W
            const unsigned in_r0 = r_out * 2u;
            const unsigned in_r1 = in_r0 + 1u;
            const unsigned base1 = base0 + row_words;
            // stride=2 is never combined with padding (asserted above), so
            // in_r1 is in bounds whenever the output row exists - except for
            // an odd img_h, where the last input row has no partner. The
            // guard keeps that case well-defined (the testbench's MP-C
            // config exercises an odd dimension deliberately).
            const bool r1_ok = (in_r1 < (unsigned)img_h);

        RB_LOAD0:
            for (unsigned i = 0; i < row_words; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROW_WORDS
                row0[i] = ifmap[base0 + i];
            }
        RB_LOAD1:
            for (unsigned i = 0; i < row_words; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROW_WORDS
                row1[i] = r1_ok ? ifmap[base1 + i] : (pack4_t)PACK4_ALL_MINUS_128;
            }

            // ONE pipelined loop over the whole output row. c_out/w are
            // carried as counters rather than derived by division, and the
            // column base advances by 2*ch_words per output column, so the
            // body has no divides and no multiplies on the critical path.
            unsigned w      = 0;
            unsigned col_in = 0;   // (2*c_out) * ch_words
        RB_COMPUTE:
            for (unsigned idx = 0; idx < out_row_words; idx++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROW_WORDS
                pack4_t a = row0[col_in + w];
                pack4_t b = row0[col_in + ch_words + w];
                pack4_t c = row1[col_in + w];
                pack4_t d = row1[col_in + ch_words + w];
                pack4_t nm = 0;
            RB_LANES:
                for (unsigned lane = 0; lane < PACK4_LANES; lane++) {
#pragma HLS UNROLL
                    act_t va = pack4_get(a, lane);
                    act_t vb = pack4_get(b, lane);
                    act_t vc = pack4_get(c, lane);
                    act_t vd = pack4_get(d, lane);
                    act_t m = va;
                    if (vb > m) m = vb;
                    if (vc > m) m = vc;
                    if (vd > m) m = vd;
                    pack4_set(nm, lane, m);
                }
                ofmap[obase + idx] = nm;

                if (w + 1u == ch_words) { w = 0; col_in += 2u * ch_words; }
                else                    { w++; }
            }

            // Advance to the next output row. Per-row strides, so this must
            // stay at RB_ROW_LOOP scope, outside RB_WRITE.
            base0 += 2u * row_words;
            obase += out_row_words;
        }
        return;
    }

ROW_LOOP:
    for (unsigned r_out = 0; r_out < out_h; r_out++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IMG_W
    COL_LOOP:
        for (unsigned c_out = 0; c_out < out_w; c_out++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IMG_W
            // Deliberately not pipelined at this level - see
            // conv_engine.cpp's ROW_LOOP/COL_LOOP for the same reasoning:
            // the parallelism knob is the inner channel loop's PIPELINE
            // II=1, not this level.

            // 4 running maxima per entry, one per lane. Same total bits as
            // the baseline's act_t maxval[MAX_CH], just addressed by word.
            pack4_t maxval[MAX_CH_WORDS];

        MAXPOOL_INIT:
            for (unsigned w = 0; w < ch_words; w++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_CH_WORDS
                maxval[w] = PACK4_ALL_MINUS_128;
            }

            // Fixed 2x2 window - dy/dx are compile-time constants (0,1),
            // not unrolled over a runtime bound, so this is 4 literal
            // passes, each independently PIPELINE'd over the channel loop.
            for (unsigned dy = 0; dy < 2; dy++) {
                for (unsigned dx = 0; dx < 2; dx++) {
                    unsigned in_r = r_out * stride + dy;
                    unsigned in_c = c_out * stride + dx;
                    // Upper-bound check only: padding is right/bottom-only
                    // (RTL_HANDOFF_KO.md section 6), so unlike conv_engine's
                    // symmetric pad there is no lower-bound underflow case
                    // to guard against here.
                    bool in_bounds = (in_r < img_h) && (in_c < img_w);

                MAXPOOL_UPDATE:
                    for (unsigned w = 0; w < ch_words; w++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_CH_WORDS
                        // if/else (not a ternary selecting the read),
                        // matching conv_engine.cpp's READ_CH convention -
                        // keeps the m_axi read conditionally EXECUTED, not
                        // just conditionally selected, so padding
                        // positions never issue a garbage-address AXI
                        // transaction.
                        if (in_bounds) {
                            pack4_t v = ifmap[((unsigned)in_r * img_w + in_c) * ch_words + w];
                            pack4_t m = maxval[w];
                            pack4_t nm = 0;
                        MAXPOOL_LANES:
                            for (unsigned lane = 0; lane < PACK4_LANES; lane++) {
#pragma HLS UNROLL
                                // 4 independent int8 comparators in one
                                // cycle. UNROLL (not PIPELINE) because the
                                // lanes are the WIDTH of a single beat -
                                // they must all retire together for this
                                // loop to keep II=1 over `w`.
                                act_t a = pack4_get(v, lane);
                                act_t b = pack4_get(m, lane);
                                // Explicit if/else rather than a ternary,
                                // matching route_concat_engine.cpp's
                                // round_shift() convention for ap_int
                                // operands.
                                if (a > b) {
                                    pack4_set(nm, lane, a);
                                } else {
                                    pack4_set(nm, lane, b);
                                }
                            }
                            maxval[w] = nm;
                        }
                    }
                }
            }

        MAXPOOL_WRITE:
            for (unsigned w = 0; w < ch_words; w++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_CH_WORDS
                ofmap[((unsigned)r_out * out_w + c_out) * ch_words + w] = maxval[w];
            }
        }
    }
}
