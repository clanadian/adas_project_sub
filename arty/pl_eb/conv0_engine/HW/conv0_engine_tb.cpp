// ===========================================================================
// conv0_engine_tb.cpp - ARTY CLASSIFIER FORK (2026-08-18)
//
// 원본(hls/zybo_conv0_engine/HW/conv0_engine_tb.cpp)은 REAL_LAYERS[0] 골든에
// 맞춰 검사했습니다. 분류기는 아직 학습 전이라 골든이 없고, 그 골든을 담은
// real_layers_data.h 는 64 MB 라 포크에 끌고 올 것도 아닙니다.
//
// 그래서 **독립 참조 합성곱**과 비교합니다. 참조 수식은
// ../../conv_engine/HW/conv_engine_tb.cpp 의 reference_conv() 와 동일합니다 -
// conv0_engine.h 가 "rounding 은 conv_engine 것과 동일하게 유지하라"고
// 명시하고 있으므로, 두 엔진을 같은 참조로 재는 것이 그 계약의 검사이기도
// 합니다.
//
// ⚠️ conv0_engine 의 계약은 conv_engine 과 다릅니다:
//   - ifmap 은 **PRE-PADDED** 입니다. pad=0 으로 돌고, 출력은 [img_h-2][img_w-2].
//     64x64 ROI 를 쓰려면 PS 가 66x66 으로 0-패딩해서 넘겨야 합니다.
//   - weights 는 **OIHW [16][3][3][3]** 이고 conv_engine 의 WPACK 이 아닙니다.
//     크기가 432 로 같아서 **틀려도 조용히 지나갑니다** - 이 저장소가 실제로
//     한 번 밟을 뻔한 지뢰입니다.
//   - ofmap 은 pack4_t* 입니다(같은 DDR 바이트, NHWC).
//
// 증명하는 것: 위 계약대로 부르면 비트 정확하다는 것, 그리고 cosim 에서
// 위치당 사이클.  증명하지 않는 것: 분류 정확도(가중치가 난수).
// ===========================================================================
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "conv0_engine.h"

// ---- 참조 모델 (conv_engine_tb.cpp 와 동일 수식) --------------------------
static int8_t clamp_i8(int64_t v)
{
    if (v >  127) return  127;
    if (v < -128) return -128;
    return (int8_t)v;
}

static int64_t round_shift_ref(int64_t x, unsigned s)
{
    if (s == 0) return x;
    int64_t half = (int64_t)1 << (s - 1);
    return (x >= 0) ? ((x + half) >> s) : -(((-x) + half) >> s);
}

// pad=0, PRE-PADDED 입력. 의도적으로 conv0_engine.cpp 와 코드 경로를 공유하지
// 않습니다 - 공유하면 같은 버그에 조용히 합의합니다.
static void reference_conv0(
    const std::vector<int8_t> &x, const std::vector<int8_t> &w,
    const std::vector<int32_t> &b, std::vector<int8_t> &out,
    unsigned img_h, unsigned img_w,
    bool leaky, int32_t mult, unsigned shift)
{
    unsigned out_h = img_h - K + 1;
    unsigned out_w = img_w - K + 1;
    out.assign((size_t)out_h * out_w * OUT_CH, 0);

    for (unsigned r = 0; r < out_h; r++)
        for (unsigned c = 0; c < out_w; c++)
            for (unsigned oc = 0; oc < OUT_CH; oc++) {
                int64_t acc = b[oc];
                for (unsigned ic = 0; ic < IN_CH; ic++)
                    for (unsigned ky = 0; ky < K; ky++)
                        for (unsigned kx = 0; kx < K; kx++) {
                            int8_t px = x[((size_t)(r + ky) * img_w + (c + kx)) * IN_CH + ic];
                            unsigned wi = ((oc * IN_CH + ic) * K + ky) * K + kx;  // OIHW
                            acc += (int64_t)px * (int64_t)w[wi];
                        }
                if (leaky && acc < 0) acc = round_shift_ref(acc * 13, 7);
                int64_t scaled = acc * (int64_t)mult;
                out[((size_t)r * out_w + c) * OUT_CH + oc] =
                    clamp_i8(round_shift_ref(scaled, shift));
            }
}

// ---- pack4 헬퍼 -----------------------------------------------------------
static inline size_t pack4_words(size_t elems) { return (elems + PACK4_LANES - 1) / PACK4_LANES; }

