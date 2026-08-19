// ===========================================================================
// tb_sequencer.c - TB-S: classifier_run.c 의 s_axilite 트랜잭션 검사 (호스트)
//
// 이 저장소에는 "드라이버 C 함수가 그때까지 어디서도 실행된 적이 없었다"는
// 전례가 있습니다(2026-08-08, verif/ 신설 계기). cosim 은 생성 TB 로 엔진을
// 몰기 때문에 s_axilite 를 안 지나가고, HLS TB 는 레지스터를 아예 안 만집니다.
// 그 사이에 있는 것이 시퀀서이고, 이 파일이 그것을 실제로 실행합니다.
//
// 검사하는 것:
//   1. ap_start 가 정확히 6번, **conv0/pool/conv/pool/conv/pool** 순서로
//      (첫 conv 는 전용 엔진 conv0_engine 이다 - 다른 베이스 주소)
//   2. 각 conv 의 img_h/img_w/in_ch/out_ch/k/stride/pad 가 헤더와 일치
//   3. conv 의 stride 레지스터가 항상 1        (하드웨어가 stride=1 만 구현)
//   4. weights 와 weights_hi 가 **같은 주소**  (다르면 조용히 틀린 결과)
//   5. 가중치/바이어스 오프셋이 형상대로 전진
//   6. ping-pong: 각 op 의 ifmap == 직전 op 의 ofmap, 그리고 ifmap != ofmap
//   7. requant 가 0 이면 ap_start 전에 거부
//   8. conv0 은 **PRE-PADDED 형상**(66x66)을 프로그램하고, conv_engine 의
//      베이스 주소는 **건드리지 않는다** (엔진을 헷갈리면 조용히 틀린다)
//
// 검사하지 않는 것: 핸드셰이크 타이밍, 실제 DDR 내용, 엔진 연산.
// 판정은 종료 코드입니다.
// ===========================================================================
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <stdarg.h>
#include "xil_io.h"
shim_txn_t shim_txn[SHIM_MAX_TXN];
unsigned   shim_n_txn = 0;
int        shim_overflow = 0;

#include "../SW/classifier_run.c"

#define CE  XPAR_CONV_ENGINE_0_S_AXI_CTRL_BASEADDR
#define C0  XPAR_CONV0_ENGINE_0_S_AXI_CTRL_BASEADDR
#define MP  XPAR_MAXPOOL_ENGINE_0_S_AXI_CTRL_BASEADDR

