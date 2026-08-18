#include "classifier_engine.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

// -----------------------------------------------------------------------------
// Deterministic bit-exact C/RTL co-simulation testbench.
//
// Purpose
//   1) exercise real non-zero convolution math (not a zero-weight smoke test),
//   2) verify conv0 OIHW indexing,
//   3) verify conv1/conv2 WPACK [oc][ky][kx][ic] indexing,
//   4) verify conv0 98x98 pre-padding and conv1/conv2 SAME padding,
//   5) verify requant multiplier/shift + ReLU + INT8 clamp,
//   6) verify all three 2x2 max-pool stages,
//   7) automatically compare classifier_top() output against an independent
//      plain-integer software reference in both C simulation and RTL co-sim.
//
// No std::random_device / runtime-random input is used. Every value is generated
// by a fixed integer formula, so C simulation and RTL co-simulation receive the
// exact same vectors on every run.
// -----------------------------------------------------------------------------

static int32_t arshift_ref(int64_t value, unsigned shift) {
    if (shift == 0) return (int32_t)value;
    if (value >= 0) return (int32_t)(value >> shift);

    // Emulate arithmetic right shift (floor toward -infinity) explicitly.
    const int64_t denom_mask = (((int64_t)1 << shift) - 1);
    const int64_t mag = -value;
    return (int32_t)(-((mag + denom_mask) >> shift));
}

static int8_t requant_relu_ref(int32_t acc, int32_t multiplier, unsigned shift) {
    int64_t scaled = (int64_t)acc * (int64_t)multiplier;
    int32_t q = arshift_ref(scaled, shift);
    if (q < 0) q = 0;
    if (q > 127) q = 127;
    return (int8_t)q;
}

static void make_vectors(
    act_t ifmap_padded[C0_PAD_SIZE][C0_PAD_SIZE][C0_IN_CH],
    weight_t w_conv0[C0_OUT_CH][C0_IN_CH][K][K],
    bias_t b_conv0[C0_OUT_CH],
    weight_t w_conv1[C1_OUT_CH][K][K][C1_IN_CH],
    bias_t b_conv1[C1_OUT_CH],
    weight_t w_conv2[C2_OUT_CH][K][K][C2_IN_CH],
    bias_t b_conv2[C2_OUT_CH]
) {
    // Exact PL contract: 96x96 ROI surrounded by a one-pixel INT8-zero border.
    std::memset(ifmap_padded, 0, sizeof(act_t) * C0_PAD_SIZE * C0_PAD_SIZE * C0_IN_CH);
    for (unsigned y = 0; y < C0_OUT_SIZE; ++y) {
        for (unsigned x = 0; x < C0_OUT_SIZE; ++x) {
            for (unsigned c = 0; c < C0_IN_CH; ++c) {
                int v = (int)((y * 17u + x * 13u + c * 29u + ((x * y) % 31u)) % 127u) - 63;
                ifmap_padded[y + 1][x + 1][c] = (act_t)v;
            }
        }
    }

    // conv0: OIHW [oc][ic][ky][kx], values in [-2, 2].
    for (unsigned oc = 0; oc < C0_OUT_CH; ++oc) {
        b_conv0[oc] = (bias_t)((int)(oc % 9u) - 4);
        for (unsigned ic = 0; ic < C0_IN_CH; ++ic)
            for (unsigned ky = 0; ky < K; ++ky)
                for (unsigned kx = 0; kx < K; ++kx) {
                    int w = (int)((oc * 7u + ic * 5u + ky * 3u + kx * 11u) % 5u) - 2;
                    w_conv0[oc][ic][ky][kx] = (weight_t)w;
                }
    }

    // conv1: WPACK [oc][ky][kx][ic], values in [-2, 2].
    for (unsigned oc = 0; oc < C1_OUT_CH; ++oc) {
        b_conv1[oc] = (bias_t)((int)(oc % 13u) - 6);
        for (unsigned ky = 0; ky < K; ++ky)
            for (unsigned kx = 0; kx < K; ++kx)
                for (unsigned ic = 0; ic < C1_IN_CH; ++ic) {
                    int w = (int)((oc * 7u + ky * 5u + kx * 3u + ic * 11u) % 5u) - 2;
                    w_conv1[oc][ky][kx][ic] = (weight_t)w;
                }
    }

    // conv2: WPACK [oc][ky][kx][ic], values in [-2, 2].
    for (unsigned oc = 0; oc < C2_OUT_CH; ++oc) {
        b_conv2[oc] = (bias_t)((int)(oc % 17u) - 8);
        for (unsigned ky = 0; ky < K; ++ky)
            for (unsigned kx = 0; kx < K; ++kx)
                for (unsigned ic = 0; ic < C2_IN_CH; ++ic) {
                    int w = (int)((oc * 3u + ky * 7u + kx * 5u + ic * 11u) % 5u) - 2;
                    w_conv2[oc][ky][kx][ic] = (weight_t)w;
                }
    }
}

