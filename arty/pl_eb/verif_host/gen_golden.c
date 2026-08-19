// ===========================================================================
// gen_golden.c - PL 경계 golden 생성기 (학습 전 인계용)
//
// 모델이 아직 학습 전이라 **진짜** golden 은 없습니다. 이 도구가 만드는 것은
// 난수 가중치로 만든 **레이아웃/주소 계약 검증용** golden 입니다. 그것으로
// PS 쪽이 학습 완료를 기다리지 않고 다음을 닫을 수 있습니다:
//
//   - NHWC 레이아웃과 바이트 순서
//   - conv0 의 **PRE-PADDED 66x66** 입력 처리
//   - conv0 의 **OIHW** vs conv1/conv2 의 **WPACK** 가중치 구분
//   - 엔진 간 ping-pong 주소 전달
//   - PL 출력이 8x8x64 INT8 이라는 경계
//
// 여기 참조 구현은 conv_engine / conv0_engine / maxpool_engine 의 csim 이
// 비트정확으로 통과한 바로 그 수식입니다(각 TB 의 reference_* 함수와 동일).
//
// 결정론적입니다: 같은 시드 -> 같은 바이트. 시드는 argv[1] (기본 42).
//
//   gcc -O2 -o gen_golden verif_host/gen_golden.c && ./gen_golden 42 out_dir/
// ===========================================================================
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../HW/classifier_net.h"

// ---- 결정론적 PRNG (libc rand() 에 기대지 않는다 - 구현마다 다르다) -------
static uint32_t rng_state;
static void rng_seed(uint32_t s) { rng_state = s ? s : 1u; }
static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int8_t rnd_act(void)  { return (int8_t)((int)(rng_next() % 41u) - 20); }
static int8_t rnd_w(void)    { return (int8_t)((int)(rng_next() % 11u) - 5); }
static int32_t rnd_bias(void){ return (int32_t)(rng_next() % 2001u) - 1000; }

// ---- 참조 수식 (엔진과 비트정확) ------------------------------------------
static int8_t clamp_i8(int64_t v)
{
    return v > 127 ? 127 : (v < -128 ? -128 : (int8_t)v);
}
static int64_t round_shift(int64_t x, unsigned s)
{
    if (!s) return x;
    int64_t half = (int64_t)1 << (s - 1);
    return x >= 0 ? ((x + half) >> s) : -(((-x) + half) >> s);
}

// pad 는 입력 바깥을 0 으로 본다 (conv_engine). conv0 은 pad=0 으로 부르고
// 입력이 이미 패딩돼 있다.
static void ref_conv(const int8_t *x, const int8_t *w, const int32_t *b,
                     int8_t *out, unsigned ih, unsigned iw,
                     unsigned ic_n, unsigned oc_n, unsigned k, unsigned pad,
                     int leaky, int32_t mult, unsigned shift)
{
    unsigned oh = ih + 2 * pad - k + 1, ow = iw + 2 * pad - k + 1;
    for (unsigned r = 0; r < oh; r++)
        for (unsigned c = 0; c < ow; c++)
            for (unsigned oc = 0; oc < oc_n; oc++) {
                int64_t acc = b[oc];
                for (unsigned ic = 0; ic < ic_n; ic++)
                    for (unsigned ky = 0; ky < k; ky++)
                        for (unsigned kx = 0; kx < k; kx++) {
                            int rr = (int)r + (int)ky - (int)pad;
                            int cc = (int)c + (int)kx - (int)pad;
                            int8_t px = 0;
                            if (rr >= 0 && rr < (int)ih && cc >= 0 && cc < (int)iw)
                                px = x[((size_t)rr * iw + cc) * ic_n + ic];
                            // OIHW: [oc][ic][ky][kx] - conv0_engine 의 순서이자
                            // conv_engine TB 참조의 순서다. WPACK 은 **하드웨어가
                            // 읽는 형태**일 뿐 이 논리 순서와 같은 값을 담는다.
                            acc += (int64_t)px * (int64_t)w[((oc * ic_n + ic) * k + ky) * k + kx];
                        }
                if (leaky && acc < 0) acc = round_shift(acc * 13, 7);
                out[((size_t)r * ow + c) * oc_n + oc] =
                    clamp_i8(round_shift(acc * (int64_t)mult, shift));
            }
}

static void ref_maxpool(const int8_t *in, int8_t *out,
                        unsigned ih, unsigned iw, unsigned ch)
{
    unsigned oh = ih / 2, ow = iw / 2;
    for (unsigned r = 0; r < oh; r++)
        for (unsigned c = 0; c < ow; c++)
            for (unsigned k = 0; k < ch; k++) {
                int8_t m = -128;
                for (unsigned dy = 0; dy < 2; dy++)
                    for (unsigned dx = 0; dx < 2; dx++) {
                        int8_t v = in[((size_t)(2 * r + dy) * iw + (2 * c + dx)) * ch + k];
                        if (v > m) m = v;
                    }
                out[((size_t)r * ow + c) * ch + k] = m;
            }
}