static inline int8_t pack4_take(const std::vector<pack4_t> &buf, size_t i)
{
    unsigned lane = (unsigned)(i % PACK4_LANES);
    uint32_t w = (uint32_t)buf[i / PACK4_LANES];
    return (int8_t)((w >> (8u * lane)) & 0xffu);
}

// cosim 은 m_axi `depth=` 프라그마에서 RTL 버퍼를 잡습니다. 짧은 호스트 배열을
// 넘기면 ENTER_WRAPC 에서 SIGSEGV 입니다 - conv_engine_tb.cpp 가 기록한 그 함정.
//
// ⚠️ 아래 세 상수는 conv0_engine.cpp 의 `depth=` **식과 글자 그대로 같아야**
//    합니다. 2026-08-18 에 여기서 죽었습니다: 프라그마가 447180 리터럴(YOLO
//    레이어 0 크기)이었고 MAX_IMG_W 축소를 안 따라와서, 이 TB 가 13,068 짜리
//    배열을 넘겼습니다. 프라그마를 식으로 고쳐 양쪽이 같은 상수를 보게 했습니다.
static const size_t IFMAP_DEPTH  = (size_t)MAX_IMG_W * MAX_IMG_W * IN_CH;
static const size_t OFMAP_DEPTH  = (size_t)MAX_IMG_W * MAX_IMG_W * OUT_CH;
static const size_t WEIGHT_DEPTH = (size_t)OUT_CH * IN_CH * K * K;

static int run_config(const char *name, unsigned img_h, unsigned img_w,
                      unsigned seed, bool leaky, int32_t mult, unsigned shift,
                      bool full_depth)
{
    if (img_w > MAX_IMG_W) { printf("[%s] FAIL: img_w %u > MAX_IMG_W %u\n", name, img_w, MAX_IMG_W); return 1; }
    srand(seed);
    unsigned out_h = img_h - K + 1, out_w = img_w - K + 1;

    std::vector<int8_t>  x((size_t)img_h * img_w * IN_CH);
    std::vector<int8_t>  w(WEIGHT_DEPTH);
    std::vector<int32_t> b(OUT_CH);
    for (auto &v : x) v = (int8_t)((rand() % 41) - 20);
    for (auto &v : w) v = (int8_t)((rand() % 11) - 5);
    for (auto &v : b) v = (int32_t)((rand() % 2001) - 1000);

    std::vector<int8_t> ref;
    reference_conv0(x, w, b, ref, img_h, img_w, leaky, mult, shift);

    size_t in_n  = full_depth ? IFMAP_DEPTH  : x.size();
    size_t out_n = full_depth ? OFMAP_DEPTH  : (size_t)out_h * out_w * OUT_CH;
    size_t w_n   = full_depth ? WEIGHT_DEPTH : w.size();

    std::vector<act_t>    x_hw(in_n, (act_t)0);
    std::vector<weight_t> w_hw(w_n,  (weight_t)0);
    std::vector<bias_t>   b_hw(OUT_CH);
    std::vector<pack4_t>  o_hw(pack4_words(out_n), (pack4_t)0);
    for (size_t i = 0; i < x.size(); i++) x_hw[i] = (act_t)x[i];
    for (size_t i = 0; i < w.size(); i++) w_hw[i] = (weight_t)w[i];
    for (unsigned i = 0; i < OUT_CH; i++)  b_hw[i] = (bias_t)b[i];

    conv0_engine(x_hw.data(), w_hw.data(), b_hw.data(), o_hw.data(),
                 (uint16_t)img_h, (uint16_t)img_w, mult, (uint8_t)shift,
                 (uint8_t)(leaky ? 1 : 0));

    unsigned mism = 0;
    size_t total = (size_t)out_h * out_w * OUT_CH;
    for (size_t i = 0; i < total; i++)
        if (pack4_take(o_hw, i) != ref[i]) {
            if (mism < 5)
                printf("[%s]   mismatch @%zu: hw %d vs ref %d\n",
                       name, i, (int)pack4_take(o_hw, i), (int)ref[i]);
            mism++;
        }
    if (mism) { printf("[%s] FAIL: %u/%zu mismatched\n", name, mism, total); return 1; }
    printf("[%s] PASS: %zu output values matched the reference bit-exactly "
           "(pre-padded %ux%ux%u -> %ux%ux%u, k=3, pad=0)\n",
           name, total, img_w, img_h, IN_CH, out_w, out_h, OUT_CH);
    return 0;
}

