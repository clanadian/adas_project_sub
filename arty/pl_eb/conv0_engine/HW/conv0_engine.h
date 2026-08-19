#ifndef CONV0_ENGINE_H
#define CONV0_ENGINE_H

#include <ap_int.h>
#include <stdint.h>
#include "pack4.h"

// ---------------------------------------------------------------------------
// conv0_engine - dedicated engine for YOLOv3-tiny-ADAS manifest layer 0 only.
//
// WHY THIS EXISTS (see doc/02_plans/layer0-dedicated-engine.md for the full case)
// --------------------------------------------------------------------------
// Layer 0 is the ONLY layer in this network with in_ch < TR and out_ch <
// PE_OC, so the general conv_engine's parallelism is structurally wasted on
// it: 3/16 reduction lanes (81% idle) x 16/32 output lanes (50% idle) = 9.4%
// of nominal 384 MAC/cycle is its theoretical maximum, before pipeline
// efficiency is even discussed. Meanwhile it has 149,060 scan positions
// (59x layer 3's) for 2.7% of the network's MACs - it spends time on fixed
// per-position overhead, not on arithmetic. The whole-frame cost model
// (doc/06_troubleshooting/conv-engine-main-log.md §29) puts it at 25.5% of the frame
// and, critically, at 1.00x from BOTH queued performance levers (PE_OC=32 and
// OC-hoist), because both scale things layer 0 does not use.
//
// THE DESIGN POINT: THIS ENGINE IS DDR-WRITE-BOUND, NOT COMPUTE-BOUND
// -------------------------------------------------------------------
// Per output position this engine must emit OUT_CH=16 int8 values = 16 bytes.
// On a 32-bit AXI port that is 4 beats, and no amount of arithmetic
// parallelism can go below it. The input side needs only ONE new pixel
// (IN_CH=3 bytes) per position thanks to the row buffer, i.e. 3 beats - under
// the write bound. So:
//
//     target II = 4 cycles/position
//       -> MAC requirement  = 432/4 = 108 MAC/cycle  (NOT 432 unrolled)
//       -> activation units = 16/4  = 4              (NOT 16 spatial copies)
//       -> 147,456 positions x 4 = 589,824 cycles    (vs ~11.78M modelled today)
//
// Sizing to the write bound rather than to "unroll everything" is what keeps
// this cheap. It also structurally avoids this project's known LUT trap: 20
// spatially-replicated activation units once took csynth LUT from 89% to 105%
// (doc/06_troubleshooting/conv-engine-main-log.md); 4 cannot repeat that.
//
// "4 beats = 4 cycles" IS ONLY TRUE INSIDE A BURST
// ------------------------------------------------
// The first cosim of this engine measured 5,030,205 cycles = 34.1 cycles per
// position, 8.5x the model above, and the csynth burst table said why: the
// `ofmap` write NEVER inferred a burst ("Could not analyze pattern", and after
// the §34 restructure "Access store is in the conditional branch"). Reads were
// always fine ("Inferred"). Outside a burst each of those 4 beats is its own
// AW+W+B transaction, so the engine was never write-BANDWIDTH bound - it was
// write-TRANSACTION bound, at 4 transactions per position.
//
// The fix (2026-08-06) was to give every m_axi access its own pipelined,
// unconditional, unit-stride loop - see conv0_engine.cpp's three-phase
// ROW_LOOP (READ_ROW / COL_GROUP / WRITE_ROW). Doing that for the ofmap store
// made it burst at last; doing it for the 3-byte ifmap prefetch (which was
// pinning COL_GROUP at II=3 by issuing 3 requests on one port) took the
// compute phase to II=1 as well.
//
// THE "4 CYCLES/POSITION" TARGET ABOVE IS SUPERSEDED - do not plan against
// it. csynth on the three-phase structure: 11.2 cycles/position, all three
// phases II=1, every access bursting. Nothing is left to schedule better;
// what remains costs either a SW data-contract change (RGBX input padding, to
// make `ifmap` a 4-byte-aligned pack4_t* and collapse 3 byte-beats per column
// into 1) or DATAFLOW overlap (~4.1 cycles/position, and the riskiest change
// available - see the WRITE_ROW comment). Read that comment before planning
// any further work on this engine.
//
// Widening ofmap to 128-bit would reach 147,456 cycles (another 4x), and is
// deliberately NOT done - by then layer 0 is ~2% of the frame, while HP-port
// data-width changes are a repeat-offender failure mode in this project
// (doc/06_troubleshooting/vivado-bringup-log.md §3/§4). Same reasoning §29-d
// used to reject 128-bit for conv_engine.
//
// WHAT IS FIXED AND WHAT IS A REGISTER
// ------------------------------------
// Fixed at compile time (this engine runs exactly one layer, forever):
//   IN_CH=3, OUT_CH=16, K=3, STRIDE=1, and pad=0 (the input is PRE-PADDED by
//   software to 290x514 - see forteammate/03_layer0_contract).
// Runtime registers: img_h/img_w (so a resolution change does not need an RTL
// rebuild) + the 3 quantization scalars + 4 DDR base addresses.
//
// There is deliberately NO `accum` port. conv_engine needs one to carry
// partial sums across ic_tile passes; layer 0 has num_ic_tiles=1, so there is
// nothing to carry. One fewer m_axi port than conv_engine.
//
// BIT-EXACTNESS
// -------------
// round_shift()/saturate()/apply_activation() in conv0_engine.cpp are COPIED
// VERBATIM from conv_engine.cpp - not re-derived. That code is already
// bit-exact against the golden model on all 13 real layers; re-implementing
// the rounding is exactly how this project has produced silent mismatches
// before. If conv_engine's version ever changes, change this one identically.
// ---------------------------------------------------------------------------

