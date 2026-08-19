#include "conv_engine.h"
#include <cassert>

// Widened to ap_int<64> (was accum_t/32-bit): apply_activation()'s
// round_shift(acc * requant_multiplier, requant_shift) product can reach
// ~62 bits (27-bit accumulator_bound x 31-bit multiplier, per
// model_manifest.json) before the shift brings it back into int8 range -
// saturating on the narrower, already-truncated value would silently
// corrupt exactly the large-magnitude cases this check exists to catch.
static act_t saturate(ap_int<64> v) {
    if (v > 127)  return (act_t)127;
    if (v < -128) return (act_t)-128;
    return (act_t)v;
}

// round(x / 2^s), ties away from zero - matches RTL_HANDOFF_KO.md section 5
// exactly: `round_shift(x, s) = sign(x) * ((abs(x) + 2^(s-1)) >> s)`. NOT the
// same as a plain arithmetic `x >> s` on a biased value: two's-complement
// right shift rounds negative numbers toward -infinity, not away from zero,
// so the sign has to be split out explicitly. Used for both the LeakyReLU
// slope (13/128) and the final per-layer requantization - one definition,
// not two, specifically so a rounding bug (RESOURCE_BUDGET.md/TROUBLESHOOTING
// #8 already has one war story about a split-arithmetic bug like this) can't
// silently diverge between the two call sites.
static ap_int<64> round_shift(ap_int<64> x, ap_uint<6> s) {
#pragma HLS INLINE
    if (s == 0) return x;
    ap_int<64> half = (ap_int<64>)1 << (s - 1);
    // Explicit if/else with an explicit ap_int<64> cast on each return -
    // NOT a ternary: ap_int's natural bit-growth widens `x + half` to
    // ap_int<65> on this branch, which the compiler cannot unify with the
    // other branch's ap_int<64> inside a single `?:` expression (a real
    // build error, caught by actually compiling this against Vitis's
    // standalone ap_int.h, not by inspection).
    if (x >= 0) {
        return (ap_int<64>)((x + half) >> s);
    } else {
        return (ap_int<64>)(-(((-x) + half) >> s));
    }
}

// LeakyReLU (13/128, applied to the raw accumulator BEFORE requantization -
// RTL_HANDOFF_KO.md section 5, not the generic "0.1" a float model would use)
// + per-layer requantization (requant_multiplier/requant_shift, from the
// golden model's model_manifest.json) + int8 saturation, in that order.
//
// INLINE (2026-08-04) - this reverses an earlier, deliberate "NOT INLINE"
// decision, and the reversal is safe only because the CALLER's structure
// changed, not because the old reasoning was wrong. History: a first
// version inlined this into PE_PAIR_LOOP's UNROLL'd body and a real csynth
// run came back at 105% LUT - round_shift()'s final call shifts by
// `requant_shift`, a genuine runtime value, needing a real variable-width
// 64-bit barrel-shift mux, and the UNROLL gave it 20 separate spatial
// copies (PE_PAIRS x lo/hi), one mux tree each. Un-inlining collapsed that
// to ONE shared block time-multiplexed across the call sites.
//
// That shared block is exactly what real-layer cosim (2026-08-03/04,
// TROUBLESHOOTING.md §23-§24 + the csynth Instance table) then measured as
// the design's dominant per-position cost once the SHIFT_WINDOW fix
// landed: `accumulate_or_finish` (this function's only caller) synthesized
// as ONE NON-PIPELINED instance with Interval 10-21 cycles, and the old
// PE_PAIR_FINISH made 2*PE_PAIRS=24 back-to-back calls into it per output
// position - 240-500 cycles/position, vs ~36-78 for the whole MAC_REDUCE.
// That serialization, not LUT, is why measured conv efficiency was 4.3%.
//
// The only caller is now FINISH_WR (see scan_and_compute's phase-split
// finish loops): a single PIPELINE II=1 loop over lanes, NOT an UNROLL -
// so inlining this creates exactly ONE spatial copy of the barrel
// shift/multiply (same spatial count as the shared-instance version),
// scheduled inside the pipelined body instead of behind a 10-21-cycle
// non-pipelined function-call interval per lane. The 20-copy LUT
// explosion cannot recur because nothing UNROLLs over this function
// anymore; if a future change puts a call to this back under an UNROLL'd
// loop, re-read the history above before keeping the INLINE.
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

// Input-channel tiling (added once the real YOLOv3-tiny-ADAS network showed
// 8 of its 13 conv layers need in_ch > MAX_IN_CH - see conv_engine.h): one
// PE_PAIR's raw (pre-activation) sum for ONE output pixel, carried across
// ic_tile passes via `accum` (DDR scratch, [out_h][out_w][PE_OC] - see
// conv_engine.h's note on the top-level `accum` parameter for why PE_OC, not
// out_ch). `num_ic_tiles == 1` (true for 5 of the 13 real conv layers, and
// every conv_engine_tb.cpp config before this feature existed) takes the
// bottom branch and never touches `accum` at all - byte-for-byte the same
// behavior this function had before tiling existed.
//
// accumulate_or_finish() used to live here - the per-lane helper carrying
// one output pixel's raw sum across ic_tile passes via the DDR `accum`
// scratch (see conv_engine.h's note on the top-level `accum` parameter).
// Removed 2026-08-04: as a NON-INLINED helper it synthesized to one shared
// non-pipelined instance (Interval 10-21) that the old UNROLL'd
// PE_PAIR_FINISH called 24x back-to-back per output position (~240-500
// cycles/position, ~half of layer 9's measured per-position cost), and
// re-INLINING it into a single pipelined lane loop only achieved II~14
// because its internal first/middle/last-ic_tile branching mixed a
// conditional accum read with accum/ofmap writes in one loop body. Its
// logic now lives flattened in scan_and_compute()'s FINISH_GATHER/
// ACCUM_RD/FINISH_WR/ACCUM_WR phase loops - see the comment there for the
// full two-step history and the measurements behind it. Behavior
// (including the num_ic_tiles==1 fast path never touching `accum`) is
// preserved exactly.

// ---------------------------------------------------------------------------
// load_weight_tile / scan_and_compute are split into separate functions -
// not just for readability, but so weight-tile double buffering (see
// RESOURCE_BUDGET.md §5, tried 2026-07-23, reverted - see that section for
// why) can later wrap these two calls in #pragma HLS DATAFLOW with ping-pong
// wtile/btile buffers, without restructuring the algorithm itself. wtile/
// btile are owned by conv_engine() (the caller) and passed in, precisely so
// a future ping-pong version only has to change the caller, not these two
// functions' internals.
// ---------------------------------------------------------------------------

