// ===========================================================================
// classifier_run.c - ROI classifier PL sequencer, Arty Z7-20 (2026-08-18)
//
// OWNERSHIP: the PL half of this file (the engine call sequence, buffer
// layout, and address contract) is the PL deliverable. The PS half (Ethernet
// receive, cache maintenance, DDR allocation) is NOT - it is left as clearly
// marked stubs for the PS owner. Do not wire this into the PL regression;
// nothing here has run on hardware, because there is no board.
//
// The drivers in this directory are BYTE COPIES of
// zybo_forteammate/05_layer_config/*.h - the classifier reuses the same two
// IPs with the same s_axilite register map, so the register offsets did not
// change. DRIVERS.sha256 records what was copied; if the source diverges,
// python/check_shapes.py fails rather than letting the copies rot silently.
//
// Sequence (HW/classifier_net.h):
//   conv0  3->16 @96x96  ON conv0_engine   -> pool0
//   conv1  16->32 @48x48 ON conv_engine    -> pool1
//   conv2  32->64 @24x24 ON conv_engine    -> pool2 -> [PS] GAP/FC/softmax
//
// Three engines, not two. conv0 gets its own because it is 59% of the ROI's
// cycles and the shared engine cannot help it (in_ch=3 idles most TR lanes,
// out_ch=16 is already one oc_tile). Adopted config is conv_engine at TR=8 -
// TR=16 plus conv0_engine is 256 DSP against 220 available.
// ===========================================================================

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "conv_engine_hw_driver.h"

// ⚠️ 두 드라이버 헤더는 같은 매크로 이름을 **다른 오프셋**으로 정의한다:
//        REG_IMG_H   conv 0x4c  vs  maxpool 0x28
//        REG_IMG_W   conv 0x54  vs  maxpool 0x30
//        REG_STRIDE  conv 0x74  vs  maxpool 0x40
//        REG_OFMAP_ADDR_LO/HI, REG_IFMAP_ADDR_LO/HI, REG_CTRL, CTRL_AP_* 도 겹친다.
// 각 헤더의 inline 함수 본문은 자기 정의가 살아 있는 시점에 토큰화되므로
// 동작 자체는 맞다. 하지만 재정의 경고가 쏟아지고, 이 파일이 나중에 이 이름들을
// 직접 쓰면 **뒤에 include 된 쪽 값**을 조용히 집는다. 그래서 사이에서 끊는다.
// (이 저장소에서 pack4.h / MAX_IMG_W / act_t 로 이미 세 번 겪은 충돌과 같은 부류다.)
#undef REG_CTRL
#undef CTRL_AP_START
#undef CTRL_AP_DONE
#undef CTRL_AP_IDLE
#undef CTRL_AP_READY
#undef REG_IFMAP_ADDR_LO
#undef REG_IFMAP_ADDR_HI
#undef REG_OFMAP_ADDR_LO
#undef REG_OFMAP_ADDR_HI
#undef REG_IMG_H
#undef REG_IMG_W
#undef REG_STRIDE
#include "maxpool_engine_hw_driver.h"
// conv0_engine 드라이버는 모든 매크로에 C0_ 접두사를 붙여서 위 두 헤더와
// **충돌하지 않는다**. 그래서 #undef 방벽이 필요 없다 - 앞의 두 드라이버가
// 같은 이름을 다르게 정의한 것과 대조적이다.
#include "conv0_engine_hw_driver.h"
#include "../HW/classifier_net.h"

// ---------------------------------------------------------------------------
// DDR buffer contract
// ---------------------------------------------------------------------------
// Two ping-pong activation buffers, sized by the largest intermediate tensor.
// Largest is conv0's output (ROI_SIZE^2 * CONV0_OUT_CH). Everything after it
// is smaller, so one pair of these covers the whole chain and no op ever
// reads the buffer it is writing.
//
//   conv0 : IN(roi) -> A      pool0 : A -> B
//   conv1 : B       -> A      pool1 : A -> B
//   conv2 : B       -> A      pool2 : A -> B    (B holds 12x12x64 for the PS)
//
// Alignment: the engines' m_axi ports are 32-bit (pack4_t), so every base
// address must be 4-byte aligned. 64-byte alignment is used instead, to keep
// each buffer on its own cache line - the PS must flush/invalidate around
// every engine call and partial lines are how that goes wrong.
#define ACT_BUF_BYTES   CLS_ACT_BUF_BYTES    // HW/classifier_net.h, derived
#define ALIGN64(x)      (((x) + 63u) & ~63u)

typedef struct {
    uint64_t roi;        // ROI_SIZE * ROI_SIZE * 3, INT8 NHWC, from the Jetson
    uint64_t act_a;      // ACT_BUF_BYTES
    uint64_t act_b;      // ACT_BUF_BYTES
    uint64_t weights;    // all conv weights, WPACK, concatenated
    uint64_t bias;       // all conv biases, INT32, concatenated
} cls_buffers_t;