static void ref_conv0(
    const act_t ifmap[C0_PAD_SIZE][C0_PAD_SIZE][C0_IN_CH],
    const weight_t w[C0_OUT_CH][C0_IN_CH][K][K],
    const bias_t b[C0_OUT_CH],
    int32_t multiplier,
    unsigned shift,
    int8_t out[C0_OUT_SIZE][C0_OUT_SIZE][C0_OUT_CH]
) {
    for (unsigned oy = 0; oy < C0_OUT_SIZE; ++oy) {
        for (unsigned ox = 0; ox < C0_OUT_SIZE; ++ox) {
            for (unsigned oc = 0; oc < C0_OUT_CH; ++oc) {
                int32_t acc = (int32_t)b[oc];
                for (unsigned ic = 0; ic < C0_IN_CH; ++ic)
                    for (unsigned ky = 0; ky < K; ++ky)
                        for (unsigned kx = 0; kx < K; ++kx)
                            acc += (int32_t)((int)ifmap[oy + ky][ox + kx][ic]) *
                                   (int32_t)((int)w[oc][ic][ky][kx]);
                out[oy][ox][oc] = requant_relu_ref(acc, multiplier, shift);
            }
        }
    }
}

template<unsigned CH, unsigned IN_SIZE, unsigned OUT_SIZE>
static void ref_pool(
    const int8_t in[IN_SIZE][IN_SIZE][CH],
    int8_t out[OUT_SIZE][OUT_SIZE][CH]
) {
    for (unsigned oy = 0; oy < OUT_SIZE; ++oy) {
        for (unsigned ox = 0; ox < OUT_SIZE; ++ox) {
            for (unsigned c = 0; c < CH; ++c) {
                int8_t m = in[oy * 2][ox * 2][c];
                for (unsigned py = 0; py < 2; ++py)
                    for (unsigned px = 0; px < 2; ++px)
                        if (in[oy * 2 + py][ox * 2 + px][c] > m)
                            m = in[oy * 2 + py][ox * 2 + px][c];
                out[oy][ox][c] = m;
            }
        }
    }
}

template<unsigned IN_CH, unsigned OUT_CH, unsigned SIZE>
static void ref_conv_same_wpack(
    const int8_t ifmap[SIZE][SIZE][IN_CH],
    const weight_t w[OUT_CH][K][K][IN_CH],
    const bias_t b[OUT_CH],
    int32_t multiplier,
    unsigned shift,
    int8_t out[SIZE][SIZE][OUT_CH]
) {
    for (unsigned oy = 0; oy < SIZE; ++oy) {
        for (unsigned ox = 0; ox < SIZE; ++ox) {
            for (unsigned oc = 0; oc < OUT_CH; ++oc) {
                int32_t acc = (int32_t)b[oc];
                for (unsigned ky = 0; ky < K; ++ky) {
                    for (unsigned kx = 0; kx < K; ++kx) {
                        int iy = (int)oy - 1 + (int)ky;
                        int ix = (int)ox - 1 + (int)kx;
                        if (iy < 0 || iy >= (int)SIZE || ix < 0 || ix >= (int)SIZE)
                            continue;
                        for (unsigned ic = 0; ic < IN_CH; ++ic) {
                            acc += (int32_t)ifmap[iy][ix][ic] *
                                   (int32_t)((int)w[oc][ky][kx][ic]);
                        }
                    }
                }
                out[oy][ox][oc] = requant_relu_ref(acc, multiplier, shift);
            }
        }
    }
}

static uint64_t fnv1a_output(const int8_t out[P2_OUT_SIZE][P2_OUT_SIZE][C2_OUT_CH]) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned y = 0; y < P2_OUT_SIZE; ++y)
        for (unsigned x = 0; x < P2_OUT_SIZE; ++x)
            for (unsigned c = 0; c < C2_OUT_CH; ++c) {
                h ^= (uint8_t)out[y][x][c];
                h *= 1099511628211ULL;
            }
    return h;
}