static void load_weight_tile(
    const pack4_t *weights, const pack4_t *weights_hi, const bias_t *bias,
    unsigned oc_tile, unsigned ic_lo, unsigned ic_count, unsigned in_ch, unsigned out_ch, unsigned k,
    packed_weight_t wtile_packed[PE_PAIRS][MAX_IN_CH][MAX_K][MAX_K],
    bias_t btile[PE_OC]
) {
    // Forced INLINE, not left to the tool's default heuristics: Vitis HLS
    // has a real, documented gotcha where fully-partitioned arrays passed
    // across a NON-inlined function boundary can silently lose their
    // parallel-access partitioning (the boundary implies a port-like
    // interface that doesn't always preserve per-element parallel access).
    // wtile_packed is partitioned in conv_engine() (its declaration site) for
    // exactly the parallelism the MAC reduction in scan_and_compute()
    // depends on - relying on the tool happening to auto-inline this
    // function by default would make that parallelism dependent on
    // unstated tool-version behavior instead of an explicit pragma. A
    // DATAFLOW/double-buffered version (RESOURCE_BUDGET.md §5) would
    // deliberately remove this pragma, since DATAFLOW requires its stages
    // to NOT be inlined - tried 2026-07-23, reverted (see that section):
    // DATAFLOW itself hit a hard error unrelated to inlining (ifmap/weights/
    // bias sharing one AXI bundle), so this pragma's own correctness was
    // never actually tested in that attempt.
#pragma HLS INLINE
    // INT8 DSP-packing experiment (conv_engine.h): the pack step (bias-XOR +
    // shift + add) happens HERE, once per oc_tile, deliberately NOT inside
    // scan_and_compute()'s per-cycle MAC_IC/MAC_KY/MAC_KX reduction - hoisting
    // it out is what actually halves wtile's own instance count (256 -> 128)
    // on top of halving the multiplier instance count, since this loop runs
    // once per tile rather than being replicated TR*PE_PAIRS times.
LOAD_WTILE:
    for (unsigned j = 0; j < PE_PAIRS; j++) {
        unsigned oc_lo = oc_tile * PE_OC + 2 * j;
        unsigned oc_hi = oc_tile * PE_OC + 2 * j + 1;
        bool lo_valid = oc_lo < out_ch;
        bool hi_valid = oc_hi < out_ch;
        if (lo_valid || hi_valid) {
            if (lo_valid) btile[2 * j]     = bias[oc_lo];
            if (hi_valid) btile[2 * j + 1] = bias[oc_hi];
        LOAD_W_IC:
            // Each nesting level gets its OWN LOOP_TRIPCOUNT, bounding only
            // that loop's trip count - NOT a single combined max=IN_CH*K*K on
            // the innermost loop (tried first, and wrong): Vitis HLS checks a
            // directive's max against its own internally-computed average for
            // THAT loop level, and a product-of-all-levels value on the
            // innermost loop failed that check ("Ignored invalid trip count
            // directive (MAX (= 255) < AVE (= 576))" - the tool had already
            // clipped the requested 1,152 down to 255 before even comparing).
            // Without a per-level bound, the tool assumed the full range of
            // in_ch/k's underlying types (in_ch up to 65,535, k up to 255)
            // instead of MAX_IN_CH/MAX_K, reporting an absurd ~4.26 billion
            // cycle / 21 second worst case for this one region alone.
            // Bound `ic_count` (this ic_tile's local channel count, <=
            // MAX_IN_CH), NOT `in_ch` (the layer's real total) - `ic_count`
            // is what determines how many of wtile_packed's MAX_IN_CH slots
            // this pass actually fills; the global channel index fed into
            // weights[]'s address expression is `ic_lo + ic` instead of
            // plain `ic`, to select this tile's slice of the real weight
            // array (still laid out for the FULL `in_ch`, unaffected by
            // tiling - only which slice of it a given pass reads changes).
            // WPACK: ky/kx are now the OUTER loops and `ic` the innermost,
            // because the DDR layout changed to [oc][ky][kx][ic] so that one
            // 32-bit beat carries 4 CONSECUTIVE ic. See pack4.h's WPACK note.
            //
            // `in_ch % PACK4_LANES == 0` is what makes the word index exact:
            // base = ((oc*k + ky)*k + kx)*in_ch + ic_lo, and ic_lo is always a
            // multiple of MAX_IN_CH (128), so base is a multiple of 4 exactly
            // when in_ch is. Every real conv layer except layer 0 (in_ch=3,
            // and layer 0 runs on conv0_engine now) complies, but the config
            // sweep deliberately includes odd channel counts, so the
            // unaligned case gets its own byte-at-a-time path below rather
            // than an assert.
            const bool w_aligned = (in_ch % PACK4_LANES == 0);
            const unsigned ic_words = (ic_count + PACK4_LANES - 1) / PACK4_LANES;
            for (unsigned ky = 0; ky < k; ky++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_K
                for (unsigned kx = 0; kx < k; kx++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_K
                    // Element (not word) index of this (oc, ky, kx) row's
                    // run of ic values. Guard each half independently -
                    // reading weights[] for an out-of-range oc_hi/oc_lo would
                    // be an OOB DDR access, same reasoning as the original
                    // per-pe "if (oc < out_ch)" guard.
                    const unsigned base_lo = ((oc_lo * k + ky) * k + kx) * in_ch + ic_lo;
                    const unsigned base_hi = ((oc_hi * k + ky) * k + kx) * in_ch + ic_lo;
                    if (w_aligned) {
                    LOAD_W_IC4:
                        for (unsigned ic4 = 0; ic4 < ic_words; ic4++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IN_CH/PACK4_LANES
                            // Read via weights_hi, NOT weights - both alias
                            // the same DRAM data, but this is a second,
                            // independent AXI4 master port (see
                            // conv_engine.h's note on this param). Reading
                            // both halves from ONE port serialized to II=2
                            // (confirmed by csynth); splitting the port is
                            // what gets this loop to II=1.
                            pack4_t w_lo = 0, w_hi = 0;
                            if (lo_valid) w_lo = weights[base_lo / PACK4_LANES + ic4];
                            if (hi_valid) w_hi = weights_hi[base_hi / PACK4_LANES + ic4];
                        LOAD_W_LANE:
                            for (unsigned L = 0; L < PACK4_LANES; L++) {
#pragma HLS UNROLL
                                unsigned ic = ic4 * PACK4_LANES + L;
                                if (ic < ic_count) {
                                    // The 4 lanes are 4 DIFFERENT ic, so
                                    // wtile_packed's existing
                                    // "cyclic factor=TR dim=3" partitioning
                                    // puts them in 4 different banks - this
                                    // unroll needs no new partition pragma.
                                    weight_t y_lo = pack4_get(w_lo, L);
                                    weight_t y_hi = pack4_get(w_hi, L);
                                    // Offset-binary bias via sign-bit flip
                                    // (exact for y_lo in [-128,127]: -128->0,
                                    // -1->127, 0->128, 127->255) - a pure bit
                                    // op, no add/width-growth risk.
                                    ap_uint<8> y_lo_biased = (ap_uint<8>)y_lo ^ (ap_uint<8>)0x80;
                                    wtile_packed[j][ic][ky][kx] =
                                        (((ap_int<25>)y_hi) << 16) + (ap_int<25>)y_lo_biased;
                                }
                            }
                        }
                    } else {
                        // Unaligned fallback: one ic per iteration, reading
                        // the containing word and selecting the lane. Same
                        // 1-element-per-cycle rate the whole loop had before
                        // WPACK, so this path is no slower than the original
                        // - it just does not get the 4x.
                    LOAD_W_IC_SLOW:
                        for (unsigned ic = 0; ic < ic_count; ic++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IN_CH
                            weight_t y_lo = 0, y_hi = 0;
                            if (lo_valid) {
                                unsigned e = base_lo + ic;
                                y_lo = pack4_get(weights[e / PACK4_LANES], e % PACK4_LANES);
                            }
                            if (hi_valid) {
                                unsigned e = base_hi + ic;
                                y_hi = pack4_get(weights_hi[e / PACK4_LANES], e % PACK4_LANES);
                            }
                            ap_uint<8> y_lo_biased = (ap_uint<8>)y_lo ^ (ap_uint<8>)0x80;
                            wtile_packed[j][ic][ky][kx] =
                                (((ap_int<25>)y_hi) << 16) + (ap_int<25>)y_lo_biased;
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// On-chip accum scratch (2026-08-08, migrated out of DDR)
//
// Why file-scope static: scan_and_compute() is called afresh for every
// (oc_group, ic_tile), but the partial sums must stay alive **across** those
// calls. A local would be reinitialised on every call and break the carry-over.
// This is exactly the role the DDR version played.
//
// Why cyclic factor=PE_OC: the ACCUM_RD/ACCUM_WR index is
// `pixel_idx*(G*PE_OC) + u*PE_OC + lane`, and the first two terms are multiples
// of PE_OC, so `index % PE_OC == lane`. Every lane therefore gets its own bank.
//
// WARNING: **this partition alone makes neither loop faster** (measured by
// csynth on 2026-08-08). ACCUM_RD still reports trip 1-32 / II=1 / latency 2-33
// cycles - **the same numbers as the DDR version** - because the loop is
// PIPELINE, not UNROLL, so even with 32 banks only one lane leaves per cycle.
// The win this change targets is not loop latency but **removing the AXI round
// trip itself**, which never shows up in a csynth report; only cosim measures it.
// Actually exploiting the banking requires UNROLLing both loops, and that is a
// separate lever with its own measurement, deliberately not mixed in here.
//
// Why URAM: 576 x 64 x 4B = 144 KB. BRAM is already at 92/144 (63.9%) so it
// does not fit, while URAM sits completely idle at 0/64. WARNING: on 2026-08-05
// line_buf was pushed to 100% URAM and had to be reverted over a -0.207 ns clock
// skew - always confirm timing with impl (the current WNS is +0.029 ns, almost
// no headroom).
// Both pragmas live inside scan_and_compute(), not here at file scope: HLS
// rejects them with `'#pragma HLS' is only allowed in function scope` (a real
// csynth error hit on 2026-08-08). An array pragma on a global belongs in the
// body of a function where that array is visible.
static accum_t accum_onchip[ACCUM_POSITIONS * OC_GROUP_TILES * PE_OC];

static void scan_and_compute(
    // PACK4: ifmap is int8x4-packed (pack4.h). accum/ofmap are unchanged -
    // accum_t is already 32 bits (full beat), and ofmap's FINISH_WR is a
    // smaller candidate left for a separate change so this one can be
    // measured on its own.
    const pack4_t *ifmap, act_t *ofmap,
    unsigned img_h, unsigned img_w, unsigned ic_lo, unsigned ic_count, unsigned in_ch, unsigned out_ch,
    unsigned k, unsigned pad, unsigned oc_group, unsigned tiles_in_group,
    unsigned ic_tile, unsigned num_ic_tiles,
    // OC-hoist: both arrays gain a leading group dimension. Element [u] is
    // the tile for group member u, i.e. oc_tile = oc_group*OC_GROUP_TILES+u.
    const packed_weight_t wtile_packed[OC_GROUP_TILES][PE_PAIRS][MAX_IN_CH][MAX_K][MAX_K],
    const bias_t btile[OC_GROUP_TILES][PE_OC],
    bool leaky_relu_enable, int32_t requant_multiplier, unsigned requant_shift
) {
    // Forced INLINE - same reasoning as load_weight_tile() above.
#pragma HLS INLINE
    // Array pragmas for the on-chip accum scratch. The array itself is declared
    // as a file-scope static above this function (partial sums must survive
    // across calls); only the pragmas live here in function scope - see the
    // comment above that declaration.
#pragma HLS ARRAY_PARTITION variable=accum_onchip cyclic factor=PE_OC dim=1
// ZYBO PORT (2026-08-14): impl=URAM -> impl=BRAM. Not a tuning choice - the
// XC7Z020 is 7-series and has NO UltraRAM at all, so impl=URAM cannot be
// honoured here. The comment block above describes why URAM was picked on
// KV260; that reasoning is inapplicable, not wrong.
//
// The move is affordable on this part because PE_OC also halved:
//   accum_onchip = ACCUM_POSITIONS(576) x OC_GROUP_TILES(2) x PE_OC x 4 B
//   KV260 PE_OC=32 -> 144 KB      this fork PE_OC=16 -> 72 KB
// and the cyclic(PE_OC) partition splits that into 16 banks of 576*2 = 1,152
// x 32 bit = 36,864 bits, i.e. EXACTLY one BRAM36 per bank -> 16 BRAM36 of
// the part's 140. That exact fit is luck, not design: raising PE_OC back to
// 32 would make each bank 36,864 bits again (banks stay the same size, there
// are just twice as many) -> 32 BRAM36, still fine; but changing
// ACCUM_POSITIONS or OC_GROUP_TILES pushes a bank past 36 Kb and each one
// then costs 2 BRAM36, doubling this line item. Re-check the arithmetic
// above if either constant moves.
#pragma HLS BIND_STORAGE variable=accum_onchip type=RAM_2P impl=BRAM
    const unsigned out_w = img_w + 2 * pad - k + 1; // stride=1
    const unsigned pad_h = img_h + 2u * pad;
    const unsigned pad_w = img_w + 2u * pad;

    // Line buffers / sliding window - same raster-scan structure as
    // conv_layer1.cpp, generalized to runtime in_ch/k bounded by MAX_*.
    // Both partitioned cyclic(TR) on the in_ch dimension only - see the FIX
    // comment on the partition pragmas below for why that's the dimension
    // that actually needs it. `line_buf`'s column dimension is MAX_IMG_W
    // (416) wide and indexed by COL_LOOP's `c`, a plain sequential loop
    // variable, so it must stay a real (unpartitioned) memory; see
    // RESOURCE_BUDGET.md §4, which sizes line_buf as "32 BRAMs, 416 elements
    // each" - `complete`-partitioning that dimension too (as an earlier
    // version of this file did) silently turned each of those 32 BRAMs into
    // ~416 individual muxed registers, which is what made C synthesis hang
    // in scheduling/binding instead of just costing extra area.
    //
    // TRIED AND REJECTED (real csynth run, both configs measured - see
    // RESOURCE_BUDGET.md §4): `line_buf`'s dim=2 (its MAX_K-1=2 row
    // dimension) is `complete`-partitioned even though SHIFT_WINDOW's row
    // shift (`for (ky=0; ky<k-2; ky++) line_buf[ic][ky][c] = ...`) never
    // unrolls it - on paper the same never-unrolled-dimension anti-pattern
    // documented in FIX 2 below for `window`/`wtile`. Dropping it to
    // cyclic(TR) dim=1 only (matching `window`) does shrink line_buf's own
    // Multiplexer-table entry (1,843 LUT / 32 instances -> 432 LUT / 16
    // instances), but Vitis re-implements the row addressing it frees up
    // using DSP-based address arithmetic (32 new `ama_addmuladd` instances)
    // elsewhere instead of LUT muxes - net design LUT barely moved
    // (108,294 -> 108,100, -0.18%) while DSP went 267 -> 315 (+48, +18%),
    // an unrequested DSP-budget cost for no real LUT win. Left `complete`
    // here deliberately; unlike `window`/`wtile`'s K,K fix (FIX 2), this
    // one is a measured net loss, not a free win - don't redo it without
    // re-checking the DSP column, not just the LUT column.
    // FIX (found via a real conv_engine_tb.cpp C-sim run against the real
    // network's actual layer-0 width, not by inspection): the column
    // dimension must hold `pad_w` (`img_w + 2*pad`) entries, not just
    // `img_w` - `COL_LOOP` below already documented this via its own
    // `LOOP_TRIPCOUNT max=MAX_IMG_W+2`, but the array itself was still
    // declared `[MAX_IMG_W]`, 2 short of what COL_LOOP's own stated trip
    // count implies. Never caught before because no test prior to
    // REAL_WEIGHTS_PLAN.md's real-layer suite ever actually ran img_w at
    // the true MAX_IMG_W boundary with pad=1 at the same time - the real
    // network's layer 0 (in_ch=3, img_w=512, pad=1) is the first case that
    // does (TROUBLESHOOTING.md #19's `img_w<=MAX_IMG_W` assertion failure
    // was a *different*, later symptom of this same underlying gap, hit
    // once layer 0's pre-padding fix pushed `img_w` itself past 512).
    // `+2`, not `+2*MAX_K`: matches COL_LOOP's own existing annotation and
    // every real layer's actual pad (0 or 1) - `pad` up to `MAX_K` (3) is
    // permitted by the separate `assert(pad <= MAX_K ...)` sanity check
    // below but is not real-network usage; revisit this `+2` together with
    // that assert if a real layer ever legitimately needs pad>1.
    static act_t line_buf[MAX_IN_CH][MAX_K - 1][MAX_IMG_W + 2];
    static act_t window[MAX_IN_CH][MAX_K][MAX_K];
    // FIX (found from a real Vitis HLS 2021.1 run, not by inspection):
    // `complete dim=0` was originally written assuming SHIFT_WINDOW's `ic`
    // loop would be fully unrolled (giving compile-time-constant per-lane
    // indices into these arrays). It isn't - `in_ch` is a runtime
    // parameter, and Vitis HLS cannot fully unroll a variable-trip-count
    // loop (confirmed by an actual synthesis run: "WARNING: [HLS 200-936]
    // Cannot unroll loop 'SHIFT_WINDOW' ... cannot completely unroll a loop
    // with a variable trip count"). The unroll pragma on SHIFT_WINDOW below
    // is now `factor=TR` (partial, confirmed working - see MAC_IC further
    // down, which already used this correctly), so the matching partition
    // here is cyclic(TR) on the in_ch dimension, not complete - a complete
    // partition accessed through the resulting non-constant index is
    // exactly what the same run flagged as "may result in long runtime and
    // suboptimal QoR due to large multiplexers" for this array.
    //
    // FIX 2 (found from the csynth report after C synthesis actually
    // completed, not from a tool warning): a previous version of this file
    // also had `complete dim=2`/`dim=3` here (window's two K,K dims), on the
    // assumption that MAC_KY/MAC_KX's ky/kx loops would be unrolled and need
    // the parallel access. They aren't - MAC_KY/MAC_KX (see the compute loop
    // below) are `#pragma HLS PIPELINE II=1`, not UNROLL, same as
    // SHIFT_WINDOW's and load_weight_tile's ky/kx loops - so nothing ever
    // reads or writes window (or wtile, see its declaration in conv_engine()
    // below) through a compile-time-constant ky/kx index. That made the
    // complete partitioning pure overhead: the csynth report's Multiplexer
    // detail table showed window split into 144 separate memory instances
    // (16 cyclic in_ch banks x 9 K,K positions), each paying a fixed
    // ~3 address0/ce0/we0 arbitration signals x ~9 LUT for sharing that
    // instance's port between SHIFT_WINDOW's writer and MAC_KY/MAC_KX's
    // reader - 17,713 LUT total for window alone, on top of 60,264 LUT for
    // the analogous problem on wtile (2,304 instances there, since it also
    // has a `complete`-partitioned pe dimension) - together over 40% of the
    // whole design's LUT count and the reason total LUT utilization was 155%
    // of the xck26's budget. Dropping the K,K partitioning cuts window to 16
    // instances and wtile to 256, each now absorbing the ky/kx addressing
    // internally instead of paying per-position port-arbitration overhead.
#pragma HLS ARRAY_PARTITION variable=line_buf cyclic factor=TR dim=1
#pragma HLS ARRAY_PARTITION variable=line_buf complete dim=2
// BRAM->UltraRAM pivot (2026-08-04, merged from teammate's PL branch):
// line_buf is the dominant BRAM_18K consumer (128 of ~136 total blocks in
// the csynth report before this change) and UltraRAM sat at 0% used on this
// device - moving this specific array there relieves BRAM without touching
// any MAC/tree logic (unlike the abandoned TR=32 attempt, see conv_engine.h),
// so functional correctness is unaffected (BIND_STORAGE only changes which
// physical memory primitive implements the array, never its C-level
// behavior - csim results must be identical to before this pragma).
// Teammate's real measurements with this binding (on the pre-restructure
// code, PE_OC=24): LUT 65%, BRAM 53%->2%, URAM 100%, DSP 219, real Vivado
// WNS=+1.85ns@100MHz. RAM_1P is a starting point, NOT confirmed against
// this array's access pattern under the CURRENT loop structure - if csynth
// reports a port conflict or a new II violation on the loops that access
// line_buf (WINDOW_TAIL_FILL reads it; LINE_BUF_ROW_SHIFT reads AND writes
// it in the same II=1 iteration; LINE_BUF_TAIL_FILL writes it), that means
// the array genuinely needs simultaneous independent read+write and RAM_1P
// is the wrong choice - switch to RAM_2P (or RAM_T2P) and re-check, rather
// than assuming RAM_1P is correct just because it compiles.
// TIER C1 REVERSAL (2026-08-05, TIMING_CLOSURE_PLAN.md §4 Tier C / §3 cause C).
// impl=URAM -> impl=BRAM. The comment above describes why URAM was chosen,
// and that reasoning was correct WHEN IT WAS MADE - BRAM was the binding
// constraint at 53% and URAM sat unused at 0%. That balance has since
// inverted completely:
//
//     URAM  64 / 64  = 100.00%   <- saturated
//     BRAM   5 / 144 =   3.47%   <- nearly empty
//
// and nothing else in the design now wants BRAM. Meanwhile the 200 MHz
// routed run (WNS -0.021) shows CLOCK SKEW alone at -0.207 ns - ten times
// the shortfall - because using every URAM column forces conv_engine_0 to
// span clock regions (SLICE_X27~X53, Y108~Y168 in the placed design). URAM
// columns are few and fixed in position; BRAM tiles are 144 and far more
// evenly distributed, so this should let the placer keep the engine local.
//
// This targets a different term than B1/B2 do: they attack the 4.728 ns data
// path, this attacks the -0.207 ns skew and the 62.6% route share.
//
// What this costs: BRAM returns to ~53%. That only matters if something
// later needs BRAM - the obvious candidate, TR=32, is already blocked twice
// over on LUT (see conv_engine.h), so the loss is theoretical today.
//
// WHAT MUST BE RE-CHECKED, NOT ASSUMED (the note above already warns about
// exactly this for RAM_1P): BRAM and URAM differ in read latency, and this
// array is read AND written inside II=1 loops (WINDOW_TAIL_FILL reads it;
// the fused FUSED_SHIFT_STEP loop both reads and writes it in one
// iteration). If csynth reports an II violation on those loops, or cosim
// cycles jump by more than the ~3% B1 cost, this change is wrong and must be
// reverted rather than worked around. Confirm II=1 in the csynth loop table
// and compare layer-3 cosim against the 1,999,021-cycle post-B1/B2 baseline.
#pragma HLS BIND_STORAGE variable=line_buf type=RAM_1P impl=BRAM
#pragma HLS ARRAY_PARTITION variable=window cyclic factor=TR dim=1

ROW_LOOP:
    for (unsigned r = 0; r < pad_h; r++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IMG_W+2
    COL_LOOP:
        for (unsigned c = 0; c < pad_w; c++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IMG_W+2
            // Deliberately NOT pipelined at this level - see
            // RESOURCE_BUDGET.md and the MAC_IC loop below for why: the
            // parallelism knob lives on the reduction loop (UNROLL factor=TR
            // there), not here. Pipelining COL_LOOP would force the tool to
            // fully unroll everything nested inside it, defeating the
            // bounded-DSP design this engine exists to provide.

            bool in_bounds = (r >= pad) && (r < pad + img_h) &&
                              (c >= pad) && (c < pad + img_w);
            unsigned in_r = r - pad;
            unsigned in_c = c - pad;

            act_t px[MAX_IN_CH];
#pragma HLS ARRAY_PARTITION variable=px cyclic factor=TR dim=1

        READ_CH:
            // Regression fix (found in self-review): conv_layer1.cpp's
            // original code uses if/else here, not a ternary, and that
            // matters in synthesized hardware even though it's equivalent
            // in C-simulation. A ternary typically synthesizes as a mux
            // applied AFTER both sides are computed - which for this read
            // means the m_axi address expression `in_r*img_w+in_c` (built
            // from in_r/in_c that underflow to huge unsigned values when
            // r/c < pad) could still be issued as an actual AXI read every
            // cycle, just with its result discarded, instead of the read
            // being skipped entirely. An if/else makes the read
            // conditionally EXECUTED, not just conditionally SELECTED -
            // required so padding positions never issue a garbage-address
            // bus transaction.
            //
            // FIX (found from a real Vitis HLS 2021.1 run): this loop was
            // `#pragma HLS UNROLL` (full), which both (a) failed outright -
            // in_ch is a runtime parameter, and Vitis HLS cannot fully
            // unroll a variable-trip-count loop - and (b) even if it had
            // worked, would have been the wrong choice for a loop reading
            // from an m_axi port: LOAD_WTILE's read of weights[] (which
            // DOES burst-coalesce, confirmed by the same run: "Multiple
            // burst reads ... has been inferred on port 'RD_BUS'") is
            // structured as a plain sequential PIPELINE, not an unroll -
            // that is the pattern the tool's burst inference is built to
            // recognize. Matching that pattern here fixes both problems
            // with one change.
            //
            // TRIED AND REJECTED (2026-08-03, real csynth + layer-9 cosim,
            // both measured): the `if (in_bounds)` test below is what stops
            // Vitis inferring a burst on this loop - `burst.xml` reports
            // ifmap/READ_CH under `AccessInCondBranchMissed` ("Access load
            // is in the conditional branch"), the same reason `bias` misses
            // in LOAD_WTILE. `in_bounds` depends only on r/c/pad/img_h/img_w
            // and never on `ic`, so hoisting it out of this loop (splitting
            // into an unconditional READ_CH plus a ZERO_CH zero-fill loop)
            // is bit-exact - verified against all 12 configs and all 13 real
            // layers - and it DOES make the burst pass
            // (`BurstInferredPassed`, bundle RD_BUS, loop READ_CH).
            //
            // It is still not worth doing. Real layer-9 cosim went
            // 1,291,969 -> 1,293,121 cycles, i.e. +1,152 - EXACTLY the
            // number of COL_LOOP scan positions in that run
            // (2 oc_tiles x 4 ic_tiles x 9 x 16), one extra cycle per
            // position for the hoisted branch, for zero measured gain.
            // The reason the burst buys nothing here: this loop already
            // achieves II=1, and the m_axi ports are 32-bit while `act_t`
            // is 8-bit, so `burst.xml` also reports
            // `GreaterOrEqualThresholdMissed` - the burst cannot be WIDENED
            // (`m_axi_max_widen_bitwidth` defaults to 0, and run_hls.tcl
            // sets no `config_interface` at all). An un-widened burst still
            // moves one int8 per beat, which is what II=1 was already doing;
            // bursting only saves AR-handshake overhead, not data beats.
            //
            // Do not redo this on the burst-inference argument alone. It
            // only becomes interesting TOGETHER with
            // `config_interface -m_axi_max_widen_bitwidth 32` (32, not more:
            // vivado/create_system_bd.tcl §3 deliberately narrows HP0/HP1 to
            // 32-bit to match every engine's m_axi ports, so a wider HLS
            // port would be a real system-level change, not a pragma). Even
            // then the ceiling is small: the csynth loop table puts READ_CH
            // at 12~139 cycles against SHIFT_WINDOW's 49~3554 in the same
            // COL_LOOP body, so the whole ifmap-read path is a minority of
            // real runtime.
            //
            // Bound by `ic_count` (this ic_tile's local channel count), not
            // `in_ch` - but the m_axi address expression still uses `in_ch`
            // (the real total) as the NHWC channel stride and `ic_lo + ic`
            // as the channel offset, since ifmap's actual DRAM layout is
            // unaffected by tiling - only which channel slice a given
            // ic_tile pass reads changes.
            //
            // PACK4 (this variant): `ifmap` is now pack4_t*, carrying 4
            // consecutive NHWC channels per AXI beat. This loop is the
            // reason the whole variant exists - see pack4.h for the csynth
            // table showing READ_CH is now the LARGEST term in COL_LOOP
            // (139 of 411 cycles), not the minority the stale comment above
            // still claims.
            //
            // Two paths, because layer 0 is a genuine exception:
            //   FAST  in_ch % 4 == 0 - one beat per 4 channels. This is
            //         every real layer except layer 0 (in_ch is
            //         16/32/64/128/256/384/512/1024).
            //   SLOW  in_ch % 4 != 0 - one channel per beat, extracted from
            //         its containing word. Only layer 0 (in_ch=3) takes
            //         this, and its READ_CH trip count is 3, so the lost
            //         speedup there is worth nothing anyway.
            //
            // Keeping the slow path is what preserves the "no PS-side data
            // change" property that makes this technique cheap: layer 0's
            // RGB input does NOT have to be re-laid-out as RGBX in DDR, and
            // its weights do not need a zero 4th input channel.
            // W6 (2026-08-18): READ_CH_WORDS used to live here as its own
            // pipelined loop, filling px[] for FUSED_SHIFT_STEP below to read.
            // That was one loop REGION per position, and a loop region costs a
            // fixed ~12.9 cycles on this engine no matter how little it does
            // (measured 2026-08-18 by W1's failure: the ROW_ACC_WR it added
            // cost 59,369 cycles over 4,608 calls). The aligned path now folds
            // the ifmap word fetch into FUSED_SHIFT_STEP itself, which already
            // iterates over exactly the same ic_step space.
            //
            // Exact only because TR == PACK4_LANES: FUSED_SHIFT_STEP's
            // `ic_step` IS the word index, and its lane `t` IS the pack lane.
            // static_assert below enforces that - if TR moves off 4, un-fold
            // this rather than "fixing" the assert.
            const bool     ch_aligned = ((in_ch % PACK4_LANES) == 0u);
            const unsigned base_word  =
                (((unsigned)in_r * img_w + in_c) * in_ch + ic_lo) / PACK4_LANES;
            if (!ch_aligned) {
            READ_CH_ELEMS:
                for (unsigned ic = 0; ic < ic_count; ic++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IN_CH
                    act_t val = 0;
                    if (in_bounds) {
                        unsigned flat =
                            ((unsigned)in_r * img_w + in_c) * in_ch + (ic_lo + ic);
                        pack4_t v = ifmap[flat / PACK4_LANES];
                        val = pack4_get(v, flat % PACK4_LANES);
                    }
                    px[ic] = val;
                }
            }

        SHIFT_WINDOW:
            // FIX (found from a real Vitis HLS 2021.1 run): `#pragma HLS
            // UNROLL` (full) cannot unroll a loop with a runtime trip count
            // (in_ch), confirmed by that synthesis run's "WARNING: [HLS
            // 200-936] Cannot unroll loop 'SHIFT_WINDOW' ... variable trip
            // count" - a partial unroll (factor=TR) on the ic dimension is
            // required for the same reason it is on MAC_IC/MAC_TR below.
            // window/line_buf are cyclic(TR)-partitioned on the ic dimension
            // (not complete) to match, unchanged by the 2026-08-03
            // restructure below - only WHICH loop level carries the
            // `UNROLL factor=TR` changed (now the innermost lane loop, not
            // the outermost one), not the partitioning itself.
            //
            // Each nested loop below gets its OWN LOOP_TRIPCOUNT (same
            // per-level rule as LOAD_W_IC above, TROUBLESHOOTING.md #4):
            // without one, `hls_compile.log` showed the tool inferring its
            // own max=2 bound for the ky/kx loops (from static range
            // analysis of k-1 <= MAX_K-1) and then rejecting it against an
            // internally-computed average of 127 for that loop level -
            // "Ignored invalid trip count directive (MAX (= 2) < AVE (=
            // 127))" - falling back to an unbounded default and inflating
            // OC_TILE/ROW_LOOP/COL_LOOP's reported worst-case latency into
            // the quadrillions of cycles.
            // Bound by `ic_count`, not `in_ch` - see READ_CH above for why.
            //
            // RESTRUCTURED (2026-08-03, TROUBLESHOOTING.md §23-e, real
            // csynth loop table + ablation, not just inspection): the
            // original version of this loop had `ic` as the OUTERMOST
            // dimension (`UNROLL factor=TR`) wrapping these sequential ky/kx
            // loops - which replicates the ky/kx loop control TR-way, one
            // full copy per unrolled lane. That's the exact anti-pattern
            // §21/LUT_REDUCTION_PLAN.md Lever 3 already diagnosed and fixed
            // for MAC_KY/MAC_KX below, just never applied here: a real
            // csynth run measured this block (SHIFT_WINDOW) at 3,464 of
            // COL_LOOP's 4,328 worst-case cycles (80%), and an ablation
            // (deleting these loops, re-synthesizing) confirmed the nested
            // ky/kx structure itself - not the memory accesses - as the
            // cost: SHIFT_WINDOW's iteration latency dropped 433 -> 97 with
            // the loops gone.
            //
            // Fixed the same way Lever 3 fixed MAC_KY/MAC_KX: flip the nest
            // so the sequential dimension (ky, kx) is OUTERMOST and
            // PIPELINE'd, with the TR-wide lane dimension (`ic_step`/`t`,
            // same split MAC_REDUCE/MAC_TR below already use) UNROLLed
            // INSIDE the pipelined body. Unlike MAC_REDUCE's accumulator
            // (RESOURCE_BUDGET.md §10 - TR lanes all writing into the SAME
            // loop-carried acc_lo/acc_hi forced a real 16-deep sequential
            // chain and blew the timing budget 4x), every lane here writes
            // an independent `window[ic]`/`line_buf[ic]` slot - cyclic(TR)-
            // partitioned, `ic = ic_step*TR+t` lands each lane `t` on a
            // distinct physical partition, identical indexing to MAC_TR's
            // own `ic = ic_step*TR+t` - so there is no shared write target
            // and no reduction, hence no reason to expect that specific
            // failure mode here. Still re-check csynth's estimated clock
            // regardless (§21's own warning: a cycle/LUT win can hide a
            // timing loss) rather than assuming this reasoning is enough.
            //
            // SHIFT-LOOP FUSION (2026-08-05, this fork - see
            // doc/02_plans/datapath-efficiency.md §1). This block previously WAS
            // 4 separate pipelined loop nests (row-shift, tail-fill,
            // line_buf-shift, line_buf-tail). That split was deliberate and
            // its reasoning is preserved below, because it is what makes the
            // merge provably safe rather than merely plausible.
            //
            // WHY THE SPLIT COST SO MUCH. All four nests walk the SAME
            // `ic_step` axis (TR channels per cycle, because `window`/
            // `line_buf` are cyclic(TR)-partitioned on the channel dim), and
            // each paid its own pipeline entry latency. §29's fitted
            // formulas, which reproduce the csynth min~max exactly:
            //
            //   WINDOW_ROW/COL_SHIFT  3 + k(k-1)*ceil(ic/TR) = 51  (48 iters)
            //   WINDOW_TAIL_FILL      3 + k*ceil(ic/TR)      = 27  (24 iters)
            //   LINE_BUF_ROW_SHIFT    2 + (k-1)*ceil(ic/TR)  = 18  (16 iters)
            //   LINE_BUF_TAIL_FILL    2 + ceil(ic/TR)        = 10  ( 8 iters)
            //                                          total = 106 cycles
            //
            // 106 of COL_LOOP's 413-cycle worst case - 25.7%, more than
            // MAC_REDUCE's own 78. But the per-channel work is only ~12
            // register moves; `k` is at most MAX_K=3, so ky/kx can simply be
            // UNROLL'd into ONE ic_step loop: ~8 iterations + one entry
            // instead of 96 + four.
            //
            // WHY MERGING IS SAFE. The old comment justified the split by
            // noting that ALL row-shift writes must precede ANY tail-fill
            // write "across every `ic`, not just within one". Re-reading the
            // accesses, that cross-`ic` requirement does not actually exist:
            // every access here - window[ic][..], line_buf[ic][..], px[ic] -
            // is indexed by the SAME `ic` as the lane performing it. There is
            // no cross-channel read/write pair anywhere in these four pieces,
            // so per-lane ordering is sufficient, and this loop preserves it:
            //
            //   (a) snapshot line_buf's OLD column-c values into lb_old
            //   (b) window column shift, ASCENDING kx (so window[..][kx+1] is
            //       still the original value when read - same as the old
            //       nest's ascending kx loop)
            //   (c) window tail fill, reading lb_old (not line_buf), so it
            //       cannot see (d)'s writes
            //   (d) line_buf row shift + tail fill, last
            //
            // (a) exists specifically so (c)'s "reads line_buf's OLD rows"
            // guarantee survives even if Vitis schedules (d) early: the value
            // was already captured. That is strictly stronger than relying on
            // C-sim's top-to-bottom order, which is what the split version
            // depended on.
            //
            // NOT SYNTHESIS-CONFIRMED AT THE TIME THIS WAS WRITTEN - the same
            // caveat the split version carried, and for the same reason. What
            // IS confirmed: g++-against-standalone-ap_int.h C-sim, all 12
            // configs + all 13 real layers, bit-exact against the split
            // version's own golden results. Before trusting this delivered a
            // real win: re-run csynth, confirm COL_LOOP's max drops from 413
            // toward ~320 AND that the estimated clock is still under the
            // 5.00 ns target (§21's warning: a cycle/LUT win can hide a
            // timing loss - and this loop now does ~192 register moves per
            // cycle instead of ~16, which is exactly the kind of change that
            // buys cycles with routing pressure). Then re-run a real-layer
            // cosim and compare against the pre-fusion baseline.
            const unsigned ic_steps_sw = (ic_count + TR - 1) / TR;

            // The four former nests, fused. `ky`/`kx` are bounded by MAX_K=3
            // so they UNROLL into the body; only `ic_step` remains as a real
            // loop, which is the axis the arrays are partitioned on.
            //
            // Every runtime-`k` guard below is written to avoid UNSIGNED
            // UNDERFLOW rather than relying on a signed comparison - the
            // exact class of bug the old LINE_BUF_ROW_SHIFT comment records
            // (`k - 2` on unsigned k=1 wrapping to a huge value, which config
            // C in conv_engine_tb.cpp exercises for real). Hence `kx + 1 < k`
            // instead of `kx < k - 1`, and `ky + 2 < k` instead of
            // `ky < k - 2`. The `k >= 2` guard on the line_buf half is kept
            // for the same reason it was added: a k=1 layer needs zero
            // line-buffer rows, so the whole block must be skipped, not just
            // its final write.
        FUSED_SHIFT_STEP:
            for (unsigned ic_step = 0; ic_step < ic_steps_sw; ic_step++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IN_CH/TR
                // W6: the aligned path's ifmap read moved in here. Same
                // "conditionally EXECUTED, not conditionally selected"
                // property READ_CH_WORDS had - an out-of-bounds position
                // issues no AXI transaction and v stays 0, so every lane
                // unpacks to 0, which is the `px[ic] = 0` the else-branch
                // used to write.
                static_assert(TR == PACK4_LANES, "W6 folds the pack4 word "
                              "fetch into this loop by making ic_step the "
                              "word index; that identity needs TR == "
                              "PACK4_LANES. Un-fold it before changing TR.");
                pack4_t vw = 0;
                if (ch_aligned && in_bounds) {
                    vw = ifmap[base_word + ic_step];
                }
            FUSED_SHIFT_LANE:
                for (unsigned t = 0; t < TR; t++) {
#pragma HLS UNROLL
                    unsigned ic = ic_step * TR + t;
                    if (ic < ic_count) {
                        // aligned: straight from the word just fetched.
                        // unaligned: from px[], which READ_CH_ELEMS filled.
                        const act_t pv =
                            ch_aligned ? pack4_get(vw, t) : px[ic];

                        // (a) Snapshot line_buf's OLD column-c values BEFORE
                        // anything writes line_buf. MAX_K-1 = 2 elements of
                        // act_t; small enough that no partition pragma is
                        // needed (constant indices under UNROLL scalarize it).
                        act_t lb_old[MAX_K - 1];
                    FUSED_LB_SNAPSHOT:
                        for (unsigned ky = 0; ky + 1 < MAX_K; ky++) {
#pragma HLS UNROLL
                            lb_old[ky] = (ky + 1 < k) ? line_buf[ic][ky][c] : (act_t)0;
                        }

                        // (b) Window column shift, ASCENDING kx so each read
                        // of [kx+1] still sees the original value.
                    FUSED_WIN_SHIFT:
                        for (unsigned ky = 0; ky < MAX_K; ky++) {
#pragma HLS UNROLL
                            for (unsigned kx = 0; kx + 1 < MAX_K; kx++) {
#pragma HLS UNROLL
                                if (ky < k && kx + 1 < k) {
                                    window[ic][ky][kx] = window[ic][ky][kx + 1];
                                }
                            }
                        }

                        // (c) Window tail fill. Reads lb_old, NOT line_buf, so
                        // it cannot observe (d)'s writes no matter how Vitis
                        // schedules them.
                    FUSED_WIN_TAIL:
                        for (unsigned ky = 0; ky < MAX_K; ky++) {
#pragma HLS UNROLL
                            if (ky < k) {
                                window[ic][ky][k - 1] =
                                    (ky + 1 < k) ? lb_old[ky] : pv;
                            }
                        }

                        // (d) line_buf row shift + tail fill, last.
                        if (k >= 2) {
                        FUSED_LB_SHIFT:
                            for (unsigned ky = 0; ky + 2 < MAX_K; ky++) {
#pragma HLS UNROLL
                                if (ky + 2 < k) {
                                    line_buf[ic][ky][c] = line_buf[ic][ky + 1][c];
                                }
                            }
                            line_buf[ic][k - 2][c] = pv;
                        }
                    }
                }
            }

            if (r >= k - 1 && c >= k - 1) {
                unsigned out_r = r - (k - 1);
                unsigned out_c = c - (k - 1);

            // OC-hoist: everything ABOVE this point in COL_LOOP (READ_CH,
            // SHIFT_WINDOW, FUSED_SHIFT_*) is oc_tile-independent and is now
            // shared by all `tiles_in_group` members. Everything BELOW is
            // per-oc_tile and repeats here instead of re-running the whole
            // spatial scan.
            //
            // UNROLL off is LOAD-BEARING, not a style choice. OC_GROUP_TILES
            // is a small compile-time constant, exactly the shape Vitis
            // auto-unrolls; unrolling here would replicate the MAC datapath
            // G times (DSP 283 -> 283*G) and defeat the entire point, which
            // is to REUSE one MAC array across more oc_tiles, not to build
            // more MAC arrays. `tiles_in_group` is deliberately a runtime
            // value rather than the constant OC_GROUP_TILES for the same
            // reason - a constant trip count invites the same auto-unroll.
            // conv_engine_csynth.rpt's DSP count is the check: it must not
            // move at all. If it does, fix this before reading any other
            // number.
            OC_IN_GROUP:
                for (unsigned u = 0; u < tiles_in_group; u++) {
#pragma HLS UNROLL off
#pragma HLS LOOP_TRIPCOUNT min=1 max=OC_GROUP_TILES
                unsigned oc_tile = oc_group * OC_GROUP_TILES + u;

            // INT8 DSP-packing experiment (conv_engine.h): PE_LOOP over
            // individual pe replaced with PE_PAIR_LOOP over PE_PAIRS - each
            // pair (2j, 2j+1) shares ONE multiply per (ic,ky,kx) step against
            // the common activation window[ic][ky][kx], since that value
            // never depended on pe in the first place.
            //
            // LUT_REDUCTION_PLAN.md Lever 3 (2026-07-24): this whole block
            // was rewritten from "PE_PAIR_LOOP(UNROLL) -> MAC_IC(UNROLL
            // factor=TR) -> MAC_KY(seq) -> MAC_KX(seq,PIPELINE II=1)" to
            // "MAC_REDUCE(seq,PIPELINE II=1, over ic_step*ky*kx) ->
            // PE_PAIR_LOOP(UNROLL) -> MAC_TR(UNROLL)". Reasoning: nesting
            // ky/kx (a genuinely sequential, pipelined dimension - MAC_KY/
            // MAC_KX were always PIPELINE, never UNROLL) INSIDE two UNROLLed
            // dimensions (PE_PAIRS x TR = 160-way at PE_OC=20) meant Vitis
            // HLS synthesized 160 separate, fully independent copies of the
            // ky/kx loop's own control logic (FSM state, trip counter,
            // address arithmetic) - a real csynth report's Instance table
            // showed each of those 160 `MAC_KY_MAC_KX` copies at 410 LUT
            // (398 after Lever 1), with roughly 130 of that being pure loop
            // bookkeeping rather than MAC/unpack arithmetic (~65,600 LUT
            // total, 61% of the whole design - LUT_REDUCTION_PLAN.md §1-a).
            // Flipping the nest so the sequential (ic_step, ky, kx) loop is
            // OUTERMOST and PIPELINE'd, with PE_PAIRS/TR's spatial
            // parallelism UNROLLed INSIDE its body instead of wrapping it,
            // gives Vitis one shared FSM/counter/address-generator for the
            // whole datapath instead of one per lane - same total MAC/
            // unpack operation count and same per-cycle spatial parallelism
            // as before, just described so the tool only pays for loop
            // control once.
            //
            // NOT SYNTHESIS-CONFIRMED AT THE TIME THIS WAS WRITTEN - only
            // g++-against-standalone-ap_int.h C-sim (conv_engine_tb.cpp, all
            // configs + all 13 real layers, bit-exact) has verified this is
            // functionally identical to the version it replaces. Loop
            // restructuring is the single riskiest class of change in this
            // file's own history (see e.g. the weight-tile-double-buffering
            // DATAFLOW attempt, RESOURCE_BUDGET.md §5, which failed outright
            // before even reaching the question it set out to answer) -
            // re-run `run_hls.bat`/`run_hls.sh` and check
            // conv_engine_csynth.rpt's Instance table for how many
            // `MAC_KY_MAC_KX`-equivalent copies actually got created (should
            // be 1, not PE_PAIRS x TR) before trusting this delivered the
            // LUT win it was written for.
            //
            // acc_lo/acc_hi are now PE_PAIRS-element arrays, not per-j
            // locals - they must survive across the ENTIRE combined
            // MAC_REDUCE loop below (all ic_step/ky/kx iterations), not just
            // one PE_PAIR_LOOP unroll instance, since PE_PAIR_LOOP is now
            // nested INSIDE MAC_REDUCE instead of wrapping it. `complete`-
            // partitioned so every PE_PAIRS-way UNROLLed access below is to
            // an independent register, matching wtile_packed's dim=1 (pair
            // index) partition in conv_engine() below.
            partial_t acc_lo[PE_PAIRS];
            partial_t acc_hi[PE_PAIRS];
#pragma HLS ARRAY_PARTITION variable=acc_lo complete dim=0
#pragma HLS ARRAY_PARTITION variable=acc_hi complete dim=0

            // Bias folded in exactly once per output pixel/channel, on the
            // FIRST ic_tile pass - same rule as before (see accumulate_or_
            // finish()'s own comment), just hoisted into its own small
            // UNROLLed init loop now that PE_PAIR_LOOP no longer wraps the
            // whole per-pair computation.
        PE_PAIR_INIT:
            for (unsigned j = 0; j < PE_PAIRS; j++) {
#pragma HLS UNROLL
                acc_lo[j] = (ic_tile == 0) ? (partial_t)btile[u][2 * j] : (partial_t)0;
                acc_hi[j] = (ic_tile == 0) ? (partial_t)btile[u][2 * j + 1] : (partial_t)0;
                // LUT_REDUCTION_PLAN.md Lever 2 (kept from the pre-Lever-3
                // version - see RESOURCE_BUDGET.md §8 for its measured
                // effect, DSP +4 only, smaller than hoped but harmless).
// ZYBO PORT - EXPERIMENT D1 (2026-08-14): REMOVING these two pragmas was
// tried and is a MEASURED NULL RESULT. Do not re-try it.
//
// Reasoning at the time: on KV260 this traded DSP for LUT, which was right
// there (DSP 33.8%, CLB packing 93.7%). On XC7Z020 the trade should run the
// other way - the 5-engine projection came back LUT 81.3% (fits) but DSP
// 104.1% (does not). Predicted up to 16 DSP freed (PE_PAIRS=8 x lo+hi).
//
// MEASURED, both pragmas commented out, same part/clock, csynth:
//     DSP   93 -> 93     (zero)
//     LUT   45,489 -> 45,489   (identical to the digit)
//     FF    22,249 -> 22,217   (-32, the only thing that moved)
// Baseline preserved in ../syn_report_baseline_2026-08-14/.
//
// The FF delta proves the edit really did take effect - this is not a
// stale-report artifact. The pragmas simply are not doing what their
// comment claims: the accumulate is already absorbed by the DSP48's own
// post-adder whether or not it is asked for. The parent's own note ("DSP +4
// only, smaller than hoped") was already pointing at this; on 7-series it
// rounds to zero.
//
// Where conv_engine's DSPs actually are: 64 of 93 are in MAC_REDUCE (real
// packed MACs, 2 per DSP - not reducible without losing throughput). The
// other 29 are address/index arithmetic, which IS the place to look if this
// engine ever has to give DSPs back.
#pragma HLS BIND_OP variable=acc_lo op=add impl=dsp
#pragma HLS BIND_OP variable=acc_hi op=add impl=dsp
            }

            // Combined (ic_step, ky, kx) reduction - ONE shared PIPELINE
            // loop instead of PE_PAIRS x TR independent copies, see this
            // block's top comment. `ic_step` replaces the old MAC_IC loop's
            // UNROLL-factor=TR trip count: steps through ic_count in chunks
            // of TR, same total iteration count as before
            // (ceil(ic_count/TR)), just an explicit sequential variable
            // instead of being implicit in a partial-UNROLL's remainder
            // handling.
            const unsigned ic_steps = (ic_count + TR - 1) / TR;
        MAC_REDUCE:
            for (unsigned ic_step = 0; ic_step < ic_steps; ic_step++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_IN_CH/TR
            MAC_KY:
                for (unsigned ky = 0; ky < k; ky++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_K
                MAC_KX:
                    for (unsigned kx = 0; kx < k; kx++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_K
                    PE_PAIR_LOOP:
                        for (unsigned j = 0; j < PE_PAIRS; j++) {
#pragma HLS UNROLL
                            // RESOURCE_BUDGET.md §10 (2026-07-24): a first
                            // version of this block wrote `acc_lo[j] +=
                            // contrib_lo;` directly inside the UNROLLed
                            // MAC_TR loop below - functionally correct
                            // (bit-exact in g++ C-sim) but a real csynth run
                            // came back with the estimated clock period at
                            // 21.231 ns against a 5.00 ns target (~4x over,
                            // Fmax ~47 MHz vs the ~263 MHz this design ran
                            // at before Lever 3). `vitis_hls.log`'s own
                            // critical-path dump showed why: TR (16)
                            // UNROLLed lanes all writing `+=` into the SAME
                            // loop-carried `acc_hi[j]`, each ALSO gated by
                            // `if (ic < ic_count)`, forced a genuine 16-deep
                            // SEQUENTIAL chain of (add, select) pairs, all
                            // inside one PIPELINE II=1 iteration's
                            // combinational logic - not the balanced,
                            // multi-cycle-friendly tree HLS's own automatic
                            // reduction-variable handling would have built
                            // if TR had stayed a genuinely UNROLLed OUTER
                            // loop (the pre-Lever-3 structure) instead of an
                            // UNROLLed loop feeding a single per-cycle
                            // accumulate. Fix: compute all TR contributions
                            // into independent temporaries first (each
                            // lane's own `if` selects between its computed
                            // value and 0, in PARALLEL, not chained), then
                            // combine them with an explicit balanced binary
                            // tree (log2(TR)=4 levels of adds), and only
                            // THEN do ONE add into the loop-carried
                            // `acc_lo[j]`/`acc_hi[j]` per cycle - matching
                            // the "one add per cycle into the persistent
                            // accumulator" cadence the pre-Lever-3 design
                            // always had, just now combining TR spatial
                            // contributions instead of 1.
                            partial_t lo0[TR], hi0[TR];
#pragma HLS ARRAY_PARTITION variable=lo0 complete dim=0
#pragma HLS ARRAY_PARTITION variable=hi0 complete dim=0
                        MAC_TR:
                            // t is the TR-way spatial lane within this
                            // ic_step - ic = ic_step*TR+t. Guarding
                            // `ic < ic_count` here (rather than relying on a
                            // partial-UNROLL loop bound like the pre-Lever-3
                            // version did) is required for the same reason
                            // it always was: window[]/wtile_packed[] beyond
                            // ic_count hold STALE data left over from a
                            // previous ic_tile pass (SHIFT_WINDOW/
                            // load_weight_tile only ever write up to
                            // ic_count), not zeros - reading them
                            // unconditionally would silently corrupt the sum
                            // for any layer where ic_count isn't a multiple
                            // of TR (conv_engine_tb.cpp's config-F exercises
                            // exactly this). `ic` itself never exceeds
                            // MAX_IN_CH-1 even without this guard (TR
                            // divides MAX_IN_CH evenly - conv_engine.h - so
                            // ic_steps*TR <= MAX_IN_CH whenever ic_count <=
                            // MAX_IN_CH), so this guard is purely a
                            // correctness requirement, not an out-of-bounds-
                            // access one. Now gates which VALUE this lane
                            // contributes (0 when out of range), not whether
                            // the accumulate happens - see this loop's outer
                            // comment for why that distinction is the whole
                            // point of this rewrite.
                            for (unsigned t = 0; t < TR; t++) {
#pragma HLS UNROLL
                                unsigned ic = ic_step * TR + t;
                                if (ic < ic_count) {
                                    act_t x = window[ic][ky][kx];
                                    packed_weight_t packed = wtile_packed[u][j][ic][ky][kx];

                                    // ONE DSP48E2-mappable signed multiply
                                    // (8b x 25b, both within the 27x18 port
                                    // limits) computing X*Y_lo and X*Y_hi
                                    // simultaneously. Natural ap_int
                                    // bit-growth (8+25=33) - do NOT pre-cast
                                    // operands to a wider type before
                                    // multiplying (that would force a
                                    // wider-than-33-bit multiply, spilling
                                    // across several DSP48E2s instead of one).
                                    ap_int<33> product = x * packed;
// TIER B1 (2026-08-05, TIMING_CLOSURE_PLAN.md §4 Tier B / §3 cause B).
// Without this the DSP48E2 this maps to is PURELY COMBINATIONAL - the
// MAC_REDUCE csynth report shows `mul_24s_8s_32_1_1_*` instances with
// **FF = 0**, i.e. AREG/BREG/MREG/PREG all unused. That leaves the
// multiplier electrically continuous with the fabric logic on both sides,
// and the 200 MHz routed run's worst path (WNS -0.021) runs straight
// through it. On the KV260 parent that path was: 16-lane guard mux ->
// TREE_L1 -> TREE_L2 -> TREE_L3, four CARRY8 stages in one pipeline stage.
// In THIS fork (TR=8) the tree is one level shorter - 8-lane mux -> TREE_L1
// -> TREE_L2 - and the part has CARRY4, not CARRY8.
//
// latency=2 turns on the DSP's internal registers, isolating the multiply
// from fabric on both sides. Start at 2; raise to 3 if still short.
//
// Costs pipeline DEPTH, not throughput: the enclosing loop is PIPELINE
// II=1, so extra multiplier latency is absorbed by the pipeline rather
// than multiplying the trip count. Arithmetic is untouched - this is a
// binding directive, so C-sim results must be bit-identical (that is the
// first thing to check after applying it, precisely because a pragma that
// silently does nothing looks the same as one that works).
#pragma HLS BIND_OP variable=product op=mul impl=dsp latency=2

                                    // Split, WITH a +1 carry correction on the
                                    // high half - caught wrong by
                                    // pack_unpack_selftest (conv_engine_tb.cpp)
                                    // before this fix. `product >> 16` (H_raw)
                                    // and "low 16 bits taken UNSIGNED" satisfy
                                    // product = H_raw*65536 + L_unsigned
                                    // unconditionally (that's just what an
                                    // arithmetic shift + low-bits split means).
                                    // But we want L reinterpreted as SIGNED
                                    // (l_signed = X*y_lo_biased, which is
                                    // negative whenever X<0, since
                                    // y_lo_biased is always >=0) - and
                                    // whenever L_unsigned's top bit is set
                                    // (i.e. l_signed < 0), that reinterpretation
                                    // silently "steals" 65536 from the high
                                    // half that a plain arithmetic shift never
                                    // gives back. H_raw is short by exactly 1
                                    // in exactly that case; nothing else needs
                                    // adjusting (l_signed/contrib_lo are
                                    // already exact on their own).
                                    ap_int<16> l_signed = product.range(15, 0);
                                    // partial_t, NOT accum_t - completes
                                    // Lever 1 (contrib_hi/contrib_lo were
                                    // left at accum_t/32-bit in Lever 1's
                                    // first pass, an oversight caught while
                                    // rewriting this block for Lever 3; both
                                    // contrib_hi/contrib_lo's own natural
                                    // range - an 8b x 8b product, |.| <=
                                    // 16384 - and acc_lo/acc_hi's declared
                                    // width already bound this comfortably).
                                    partial_t contrib_hi = (partial_t)(product >> 16);
                                    if (l_signed < 0) {
                                        contrib_hi += 1;
                                    }
                                    partial_t contrib_lo = (partial_t)l_signed - ((partial_t)x << 7);

                                    lo0[t] = contrib_lo;
                                    hi0[t] = contrib_hi;
                                } else {
                                    lo0[t] = 0;
                                    hi0[t] = 0;
                                }
                            }

                            // Balanced binary-tree reduce of the TR
                            // contributions - log2(TR) levels of PARALLEL
                            // adds, not a serial chain (see this loop's top
                            // comment for why that distinction is the point).
                            // Hardcoded to the current TR (explicit levels,
                            // not a generic runtime-bounded loop) - this
                            // file's own established preference (see e.g.
                            // the if/else-not-ternary and per-level-
                            // LOOP_TRIPCOUNT fixes elsewhere) is explicit
                            // code over a clever construct whose UNROLL/
                            // scheduling behavior would need its own
                            // separate synthesis-confirmation before trust.
                            // conv_engine.h constrains TR to a power-of-2
                            // divisor of MAX_IN_CH - if TR ever changes,
                            // this tree must be re-written by hand to match
                            // (the static_assert below only catches silently
                            // building this against the wrong TR, it doesn't
                            // generalize the tree itself).
                            //
                            // ZYBO PORT (2026-08-14): TR 16 -> 8, so this
                            // went from 4 explicit levels (16->8->4->2->1)
                            // to 3 (8->4->2->1) - TREE_L3 and the lo3/hi3
                            // arrays are gone, and TREE_L1/L2 halved their
                            // widths. The parent's static_assert is what
                            // caught this: dropping TR without touching the
                            // tree would have silently summed only the first
                            // 8 of 16 lanes on the KV260 code, or read past
                            // lo0[] here. Keep the assert in sync with TR -
                            // it is the only thing standing between a knob
                            // change and wrong arithmetic.
                            //
                            // Depth matters more on this part than it did on
                            // the parent: 7-series has CARRY4, not CARRY8,
                            // so each of these adds costs twice the carry
                            // primitives. Losing a whole tree level is a
                            // timing win here, not just an area one.
                            // ARTY CLASSIFIER TR8 FORK (2026-08-18): TR 16 -> 8,
                            // so the tree drops back to THREE levels
                            // (8->4->2->1): TREE_L3 and the lo3/hi3 arrays are
                            // gone and TREE_L1/L2 halve their widths. This is
                            // the hand rewrite the assert demands.
                            //
                            // The assert is doing real work: TR=8 with the
                            // 4-level tree would have read lo0[8..15] past the
                            // end of a TR-sized array. It caught exactly that
                            // on the first build of this fork.
                            //
                            // Losing a tree level is also a timing/area win on
                            // 7-series, which has CARRY4 rather than CARRY8 -
                            // each of these adds costs twice the carry
                            // primitives it would on the parent part.
                            static_assert(TR == 4, "MAC_TR's explicit 2-level "
                                          "tree reduction is hardcoded for TR==4 "
                                          "- update it by hand if TR changes "
                                          "(conv_engine.h). W4 rewrote this from "
                                          "the 3-level TR==8 form; without the "
                                          "rewrite the build silently sums only "
                                          "half the lanes.");
                            partial_t lo1[2], hi1[2];
#pragma HLS ARRAY_PARTITION variable=lo1 complete dim=0
#pragma HLS ARRAY_PARTITION variable=hi1 complete dim=0
                        TREE_L1:
                            for (unsigned i = 0; i < 2; i++) {
#pragma HLS UNROLL
                                lo1[i] = lo0[2 * i] + lo0[2 * i + 1];
                                hi1[i] = hi0[2 * i] + hi0[2 * i + 1];
                            }

                            acc_lo[j] += lo1[0] + lo1[1];
                            acc_hi[j] += hi1[0] + hi1[1];
                        }
                    }
                }
            }

            // On the LAST ic_tile (or the only one, when num_ic_tiles==1):
            // real per-layer LeakyReLU(13/128) + requantize + saturate,
            // matching RTL_HANDOFF_KO.md section 5. On any earlier ic_tile,
            // writes a raw partial sum to `accum` instead - see
            // accumulate_or_finish() above. Runs after MAC_REDUCE since
            // acc_lo[j]/acc_hi[j] don't finish accumulating until every
            // MAC_REDUCE iteration above has run for every j.
            //
            // RESTRUCTURED TWICE (2026-08-04, both steps against real
            // measurements - TROUBLESHOOTING.md has the full story):
            //
            // Original: `PE_PAIR_FINISH`, an UNROLL'd loop making 24 calls
            // (2*PE_PAIRS at PE_OC=24) into the then-un-inlined
            // accumulate_or_finish() - ONE shared NON-PIPELINED instance
            // ("Interval 10-21 ... Pipeline: no" in the csynth Instance
            // table). An UNROLL'd loop calling one shared sequential
            // instance serializes: ~240-500 cycles per output position,
            // roughly half of layer 9's measured 742 cycles/COL_LOOP-body.
            //
            // First fix attempt (single `FINISH_LANES` loop, PIPELINE II=1,
            // callee inlined) DID NOT WORK - measured: layer-9 cosim
            // 855,307 -> 861,811 (+0.76%, i.e. nothing), and the csynth
            // Instance table showed why: `Pipeline_FINISH_LANES` came back
            // with fixed latency 340 for 24 iterations, i.e. achieved
            // II~14, not 1. One loop body mixing a CONDITIONAL m_axi accum
            // read, an accum write, and an ofmap write - all on the same
            // WR_BUS bundle, with the read inside a branch (the exact
            // `AccessInCondBranchMissed` pattern §23-d documented for
            // READ_CH) and a potential read-after-write alias on `accum`
            // within one body - left the scheduler no room to pipeline the
            // requests; II collapsed to roughly the old call interval.
            //
            // Current fix: PHASE-SPLIT into separate, unconditional,
            // consecutive-address pipelined loops - the same shape that
            // already gets II=1 for READ_CH's m_axi reads:
            //   gather (registers only) -> optional ACCUM_RD (read-only
            //   loop) -> ONE of FINISH_WR/ACCUM_WR (write-only loop).
            // The per-lane conditions (`ic_tile > 0`, "is this the last
            // ic_tile") never depended on `lane`, so they hoist out of the
            // loops entirely; the lane-validity guard (`global_oc <
            // out_ch`) becomes the loop BOUND (`lanes`) instead of an
            // in-body branch. No loop both reads and writes `accum`, so
            // there is no intra-loop alias to serialize around. Write
            // order across lanes is identical to the old 2j/2j+1 call
            // order, so the ofmap/accum contents are byte-identical - only
            // the timing changes.
            //
            // accumulate_or_finish() (the helper both earlier versions
            // called) is gone - its 4-way (first/middle/last/only ic_tile)
            // branch structure is exactly what's been flattened into these
            // phase loops; keeping a per-lane helper would just re-create
            // the in-body branching this fix exists to remove.
            unsigned pixel_idx = out_r * out_w + out_c;
            unsigned oc_base = oc_tile * PE_OC;
            unsigned lanes = out_ch - oc_base;
            if (lanes > 2 * PE_PAIRS) lanes = 2 * PE_PAIRS;

            accum_t acc_fin[2 * PE_PAIRS];
#pragma HLS ARRAY_PARTITION variable=acc_fin complete dim=0

        FINISH_GATHER:
            for (unsigned lane = 0; lane < 2 * PE_PAIRS; lane++) {
#pragma HLS UNROLL
                unsigned j = lane / 2;
                acc_fin[lane] = (lane % 2 == 0) ? (accum_t)acc_lo[j] : (accum_t)acc_hi[j];
            }

            if (num_ic_tiles > 1 && ic_tile > 0) {
            ACCUM_RD:
                // UNROLL (2026-08-08). This was PIPELINE II=1 before, so lanes
                // left one at a time: 32 iterations + drain = ~35 cycles per
                // call, and on top of that Vitis split the loop into its own
                // grp_*_Pipeline_ACCUM_RD module, adding an ap_start/ap_done
                // handshake to every call.
                //
                // The banking cost was already paid. accum_onchip is partitioned
                // `cyclic factor=PE_OC` (32 banks), and the index is
                // pixel_idx*(G*PE_OC) + u*PE_OC + lane where G*PE_OC=64 is a
                // multiple of 32, so bank == lane - **a different bank per lane**.
                // Nothing structural stopped 32 simultaneous accesses; PIPELINE
                // was simply leaving them unused.
                //
                // Why the bound moved from the runtime `lanes` to the compile-time
                // 2*PE_PAIRS, with the guard pushed into the body: UNROLL requires
                // a compile-time trip count. acc_fin is already completely
                // partitioned, i.e. 32 registers.
                for (unsigned lane = 0; lane < 2 * PE_PAIRS; lane++) {
#pragma HLS UNROLL
                    // OC-hoist: accum's per-pixel stride is now the whole
                    // group (OC_GROUP_TILES*PE_OC), and member u owns the
                    // u-th PE_OC-wide slice of it. At G=1 this reduces to
                    // the original `pixel_idx * PE_OC + lane`.
                    if (lane < lanes) {
                        acc_fin[lane] += accum_onchip[pixel_idx * (OC_GROUP_TILES * PE_OC)
                                               + u * PE_OC + lane];
                    }
                }
            }

            if (ic_tile == num_ic_tiles - 1) {
            // TRIED AND REJECTED (2026-08-16/17, cosim): packing this store.
            //
            // The obvious optimisation here is to stop writing 1 byte at a
            // time. A throttle build (ofmap store replaced by an on-chip sink)
            // measured this store costing **2,506,782 cycles = 18.2% of layer
            // 0**, so the prize is real and it was measured, not guessed.
            //
            // It does not work. `hls/zybo_conv_engine_opack` retyped `ofmap`
            // to pack4_t* and assembled 4 bytes per store (16 transactions per
            // position/tile -> 4). It is **bit-exact** - 2,359,296 values match
            // the golden model - and csynth liked it (LUT +349, DSP -5). cosim
            // came back **16,122,809 vs 13,763,513, i.e. +17.1% SLOWER**, and
            // the excess was +2,359,296 = **exactly the output byte count**,
            // one cycle per byte.
            //
            // Why: the address below is AFFINE in `lane`, so the m_axi adapter
            // walks it and streams. A packed store needs a carried word index
            // (`wr_word++`) under a condition, and that costs a fresh handshake
            // every iteration. **The write cost is attached to loop
            // ITERATIONS, not to transaction count** - which is why removing
            // 3/4 of the transactions bought nothing and cost extra.
            //
            // TRIED AND REJECTED TOO (2026-08-18, cosim, this tree): the
            // "only lever left" named here - a word-major loop with a
            // compile-time 4-lane UNROLL - **does not work either, and the
            // reason is not area.** Measured on conv1 @48x48: 341,657 ->
            // 456,660 cycles, **+33.7% SLOWER** (148.3 -> 198.2 per position).
            //
            // csynth had already said so in a column that was not read: the
            // FINISH_WR sub-report came back **achieved II = 4** against a
            // target of 1. Iterations went 32 -> 8 and II went 1 -> 4, so the
            // product is unchanged, while the iteration latency grew 12 -> 22.
            // Pure loss.
            //
            // Why II=4: `ofmap` is ONE m_axi write port. UNROLL replicates the
            // datapath but not the port, so the four byte stores serialise on
            // it. Four bytes per cycle needs a four-byte WORD, which is the
            // opack experiment above - and that one is closed by the affine
            // address argument. **Both directions are now closed by
            // measurement; the write cost is a floor on this interface.**
            //
            // (The Zybo estimate that used to live here - +3,800 LUT / +18 DSP,
            // "unaffordable on this part" - was area-only and turned out to be
            // the wrong objection. This tree's csynth: LUT +3,038, DSP +6,
            // Estimated clock unchanged. It was affordable. It was just slower.)
            //
            // Full story: doc/01_status/2026-08-17_axi-campaign-closed.md,
            // doc/06_troubleshooting/conv-engine-main-log.md #38.
            // (This block is a comment only - the RTL is unchanged.)
            FINISH_WR:
                // WARNING: this loop is **deliberately left on PIPELINE.** Do
                // not "make it match" just because ACCUM_RD/ACCUM_WR above and
                // below were UNROLLed. This is the only loop that calls
                // apply_activation(), and that function is `#pragma HLS INLINE`.
                // The whole reason that INLINE is safe is that "FINISH_WR is
                // PIPELINE, not UNROLL, so there is only one spatial copy" (see
                // the comment above that function - there is a history of a
                // 20-copy LUT explosion). UNROLLing here replicates the barrel
                // shifter and multiplier 32 times and reproduces that incident
                // exactly.
                for (unsigned lane = 0; lane < lanes; lane++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=PE_OC
                    ofmap[pixel_idx * out_ch + oc_base + lane] =
                        apply_activation(acc_fin[lane], leaky_relu_enable,
                                         requant_multiplier, requant_shift);
                }
            } else {
            ACCUM_WR:
                // UNROLL - same reason and same banking argument as ACCUM_RD.
                // The write side also lands in a different bank per lane, so
                // there is no conflict.
                for (unsigned lane = 0; lane < 2 * PE_PAIRS; lane++) {
#pragma HLS UNROLL
                    if (lane < lanes) {
                        accum_onchip[pixel_idx * (OC_GROUP_TILES * PE_OC)
                              + u * PE_OC + lane] = acc_fin[lane];
                    }
                }
            }
                } // OC_IN_GROUP
            }
        }
    }
}

void conv_engine(
    // PACK4: int8x4-packed ifmap - see pack4.h. Same DDR bytes, same
    // register map, `in_ch` still in channels.
    const pack4_t  *ifmap,
    const pack4_t *weights,
    const pack4_t *weights_hi,
    const bias_t   *bias,
    act_t          *ofmap,
    // accum moved on-chip (URAM) on 2026-08-08 and is gone - see the
    // ACCUM_POSITIONS comment in conv_engine.h. Its s_axilite register
    // disappears with it, which shifts every following scalar register offset
    // 0x0c earlier: re-check SW/conv_engine_hw_driver.h against the generated
    // xconv_engine_hw.h.
    uint16_t img_h, uint16_t img_w, uint16_t in_ch, uint16_t out_ch,
    uint8_t  k, uint8_t stride, uint8_t pad,
    int32_t  requant_multiplier, uint8_t requant_shift, uint8_t leaky_relu_enable
) {
    // ---- AMBA interface pragmas -------------------------------------------
    // ifmap/weights depth now scales with MAX_TOTAL_IN_CH (1024), not
    // MAX_IN_CH (128) - a real 8x depth increase, since these ports must be
    // addressable across a layer's FULL in_ch even though on-chip buffers
    // (window/wtile_packed) only ever hold one MAX_IN_CH-wide tile at a
    // time. `depth=` is a cosim/IP-XACT sizing hint only (synthesis ignores
    // it - confirmed in TROUBLESHOOTING.md), so this costs nothing in
    // synthesized hardware, but DOES make full-depth cosim buffers (see
    // conv_engine_tb.cpp's IFMAP_DEPTH/WEIGHTS_DEPTH) bigger - the existing
    // "only the one dedicated config-A-cosim call pays full depth" pattern
    // is what keeps that from slowing down cosim further.
// PACK4: ifmap's depth= is now in 32-bit WORDS, not int8 elements. The
// total addressable byte footprint is unchanged - only the pointer's unit
// changed. Every other port keeps its original element type and depth.
#pragma HLS INTERFACE m_axi port=ifmap   offset=slave bundle=RD_BUS  depth=MAX_IMG_W*MAX_IMG_W*MAX_TOTAL_IN_CH/PACK4_LANES
#pragma HLS INTERFACE m_axi port=weights offset=slave bundle=RD_BUS  depth=MAX_OUT_CH*MAX_TOTAL_IN_CH*MAX_K*MAX_K/PACK4_LANES
    // Second, independent AXI4 master port aliasing the same DRAM region as
    // `weights` (own bundle, NOT RD_BUS) - see conv_engine.h's note on this
    // param and LOAD_W_IC's read of it below. This is the fix for the real
    // csynth-measured LOAD_W_IC II=2 (target II=1): two reads to `weights`
    // in one iteration could not both issue on RD_BUS in the same cycle, no
    // matter how the rest of RD_BUS's contention (ifmap/bias) was managed -
    // splitting `weights` onto its own physical port is what buys the second
    // read/cycle, not a bundle-sharing tweak. Costs one more physical AXI4
    // master port at the Vivado level - see vivado/create_bd.tcl.
#pragma HLS INTERFACE m_axi port=weights_hi offset=slave bundle=RD_BUS2 depth=MAX_OUT_CH*MAX_TOTAL_IN_CH*MAX_K*MAX_K/PACK4_LANES
#pragma HLS INTERFACE m_axi port=bias    offset=slave bundle=RD_BUS  depth=MAX_OUT_CH
#pragma HLS INTERFACE m_axi port=ofmap   offset=slave bundle=WR_BUS  depth=MAX_IMG_W*MAX_IMG_W*MAX_OUT_CH

#pragma HLS INTERFACE s_axilite port=ifmap   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=weights bundle=CTRL
#pragma HLS INTERFACE s_axilite port=weights_hi bundle=CTRL
#pragma HLS INTERFACE s_axilite port=bias    bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ofmap   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=img_h   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=img_w   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=in_ch   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=out_ch  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=k       bundle=CTRL
#pragma HLS INTERFACE s_axilite port=stride  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=pad     bundle=CTRL
#pragma HLS INTERFACE s_axilite port=requant_multiplier bundle=CTRL
#pragma HLS INTERFACE s_axilite port=requant_shift       bundle=CTRL
#pragma HLS INTERFACE s_axilite port=leaky_relu_enable   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return  bundle=CTRL

    // ---- Defensive bounds checks (simulation only - __SYNTHESIS__ is a
    // Vitis HLS built-in macro defined during C/RTL synthesis and NOT
    // during C-simulation, the standard idiom for sim-only sanity checks
    // that cost zero hardware). These exist because MAX_IN_CH/MAX_K are
    // placeholders (see conv_engine.h) and stride>1 is unimplemented -
    // misconfiguration from SW would otherwise silently index out of bounds
    // or silently compute a stride-1 result for a stride>1 request instead
    // of failing loudly in simulation where it's cheap to catch. ------------
#ifndef __SYNTHESIS__
    // Checks img_h/img_w PLUS padding against line_buf's real capacity
    // (MAX_IMG_W+2, scan_and_compute()'s FIX comment on line_buf's
    // declaration) - not just img_h/img_w alone against MAX_IMG_W. A
    // plain `img_w <= MAX_IMG_W` check (this file's earlier version, see
    // TROUBLESHOOTING.md #19) passes for the real network's actual layer 0
    // (img_w=512, pad=1) while `pad_w = img_w+2*pad = 514` still overflows
    // line_buf's column dimension by 2 - the padded size, not the raw
    // size, is what COL_LOOP actually indexes with.
    assert(img_h + 2u * pad <= MAX_IMG_W + 2u &&
           "img_h+2*pad exceeds line_buf's capacity (MAX_IMG_W+2) - see conv_engine.h");
    assert(img_w + 2u * pad <= MAX_IMG_W + 2u &&
           "img_w+2*pad exceeds line_buf's capacity (MAX_IMG_W+2) - see conv_engine.h");
    // Was `in_ch <= MAX_IN_CH` - MAX_IN_CH is now just the on-chip tile
    // width (see conv_engine.h); a layer's real total in_ch is tiled across
    // multiple passes (IC_TILE below) up to MAX_TOTAL_IN_CH instead.
    assert(in_ch <= MAX_TOTAL_IN_CH && "in_ch exceeds MAX_TOTAL_IN_CH - see README design limits");
    // PACK4 contract. Deliberately NOT "in_ch % 4 == 0" - READ_CH has a
    // slow path for the remainder case precisely so layer 0 (in_ch=3) keeps
    // working without re-laying-out its RGB input in DDR. What IS required
    // is that the fast path's assumption holds whenever it is taken:
    // ic_lo must be word-aligned so each ic_tile pass starts on a word
    // boundary, and ic_count must then be a whole number of words. The
    // ic_tile width is MAX_IN_CH (128) - see `ic_lo = ic_tile * MAX_IN_CH`
    // below - so both hold by construction. Asserted rather than assumed,
    // since a future MAX_IN_CH change could silently break the fast path.
    assert((MAX_IN_CH % PACK4_LANES) == 0 &&
           "MAX_IN_CH (the ic_tile width) must be a multiple of 4 for READ_CH's "
           "packed fast path - see pack4.h");
    assert(out_ch <= MAX_OUT_CH && "out_ch exceeds MAX_OUT_CH - see conv_engine.h");
    assert(k <= MAX_K && k >= 1 && "k out of [1, MAX_K] range");
    assert(stride == 1 && "only stride=1 implemented - see README design limits");
    assert(pad <= MAX_K && "implausible pad value");
    // out_w/pad_h/pad_w (scan_and_compute) are computed as img_w+2*pad-k+1
    // etc. on unsigned types - a layer where the padded image is smaller
    // than the kernel would underflow that subtraction instead of just
    // being an invalid config. Not a realistic network layer (feature maps
    // are always far larger than a 1x1/3x3 kernel), but cheap to make an
    // explicit assertion rather than an implicit assumption.
    assert((unsigned)img_h + 2u * pad >= k && "img_h+2*pad must be >= k");
    assert((unsigned)img_w + 2u * pad >= k && "img_w+2*pad must be >= k");
    // round_shift()'s ap_int<64> intermediate has room to spare, but a
    // shift this large would mean the caller mis-programmed the register
    // (model_manifest.json's real values top out at 42) rather than any
    // legitimate layer needing it.
    assert(requant_shift <= 48 && "requant_shift implausibly large for round_shift's ap_int<64>");
#else
    (void)0; // no-op in synthesis - assertions above compile out entirely
#endif
    (void)stride; // enforced by the assertion above, not by branching hardware

    const unsigned num_oc_tiles = ((unsigned)out_ch + PE_OC - 1) / PE_OC;
    const unsigned num_ic_tiles = ((unsigned)in_ch + MAX_IN_CH - 1) / MAX_IN_CH;

    // On-chip accum sizing contract (2026-08-08). accum_onchip holds only
    // ACCUM_POSITIONS output positions. That array is used only when
    // num_ic_tiles > 1 (see the ACCUM_RD/ACCUM_WR guards below), so the bound
    // only has to hold in that case.
    //
    // **This is the one place this change can break silently.** The current
    // network is safe almost by luck: the spatially large layers (idx 1 has
    // 36,864 positions) have a small in_ch and never spill, and the layers that
    // do spill (idx 5-12) are spatially small. The moment one layer has both,
    // this indexes past the end of the array. The assert only fires in csim, so
    // always run csim after changing the layer table.
    {
        const unsigned _out_h = (unsigned)img_h + 2u * pad - k + 1u;
        const unsigned _out_w = (unsigned)img_w + 2u * pad - k + 1u;
        assert((num_ic_tiles <= 1 || (size_t)_out_h * _out_w <= ACCUM_POSITIONS) &&
               "out_h*out_w exceeds ACCUM_POSITIONS on an ic-tiled layer - "
               "on-chip accum would overflow; raise ACCUM_POSITIONS in conv_engine.h "
               "(and re-check URAM budget) or keep this layer's in_ch <= MAX_IN_CH");
        (void)_out_h; (void)_out_w;
    }

    // wtile/btile are owned here (not inside load_weight_tile/scan_and_compute)
    // specifically so a future double-buffered version only needs to
    // duplicate these two arrays and add a DATAFLOW pragma around the loop
    // body below - see RESOURCE_BUDGET.md §5. Tried 2026-07-23 (moving these
    // inside the loop, non-static, plus DATAFLOW on OC_TILE): hit a hard
    // Vitis HLS error before ever reaching the ping-pong-buffering question -
    // `ifmap`/`weights`/`bias` all share one AXI bundle (RD_BUS below), and
    // DATAFLOW requires each AXI bundle be read by only one process; with
    // load_weight_tile() (weights/bias) and scan_and_compute() (ifmap) now
    // meant to run concurrently, sharing RD_BUS became a real conflict, not
    // just a style issue: "[HLS 200-1013] Bundled bus interface 'RD_BUS' ...
    // failed dataflow checking: it cannot read data in multiple processes."
    // Reverted rather than also splitting `ifmap` onto its own AXI bundle -
    // that's a real interface change (one more physical AXI master port),
    // not a pragma-only fix, and would also need README.md's AMBA interface
    // map and the eventual Vivado `create_bd.tcl` wiring updated to match.
    // See RESOURCE_BUDGET.md §5 for the full finding and what doing this for
    // real would require.
    //
    // wtile_packed is complete on dim=1 (pair index j) - PE_PAIR_LOOP in
    // scan_and_compute UNROLLs j, so each of the PE_PAIRS lanes needs a
    // compile-time-constant-indexed, simultaneously-readable slice - and
    // cyclic(TR) on dim=2 (ic), matching MAC_IC's UNROLL factor=TR for the
    // same reason. It is NOT partitioned on dim=3/dim=4 (ky/kx) - see the
    // "FIX 2" comment on window's declaration in scan_and_compute() for why:
    // nothing accesses it through a ky/kx index that's ever actually
    // unrolled (load_weight_tile's and MAC_KY/MAC_KX's ky/kx loops are
    // PIPELINE, not UNROLL).
    //
    // INT8 DSP-packing experiment (conv_engine.h): this replaces the
    // original wtile[PE_OC][...] with wtile_packed[PE_PAIRS][...] - half as
    // many instances (128 vs 256) for the same reason the multiplier count
    // halves, since each pair now shares one packed weight value instead of
    // two independent weight_t values.
    // OC-hoist: leading OC_GROUP_TILES dimension holds one weight/bias tile
    // per group member, so a single spatial scan can serve all of them.
    // dim=1 is that group index and is deliberately NOT partitioned - the
    // group members are processed SEQUENTIALLY (OC_IN_GROUP, `UNROLL off`),
    // so they never need concurrent access, and partitioning it would hand
    // the tool exactly the excuse it needs to replicate the MAC datapath.
    // The two existing partitions keep their meaning and shift up one dim.
    static packed_weight_t wtile_packed[OC_GROUP_TILES][PE_PAIRS][MAX_IN_CH][MAX_K][MAX_K];
    static bias_t btile[OC_GROUP_TILES][PE_OC];
#pragma HLS ARRAY_PARTITION variable=wtile_packed complete dim=2
#pragma HLS ARRAY_PARTITION variable=wtile_packed cyclic factor=TR dim=3
#pragma HLS ARRAY_PARTITION variable=btile complete dim=2
    // History (PE_OC=16, real csynth run): packed_weight_t (25 bits) is ~3x
    // wider per element than the original weight_t (8 bits), and even though
    // instance count halved (256 -> 128, cyclic(TR) x PE_PAIRS), each
    // surviving instance's data volume (72 elements x 25 bits = 1,800 bits)
    // crossed Vitis's automatic BRAM-vs-LUTRAM inference threshold - the
    // original 576-bit instances stayed off BRAM entirely, but all 128 of
    // these got mapped to individual BRAM_18K blocks, pushing total design
    // BRAM_18K from 23% to 67%. Forced back into distributed/LUT-based
    // storage (impl=LUTRAM) at the time to trade that back for LUT headroom.
    //
    // TRIED AND REJECTED (2026-07-24, real csynth run at the current
    // PE_OC=20/weights_hi config): re-tested impl=BRAM on the theory that
    // BRAM_18K's headroom (72/288, 25%, unlike the PE_OC=16 measurement
    // above where BRAM was the one under pressure) could be spent to pull
    // LUT down from its 92% baseline (107,732/117,120, the tightest
    // resource in the design). Real result was a regression on every
    // metric, not a trade: LUT 107,732 -> 111,412 (92% -> 95%, WORSE, not
    // better), FF 47,335 -> 59,181 (+25%, from BRAM's synchronous
    // read-latency output registers on ~80-128 partitioned instances), and
    // BRAM_18K itself 72 -> 232 (25% -> 81%, most of the "headroom" this was
    // supposed to spend). DSP and Fmax were unaffected either way. Per-
    // instance BRAM control/address/output-register overhead across this
    // many small partitioned instances apparently costs more than LUTRAM's
    // direct implementation saves - impl=LUTRAM was already the correct
    // choice at this config. Do not retry BRAM here without a real
    // architecture change (e.g. fewer, larger partitions) first.
#pragma HLS BIND_STORAGE variable=wtile_packed type=RAM_1P impl=LUTRAM

    // OC-hoist: OC_TILE is now OC_GROUP, stepping OC_GROUP_TILES tiles at a
    // time. At G=1 this is exactly the old loop (num_oc_groups ==
    // num_oc_tiles, tiles_in_group == 1 always).
    const unsigned num_oc_groups =
        (num_oc_tiles + OC_GROUP_TILES - 1) / OC_GROUP_TILES;

OC_GROUP:
    for (unsigned g = 0; g < num_oc_groups; g++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=64
        // The last group is partial when num_oc_tiles is not a multiple of
        // OC_GROUP_TILES (e.g. out_ch=30 -> 1 tile, or layer 6's 32 tiles at
        // G=3). Every layer with out_ch not a multiple of G*PE_OC hits this.
        const unsigned tiles_in_group =
            (num_oc_tiles - g * OC_GROUP_TILES < OC_GROUP_TILES)
                ? (num_oc_tiles - g * OC_GROUP_TILES) : OC_GROUP_TILES;
    IC_TILE:
        // Known, expected trade-off - now on TWO axes instead of one: ifmap
        // is re-read from DDR once per (oc_tile, ic_tile) pair
        // (num_oc_tiles*num_ic_tiles total passes) instead of once per
        // oc_tile. See RESOURCE_BUDGET.md §2 - the "engine efficiency"
        // derating factor there already accounted for the oc_tile axis
        // without a real cosim trace to measure it from; this multiplies
        // that same unmeasured factor by num_ic_tiles too, for any layer
        // where it's >1 (8 of the real network's 13 conv layers - see
        // conv_engine.h). `ic_tile` is the loop that actually needs
        // `accum` (see accumulate_or_finish() above) - `oc_tile` staying
        // the OUTER loop is what lets `accum` be sized [out_h][out_w][PE_OC]
        // instead of [out_h][out_w][out_ch]: one oc_tile fully finishes
        // (across all its ic_tiles) before the next one starts, so the
        // same small accum region is safely reused/overwritten fresh each
        // time, never needing to hold more than one oc_tile's channels.
        for (unsigned ic_tile = 0; ic_tile < num_ic_tiles; ic_tile++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=8
            unsigned ic_lo = ic_tile * MAX_IN_CH;
            unsigned ic_count = ((unsigned)in_ch - ic_lo < MAX_IN_CH)
                                     ? ((unsigned)in_ch - ic_lo) : MAX_IN_CH;
            // One weight tile per group member, loaded ONCE per (group,
            // ic_tile) - outside the spatial scan below. This is what
            // completion condition 4 checks in conv_engine_csynth.rpt's loop
            // hierarchy: no weight load may appear under COL_LOOP.
            // load_weight_tile() itself is unchanged - wtile_packed[u] and
            // btile[u] are slices with exactly its existing parameter shape.
        LOAD_GROUP:
            for (unsigned u = 0; u < tiles_in_group; u++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=OC_GROUP_TILES
                load_weight_tile(weights, weights_hi, bias,
                                  g * OC_GROUP_TILES + u,
                                  ic_lo, ic_count, in_ch, out_ch, k,
                                  wtile_packed[u], btile[u]);
            }
            scan_and_compute(ifmap, ofmap, img_h, img_w, ic_lo, ic_count, in_ch, out_ch,
                              k, pad, g, tiles_in_group, ic_tile, num_ic_tiles, wtile_packed, btile,
                              leaky_relu_enable != 0, requant_multiplier, requant_shift);
        }
    }
}
