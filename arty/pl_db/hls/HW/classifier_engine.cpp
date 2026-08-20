#include "classifier_engine.h"

// -----------------------------------------------------------------------------
// Arty Z7-20 96x96 classifier - 100 MHz timing-focused revision.
//
// Datapath inherited from the 96x96 fused build:
//   * conv0 + pool0 fused, 2-OC full-kernel schedule
//   * conv1 + pool1 fused, 8 OC x 8 IC schedule
//   * conv2, 8 OC x 8 IC schedule
//
// Post-route analysis of the previous Vivado build showed the critical setup
// path entering the shared 32x32->64 requant multiplier from conv1 control.
// The arithmetic and external interfaces below are unchanged; only the
// requant multiplier implementation is timing-tuned later in this file.
// -----------------------------------------------------------------------------
static const unsigned C0_OC_PAR = 2;   // unpacked - packing hurts conv0, see header note
static const unsigned CS_IC_PAR = 8;
static const unsigned C1_OC_PAR = 16;  // doubled + DSP-packed (DSP-neutral)
static const unsigned C2_OC_PAR = 16;  // doubled + DSP-packed (DSP-neutral)

static inline accum_t reduce8(const accum_t v[CS_IC_PAR]) {
#pragma HLS INLINE
    accum_t s01 = v[0] + v[1];
    accum_t s23 = v[2] + v[3];
    accum_t s45 = v[4] + v[5];
    accum_t s67 = v[6] + v[7];
    accum_t s0123 = s01 + s23;
    accum_t s4567 = s45 + s67;
    return s0123 + s4567;
}

// -----------------------------------------------------------------------------
// mac_pair: two independent signed INT8 x INT8 products that share one
// operand (`a`), computed with a SINGLE DSP48E1 25x18 multiply instead of
// two separate multiplies. This is the DSP-packing lever behind the 100 MHz
// perf rework (see PROJECT header note): every place in this file that used
// to do "same activation times OC_PAR different weights" one DSP per weight
// now pairs adjacent OC lanes through this function, roughly halving DSP
// count per unit of parallelism - which is reinvested as doubled OC_PAR
// (C0/C1/C2_OC_PAR) so total DSP stays close to the original budget while
// the OC_BLOCK loop trip count (and so total latency) is roughly halved.
//
// DSP48E1 natively computes a signed 25(A) x 18(B) -> 43-bit product. `a`
// (8-bit) goes on the narrow B port; both weights are packed into one 25-bit
// A operand:
//   y = w_hi * 65536 + (w_lo + 128)     // w_lo biased into unsigned [0,255]
//   p = a * y                          // one DSP48E1 multiply
// a*(w_lo+128) is always exactly representable in 16 signed bits (range
// [-32640, 32385] within [-32768, 32767)), so the low 16 bits of the two's
// complement product p equal that term EXACTLY - no truncation and no carry
// into the high field. Subtracting it back out before shifting recovers the
// high term exactly too (no off-by-one rounding, unlike the common variant
// of this trick that just does a naive p >> 16).
// -----------------------------------------------------------------------------
static inline void mac_pair(
    act_t a, weight_t w_lo, weight_t w_hi,
    accum_t &prod_lo, accum_t &prod_hi
) {
#pragma HLS INLINE
    ap_uint<8> w_lo_biased = (ap_uint<8>)((ap_int<9>)w_lo + 128);  // 0..255, exact
    // Build y by placing w_hi and w_lo_biased into disjoint bit ranges of a
    // zeroed container instead of computing "w_hi*65536 + w_lo_biased" as an
    // arithmetic add - the two fields never overlap, so this is pure wiring
    // (no adder), unlike the '+' version which cost a full 32-bit add per
    // call. y.range(24,16) both places w_hi's 8 bits AND sign-extends them
    // into the container's own MSB (bit 24) in one range-assign, since the
    // source is widened to 9 bits (sign, then the 8 original bits) first.
    ap_int<25> y = 0;
    y.range(24, 16) = (ap_int<9>)w_hi;
    y.range(7, 0)    = w_lo_biased;

    // IMPORTANT: multiply the operands at their NATURAL widths (8b x 25b).
    // Pre-widening both sides to a common type before '*' (e.g. casting both
    // to ap_int<43>) makes HLS synthesize a 43x43 multiply - several cascaded
    // DSP48E1s instead of one, and a pile of extra LUTs for the wide
    // add/shift chain around it. a*y here has a natural 8+25=33-bit product
    // width, which is exactly what the DSP48E1's 25x18 multiplier covers.
    ap_int<33> p;
#pragma HLS BIND_OP variable=p op=mul impl=dsp
    p = a * y;

    ap_int<16> lo16 = p.range(15, 0);                          // exact a*w_lo_biased
    ap_int<33> hi33 = (p - (ap_int<33>)lo16) >> 16;             // exact a*w_hi

    prod_lo = (accum_t)lo16 - (accum_t)a * 128;                // remove bias -> a*w_lo
    prod_hi = (accum_t)hi33;
}

