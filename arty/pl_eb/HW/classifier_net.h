#ifndef CLASSIFIER_NET_H
#define CLASSIFIER_NET_H

// ===========================================================================
// ROI classifier network definition - Arty Z7-20 (xc7z020clg400-1).
//
// This header is the CONTRACT between the training side and the PL side.
// The training team may change channel counts and class count freely; the
// shape rules below are what the existing engines can actually execute, and
// breaking one of them costs a retrain, not a rebuild.
//
// ---------------------------------------------------------------------------
// PL CONSTRAINTS - the training side must respect these
// ---------------------------------------------------------------------------
//   conv  : k = 1 or 3, **stride = 1 only**, pad 0 or 1, INT8, NHWC.
//           stride > 1 is NOT implemented in hardware. conv_engine.cpp
//           asserts stride == 1 and does not branch on the port. Downsample
//           with maxpool instead - that is how the YOLO net this engine came
//           from does it.
//   pool  : 2x2 stride 2 maxpool only.
//   act   : leaky ReLU (per-layer enable), applied inside conv_engine's
//           requantize stage. No other activation exists in hardware.
//   quant : INT8 weights/activations, per-layer requant (multiplier, shift).
//   GAP / fully-connected / softmax run on the PS, not in PL. They are a
//           few hundred MACs total and do not justify an engine.
//
// ---------------------------------------------------------------------------
// COST MODEL - why the shapes are what they are
// ---------------------------------------------------------------------------
// Cycles on this engine follow the number of OUTPUT POSITIONS and the number
// of out_ch tiles, NOT the MAC count. Measured cycles-per-position on
// xc7z020 @100MHz (source: doc/01_status/2026-08-15_zybo-frame-measured.md,
// 22-op cosim sweep, tr16 configuration):
//
//      3 -> 16 ch :  93.3 cyc/pos
//     16 -> 32 ch : 155.7 cyc/pos
//     32 -> 64 ch : 361.7 cyc/pos
//
// So the FIRST conv, at full ROI resolution, dominates. Halving the ROI edge
// quarters its cost. Compare candidate architectures with
// python/cycle_model.py, not with MMAC counts - a 20x MAC reduction bought
// almost nothing in this codebase's history because the cost is per-position
// overhead and AXI transactions, not arithmetic.
// ===========================================================================

// --- ROI input geometry ----------------------------------------------------
// 96x96 build. Siblings: hls/arty_classifier/ (64) and hls/arty_128_classifier/
// (128). Cost scales with POSITIONS, but SUB-quadratically - measured, not
// assumed: 128 has 4x the positions of 64 and 3.75x the cycles, because the
// fixed per-op overhead amortizes better. 96 sits between the two measured
// points, so its budget is an INTERPOLATION, not an extrapolation.
// Changing this requires a retrain AND a re-synthesis (MAX_IMG_W is a
// compile-time bound; img_h/img_w are runtime but capped by it).
#define ROI_SIZE      96
#define ROI_IN_CH      3

#define NUM_CLASSES    5   // background, pedestrian, vehicle, stop, speed-limit
                           // 'background' is mandatory: a Jetson-proposed ROI
                           // is not guaranteed to contain an object.

// --- op sequence -----------------------------------------------------------
// 6 PL ops. Everything after op5 is PS.
typedef enum {
    OP_CONV  = 0,   // shared conv_engine (TR=8 build)
    OP_MAXPOOL = 1,
    OP_CONV0 = 2    // dedicated first-layer engine - DIFFERENT CONTRACT, see below
} classifier_op_kind_t;

// ---------------------------------------------------------------------------
// ⚠️ OP_CONV0 uses conv0_engine, whose interface is NOT conv_engine's
// ---------------------------------------------------------------------------
// conv0_engine exists because the first conv is 59% of the ROI's cycles and
// the shared engine cannot help it: in_ch=3 leaves 5 of 8 TR lanes idle and
// out_ch=16 is already one oc_tile, so parallelism does nothing. The Zybo
// campaign measured this directly ("layer 0 does not shrink by one tick with
// parallelism"). The dedicated engine hardwires IN_CH=3/OUT_CH=16/K=3 and
// fully unrolls all 108 MACs.
//
// Three differences the PS MUST honour. Each fails SILENTLY if got wrong:
//
//   1. ifmap is PRE-PADDED and pad=0.
//      For this build's 96x96 ROI the PS hands it a 98x98x3 buffer with a
//      zero border, and the output is 96x96x16. Handing it an unpadded 96x96
//      buffer produces a 94x94 output - the wrong SHAPE, which the next op
//      will read as if it were 96x96. (Sizes here follow ROI_SIZE; the
//      64-build numbers this paragraph used to quote are one fork over.)
//
//   2. weights are OIHW [16][3][3][3], NOT conv_engine's WPACK transpose.
//      Both are 432 bytes. **The size is identical, so a wrongly-transposed
//      blob loads without error and produces wrong numbers.** This project
//      nearly shipped exactly that bug once
//      (project_conv0_dispatch_and_weight_contract).
//
//   3. ofmap is pack4_t* (4 int8 per 32-bit word). Same DDR bytes as an
//      int8 NHWC buffer since out_ch=16 is a multiple of 4, so maxpool reads
//      it directly - but the pointer TYPE differs in the driver.
//
// conv0_engine also has no weights_hi port and no stride/pad ports at all.
// ---------------------------------------------------------------------------

