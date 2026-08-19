#ifndef MAXPOOL_ENGINE_H
#define MAXPOOL_ENGINE_H

#include <ap_int.h>
#include <stdint.h>
#include "pack4.h"

// ---------------------------------------------------------------------------
// MaxPool engine - one of 3 small, independent HLS IPs (this one, plus
// upsample_engine and route_concat_engine) filling the gap
// conv_engine_requant/README.md item 4 flagged: that project's conv_engine
// only ever does Conv, but the real YOLOv3-tiny-ADAS network also needs
// MaxPool (5 layers), Upsample (1 layer), and Route/concat (2 layers)
// between conv layers. Kept as its own isolated project/IP rather than
// folded into conv_engine, matching RTL_HANDOFF_KO.md's own recommended
// structure ("one reusable Conv engine + separate small pooling/upsample/
// route units + a layer sequencer").
//
// Implements RTL_HANDOFF_KO.md section 6: MaxPool 2x2, either stride 2
// (5 of the real network's layers) or stride 1 with right/bottom-only
// padding using signed INT8 -128 (layer 11 only - its input is already at
// the network's smallest spatial size, and a stride-1 pool there would
// shrink it further without the pad). MaxPool is a pure comparison op - it
// preserves the input's quantization scale exactly, so unlike conv_engine
// or route_concat_engine there is NO per-layer requantization math here at
// all.
//
// ---- PACK4 VARIANT ----
// This is the int8x4-packed variant of the baseline maxpool_engine. The
// m_axi pointers are `pack4_t` (ap_uint<32>) rather than `act_t`
// (ap_int<8>), so each AXI beat carries 4 consecutive NHWC channels instead
// of 1. See pack4.h's header comment for the full rationale, for why this
// does NOT change the physical AXI port width or the PS-side DDR byte
// layout, and for the two new requirements it does introduce (ch % 4 == 0,
// 4-byte-aligned base pointers). The baseline engine measured 6.16
// cycles/output against a 6-cycle "1 element per beat" floor, so the whole
// remaining headroom here is the bus, not the pipeline.
//
// NOTE: model_manifest.json's own "maxpool" layer entries carry only
// {index, type, output_scale, output_bits} - no pool size/stride field.
// stride/pad_right/pad_bottom below are NOT read from the manifest by this
// IP or its testbench; they must be programmed by software per
// RTL_HANDOFF_KO.md section 7's execution-order table (index 11 = stride 1
// + pad; every other maxpool index = stride 2, no pad).
//
// PS integration / AXI port count (see ../vivado/create_bd.tcl): this IP
// has exactly 2 m_axi master ports - `ifmap` on bundle=RD_BUS, `ofmap` on
// bundle=WR_BUS (see maxpool_engine.cpp's INTERFACE pragmas). No bundle
// sharing within this IP (each pointer gets its own bundle name) - same as
// upsample_engine.h/route_concat_engine.h, all 3 of which have exactly 2
// masters each (route_concat_engine's 2 sources share ONE bundle, see that
// header), for 6 total across the 3 new IPs. The pack4 change does not
// alter this count, the bundle names, or the s_axilite register map - the
// SW/ drivers are unaffected apart from the alignment contract.
// ---------------------------------------------------------------------------

// ARTY 96 CLASSIFIER FORK: largest spatial width any pool sees is the ROI
// edge (96); largest channel count is 64. img_w/ch are RUNTIME ports.
// Siblings set MAX_IMG_W to 64 and 128.
const unsigned MAX_IMG_W = 96;
const unsigned MAX_CH    = 64;

// Channel bound expressed in 32-bit words - this, not MAX_CH, is what sizes
// the on-chip `maxval` array and the m_axi depth= pragmas now that both are
// word-addressed.
const unsigned MAX_CH_WORDS = MAX_CH / PACK4_LANES;