int main(int argc, char **argv)
{
    bool cosim_only = (argc > 1 && std::strcmp(argv[1], "--cosim-only") == 0);

    // 분류기 conv0 의 재양자화 자리끼우개. 학습 후 manifest 값으로 교체.
    const int32_t STANDIN_MULT  = 1264723901;
    const unsigned STANDIN_SHIFT = 37;

    if (cosim_only) {
        // 96x96 ROI = 98x98 pre-padded. 위치 9,216개.
        //
        // D3 진단 (2026-08-19): 형상을 환경변수로 바꿔 **차분**을 잰다.
        // conv0 의 행당 실측(약 1,572)이 산술 합(1,141)보다 431 크고, 그
        // 431 이 무엇인지 추측으로 손대면 W1/E1/W5 처럼 진다. 높이만 바꾸면
        // 행 1개의 한계비용이, 폭만 바꾸면 열 항들이 직접 분리된다.
        // 기본값은 98x98 이라 **아무것도 설정 안 하면 정본 측정 그대로**다.
        // 그래도 이름과 로그에 형상을 찍어 어느 형상의 수치인지 남긴다.
        unsigned dh = 98, dw = 98;
        if (const char *e = std::getenv("CONV0_DIAG_H")) dh = (unsigned)atoi(e);
        if (const char *e = std::getenv("CONV0_DIAG_W")) dw = (unsigned)atoi(e);
        char nm[64];
        std::snprintf(nm, sizeof nm, "conv0-cosim %ux%u", dh, dw);
        printf("=== COSIM: conv0 3->16, pre-padded %ux%u (out %ux%u) ===\n",
               dh, dw, dh - K + 1, dw - K + 1);
        int f = run_config(nm, dh, dw, 42, true,
                           STANDIN_MULT, STANDIN_SHIFT, /*full_depth=*/true);
        // CONV0_DIAG_NOCHECK: **스로틀 빌드 전용.** 데이터를 일부러 틀리게 만든
        // 빌드는 여기서 반드시 FAIL 하고, TB 가 0 이 아닌 값을 반환하면
        // cosim 이 테스트벡터 생성 단계에서 중단해 **사이클을 못 읽는다.**
        // 이 스위치는 그때만 켠다. 켜지면 배너를 찍어 로그만 봐도 이 실행이
        // 검증이 아니라 측정이었음을 알 수 있게 한다.
        // 이 스위치가 켜진 실행의 결과로 채택 판정을 하지 말 것.
        if (std::getenv("CONV0_DIAG_NOCHECK")) {
            printf("*** CONV0_DIAG_NOCHECK: 데이터 비교 결과(%d)를 무시한다. "
                   "이 실행은 사이클 측정 전용이며 검증이 아니다. ***\n", f);
            printf("COSIM CONFIG PASS (NOCHECK - 검증 아님)\n");
            return 0;
        }
        printf(f ? "COSIM CONFIG FAILED\n" : "COSIM CONFIG PASS\n");
        return f ? 1 : 0;
    }

    int fail = 0;
    fail |= run_config("conv0: ROI 96x96 (98x98 padded)", 98, 98, 42, true,
                       STANDIN_MULT, STANDIN_SHIFT, false);
    fail |= run_config("conv0: ROI 64x64 (66x66 padded)", 66, 66, 43, true,
                       STANDIN_MULT, STANDIN_SHIFT, false);
    // leaky off - 학습이 선형 첫 층을 고를 경우
    fail |= run_config("conv0: leaky off, 34x34", 34, 34, 44, false,
                       STANDIN_MULT, STANDIN_SHIFT, false);
    // 비정사각 / 최소 형상 - 경계 처리
    fail |= run_config("conv0: non-square 18x34", 18, 34, 45, true,
                       STANDIN_MULT, STANDIN_SHIFT, false);
    fail |= run_config("conv0: minimal 5x5", 5, 5, 46, true,
                       STANDIN_MULT, STANDIN_SHIFT, false);

    if (fail) { printf("CLASSIFIER CONV0 SUITE FAILED\n"); return 1; }
    printf("CLASSIFIER CONV0 SUITE PASS (5 shapes)\n");
    return 0;
}