static int dump(const char *dir, const char *name, const void *p, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "열기 실패: %s\n", path); return 1; }
    size_t w = fwrite(p, 1, n, f);
    fclose(f);
    if (w != n) { fprintf(stderr, "쓰기 부족: %s (%zu/%zu)\n", path, w, n); return 1; }
    printf("  %-28s %8zu bytes\n", name, n);
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t seed = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 42u;
    const char *dir = (argc > 2) ? argv[2] : "golden";
    mkdir(dir, 0755);
    rng_seed(seed);

    // 자리끼우개 재양자화. 학습 후 manifest 값으로 교체하고 재생성할 것.
    const int32_t MULT[3]  = { 1264723901, 987654321, 123456789 };
    const unsigned SHIFT[3] = { 37, 36, 35 };
    const int LEAKY[3]      = { 1, 1, 0 };

    const unsigned R = ROI_SIZE, P = CONV0_PADDED_SIZE;   // 64, 66
    int fail = 0;

    printf("=== PL 경계 golden (seed=%u, ROI %ux%u) -> %s/ ===\n", seed, R, R, dir);
    printf("⚠️ 가중치는 난수다. **레이아웃/주소 계약** 검증용이지 정확도 검증용이 아니다.\n\n");

    // --- 입력: 64x64x3 ROI, 그리고 conv0 이 실제로 읽는 66x66x3 pre-padded ---
    int8_t *roi  = calloc((size_t)R * R * 3, 1);
    int8_t *pre  = calloc((size_t)P * P * 3, 1);          // 테두리는 calloc 이 0
    for (size_t i = 0; i < (size_t)R * R * 3; i++) roi[i] = rnd_act();
    for (unsigned r = 0; r < R; r++)
        memcpy(&pre[((size_t)(r + CONV0_PAD) * P + CONV0_PAD) * 3],
               &roi[(size_t)r * R * 3], (size_t)R * 3);

    // --- 가중치/바이어스 ---
    int8_t  *w0 = malloc(16 * 3 * 3 * 3);      int32_t b0[16];
    int8_t  *w1 = malloc(32 * 16 * 3 * 3);     int32_t b1[32];
    int8_t  *w2 = malloc(64 * 32 * 3 * 3);     int32_t b2[64];
    for (int i = 0; i < 16 * 3 * 3 * 3; i++)  w0[i] = rnd_w();
    for (int i = 0; i < 16; i++)              b0[i] = rnd_bias();
    for (int i = 0; i < 32 * 16 * 3 * 3; i++) w1[i] = rnd_w();
    for (int i = 0; i < 32; i++)              b1[i] = rnd_bias();
    for (int i = 0; i < 64 * 32 * 3 * 3; i++) w2[i] = rnd_w();
    for (int i = 0; i < 64; i++)              b2[i] = rnd_bias();

    // --- 참조 체인 ---
    int8_t *a0 = malloc((size_t)R * R * 16);          // conv0 out  64x64x16
    int8_t *p0 = malloc((size_t)(R/2) * (R/2) * 16);  // pool0      32x32x16
    int8_t *a1 = malloc((size_t)(R/2) * (R/2) * 32);  // conv1 out  32x32x32
    int8_t *p1 = malloc((size_t)(R/4) * (R/4) * 32);  // pool1      16x16x32
    int8_t *a2 = malloc((size_t)(R/4) * (R/4) * 64);  // conv2 out  16x16x64
    int8_t *p2 = malloc((size_t)(R/8) * (R/8) * 64);  // pool2       8x8x64

    ref_conv(pre, w0, b0, a0, P, P, 3, 16, 3, /*pad=*/0, LEAKY[0], MULT[0], SHIFT[0]);
    ref_maxpool(a0, p0, R, R, 16);
    ref_conv(p0, w1, b1, a1, R/2, R/2, 16, 32, 3, /*pad=*/1, LEAKY[1], MULT[1], SHIFT[1]);
    ref_maxpool(a1, p1, R/2, R/2, 32);
    ref_conv(p1, w2, b2, a2, R/4, R/4, 32, 64, 3, /*pad=*/1, LEAKY[2], MULT[2], SHIFT[2]);
    ref_maxpool(a2, p2, R/4, R/4, 64);

    // ⚠️ 파일 이름을 하드코딩하지 말 것. 2026-08-18 에 128 판을 만들 때
    //    버퍼 크기는 ROI_SIZE 에서 계산돼 따라왔는데 **이름만 64x64 로 남아**
    //    검증기가 66x66 을 찾다가 죽었다. 이름도 형상에서 만든다.
    char nm[128];
    printf("입력:\n");
    snprintf(nm, sizeof nm, "in_roi_%ux%ux3_int8.bin", R, R);
    fail |= dump(dir, nm, roi, (size_t)R * R * 3);
    snprintf(nm, sizeof nm, "in_prepad_%ux%ux3_int8.bin", P, P);
    fail |= dump(dir, nm, pre, (size_t)P * P * 3);
    printf("가중치 (conv0 은 OIHW, conv1/2 는 논리 OIHW - WPACK 은 익스포터가 함):\n");
    fail |= dump(dir, "w_conv0_16x3x3x3_int8.bin",  w0, 16 * 3 * 3 * 3);
    fail |= dump(dir, "b_conv0_16_int32.bin",       b0, sizeof b0);
    fail |= dump(dir, "w_conv1_32x16x3x3_int8.bin", w1, 32 * 16 * 3 * 3);
    fail |= dump(dir, "b_conv1_32_int32.bin",       b1, sizeof b1);
    fail |= dump(dir, "w_conv2_64x32x3x3_int8.bin", w2, 64 * 32 * 3 * 3);
    fail |= dump(dir, "b_conv2_64_int32.bin",       b2, sizeof b2);
    printf("중간 출력 (op 별로 비교하면 어디서 어긋났는지 바로 나온다):\n");
    struct { const char *tag; const void *buf; unsigned e; unsigned c; } mid[] = {
        { "conv0", a0, R,     16 }, { "pool0", p0, R / 2, 16 },
        { "conv1", a1, R / 2, 32 }, { "pool1", p1, R / 4, 32 },
        { "conv2", a2, R / 4, 64 },
    };
    for (unsigned i = 0; i < sizeof mid / sizeof mid[0]; i++) {
        snprintf(nm, sizeof nm, "out_%s_%ux%ux%u_int8.bin",
                 mid[i].tag, mid[i].e, mid[i].e, mid[i].c);
        fail |= dump(dir, nm, mid[i].buf, (size_t)mid[i].e * mid[i].e * mid[i].c);
    }
    printf("PL 최종 출력 (여기까지가 PL, GAP/FC/softmax 는 PS):\n");
    snprintf(nm, sizeof nm, "out_pl_final_%ux%ux64_int8.bin", R / 8, R / 8);
    fail |= dump(dir, nm, p2, (size_t)(R/8)*(R/8)*64);

    // --- 계약 요약 ---
    char path[512];
    snprintf(path, sizeof path, "%s/CONTRACT.txt", dir);
    FILE *m = fopen(path, "w");
    if (m) {
        fprintf(m,
            "PL 경계 계약 (seed=%u)\n"
            "=====================\n"
            "배열 순서   : NHWC (채널 최내측). 모든 텐서.\n"
            "dtype       : INT8 signed (-128..127). 바이어스만 INT32 little-endian.\n"
            "zero-point  : 0 고정. 엔진에 zero-point 뺄셈 경로가 없다 (symmetric only).\n"
            "\n"
            "conv0 (전용 엔진 conv0_engine)\n"
            "  입력      : %ux%ux3 **PRE-PADDED** (0 테두리 %u픽셀). pad 포트 없음.\n"
            "  가중치    : OIHW [16][3][3][3] = 432 B, **전치 없음**\n"
            "  출력      : %ux%ux16\n"
            "  ⚠️ WPACK 전치본도 432 B 라 잘못 넣어도 에러가 안 난다.\n"
            "\n"
            "conv1/conv2 (공유 엔진 conv_engine, TR=8)\n"
            "  pad=1 을 엔진이 처리. 입력은 패딩하지 말 것.\n"
            "  가중치    : 익스포터가 WPACK 으로 전치. 논리 순서는 OIHW.\n"
            "  weights_hi: weights 와 **같은 주소**를 프로그램 (드라이버가 자동).\n"
            "\n"
            "maxpool     : 2x2 stride 2, pad 없음.\n"
            "\n"
            "PL 최종 출력: %ux%ux64 INT8 NHWC.\n"
            "PS 몫       : GAP(%ux%u 평균) -> 64, FC 64xN -> INT32 logits, argmax/softmax.\n"
            "  나눗수 1/%u (= %ux%u) 는 GAP 과 FC scale 중 **한 쪽에서만** 적용할 것.\n"
            "  ⚠️ 이 값은 ROI 해상도에 따라 다르다 (64->64, 96->144, 128->256).\n"
            "     양쪽에 다 적용하면 argmax 는 그대로 맞고 confidence 만 틀린다.\n"
            "\n"
            "재양자화 (자리끼우개 - 학습 후 manifest 값으로 교체 후 재생성)\n"
            "  conv0: mult=%d shift=%u leaky=%d\n"
            "  conv1: mult=%d shift=%u leaky=%d\n"
            "  conv2: mult=%d shift=%u leaky=%d\n",
            seed, P, P, CONV0_PAD, R, R, R/8, R/8, R/8, R/8,
            (R/8)*(R/8), R/8, R/8,
            MULT[0], SHIFT[0], LEAKY[0], MULT[1], SHIFT[1], LEAKY[1],
            MULT[2], SHIFT[2], LEAKY[2]);
        fclose(m);
        printf("\n  %-28s (계약 요약)\n", "CONTRACT.txt");
    } else { fail = 1; }

    if (fail) { printf("\nGOLDEN 생성 실패\n"); return 1; }
    printf("\nGOLDEN 생성 완료: %s/\n", dir);
    return 0;
}