// ---------------------------------------------------------------------------
// Row-buffer path (2026-08-09) — sized by a property of THIS network
// ---------------------------------------------------------------------------
// One input row costs `img_w * ch_words` words. Across all six real maxpool
// layers that product is **exactly 2048 every time**, because the network
// halves the spatial width and doubles the channel count at each stage:
//
// ARTY CLASSIFIER FORK - the classifier has the same halve-width /
// double-channel property, so its three pools also land on one constant:
//
//   pool0  96x 4   pool1  48x 8   pool2  24x16        -> 384 words (1.5 KB) each
//
// The YOLO table this bound was originally derived from, for reference:
//   idx0 512x  4   idx1 256x  8   idx2 128x 16
//   idx3  64x 32   idx4  32x 64   idx5  16x128      -> 2048 words (8 KB) each
//
// So three row buffers (two input rows + one output row) cost 24 KB and cover
// the whole network, instead of the MAX_IMG_W*MAX_CH_WORDS = 65,536 words
// (256 KB each) a generic bound would demand.
//
// ⚠️ This is a NETWORK-DEPENDENT bound, like conv_engine's ACCUM_POSITIONS.
// A layer whose `img_w * ch/4` exceeds this does not break — maxpool_engine()
// falls back to the original per-position path at run time (there is no
// assert, because the testbench's synthetic configs legitimately exceed it).
// But it also gets none of the speedup, so if the layer table changes, check
// this number.
// Set to 768, not the 384 the current table needs: one doubling of channel
// headroom for 3 KB per buffer. Exceeding it does not fail loudly - the
// engine silently falls back to the slow per-position path - so if the
// training side changes channel counts, recheck img_w * ch/4 against this.
const unsigned MAX_ROW_WORDS = 768;

typedef ap_int<8> act_t;

// Padding value for the stride-1 case (RTL_HANDOFF_KO.md section 6): the
// most-negative INT8 value, so a padded position can never win a max
// comparison against any real (in-bounds) activation. The packed engine
// initialises a whole word at a time with pack4.h's PACK4_ALL_MINUS_128,
// which is this same value in all 4 lanes.
const act_t MAXPOOL_PAD_VALUE = (act_t)-128;

// ---------------------------------------------------------------------------
// Top-level function. All arrays NHWC (channel innermost), row-major,
// DDR-resident - matches conv_engine_requant's convention so a layer's
// output can feed the next layer (or this IP's own next call) without a
// reformatting pass. The pack4 pointers address that SAME byte layout in
// 32-bit units, so a buffer produced by an int8-addressed engine is a valid
// input here with no repacking (pack4.h, "what this does not change").
//
// out_h = (img_h + pad_bottom - 2) / stride + 1, out_w likewise with
// pad_right - software is expected to compute these itself (same as it
// already does for conv_engine's out_h/out_w) and size `ofmap` accordingly.
//
// Window is a fixed compile-time 2x2 - RTL_HANDOFF_KO.md never varies it,
// so unlike conv_engine's runtime `k` this is a design constant, not a
// register.
//
// pad_right/pad_bottom are deliberately NOT conv_engine's `pad` semantic
// (which pads symmetrically on all four sides): RTL_HANDOFF_KO.md's
// stride-1 case pads only the right and bottom edge by exactly one cell.
//
// `ch` is still counted in CHANNELS, not words - the register map and the
// software-visible meaning of every argument are unchanged from the
// baseline engine. It must now be a multiple of 4 (see pack4.h).
// ---------------------------------------------------------------------------
void maxpool_engine(
    const pack4_t *ifmap,   // [img_h][img_w][ch/4], NHWC, DDR-resident
    pack4_t       *ofmap,   // [out_h][out_w][ch/4], NHWC, DDR-resident
    uint16_t img_h, uint16_t img_w, uint16_t ch,
    uint8_t  stride,                     // 1 or 2 only
    uint8_t  pad_right, uint8_t pad_bottom  // 0 or 1 each
);

#endif // MAXPOOL_ENGINE_H