// Per-conv offsets into the weights/bias blobs. The exporter must emit them
// in CLASSIFIER_OPS order with no padding between layers; these are byte
// offsets computed from the shapes, so a mismatch shows up as garbage output
// rather than a crash. Cross-check against the descriptor the exporter emits
// - do not trust either one alone.
//
//   OP_CONV  : weights[oc][k][k][in_ch] INT8, **WPACK-transposed**
//   OP_CONV0 : weights[oc][in_ch][k][k] INT8, **OIHW, NOT transposed**
//   bias[oc] INT32 (both)
//
// ⚠️ conv0's blob is 16*3*3*3 = 432 bytes and a WPACK-transposed blob of the
// same layer is ALSO 432 bytes. Feeding the wrong one loads without error and
// produces wrong numbers. The exporter must special-case conv0 - this project
// nearly shipped exactly that bug once.
typedef struct {
    uint32_t w_off, b_off;
} conv_blob_off_t;

static void compute_blob_offsets(conv_blob_off_t *out, unsigned n_conv)
{
    uint32_t w = 0, b = 0;
    unsigned c = 0;
    for (unsigned i = 0; i < CLASSIFIER_NUM_OPS && c < n_conv; i++) {
        const classifier_op_t *op = &CLASSIFIER_OPS[i];
        if (op->kind != OP_CONV && op->kind != OP_CONV0) continue;
        out[c].w_off = w;
        out[c].b_off = b;
        w += op->out_ch * op->k * op->k * op->in_ch;   // INT8
        b += op->out_ch * 4u;                          // INT32
        c++;
    }
}

// ---------------------------------------------------------------------------
// Per-layer requantization. Filled from the trained model's manifest
// (multiplier / right shift, same fields the YOLO exporter reads). Zeroed
// here on purpose: shipping plausible-looking placeholders is how a wrong
// scale survives to the board. classifier_run() refuses to start if they are
// still zero.
// ---------------------------------------------------------------------------
typedef struct {
    int32_t  multiplier;
    uint8_t  shift;
    uint8_t  leaky;
} conv_quant_t;

static conv_quant_t g_quant[3] = {
    { 0, 0, 1 },   // conv0
    { 0, 0, 1 },   // conv1
    { 0, 0, 0 },   // conv2 - linear into GAP; set leaky if the model says so
};

void classifier_set_quant(unsigned idx, int32_t multiplier, uint8_t shift, uint8_t leaky)
{
    if (idx < 3) {
        g_quant[idx].multiplier = multiplier;
        g_quant[idx].shift      = shift;
        g_quant[idx].leaky      = leaky;
    }
}