static inline accum_t reduce27(const accum_t p[27]) {
#pragma HLS INLINE
    // Balanced 27-term reduction tree.  Keeping the products independent and
    // reducing them as a tree avoids a 27-deep acc += recurrence.
    accum_t s1[14];
    accum_t s2[7];
    accum_t s3[4];
    accum_t s4[2];
#pragma HLS ARRAY_PARTITION variable=s1 complete dim=1
#pragma HLS ARRAY_PARTITION variable=s2 complete dim=1
#pragma HLS ARRAY_PARTITION variable=s3 complete dim=1
#pragma HLS ARRAY_PARTITION variable=s4 complete dim=1

    for (unsigned i = 0; i < 13; i++) {
#pragma HLS UNROLL
        s1[i] = p[2*i] + p[2*i + 1];
    }
    s1[13] = p[26];

    for (unsigned i = 0; i < 7; i++) {
#pragma HLS UNROLL
        s2[i] = s1[2*i] + s1[2*i + 1];
    }

    for (unsigned i = 0; i < 3; i++) {
#pragma HLS UNROLL
        s3[i] = s2[2*i] + s2[2*i + 1];
    }
    s3[3] = s2[6];

    s4[0] = s3[0] + s3[1];
    s4[1] = s3[2] + s3[3];
    return s4[0] + s4[1];
}

// Plain (unpacked) 27-tap dot product - one DSP per tap. conv0 uses this
// directly (reverted to the original timingfix implementation; see header
// note on why conv0/conv2 stayed unpacked while conv1 did not).
static inline accum_t dot27(
    const act_t px[K][K][C0_IN_CH],
    const weight_t w[K][K][C0_IN_CH]
) {
#pragma HLS INLINE
#pragma HLS ARRAY_PARTITION variable=px complete dim=0
#pragma HLS ARRAY_PARTITION variable=w complete dim=0

    accum_t p[27];
#pragma HLS ARRAY_PARTITION variable=p complete dim=1

    for (unsigned ky = 0; ky < K; ky++) {
#pragma HLS UNROLL
        for (unsigned kx = 0; kx < K; kx++) {
#pragma HLS UNROLL
            for (unsigned ic = 0; ic < C0_IN_CH; ic++) {
#pragma HLS UNROLL
                const unsigned idx = (ky * K + kx) * C0_IN_CH + ic;
                p[idx] = (accum_t)px[ky][kx][ic] * (accum_t)w[ky][kx][ic];
            }
        }
    }
    return reduce27(p);
}

struct pair_sum_t { accum_t lo; accum_t hi; };

// Same 27-tap window as dot27(), but computes TWO output channels' sums at
// once (one DSP48E1 per tap instead of two) via mac_pair. Currently unused
// (conv0 was reverted to plain dot27() - see header note) but kept in case
// conv0 gets revisited once LUT budget allows.
static inline pair_sum_t dot27_pair(
    const act_t px[K][K][C0_IN_CH],
    const weight_t w_lo[K][K][C0_IN_CH],
    const weight_t w_hi[K][K][C0_IN_CH]
) {
#pragma HLS INLINE
#pragma HLS ARRAY_PARTITION variable=px complete dim=0
#pragma HLS ARRAY_PARTITION variable=w_lo complete dim=0
#pragma HLS ARRAY_PARTITION variable=w_hi complete dim=0

    accum_t p_lo[27];
    accum_t p_hi[27];
#pragma HLS ARRAY_PARTITION variable=p_lo complete dim=1
#pragma HLS ARRAY_PARTITION variable=p_hi complete dim=1

DOT27P_KY:
    for (unsigned ky = 0; ky < K; ky++) {
#pragma HLS UNROLL
    DOT27P_KX:
        for (unsigned kx = 0; kx < K; kx++) {
#pragma HLS UNROLL
        DOT27P_IC:
            for (unsigned ic = 0; ic < C0_IN_CH; ic++) {
#pragma HLS UNROLL
                const unsigned idx = (ky * K + kx) * C0_IN_CH + ic;
                mac_pair(px[ky][kx][ic], w_lo[ky][kx][ic], w_hi[ky][kx][ic],
                         p_lo[idx], p_hi[idx]);
            }
        }
    }

    pair_sum_t result;
    result.lo = reduce27(p_lo);
    result.hi = reduce27(p_hi);
    return result;
}

