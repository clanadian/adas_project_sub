#ifndef CLASSIFIER_ENGINE_H
#define CLASSIFIER_ENGINE_H

#include <ap_int.h>
#include <hls_stream.h>
#include <cstdint>

// ---------------------------------------------------------------------
// PL contract (per PS teammate's reply, 2026-08-18):
//
//   ifmap NHWC, INT8 signed, zero-point=0 fixed (symmetric only - no
//   zero-point subtraction path exists anywhere in this engine).
//
//   conv0_engine: DEDICATED engine, no pad port. Caller (PS/preprocessing)
//   must deliver a PRE-PADDED 98x98x3 buffer (1px zero border around the
//   96x96 ROI) - conv0 does NOT pad internally. Weight layout OIHW, NOT
//   transposed: w[oc][ic][ky][kx].
//
//   conv1/conv2: same HLS source template, but because their template
//   dimensions differ Vitis HLS synthesizes two hardware instances. SAME
//   padding is internal (pad=1, stride=1) -
//   unlike conv0, these DO pad internally since their input is a live
//   feature map, not a pre-staged DDR buffer. Weight layout WPACK
//   (transposed): w[oc][ky][kx][ic] - same convention as the KR260
//   conv_engine project's wpack layout (`w_oihw.transpose(0,2,3,1)`).
//
//   Pipeline:
//     conv0(98x98x3 prepad -> 96x96x16) -> pool0(96->48)
//     conv1(48x48x16 -> 48x48x32, SAME)  -> pool1(48->24)
//     conv2(24x24x32 -> 24x24x64, SAME)  -> pool2(24->12)
//     => PL OUTPUT: 12x12x64 INT8 NHWC. This is NOT logits.
//
//   GAP -> FC -> argmax are explicitly PS software, NOT part of this PL
//   design (~4,400 MAC total - not worth an engine).
//
//   Parallel implementation (96x96 timing-focused build):
//   conv0 banks the preloaded input by Y mod 3, X mod 3 and channel, then
//   computes the complete 3x3x3 dot product for 2 output channels in each
//   pipelined OC-block iteration (54 exposed INT8 MACs). A balanced reduction
//   tree removes the 27-term accumulator recurrence. conv1 and conv2 use
//   8 OC x 8 IC = 64 MAC-lane schedules. Requant multipliers are timing-tuned
//   per layer for the 100 MHz Vivado target. External tensor formats and
//   software contracts are unchanged. PS reference:
//   classifier_run.c's classifier_gap()/classifier_fc()/classifier_argmax().
// ---------------------------------------------------------------------

typedef ap_int<8>  act_t;
typedef ap_int<8>  weight_t;
typedef ap_int<32> bias_t;
typedef ap_int<32> accum_t;

const unsigned K = 3;

// conv0 (dedicated engine)
const unsigned C0_PAD_SIZE = 98;   // pre-padded input, 98x98x3 (96x96 ROI + 1px border)
const unsigned C0_IN_CH    = 3;
const unsigned C0_OUT_CH   = 16;
const unsigned C0_OUT_SIZE = 96;   // 98-3+1 = 96, no internal pad

// pool0: 96 -> 48
const unsigned P0_OUT_SIZE = 48;

// conv1 (shared engine instance 1): SAME padding, size unchanged
const unsigned C1_IN_CH  = 16;
const unsigned C1_OUT_CH = 32;
const unsigned C1_SIZE   = 48;

// pool1: 48 -> 24
const unsigned P1_OUT_SIZE = 24;

// conv2 (shared engine instance 2): SAME padding, size unchanged
const unsigned C2_IN_CH  = 32;
const unsigned C2_OUT_CH = 64;
const unsigned C2_SIZE   = 24;
// NOTE on OC_PAR (classifier_engine.cpp, DSP-pack perf rework): pairing OC
// lanes through one shared DSP48E1 multiply (mac_pair) and doubling OC_PAR
// is DSP-NEUTRAL for conv1/conv2 (measured: unpacked C*_OC_PAR=8 and
// packed+doubled C*_OC_PAR=16 both land around the same ~76 DSP for that
// module - packing exactly buys back the DSP that doubling the lane count
// would otherwise cost), so both are kept doubled+packed (C1_OC_PAR=16,
// C2_OC_PAR=16). conv0 is the exception: its unpacked dot27 implementation
// already lets HLS resource-share down to just 34 DSP for C0_OC_PAR=2
// (fewer than the 54 raw multiplies you'd naively expect), and packing it
// disrupts that sharing rather than helping (measured 70 DSP, worse) - so
// conv0 stays unpacked at the original C0_OC_PAR=2. First attempt (all
// three packed+doubled) blew LUT to 152% of budget at the HLS csynth
// estimate stage, but the conv1-only build's ACTUAL ROUTED LUT (24,854)
// came in at less than half of its own HLS estimate (51,448) - so the
// original LUT panic was based on an estimate that doesn't hold at the
// routed stage; DSP is the resource that actually grows during Vivado
// system integration, not LUT. See classifier_engine.cpp for mac_pair.

// pool2: 24 -> 12  (final PL output size)
const unsigned P2_OUT_SIZE = 12;

// ---- Requantization (INT32 accum -> INT8, per-layer manifest values) ----
struct requant_t {
    int32_t   multiplier;
    ap_uint<8> shift;
};

// ---- Top-level PL function ----
// Output is the 12x12x64 INT8 NHWC feature map - NOT logits. GAP/FC/argmax
// happen in PS software, outside this design.
void classifier_top(
    const act_t     ifmap_padded[C0_PAD_SIZE][C0_PAD_SIZE][C0_IN_CH],  // pre-padded 98x98x3

    const weight_t  w_conv0[C0_OUT_CH][C0_IN_CH][K][K],   // OIHW, no transpose
    const bias_t    b_conv0[C0_OUT_CH],
    const requant_t rq_conv0,

    const weight_t  w_conv1[C1_OUT_CH][K][K][C1_IN_CH],   // WPACK: [oc][ky][kx][ic]
    const bias_t    b_conv1[C1_OUT_CH],
    const requant_t rq_conv1,

    const weight_t  w_conv2[C2_OUT_CH][K][K][C2_IN_CH],   // WPACK: [oc][ky][kx][ic]
    const bias_t    b_conv2[C2_OUT_CH],
    const requant_t rq_conv2,

    act_t out[P2_OUT_SIZE][P2_OUT_SIZE][C2_OUT_CH]         // 12x12x64 INT8 NHWC
);

#endif