int main() {
    static act_t ifmap_padded[C0_PAD_SIZE][C0_PAD_SIZE][C0_IN_CH];
    static weight_t w_conv0[C0_OUT_CH][C0_IN_CH][K][K];
    static bias_t b_conv0[C0_OUT_CH];
    static weight_t w_conv1[C1_OUT_CH][K][K][C1_IN_CH];
    static bias_t b_conv1[C1_OUT_CH];
    static weight_t w_conv2[C2_OUT_CH][K][K][C2_IN_CH];
    static bias_t b_conv2[C2_OUT_CH];

    make_vectors(ifmap_padded, w_conv0, b_conv0,
                 w_conv1, b_conv1, w_conv2, b_conv2);

    // Non-trivial requantization values exercise multiplier AND shift paths.
    // They keep this deterministic vector away from wholesale saturation.
    const requant_t rq_conv0 = {3, 5};
    const requant_t rq_conv1 = {5, 6};
    const requant_t rq_conv2 = {7, 7};

    // Verify the explicit conv0 border contract before running the DUT.
    unsigned border_errors = 0;
    for (unsigned x = 0; x < C0_PAD_SIZE; ++x)
        for (unsigned c = 0; c < C0_IN_CH; ++c) {
            if ((int)ifmap_padded[0][x][c] != 0) ++border_errors;
            if ((int)ifmap_padded[C0_PAD_SIZE - 1][x][c] != 0) ++border_errors;
        }
    for (unsigned y = 1; y + 1 < C0_PAD_SIZE; ++y)
        for (unsigned c = 0; c < C0_IN_CH; ++c) {
            if ((int)ifmap_padded[y][0][c] != 0) ++border_errors;
            if ((int)ifmap_padded[y][C0_PAD_SIZE - 1][c] != 0) ++border_errors;
        }
    if (border_errors != 0) {
        std::printf("FAIL: conv0 98x98 pre-pad border has %u non-zero values\n", border_errors);
        return 1;
    }

    // Independent plain-integer software reference.
    static int8_t ref_f0[C0_OUT_SIZE][C0_OUT_SIZE][C0_OUT_CH];
    static int8_t ref_p0[P0_OUT_SIZE][P0_OUT_SIZE][C0_OUT_CH];
    static int8_t ref_f1[C1_SIZE][C1_SIZE][C1_OUT_CH];
    static int8_t ref_p1[P1_OUT_SIZE][P1_OUT_SIZE][C1_OUT_CH];
    static int8_t ref_f2[C2_SIZE][C2_SIZE][C2_OUT_CH];
    static int8_t golden[P2_OUT_SIZE][P2_OUT_SIZE][C2_OUT_CH];

    ref_conv0(ifmap_padded, w_conv0, b_conv0,
              rq_conv0.multiplier, (unsigned)rq_conv0.shift, ref_f0);
    ref_pool<C0_OUT_CH, C0_OUT_SIZE, P0_OUT_SIZE>(ref_f0, ref_p0);
    ref_conv_same_wpack<C1_IN_CH, C1_OUT_CH, C1_SIZE>(
        ref_p0, w_conv1, b_conv1,
        rq_conv1.multiplier, (unsigned)rq_conv1.shift, ref_f1);
    ref_pool<C1_OUT_CH, C1_SIZE, P1_OUT_SIZE>(ref_f1, ref_p1);
    ref_conv_same_wpack<C2_IN_CH, C2_OUT_CH, C2_SIZE>(
        ref_p1, w_conv2, b_conv2,
        rq_conv2.multiplier, (unsigned)rq_conv2.shift, ref_f2);
    ref_pool<C2_OUT_CH, C2_SIZE, P2_OUT_SIZE>(ref_f2, golden);

    // DUT: in csim this calls the HLS C model; in cosim Vitis replaces this
    // transaction with the generated RTL wrapper and compares via this TB.
    static act_t dut_out[P2_OUT_SIZE][P2_OUT_SIZE][C2_OUT_CH];
    classifier_top(ifmap_padded,
                   w_conv0, b_conv0, rq_conv0,
                   w_conv1, b_conv1, rq_conv1,
                   w_conv2, b_conv2, rq_conv2,
                   dut_out);

    unsigned mismatches = 0;
    unsigned printed = 0;
    int out_min = 127;
    int out_max = 0;
    uint64_t dut_hash = 1469598103934665603ULL;

    for (unsigned y = 0; y < P2_OUT_SIZE; ++y) {
        for (unsigned x = 0; x < P2_OUT_SIZE; ++x) {
            for (unsigned c = 0; c < C2_OUT_CH; ++c) {
                int got = (int)dut_out[y][x][c];
                int exp = (int)golden[y][x][c];
                if (got < out_min) out_min = got;
                if (got > out_max) out_max = got;
                dut_hash ^= (uint8_t)got;
                dut_hash *= 1099511628211ULL;

                if (got != exp) {
                    ++mismatches;
                    if (printed < 20) {
                        std::printf("MISMATCH[%u]: y=%u x=%u c=%u got=%d expected=%d\n",
                                    mismatches, y, x, c, got, exp);
                        ++printed;
                    }
                }
            }
        }
    }

    const unsigned total = P2_OUT_SIZE * P2_OUT_SIZE * C2_OUT_CH;
    const uint64_t golden_hash = fnv1a_output(golden);

    std::printf("------------------------------------------------------------\n");
    std::printf("Z7 classifier deterministic bit-exact test\n");
    std::printf("input      : 98x98x3 NHWC INT8 (1px zero border)\n");
    std::printf("PL output  : 12x12x64 NHWC INT8 (%u values)\n", total);
    std::printf("requant    : c0=(3,5), c1=(5,6), c2=(7,7) [mult,shift]\n");
    std::printf("output range: %d .. %d\n", out_min, out_max);
    std::printf("golden hash: 0x%016llx\n", (unsigned long long)golden_hash);
    std::printf("dut hash   : 0x%016llx\n", (unsigned long long)dut_hash);
    std::printf("mismatches : %u\n", mismatches);
    std::printf("------------------------------------------------------------\n");

    if (mismatches != 0 || dut_hash != golden_hash) {
        std::printf("FAIL: classifier_top != independent software golden\n");
        return 1;
    }

    std::printf("PASS: classifier_top is bit-exact against software golden\n");
    std::printf("This PASS is valid for both csim and C/RTL cosim runs.\n");
    return 0;
}