static int fails = 0;
static void ck(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf(ok ? "  OK   " : "  FAIL ");
    vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

// 마지막으로 <base+off> 에 쓴 값을, 트랜잭션 인덱스 [lo, hi) 구간에서 찾는다.
static int last_write(uint32_t base, uint32_t off, unsigned lo, unsigned hi,
                      uint32_t *out)
{
    int found = 0;
    for (unsigned i = lo; i < hi; i++)
        if (shim_txn[i].addr == base + off) { *out = shim_txn[i].val; found = 1; }
    return found;
}

int main(void)
{
    // "DDR" 은 그냥 호스트 메모리다. 시퀀서가 실제로 memcpy 를 하므로 주소가
    // 유효해야 한다 - 가짜 상수를 쓰면 여기서 죽는다.
    static uint8_t roi[CLS_WIRE_INPUT_BYTES];   /* PRE-PADDED, derived */
    static uint8_t a[ACT_BUF_BYTES], b[ACT_BUF_BYTES];
    static uint8_t weights[65536], bias[4096];
    static uint8_t out[GAP_IN_SIZE * GAP_IN_SIZE * GAP_IN_CH];

    cls_buffers_t buf = {
        .roi     = (uint64_t)(uintptr_t)roi,
        .act_a   = (uint64_t)(uintptr_t)a,
        .act_b   = (uint64_t)(uintptr_t)b,
        .weights = (uint64_t)(uintptr_t)weights,
        .bias    = (uint64_t)(uintptr_t)bias,
    };

    // ---- 7. requant 미설정 거부 (먼저 - ap_start 가 하나도 없어야 한다) ----
    printf("== 7. requant 가 0 이면 시작 거부\n");
    int rc = classifier_run_roi(&buf, out);
    ck(rc != 0, "requant=0 에서 classifier_run_roi 가 실패를 반환 (rc=%d)", rc);
    unsigned starts = 0;
    for (unsigned i = 0; i < shim_n_txn; i++)
        if ((shim_txn[i].addr == CE || shim_txn[i].addr == C0 || shim_txn[i].addr == MP)
            && (shim_txn[i].val & 1u))
            starts++;
    ck(starts == 0, "거부 시 ap_start 0회 (실제 %u회)", starts);

    // ---- 정상 실행 ----
    shim_n_txn = 0; shim_overflow = 0;
    classifier_set_quant(0, 1264723901, 37, 1);
    classifier_set_quant(1,  987654321, 36, 1);
    classifier_set_quant(2,  123456789, 35, 0);
    rc = classifier_run_roi(&buf, out);
    printf("\n== 실행: rc=%d, 트랜잭션 %u개\n", rc, shim_n_txn);
    ck(rc == 0, "classifier_run_roi 성공");
    ck(!shim_overflow, "트랜잭션 로그 넘침 없음");

    // ---- 1. ap_start 6번, 순서 ----
    printf("== 1. ap_start 순서\n");
    unsigned start_idx[16]; uint32_t start_base[16]; unsigned ns = 0;
    for (unsigned i = 0; i < shim_n_txn; i++) {
        if ((shim_txn[i].addr == CE || shim_txn[i].addr == C0 || shim_txn[i].addr == MP)
            && (shim_txn[i].val & 1u)) {
            if (ns < 16) { start_idx[ns] = i; start_base[ns] = shim_txn[i].addr; }
            ns++;
        }
    }
    ck(ns == CLASSIFIER_NUM_OPS, "ap_start %u회 == 헤더의 op %u개", ns, CLASSIFIER_NUM_OPS);
    for (unsigned i = 0; i < ns && i < CLASSIFIER_NUM_OPS; i++) {
        classifier_op_kind_t k = CLASSIFIER_OPS[i].kind;
        uint32_t want = (k == OP_CONV) ? CE : (k == OP_CONV0) ? C0 : MP;
        const char *ename = (k == OP_CONV) ? "conv" : (k == OP_CONV0) ? "conv0" : "maxpool";
        ck(start_base[i] == want, "op %u (%s): %s 엔진에 start",
           i, CLASSIFIER_OPS[i].name, ename);
    }
    if (ns != CLASSIFIER_NUM_OPS) { printf("\nFAILED\n"); return 1; }

    // ---- 2/3/4/5/6. op 별 레지스터 ----
    printf("== 2-6. op 별 레지스터\n");
    uint64_t prev_ofmap = 0;
    uint32_t exp_w_off = 0, exp_b_off = 0;
    for (unsigned i = 0; i < ns; i++) {
        const classifier_op_t *op = &CLASSIFIER_OPS[i];
        unsigned lo = (i == 0) ? 0 : start_idx[i - 1] + 1;
        unsigned hi = start_idx[i];
        uint32_t v, lo32, hi32;
        uint64_t ifmap, ofmap;

        if (op->kind == OP_CONV0) {
            // conv0_engine 레지스터 맵은 conv_engine 과 **다르다**:
            //   ifmap 0x10,0x14 / weights 0x1c,0x20 / bias 0x28,0x2c /
            //   ofmap 0x34,0x38 / img_h 0x40 / img_w 0x48
            // in_ch/out_ch/k/stride/pad 레지스터는 **없다**(하드와이어 3/16/3).
            uint32_t ih = 0xffffffffu, iw = 0xffffffffu;
            last_write(C0, 0x40, lo, hi, &ih);
            last_write(C0, 0x48, lo, hi, &iw);
            ck(ih == op->img_h && iw == op->img_w,
               "%s: img %ux%u (기대 PRE-PADDED %ux%u)", op->name, ih, iw,
               op->img_h, op->img_w);
            ck(ih == ROI_SIZE + 2 * CONV0_PAD,
               "%s: img_h %u == ROI_SIZE+2*CONV0_PAD (%u) - 패딩 안 하면 출력이 %ux%u 로 작아진다",
               op->name, ih, ROI_SIZE + 2 * CONV0_PAD, ROI_SIZE - 2, ROI_SIZE - 2);
            // 엔진 혼동 방지: 이 op 구간에서 conv_engine 베이스에 쓰면 안 된다.
            unsigned stray = 0;
            for (unsigned t2 = lo; t2 < hi; t2++)
                if ((shim_txn[t2].addr & 0xffff0000u) == (CE & 0xffff0000u)) stray++;
            ck(stray == 0, "%s: conv_engine 창에 쓰기 0회 (실제 %u회)", op->name, stray);

            uint32_t wl, wh, bl, bh, lo32b, hi32b;
            last_write(C0, 0x1c, lo, hi, &wl); last_write(C0, 0x20, lo, hi, &wh);
            last_write(C0, 0x28, lo, hi, &bl); last_write(C0, 0x2c, lo, hi, &bh);
            ck((((uint64_t)wh << 32) | wl) == buf.weights + exp_w_off,
               "%s: weights 오프셋 = %u", op->name, exp_w_off);
            ck((((uint64_t)bh << 32) | bl) == buf.bias + exp_b_off,
               "%s: bias 오프셋 = %u", op->name, exp_b_off);
            exp_w_off += op->out_ch * op->k * op->k * op->in_ch;
            exp_b_off += op->out_ch * 4u;

            last_write(C0, 0x10, lo, hi, &lo32b); last_write(C0, 0x14, lo, hi, &hi32b);
            ifmap = ((uint64_t)hi32b << 32) | lo32b;
            last_write(C0, 0x34, lo, hi, &lo32b); last_write(C0, 0x38, lo, hi, &hi32b);
            ofmap = ((uint64_t)hi32b << 32) | lo32b;
        } else if (op->kind == OP_CONV) {
            struct { uint32_t off; unsigned want; const char *nm; } regs[] = {
                { 0x4c, op->img_h,  "img_h" }, { 0x54, op->img_w,  "img_w" },
                { 0x5c, op->in_ch,  "in_ch" }, { 0x64, op->out_ch, "out_ch" },
                { 0x6c, op->k,      "k" },     { 0x74, op->stride, "stride" },
                { 0x7c, op->pad,    "pad" },
            };
            for (unsigned r = 0; r < 7; r++) {
                int f = last_write(CE, regs[r].off, lo, hi, &v);
                ck(f && v == regs[r].want, "%s.%s = %u (기대 %u)",
                   op->name, regs[r].nm, f ? v : 0xffffffffu, regs[r].want);
            }
            // 3. stride 는 반드시 1
            last_write(CE, 0x74, lo, hi, &v);
            ck(v == 1, "%s: stride 레지스터 == 1 (하드웨어가 stride=1 만 구현)", op->name);

            // 4. weights == weights_hi
            uint32_t wl, wh, hl, hh;
            last_write(CE, 0x1c, lo, hi, &wl); last_write(CE, 0x20, lo, hi, &wh);
            last_write(CE, 0x28, lo, hi, &hl); last_write(CE, 0x2c, lo, hi, &hh);
            ck(wl == hl && wh == hh,
               "%s: weights_hi == weights (다르면 조용히 틀린 결과)", op->name);

            // 5. 오프셋 전진
            uint64_t w_addr = ((uint64_t)wh << 32) | wl;
            ck(w_addr == buf.weights + exp_w_off,
               "%s: weights 오프셋 = %u", op->name, exp_w_off);
            uint32_t bl, bh;
            last_write(CE, 0x34, lo, hi, &bl); last_write(CE, 0x38, lo, hi, &bh);
            ck((((uint64_t)bh << 32) | bl) == buf.bias + exp_b_off,
               "%s: bias 오프셋 = %u", op->name, exp_b_off);
            exp_w_off += op->out_ch * op->k * op->k * op->in_ch;
            exp_b_off += op->out_ch * 4u;

            last_write(CE, 0x10, lo, hi, &lo32); last_write(CE, 0x14, lo, hi, &hi32);
            ifmap = ((uint64_t)hi32 << 32) | lo32;
            last_write(CE, 0x40, lo, hi, &lo32); last_write(CE, 0x44, lo, hi, &hi32);
            ofmap = ((uint64_t)hi32 << 32) | lo32;
        } else {
            // maxpool 오프셋은 conv 와 **다르다** (같은 매크로 이름, 다른 값):
            //   img_h 0x28 / img_w 0x30 / ch 0x38 / ofmap 0x1c,0x20
            // check_shapes.py 가 이 리터럴을 드라이버 헤더와 대조한다.
            struct { uint32_t off; unsigned want; const char *nm; } regs[] = {
                { 0x28, op->img_h, "img_h" }, { 0x30, op->img_w, "img_w" },
                { 0x38, op->in_ch, "ch" },
            };
            for (unsigned r = 0; r < 3; r++) {
                int f = last_write(MP, regs[r].off, lo, hi, &v);
                ck(f && v == regs[r].want, "%s.%s = %u (기대 %u)",
                   op->name, regs[r].nm, f ? v : 0xffffffffu, regs[r].want);
            }
            // stride/pad 도 반드시 본다. 반전 실험에서 stride 1 을 넘겨도
            // 안 잡히던 구멍이었다 - stride 는 출력 형상을 통째로 바꾸는데
            // 이 하네스는 형상을 레지스터로만 보므로 여기서 안 잡으면
            // 어디서도 안 잡힌다. maxpool 오프셋: stride 0x40 / pad 0x48,0x50.
            { uint32_t st = 0xffffffffu, pr = 0xffffffffu, pb = 0xffffffffu;
              last_write(MP, 0x40, lo, hi, &st);
              last_write(MP, 0x48, lo, hi, &pr);
              last_write(MP, 0x50, lo, hi, &pb);
              ck(st == op->stride, "%s.stride = %u (기대 %u)", op->name, st, op->stride);
              ck(pr == 0 && pb == 0, "%s.pad_right/bottom = %u/%u (기대 0/0)",
                 op->name, pr, pb); }
            last_write(MP, 0x10, lo, hi, &lo32); last_write(MP, 0x14, lo, hi, &hi32);
            ifmap = ((uint64_t)hi32 << 32) | lo32;
            last_write(MP, 0x1c, lo, hi, &lo32); last_write(MP, 0x20, lo, hi, &hi32);
            ofmap = ((uint64_t)hi32 << 32) | lo32;
        }

        // 6. ping-pong
        ck(ifmap != ofmap, "%s: ifmap != ofmap (제자리 덮어쓰기 아님)", op->name);
        if (i == 0)
            ck(ifmap == buf.roi, "%s: 첫 op 의 ifmap == ROI 버퍼", op->name);
        else
            ck(ifmap == prev_ofmap, "%s: ifmap == 직전 op 의 ofmap", op->name);
        prev_ofmap = ofmap;
    }

    printf("\n");
    if (fails) { printf("TB-S FAILED: %d개 검사 실패\n", fails); return 1; }
    printf("TB-S PASS: 모든 s_axilite 시퀀스 검사 통과\n");
    return 0;
}