// Pre-padding the PS must apply before OP_CONV0. The border is zeros.
#define CONV0_PAD          1
#define CONV0_PADDED_SIZE  (ROI_SIZE + 2 * CONV0_PAD)   // 98 for a 96x96 ROI

typedef struct {
    classifier_op_kind_t kind;
    const char *name;
    unsigned img_h, img_w;      // INPUT spatial size of this op
    unsigned in_ch, out_ch;
    unsigned k, stride, pad;    // conv only; stride is always 1
    unsigned leaky;             // conv only
} classifier_op_t;

// Spatial: 96 -> 96 -> 48 -> 48 -> 24 -> 24 -> 12
// Channels: 3 -> 16 -> 16 -> 32 -> 32 -> 64 -> 64
// conv0 runs on conv0_engine, so its img_h/img_w are the PRE-PADDED 98x98 and
// its pad field is 0 - the padding is already in the buffer. Every other op
// takes its own input size with pad applied by the engine.
//
// The `leaky` column is NOT executed from here - the PS driver programs the
// register from g_quant[] in SW/classifier_run.c, and the golden comes from
// LEAKY[] in verif_host/gen_golden.c. What this column DOES feed is
// gen_manifest.py -> manifest.json "activation", i.e. the document the
// training side quantizes against. So a wrong value here does not change one
// cycle of hardware; it changes what the model is trained to expect, and the
// weights come back unusable. It said leaky on conv2 until 2026-08-19 while
// all three computing sites said linear. check_shapes.py section 13 now
// cross-checks the whole (mult, shift, leaky) triple across all five sites.
static const classifier_op_t CLASSIFIER_OPS[] = {
    { OP_CONV0,   "conv0",  98, 98,  3, 16, 3, 1, 0, 1 },
    { OP_MAXPOOL, "pool0",  96, 96, 16, 16, 0, 2, 0, 0 },
    { OP_CONV,    "conv1",  48, 48, 16, 32, 3, 1, 1, 1 },
    { OP_MAXPOOL, "pool1",  48, 48, 32, 32, 0, 2, 0, 0 },
    { OP_CONV,    "conv2",  24, 24, 32, 64, 3, 1, 1, 0 },
    { OP_MAXPOOL, "pool2",  24, 24, 64, 64, 0, 2, 0, 0 },
};
#define CLASSIFIER_NUM_OPS 6

// PS tail: GAP over 12x12x64 -> 64, FC 64 x NUM_CLASSES, softmax.
#define GAP_IN_SIZE   12   // 12x12 spatial after pool2
#define GAP_IN_CH     64

// --- PS buffer sizing (DERIVED - never write the numbers by hand) ----------
// These are the only place the PS-facing byte counts are computed. They were
// three hardcoded 64x64 literals until 2026-08-18, when this tree was forked
// from the 64 build: the geometry above followed ROI_SIZE and the literals did
// not, so ACT_BUF_BYTES was 2.25x too small and act_a would have overrun
// act_b on the board. check_shapes.py section 11 recomputes all three from
// CLASSIFIER_OPS and fails if a literal creeps back in.
//
// CONV0_OUT_CH duplicates CLASSIFIER_OPS[0].out_ch because the C preprocessor
// cannot read the table; the gate compares the two.
#define CONV0_OUT_CH   16

// conv0_engine's input: the PRE-PADDED buffer the PS stages in DDR.
#define CLS_WIRE_INPUT_BYTES  ((unsigned)CONV0_PADDED_SIZE * CONV0_PADDED_SIZE * ROI_IN_CH)

// One ping-pong activation buffer. conv0's output is the largest intermediate
// in THIS chain - the gate checks that against every op's output, so a future
// channel change cannot make this silently too small.
#define CLS_ACT_BUF_BYTES     ((unsigned)ROI_SIZE * ROI_SIZE * CONV0_OUT_CH)

// What pool2 hands back to the PS.
#define CLS_PL_OUTPUT_BYTES   ((unsigned)GAP_IN_SIZE * GAP_IN_SIZE * GAP_IN_CH)

// --- engine sizing ---------------------------------------------------------
// These bound the forked engines' on-chip buffers. Keep them >= the largest
// value any op above uses; do not leave them at the YOLO values (512 / 1024),
// that is BRAM spent on a network this build cannot run.
#define CLS_MAX_IMG_W    96
#define CLS_MAX_CH       64

#endif // CLASSIFIER_NET_H