// ---------------------------------------------------------------------------
// Run one ROI through the PL. Returns 0 on success.
//
// Blocking, one engine at a time. The engines are strictly sequential here
// for the same reason the YOLO net's are: there is one HP0 port and the ops
// have a data dependency chain, so overlapping them buys nothing without a
// software change that runs independent ROIs concurrently. (See the P6
// finding: "8 masters contending on HP0" never happens while ops are
// sequential.)
// ---------------------------------------------------------------------------
int classifier_run_roi(const cls_buffers_t *buf, uint8_t *out_8x8x64)
{
    conv_blob_off_t off[3];
    compute_blob_offsets(off, 3);

    for (unsigned i = 0; i < 3; i++) {
        if (g_quant[i].multiplier == 0) {
            printf("classifier_run_roi: conv%u requant multiplier is still 0 - "
                   "call classifier_set_quant() from the trained model's "
                   "manifest first\n", i);
            return -1;
        }
    }

    uint64_t src = buf->roi;
    uint64_t dst = buf->act_a;
    uint64_t alt = buf->act_b;
    unsigned conv_idx = 0;

    for (unsigned i = 0; i < CLASSIFIER_NUM_OPS; i++) {
        const classifier_op_t *op = &CLASSIFIER_OPS[i];

        if (op->kind == OP_CONV0) {
            // conv0_engine: PRE-PADDED ifmap, pad=0, weights OIHW, ofmap
            // pack4_t. op->img_h/img_w are ALREADY the padded 66x66 (see
            // classifier_net.h); the PS must have written the zero border.
            //
            // No in_ch/out_ch/k/stride/pad registers exist - the engine
            // hardwires 3/16/3. Anything that would have set them is a bug,
            // not a no-op, so there is nothing to program here but shape,
            // addresses and quant.
            conv0_engine_set_addrs(src,
                                   buf->weights + off[conv_idx].w_off,
                                   buf->bias + off[conv_idx].b_off,
                                   dst);
            conv0_engine_set_shape((uint16_t)op->img_h, (uint16_t)op->img_w);
            conv0_engine_set_quant(g_quant[conv_idx].multiplier,
                                   g_quant[conv_idx].shift,
                                   g_quant[conv_idx].leaky);
            if (conv0_engine_wait_idle(op->name)) return -1;
            conv0_engine_start();
            if (conv0_engine_wait_done(op->name)) return -1;
            conv_idx++;
        } else if (op->kind == OP_CONV) {
            // No weights_hi argument on purpose: it is a second physical AXI
            // read port onto the SAME buffer, not a second copy, so the
            // driver programs both registers from this one address and
            // callers cannot get them out of sync (conv_engine_hw_driver.h).
            // tb_sequencer.c still checks the two registers came out equal -
            // the guarantee is worth a regression check, not blind trust.
            uint64_t w = buf->weights + off[conv_idx].w_off;
            conv_engine_set_addrs(src, w, buf->bias + off[conv_idx].b_off, dst);
            conv_engine_set_shape((uint16_t)op->img_h, (uint16_t)op->img_w,
                                  (uint16_t)op->in_ch, (uint16_t)op->out_ch,
                                  (uint8_t)op->k, (uint8_t)op->stride,
                                  (uint8_t)op->pad);
            conv_engine_set_quant(g_quant[conv_idx].multiplier,
                                  g_quant[conv_idx].shift,
                                  g_quant[conv_idx].leaky);
            if (conv_engine_wait_idle(op->name)) return -1;
            conv_engine_start();
            if (conv_engine_wait_done(op->name)) return -1;
            conv_idx++;
        } else {
            maxpool_engine_set_addrs(src, dst);
            maxpool_engine_set_shape((uint16_t)op->img_h, (uint16_t)op->img_w,
                                     (uint16_t)op->in_ch,
                                     /*stride=*/2, /*pad_right=*/0, /*pad_bottom=*/0);
            if (maxpool_engine_wait_idle(op->name)) return -1;
            maxpool_engine_start();
            if (maxpool_engine_wait_done(op->name)) return -1;
        }

        // ping-pong: this op's output becomes the next op's input.
        src = dst;
        dst = (dst == buf->act_a) ? buf->act_b : buf->act_a;
        (void)alt;
    }

    // src now points at the buffer holding 8x8x64 INT8, NHWC.
    if (out_8x8x64) {
        memcpy(out_8x8x64, (const void *)(uintptr_t)src,
               (size_t)GAP_IN_SIZE * GAP_IN_SIZE * GAP_IN_CH);
    }
    return 0;
}

// ===========================================================================
// PS TAIL - reference implementation, NOT a PL deliverable.
//
// GAP + FC + softmax on 8x8x64. Total ~4,400 MACs; at any PS clock this is
// microseconds and does not justify an engine. Given here so the PS owner has
// something bit-exact to check against, not because PL owns it.
// ===========================================================================
// gap_mode = "sum". The 1/64 is NOT applied here - it is folded into the FC
// scale, exactly once (manifest.json: gap_divisor_folded_into_fc = true).
//
// Why sum rather than mean: the PL output is INT8, so dividing here throws
// away information before the FC sees it. Summing 64 values into an INT32
// peaks at 127*64 = 8,128 - no overflow risk, full precision preserved.
//
// ⚠️ Applying 1/64 in BOTH places makes every logit 64x too small, and the
// argmax still looks plausible because the ORDER is unchanged - so the bug
// survives functional testing and only shows up in confidence values.
void classifier_gap(const int8_t *in_8x8x64, int32_t *out_64)
{
    for (unsigned c = 0; c < GAP_IN_CH; c++) {
        int32_t acc = 0;
        for (unsigned p = 0; p < GAP_IN_SIZE * GAP_IN_SIZE; p++)
            acc += in_8x8x64[p * GAP_IN_CH + c];
        out_64[c] = acc;          /* sum, not mean - see above */
    }
}

// in_64 holds GAP SUMS (not means). Whatever scale the training side derives
// for this layer must already contain the 1/64. Accumulator headroom:
// 8,128 * 127 * 64 is about 2^26, comfortably inside int32.
void classifier_fc(const int32_t *in_64, const int8_t *w, const int32_t *b,
                   int32_t *logits)
{
    for (unsigned o = 0; o < NUM_CLASSES; o++) {
        int32_t acc = b[o];
        for (unsigned i = 0; i < GAP_IN_CH; i++)
            acc += in_64[i] * (int32_t)w[o * GAP_IN_CH + i];
        logits[o] = acc;
    }
}

// argmax is enough for a class decision; softmax is only needed if the Jetson
// wants a confidence to threshold on. Kept separate so the integer path stays
// integer.
unsigned classifier_argmax(const int32_t *logits)
{
    unsigned best = 0;
    for (unsigned i = 1; i < NUM_CLASSES; i++)
        if (logits[i] > logits[best]) best = i;
    return best;
}