// -----------------------------------------------------------------------------
// Layer-specific requantizers for the 100 MHz implementation.
//
// The previous 96x96 build used one shared 32x32->64 requant multiplier pool
// across the sequential conv stages.  Post-route timing at 100 MHz showed the
// worst setup path entering that shared multiplier from conv1 control logic.
// Giving each layer a deliberately different DSP multiplier latency prevents
// cross-layer sharing and lets HLS insert additional registers in the conv1
// requant path.  Arithmetic semantics are unchanged: multiply -> arithmetic
// right shift -> ReLU -> clamp.
//
// c0: latency 2 (keep the already-fast conv0 path)
// c1: latency 4 (critical-path fix)
// c2: latency 3 (separate resource from c0/c1, extra timing margin)
// -----------------------------------------------------------------------------
static inline act_t requant_relu_c0(accum_t acc, const requant_t rq) {
#pragma HLS INLINE
    ap_int<64> scaled;
#pragma HLS BIND_OP variable=scaled op=mul impl=dsp latency=2
    scaled = (ap_int<64>)acc * (ap_int<64>)rq.multiplier;
    ap_int<32> requantized = scaled >> rq.shift;
    if (requantized < 0)   requantized = 0;
    if (requantized > 127) requantized = 127;
    return (act_t)requantized;
}

static inline act_t requant_relu_c1(accum_t acc, const requant_t rq) {
#pragma HLS INLINE
    ap_int<64> scaled;
#pragma HLS BIND_OP variable=scaled op=mul impl=dsp latency=4
    scaled = (ap_int<64>)acc * (ap_int<64>)rq.multiplier;
    ap_int<32> requantized = scaled >> rq.shift;
    if (requantized < 0)   requantized = 0;
    if (requantized > 127) requantized = 127;
    return (act_t)requantized;
}

static inline act_t requant_relu_c2(accum_t acc, const requant_t rq) {
#pragma HLS INLINE
    ap_int<64> scaled;
#pragma HLS BIND_OP variable=scaled op=mul impl=dsp latency=3
    scaled = (ap_int<64>)acc * (ap_int<64>)rq.multiplier;
    ap_int<32> requantized = scaled >> rq.shift;
    if (requantized < 0)   requantized = 0;
    if (requantized > 127) requantized = 127;
    return (act_t)requantized;
}