// Layer 0's shape, from python_teammate/darknet_golden's model_manifest.json
// (cross-checked against REAL_LAYERS[0] in conv_engine_requant_merged/HW/
// real_layers_data.h: 290x514x3 -> 288x512x16, k=3, pad=0, leaky=1).
const unsigned IN_CH  = 3;
const unsigned OUT_CH = 16;
const unsigned K      = 3;

// Bounds for the on-chip row buffer. MAX_IMG_W is the PRE-PADDED input width,
// not the output width.
//
// ARTY 96 CLASSIFIER FORK (2026-08-18): 514 -> 98. The ROI is 96x96, so the
// pre-padded input is 98x98. img_w is a RUNTIME port, so any ROI <= 96 runs on
// this build. Siblings set this to 66 (hls/arty_classifier) and 130
// (hls/arty_128_classifier).
//
// This bound sizes the row buffers ONLY. It does NOT touch the 108-MAC
// unroll (K*K*IN_CH*PACK4_LANES = 3*3*3*4), so DSP is unchanged by this
// edit - only BRAM and address logic shrink.
const unsigned MAX_IMG_W = 98;

// Rows held on chip. K=3 rows are in use for the current output row, and a
// 4th slot receives the next row's prefetch, so the prefetch never clobbers a
// row still being read. 4 x 514 x 3 = 6,168 bytes - one BRAM.
// NOTE: bind this to BRAM, never URAM - conv_engine's line_buf already holds
// URAM at 64/64 (100%), which is a deliberate choice there, and this engine
// must not compete for it. BRAM is at 5/144 tiles (3.47%) in the current
// implementation, so there is ample room.
const unsigned ROW_SLOTS = 4;

// Output row buffer depth, in PACKED words (pack4_t, 4 int8 lanes each).
//
// Sized to MAX_IMG_W, not to the output width (MAX_IMG_W - K + 1 = 512),
// deliberately: COL_GROUP iterates over all img_w input columns - the last
// K-1 of them only prefetch and emit nothing (conv0_engine.cpp / §34) - and
// giving those tail iterations a real slot to land in lets the store be
// UNCONDITIONAL. A conditional store is the exact thing the burst table
// blamed for `ofmap`, and while the condition is harmless on a BRAM port it
// would also mean trusting `emit` to keep `i` under a 2048-entry array's
// 11-bit index (it does - emit <=> i < out_w*4 - but 8 words of BRAM is a
// cheaper way to not have to make that argument).
//
// 514 * 4 = 2,056 words * 32 bits = 8,224 B, ~6 BRAM18 of 288.
const unsigned MAX_OUT_ROW_WORDS = MAX_IMG_W * (OUT_CH / PACK4_LANES);

typedef ap_int<8>  act_t;
typedef ap_int<8>  weight_t;
typedef ap_int<32> bias_t;
// 27 taps x 127 x 127 = ~435k, plus bias - int32 is comfortable.
typedef ap_int<32> accum_t;

void conv0_engine(
    const act_t    *ifmap,    // [img_h][img_w][3],  NHWC, PRE-PADDED, DDR
    const weight_t *weights,  // [16][3][3][3], OIHW (matches layer00_weights.bin)
    const bias_t   *bias,     // [16], int32 - the CORRECTED bias (see layer0 contract)
    pack4_t        *ofmap,    // [img_h-2][img_w-2][16/4], NHWC, DDR - PACKED
                              // int8x4 (pack4.h). Same DDR bytes an int8 buffer
                              // would occupy; packing is what makes 16 outputs
                              // cost 4 beats instead of 16 (see conv0_engine.cpp
                              // FINISH_WR).
    uint16_t img_h, uint16_t img_w,
    int32_t requant_multiplier, uint8_t requant_shift, uint8_t leaky_relu_enable
);

#endif // CONV0_ENGINE_H