// -----------------------------------------------------------------------------
// conv0_engine (v7 full-kernel schedule)
//   input : PRE-PADDED 98x98x3 NHWC INT8
//   output: 96x96x16 NHWC INT8
//   weight: OIHW [16][3][3][3]
//
// The full 3x3x3 input window is visible in one OC-block iteration.  To supply
// 27 activation reads in parallel, local input memory is banked by
//   Y mod 3 x X mod 3 x channel = 27 logical banks.
// Two output channels are computed together, exposing 54 INT8 MACs.
//
// The outer OC-block loop is pipelined at II=1.  Unlike v6, there is no inner
// three-trip KY pipeline to fill/drain for every output-channel block.
// -----------------------------------------------------------------------------
static void conv0_pool0_fused(
    const act_t     ifmap[C0_PAD_SIZE][C0_PAD_SIZE][C0_IN_CH],
    const weight_t  w[C0_OUT_CH][C0_IN_CH][K][K],
    const bias_t    b[C0_OUT_CH],
    const requant_t rq,
    act_t           pooled_out[P0_OUT_SIZE][P0_OUT_SIZE][C0_OUT_CH]
) {
    static act_t ifmap_local[C0_PAD_SIZE][C0_PAD_SIZE][C0_IN_CH];
#pragma HLS ARRAY_PARTITION variable=ifmap_local cyclic factor=3 dim=1
#pragma HLS ARRAY_PARTITION variable=ifmap_local cyclic factor=3 dim=2
#pragma HLS ARRAY_PARTITION variable=ifmap_local complete dim=3

    static weight_t w_local[C0_OUT_CH][C0_IN_CH][K][K];
#pragma HLS ARRAY_PARTITION variable=w_local cyclic factor=C0_OC_PAR dim=1
#pragma HLS ARRAY_PARTITION variable=w_local complete dim=2
#pragma HLS ARRAY_PARTITION variable=w_local complete dim=3
#pragma HLS ARRAY_PARTITION variable=w_local complete dim=4

C0_ILOAD_Y:
    for (unsigned y = 0; y < C0_PAD_SIZE; y++) {
    C0_ILOAD_X:
        for (unsigned x = 0; x < C0_PAD_SIZE; x++) {
        C0_ILOAD_C:
            for (unsigned c = 0; c < C0_IN_CH; c++) {
#pragma HLS PIPELINE II=1
                ifmap_local[y][x][c] = ifmap[y][x][c];
            }
        }
    }

C0_WLOAD_OC:
    for (unsigned oc = 0; oc < C0_OUT_CH; oc++) {
    C0_WLOAD_IC:
        for (unsigned ic = 0; ic < C0_IN_CH; ic++) {
        C0_WLOAD_KY:
            for (unsigned ky = 0; ky < K; ky++) {
            C0_WLOAD_KX:
                for (unsigned kx = 0; kx < K; kx++) {
#pragma HLS PIPELINE II=1
                    w_local[oc][ic][ky][kx] = w[oc][ic][ky][kx];
                }
            }
        }
    }

    // STEP-96 FIX (2026-08-18): row_buf replaces the old full-frame `fmap0`
    // (96x96x16 = 144KB, the single biggest BRAM consumer once resolution
    // went from 64x64 to 96x96 - BRAM_18K went 127% over budget). conv0's
    // own OY loop already computes one full output row at a time, so this
    // just keeps the 2 most recent rows on-chip (2*96*16 = 3KB) instead of
    // buffering all 96 rows, and runs pool0's 2x2 max as soon as a row PAIR
    // is complete. Purely sequential (no DATAFLOW/concurrency pragma) - only
    // the SIZE of the buffer changes, not the execution order, so this
    // carries none of the overlap-related risk from earlier attempts.
    static act_t row_buf[2][C0_OUT_SIZE][C0_OUT_CH];
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=3

C0_OY:
    for (unsigned oy = 0; oy < C0_OUT_SIZE; oy++) {
        unsigned buf_row = oy % 2;
    C0_OX:
        for (unsigned ox = 0; ox < C0_OUT_SIZE; ox++) {
        C0_OC_BLOCK:
            for (unsigned oc_base = 0; oc_base < C0_OUT_CH; oc_base += C0_OC_PAR) {
#pragma HLS PIPELINE II=1
                act_t px[K][K][C0_IN_CH];
#pragma HLS ARRAY_PARTITION variable=px complete dim=0

            C0_LOAD_KY:
                for (unsigned ky = 0; ky < K; ky++) {
#pragma HLS UNROLL
                C0_LOAD_KX:
                    for (unsigned kx = 0; kx < K; kx++) {
#pragma HLS UNROLL
                    C0_LOAD_IC:
                        for (unsigned ic = 0; ic < C0_IN_CH; ic++) {
#pragma HLS UNROLL
                            px[ky][kx][ic] =
                                ifmap_local[oy + ky][ox + kx][ic];
                        }
                    }
                }

            C0_OC_LANE:
                for (unsigned lane = 0; lane < C0_OC_PAR; lane++) {
#pragma HLS UNROLL
                    weight_t wr[K][K][C0_IN_CH];
#pragma HLS ARRAY_PARTITION variable=wr complete dim=0

                C0_WR_KY:
                    for (unsigned ky = 0; ky < K; ky++) {
#pragma HLS UNROLL
                    C0_WR_KX:
                        for (unsigned kx = 0; kx < K; kx++) {
#pragma HLS UNROLL
                        C0_WR_IC:
                            for (unsigned ic = 0; ic < C0_IN_CH; ic++) {
#pragma HLS UNROLL
                                wr[ky][kx][ic] =
                                    w_local[oc_base + lane][ic][ky][kx];
                            }
                        }
                    }

                    accum_t acc = b[oc_base + lane] + dot27(px, wr);
                    row_buf[buf_row][ox][oc_base + lane] =
                        requant_relu_c0(acc, rq);
                }
            }
        }

        // Row pair (oy-1, oy) complete - pool it into one output row now.
        // Row order within the pair doesn't matter for max-pooling.
        if (oy % 2 == 1) {
            unsigned out_row = oy / 2;
        C0_POOL_OX:
            for (unsigned out_col = 0; out_col < P0_OUT_SIZE; out_col++) {
            C0_POOL_C:
                for (unsigned c = 0; c < C0_OUT_CH; c++) {
#pragma HLS PIPELINE II=1
                    act_t v00 = row_buf[0][out_col * 2][c];
                    act_t v01 = row_buf[0][out_col * 2 + 1][c];
                    act_t v10 = row_buf[1][out_col * 2][c];
                    act_t v11 = row_buf[1][out_col * 2 + 1][c];
                    act_t m = v00;
                    if (v01 > m) m = v01;
                    if (v10 > m) m = v10;
                    if (v11 > m) m = v11;
                    pooled_out[out_row][out_col][c] = m;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// conv_same_parallel
//   SAME pad=1, stride=1, k=3
//   weights WPACK [oc][ky][kx][ic]
//
// IN_PAR=8 in both layers. OC_PAR is 4 for conv1 and 8 for conv2.
// Pixel channels are loaded ONCE into 8 registers and fanned out to all output
// channel lanes; this avoids asking the feature-map RAM for OC_PAR duplicate
// copies of the same channel data.
// -----------------------------------------------------------------------------
template<unsigned IN_CH, unsigned OUT_CH, unsigned SIZE, unsigned OC_PAR>
void conv_same_parallel(
    const act_t     ifmap[SIZE][SIZE][IN_CH],
    const weight_t  w[OUT_CH][K][K][IN_CH],
    const bias_t    b[OUT_CH],
    const requant_t rq,
    act_t           ofmap[SIZE][SIZE][OUT_CH]
) {
#pragma HLS ARRAY_PARTITION variable=ifmap cyclic factor=CS_IC_PAR dim=3
#pragma HLS ARRAY_PARTITION variable=ofmap cyclic factor=8 dim=3

    static weight_t w_local[OUT_CH][K][K][IN_CH];
#pragma HLS ARRAY_PARTITION variable=w_local cyclic factor=8 dim=1
#pragma HLS ARRAY_PARTITION variable=w_local cyclic factor=CS_IC_PAR dim=4

CS_WLOAD_OC:
    for (unsigned oc = 0; oc < OUT_CH; oc++) {
    CS_WLOAD_KY:
        for (unsigned ky = 0; ky < K; ky++) {
        CS_WLOAD_KX:
            for (unsigned kx = 0; kx < K; kx++) {
            CS_WLOAD_IC:
                for (unsigned ic = 0; ic < IN_CH; ic++) {
#pragma HLS PIPELINE II=1
                    w_local[oc][ky][kx][ic] = w[oc][ky][kx][ic];
                }
            }
        }
    }

CS_OY:
    for (unsigned oy = 0; oy < SIZE; oy++) {
    CS_OX:
        for (unsigned ox = 0; ox < SIZE; ox++) {
        CS_OC_BLOCK:
            for (unsigned oc_base = 0; oc_base < OUT_CH; oc_base += OC_PAR) {
                accum_t partial[OC_PAR][CS_IC_PAR];
#pragma HLS ARRAY_PARTITION variable=partial complete dim=0

            CS_INIT_OC:
                for (unsigned o = 0; o < OC_PAR; o++) {
#pragma HLS UNROLL
                CS_INIT_IC:
                    for (unsigned i = 0; i < CS_IC_PAR; i++) {
#pragma HLS UNROLL
                        partial[o][i] = 0;
                    }
                }

            CS_KY:
                for (unsigned ky = 0; ky < K; ky++) {
                CS_KX:
                    for (unsigned kx = 0; kx < K; kx++) {
                        int iy = (int)oy - 1 + (int)ky;
                        int ix = (int)ox - 1 + (int)kx;
                        bool in_bounds = (iy >= 0) && (iy < (int)SIZE) &&
                                         (ix >= 0) && (ix < (int)SIZE);

                    CS_IC_BLOCK:
                        for (unsigned ic_base = 0; ic_base < IN_CH; ic_base += CS_IC_PAR) {
#pragma HLS PIPELINE II=1
                            act_t px[CS_IC_PAR];
#pragma HLS ARRAY_PARTITION variable=px complete dim=1

                        CS_LOAD_IC:
                            for (unsigned i = 0; i < CS_IC_PAR; i++) {
#pragma HLS UNROLL
                                unsigned ic = ic_base + i;
                                px[i] = in_bounds ? ifmap[iy][ix][ic] : (act_t)0;
                            }

                        CS_OC_LANE:
                            for (unsigned o = 0; o < OC_PAR; o++) {
#pragma HLS UNROLL
                            CS_IC_LANE:
                                for (unsigned i = 0; i < CS_IC_PAR; i++) {
#pragma HLS UNROLL
                                    unsigned ic = ic_base + i;
                                    partial[o][i] +=
                                        (accum_t)px[i] *
                                        (accum_t)w_local[oc_base + o][ky][kx][ic];
                                }
                            }
                        }
                    }
                }

            CS_WRITE_OC:
                for (unsigned o = 0; o < OC_PAR; o++) {
#pragma HLS UNROLL
                    accum_t acc = b[oc_base + o] + reduce8(partial[o]);
                    ofmap[oy][ox][oc_base + o] = requant_relu_c2(acc, rq);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// conv1_pool1_fused: same computation as conv_same_parallel<C1_IN_CH,
// C1_OUT_CH, C1_SIZE, C1_OC_PAR>, but streams its output into pool1 via a
// 2-row buffer instead of the old full-frame `fmap1` (48x48x32 = 72KB, the
// single biggest remaining buffer after the conv0/pool0 fix). Only conv1 is
// fused here - conv2's own output (fmap2, 36KB) is smaller and conv2 isn't
// followed by another conv, so the marginal BRAM win there is much smaller;
// conv_same_parallel itself is left untouched and still serves conv2.
// -----------------------------------------------------------------------------
static void conv1_pool1_fused(
    const act_t     ifmap[C1_SIZE][C1_SIZE][C1_IN_CH],
    const weight_t  w[C1_OUT_CH][K][K][C1_IN_CH],
    const bias_t    b[C1_OUT_CH],
    const requant_t rq,
    act_t           pooled_out[P1_OUT_SIZE][P1_OUT_SIZE][C1_OUT_CH]
) {
#pragma HLS ARRAY_PARTITION variable=ifmap cyclic factor=CS_IC_PAR dim=3

    static weight_t w_local[C1_OUT_CH][K][K][C1_IN_CH];
#pragma HLS ARRAY_PARTITION variable=w_local cyclic factor=C1_OC_PAR dim=1
#pragma HLS ARRAY_PARTITION variable=w_local cyclic factor=CS_IC_PAR dim=4

CS1_WLOAD_OC:
    for (unsigned oc = 0; oc < C1_OUT_CH; oc++) {
    CS1_WLOAD_KY:
        for (unsigned ky = 0; ky < K; ky++) {
        CS1_WLOAD_KX:
            for (unsigned kx = 0; kx < K; kx++) {
            CS1_WLOAD_IC:
                for (unsigned ic = 0; ic < C1_IN_CH; ic++) {
#pragma HLS PIPELINE II=1
                    w_local[oc][ky][kx][ic] = w[oc][ky][kx][ic];
                }
            }
        }
    }

    // Same trick as conv0_pool0_fused: 2 rows on-chip (2*48*32=3KB) instead
    // of the full 48x48x32=72KB frame.
    static act_t row_buf[2][C1_SIZE][C1_OUT_CH];
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
#pragma HLS ARRAY_PARTITION variable=row_buf cyclic factor=C1_OC_PAR dim=3

CS1_OY:
    for (unsigned oy = 0; oy < C1_SIZE; oy++) {
        unsigned buf_row = oy % 2;
    CS1_OX:
        for (unsigned ox = 0; ox < C1_SIZE; ox++) {
        CS1_OC_BLOCK:
            for (unsigned oc_base = 0; oc_base < C1_OUT_CH; oc_base += C1_OC_PAR) {
                accum_t partial[C1_OC_PAR][CS_IC_PAR];
#pragma HLS ARRAY_PARTITION variable=partial complete dim=0

            CS1_INIT_OC:
                for (unsigned o = 0; o < C1_OC_PAR; o++) {
#pragma HLS UNROLL
                CS1_INIT_IC:
                    for (unsigned i = 0; i < CS_IC_PAR; i++) {
#pragma HLS UNROLL
                        partial[o][i] = 0;
                    }
                }

            CS1_KY:
                for (unsigned ky = 0; ky < K; ky++) {
                CS1_KX:
                    for (unsigned kx = 0; kx < K; kx++) {
                        int iy = (int)oy - 1 + (int)ky;
                        int ix = (int)ox - 1 + (int)kx;
                        bool in_bounds = (iy >= 0) && (iy < (int)C1_SIZE) &&
                                         (ix >= 0) && (ix < (int)C1_SIZE);

                    CS1_IC_BLOCK:
                        for (unsigned ic_base = 0; ic_base < C1_IN_CH; ic_base += CS_IC_PAR) {
#pragma HLS PIPELINE II=1
                            act_t px[CS_IC_PAR];
#pragma HLS ARRAY_PARTITION variable=px complete dim=1

                        CS1_LOAD_IC:
                            for (unsigned i = 0; i < CS_IC_PAR; i++) {
#pragma HLS UNROLL
                                unsigned ic = ic_base + i;
                                px[i] = in_bounds ? ifmap[iy][ix][ic] : (act_t)0;
                            }

                        CS1_OC_PAIR:
                            for (unsigned op = 0; op < C1_OC_PAR / 2; op++) {
#pragma HLS UNROLL
                                const unsigned o_lo = op * 2;
                                const unsigned o_hi = op * 2 + 1;
                            CS1_IC_LANE:
                                for (unsigned i = 0; i < CS_IC_PAR; i++) {
#pragma HLS UNROLL
                                    unsigned ic = ic_base + i;
                                    accum_t prod_lo, prod_hi;
                                    mac_pair(px[i],
                                             w_local[oc_base + o_lo][ky][kx][ic],
                                             w_local[oc_base + o_hi][ky][kx][ic],
                                             prod_lo, prod_hi);
                                    partial[o_lo][i] += prod_lo;
                                    partial[o_hi][i] += prod_hi;
                                }
                            }
                        }
                    }
                }

            CS1_WRITE_OC:
                for (unsigned o = 0; o < C1_OC_PAR; o++) {
#pragma HLS UNROLL
                    accum_t acc = b[oc_base + o] + reduce8(partial[o]);
                    row_buf[buf_row][ox][oc_base + o] = requant_relu_c1(acc, rq);
                }
            }
        }

        if (oy % 2 == 1) {
            unsigned out_row = oy / 2;
        CS1_POOL_OX:
            for (unsigned out_col = 0; out_col < P1_OUT_SIZE; out_col++) {
            CS1_POOL_C:
                for (unsigned c = 0; c < C1_OUT_CH; c++) {
#pragma HLS PIPELINE II=1
                    act_t v00 = row_buf[0][out_col * 2][c];
                    act_t v01 = row_buf[0][out_col * 2 + 1][c];
                    act_t v10 = row_buf[1][out_col * 2][c];
                    act_t v11 = row_buf[1][out_col * 2 + 1][c];
                    act_t m = v00;
                    if (v01 > m) m = v01;
                    if (v10 > m) m = v10;
                    if (v11 > m) m = v11;
                    pooled_out[out_row][out_col][c] = m;
                }
            }
        }
    }
}

// Conv2/pool2 fusion removes the last full-frame feature map
// (24*24*64 = 36 KiB).  Two rows are sufficient for 2x2/stride-2 max pool.
static void conv2_pool2_fused(
    const act_t ifmap[C2_SIZE][C2_SIZE][C2_IN_CH],
    const weight_t w[C2_OUT_CH][K][K][C2_IN_CH],
    const bias_t b[C2_OUT_CH], const requant_t rq,
    act_t out[P2_OUT_SIZE][P2_OUT_SIZE][C2_OUT_CH]) {
#pragma HLS ARRAY_PARTITION variable=ifmap cyclic factor=CS_IC_PAR dim=3
    static weight_t w_local[C2_OUT_CH][K][K][C2_IN_CH];
#pragma HLS ARRAY_PARTITION variable=w_local cyclic factor=C2_OC_PAR dim=1
#pragma HLS ARRAY_PARTITION variable=w_local cyclic factor=CS_IC_PAR dim=4
C2_WLOAD_OC:
    for (unsigned oc=0; oc<C2_OUT_CH; ++oc)
    for (unsigned ky=0; ky<K; ++ky)
    for (unsigned kx=0; kx<K; ++kx)
    for (unsigned ic=0; ic<C2_IN_CH; ++ic) {
#pragma HLS PIPELINE II=1
        w_local[oc][ky][kx][ic] = w[oc][ky][kx][ic];
    }
    static act_t row_buf[2][C2_SIZE][C2_OUT_CH];
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
#pragma HLS ARRAY_PARTITION variable=row_buf cyclic factor=C2_OC_PAR dim=3
C2_OY:
    for (unsigned oy=0; oy<C2_SIZE; ++oy) {
        const unsigned br = oy & 1;
    C2_OX:
        for (unsigned ox=0; ox<C2_SIZE; ++ox) {
        C2_OC_BLOCK:
            for (unsigned ob=0; ob<C2_OUT_CH; ob+=C2_OC_PAR) {
                accum_t partial[C2_OC_PAR][CS_IC_PAR];
#pragma HLS ARRAY_PARTITION variable=partial complete dim=0
                for (unsigned o=0; o<C2_OC_PAR; ++o) {
#pragma HLS UNROLL
                    for (unsigned i=0; i<CS_IC_PAR; ++i) {
#pragma HLS UNROLL
                        partial[o][i]=0;
                    }
                }
                for (unsigned ky=0; ky<K; ++ky) {
                    for (unsigned kx=0; kx<K; ++kx) {
                        const int iy=(int)oy-1+(int)ky;
                        const int ix=(int)ox-1+(int)kx;
                        const bool valid=iy>=0 && iy<(int)C2_SIZE && ix>=0 && ix<(int)C2_SIZE;
                    C2_IC_BLOCK:
                        for (unsigned ib=0; ib<C2_IN_CH; ib+=CS_IC_PAR) {
#pragma HLS PIPELINE II=1
                            act_t px[CS_IC_PAR];
#pragma HLS ARRAY_PARTITION variable=px complete dim=1
                            for (unsigned i=0; i<CS_IC_PAR; ++i) {
#pragma HLS UNROLL
                                px[i]=valid ? ifmap[iy][ix][ib+i] : (act_t)0;
                            }
                            for (unsigned op=0; op<C2_OC_PAR/2; ++op) {
#pragma HLS UNROLL
                                const unsigned o_lo = op * 2;
                                const unsigned o_hi = op * 2 + 1;
                                for (unsigned i=0; i<CS_IC_PAR; ++i) {
#pragma HLS UNROLL
                                    accum_t prod_lo, prod_hi;
                                    mac_pair(px[i],
                                             w_local[ob+o_lo][ky][kx][ib+i],
                                             w_local[ob+o_hi][ky][kx][ib+i],
                                             prod_lo, prod_hi);
                                    partial[o_lo][i] += prod_lo;
                                    partial[o_hi][i] += prod_hi;
                                }
                            }
                        }
                    }
                }
                for (unsigned o=0; o<C2_OC_PAR; ++o) {
#pragma HLS UNROLL
                    row_buf[br][ox][ob+o] = requant_relu_c2(
                        b[ob+o] + reduce8(partial[o]), rq);
                }
            }
        }
        if (br == 1) {
            const unsigned py=oy/2;
            for (unsigned px=0; px<P2_OUT_SIZE; ++px) {
                for (unsigned c=0; c<C2_OUT_CH; ++c) {
#pragma HLS PIPELINE II=1
                    act_t m=row_buf[0][2*px][c];
                    act_t v=row_buf[0][2*px+1][c]; if (v>m) m=v;
                    v=row_buf[1][2*px][c]; if (v>m) m=v;
                    v=row_buf[1][2*px+1][c]; if (v>m) m=v;
                    out[py][px][c]=m;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Top-level PL function. External contract unchanged from v4.
// -----------------------------------------------------------------------------
void classifier_top(
    const act_t     ifmap_padded[C0_PAD_SIZE][C0_PAD_SIZE][C0_IN_CH],

    const weight_t  w_conv0[C0_OUT_CH][C0_IN_CH][K][K],
    const bias_t    b_conv0[C0_OUT_CH],
    const requant_t rq_conv0,

    const weight_t  w_conv1[C1_OUT_CH][K][K][C1_IN_CH],
    const bias_t    b_conv1[C1_OUT_CH],
    const requant_t rq_conv1,

    const weight_t  w_conv2[C2_OUT_CH][K][K][C2_IN_CH],
    const bias_t    b_conv2[C2_OUT_CH],
    const requant_t rq_conv2,

    act_t out[P2_OUT_SIZE][P2_OUT_SIZE][C2_OUT_CH]
) {
#pragma HLS INTERFACE m_axi port=ifmap_padded offset=slave bundle=RD_BUS
#pragma HLS INTERFACE m_axi port=w_conv0 offset=slave bundle=RD_BUS2
#pragma HLS INTERFACE m_axi port=w_conv1 offset=slave bundle=RD_BUS2
#pragma HLS INTERFACE m_axi port=w_conv2 offset=slave bundle=RD_BUS2
#pragma HLS INTERFACE m_axi port=out     offset=slave bundle=WR_BUS
#pragma HLS INTERFACE s_axilite port=b_conv0  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=b_conv1  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=b_conv2  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=rq_conv0 bundle=CTRL
#pragma HLS INTERFACE s_axilite port=rq_conv1 bundle=CTRL
#pragma HLS INTERFACE s_axilite port=rq_conv2 bundle=CTRL
#pragma HLS INTERFACE s_axilite port=return   bundle=CTRL

    // fmap0 (the old 96x96x16=144KB full-frame conv0 output buffer) is gone -
    // conv0_pool0_fused streams conv0's output straight into pool0 using a
    // 2-row on-chip buffer instead. fmap1 (the old 48x48x32=72KB full-frame
    // conv1 output buffer) is gone the same way - conv1_pool1_fused streams
    // straight into pool1.
    static act_t fmap0p[P0_OUT_SIZE][P0_OUT_SIZE][C0_OUT_CH];
    static act_t fmap1p[P1_OUT_SIZE][P1_OUT_SIZE][C1_OUT_CH];

#pragma HLS ARRAY_PARTITION variable=fmap0p cyclic factor=4 dim=3
#pragma HLS ARRAY_PARTITION variable=fmap1p cyclic factor=4 dim=3

    conv0_pool0_fused(ifmap_padded, w_conv0, b_conv0, rq_conv0, fmap0p);

    conv1_pool1_fused(fmap0p, w_conv1, b_conv1, rq_conv1, fmap1p);

    conv2_pool2_fused(fmap1p, w_conv2, b_conv2, rq_conv2, out);
}
