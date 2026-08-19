// ===========================================================================
// maxpool_engine_tb.cpp - ARTY CLASSIFIER FORK (2026-08-18)
//
// Forked from hls/zybo_pool_upsample_route/HW/pool_upsample_route_tb.cpp.
// Delta: upsample_engine and route_concat_engine are gone (the classifier
// has no route or upsample op, so those IPs are not in this build), and
// real_pool_upsample_route_data.h (23 MB of YOLO activations) is dropped -
// the classifier's weights do not exist yet.
//
// Kept verbatim: the pack4 helpers, reference_maxpool(), and
// run_maxpool_config(). Their comments still apply, including the reason the
// reference model stays unpacked int8_t.
// ===========================================================================
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "maxpool_engine.h"

// Full m_axi interface depths - must match each engine's own `#pragma HLS
// INTERFACE m_axi ... depth=...` expression exactly. Only matters for
// cosim (plain C-sim never uses `depth`) - see conv_engine_tb.cpp's own
// IFMAP_DEPTH etc. for the same requirement and the SIGSEGV it avoids.
static const size_t MAXPOOL_IFMAP_DEPTH = (size_t)MAX_IMG_W * MAX_IMG_W * MAX_CH;
static const size_t MAXPOOL_OFMAP_DEPTH = (size_t)MAX_IMG_W * MAX_IMG_W * MAX_CH;

// ---------------------------------------------------------------------------
// pack4 testbench helpers
//
// The engines' m_axi ports are `pack4_t` (4 int8 channels per 32-bit beat -
// see HW/pack4.h), so every device-side buffer below is allocated and
// passed in WORDS. The reference_*() models above are deliberately left
// entirely int8_t-based and completely unaware of the packing: keeping the
// golden path unpacked is what makes a PASS here evidence that the packing
// is bit-exact, rather than evidence that both sides happen to share the
// same packing bug. Same "no shared code path" reasoning this file already
// documents for reference_conv()-style independence.
//
// The *_DEPTH constants above stay in ELEMENTS, so they still read as each
// engine's own img/ch bound product and remain directly comparable against
// the baseline project's values; pack4_words() converts at each point of
// use instead.
// ---------------------------------------------------------------------------
static inline size_t pack4_words(size_t elems) {
    return (elems + PACK4_LANES - 1) / PACK4_LANES;
}

// Flat int8 element index -> (word, lane), matching pack4.h's little-endian
// lane order: lane 0 is the lowest channel index and the low byte. This is
// the same mapping the DDR byte layout already had, which is exactly why
// no PS-side repacking pass is needed in real hardware - here it is only
// spelled out explicitly because the tb holds its golden data in a
// separate int8_t vector.
static inline void pack4_put(std::vector<pack4_t> &buf, size_t elem_idx, int8_t v) {
    pack4_set(buf[elem_idx / PACK4_LANES], (unsigned)(elem_idx % PACK4_LANES), (ap_int<8>)v);
}

static inline int8_t pack4_take(const std::vector<pack4_t> &buf, size_t elem_idx) {
    return (int8_t)pack4_get(buf[elem_idx / PACK4_LANES],
                             (unsigned)(elem_idx % PACK4_LANES)).to_int();
}

// ---------------------------------------------------------------------------
// C-simulation testbench for maxpool_engine. The fork source shared one tb
// binary across maxpool/upsample/route_concat; this build has only maxpool,
// so the other two runners are gone. Each op gets
// its own reference_*() implementation below, deliberately NOT sharing any
// code path with the engine .cpp files (whose helpers are file-local static
// functions anyway) - same "a bug in one has no way to silently agree with
// the other" reasoning conv_engine_tb.cpp already documents for its own
// reference_conv().
// ---------------------------------------------------------------------------

static int8_t clamp_i8(int64_t v) {
    if (v > 127)  return 127;
    if (v < -128) return -128;
    return (int8_t)v;
}

// Plain int64_t implementation of RTL_HANDOFF_KO.md section 5's
// round_shift formula - independent from route_concat_engine.cpp's own
// ap_int<64> copy, same reasoning as conv_engine_tb.cpp's round_shift_ref().
static int64_t round_shift_ref(int64_t x, unsigned s) {
    if (s == 0) return x;
    int64_t half = (int64_t)1 << (s - 1);
    return (x >= 0) ? ((x + half) >> s) : -(((-x) + half) >> s);
}

static void reference_maxpool(
    const std::vector<int8_t> &in, std::vector<int8_t> &out,
    unsigned img_h, unsigned img_w, unsigned ch,
    unsigned stride, unsigned pad_right, unsigned pad_bottom
) {
    unsigned out_h = (img_h + pad_bottom - 2) / stride + 1;
    unsigned out_w = (img_w + pad_right - 2) / stride + 1;
    out.assign((size_t)out_h * out_w * ch, 0);

    for (unsigned r = 0; r < out_h; r++) {
        for (unsigned c = 0; c < out_w; c++) {
            for (unsigned cc = 0; cc < ch; cc++) {
                int8_t mv = -128;
                for (unsigned dy = 0; dy < 2; dy++) {
                    for (unsigned dx = 0; dx < 2; dx++) {
                        unsigned in_r = r * stride + dy;
                        unsigned in_c = c * stride + dx;
                        int8_t v = -128;
                        if (in_r < img_h && in_c < img_w) {
                            v = in[((size_t)in_r * img_w + in_c) * ch + cc];
                        }
                        if (v > mv) mv = v;
                    }
                }
                out[((size_t)r * out_w + c) * ch + cc] = mv;
            }
        }
    }
}

static int run_maxpool_config(
    const char *name,
    unsigned img_h, unsigned img_w, unsigned ch,
    unsigned stride, unsigned pad_right, unsigned pad_bottom,
    unsigned seed, bool border_stress = false, bool pad_to_full_depth = false
) {
    srand(seed);
    std::vector<int8_t> in((size_t)img_h * img_w * ch);
    for (auto &v : in) {
        // border_stress (MP-H): real values near -100, deliberately close
        // to the -128 pad value, so a naive "pad with 0" bug (which would
        // let a real -50 value lose to a padded 0) would fail this config
        // even though it would pass every "normal" config above it.
        v = border_stress ? (int8_t)(-100 + (rand() % 20)) : (int8_t)((rand() % 200) - 100);
    }

    std::vector<int8_t> ref_out;
    reference_maxpool(in, ref_out, img_h, img_w, ch, stride, pad_right, pad_bottom);

    unsigned out_h = (img_h + pad_bottom - 2) / stride + 1;
    unsigned out_w = (img_w + pad_right - 2) / stride + 1;

    // Only the front sub-region (this config's real data) is filled when
    // pad_to_full_depth is set - maxpool_engine() never reads/writes past
    // its own img_h/img_w/ch-derived bounds, so the rest is inert padding
    // that exists purely to match the m_axi depth= pragma for cosim (see
    // MAXPOOL_IFMAP_DEPTH above / conv_engine_tb.cpp's identical convention).
    size_t ifmap_size = pad_to_full_depth ? MAXPOOL_IFMAP_DEPTH : in.size();
    size_t ofmap_size = pad_to_full_depth ? MAXPOOL_OFMAP_DEPTH : (size_t)out_h * out_w * ch;
    std::vector<pack4_t> in_hw(pack4_words(ifmap_size), (pack4_t)0);
    for (size_t i = 0; i < in.size(); i++) pack4_put(in_hw, i, in[i]);
    std::vector<pack4_t> hw_out(pack4_words(ofmap_size), (pack4_t)0);

    maxpool_engine(in_hw.data(), hw_out.data(),
                   (uint16_t)img_h, (uint16_t)img_w, (uint16_t)ch,
                   (uint8_t)stride, (uint8_t)pad_right, (uint8_t)pad_bottom);

    unsigned mismatches = 0;
    unsigned total = out_h * out_w * ch;
    for (unsigned i = 0; i < total; i++) {
        int8_t hw_val = pack4_take(hw_out, i);
        int8_t ref_val = ref_out[i];
        if (hw_val != ref_val) {
            if (mismatches < 10) {
                printf("  [%s] MISMATCH at flat idx=%u: hw=%d ref=%d\n", name, i, hw_val, ref_val);
            }
            mismatches++;
        }
    }

    if (mismatches == 0) {
        printf("[%s] PASS: %u output values matched the reference bit-exactly "
               "(img %ux%ux%u -> %ux%ux%u, stride=%u, pad_right=%u, pad_bottom=%u)\n",
               name, total, img_w, img_h, ch, out_w, out_h, ch, stride, pad_right, pad_bottom);
        return 0;
    } else {
        printf("[%s] FAIL: %u/%u values mismatched\n", name, mismatches, total);
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Classifier maxpool shapes (../../HW/classifier_net.h). All three are 2x2
// stride 2 with no padding, and all three have img_w * ch/4 == 512 words -
// see the row-buffer comment in maxpool_engine.h.
// ---------------------------------------------------------------------------
struct ClsPool {
    const char *name;
    unsigned img_h, img_w, ch;
};

static const ClsPool CLS_POOLS[] = {
    { "pool0 (96x96x16)",    96,  96, 16 },
    { "pool1 (48x48x32)",    48,  48, 32 },
    { "pool2 (24x24x64)",    24,  24, 64 },
};
static const unsigned NUM_CLS_POOLS = sizeof(CLS_POOLS) / sizeof(CLS_POOLS[0]);

int main(int argc, char **argv) {
    bool cosim_only = (argc > 1 && std::strcmp(argv[1], "--cosim-only") == 0);
    int cosim_op = 0;
    if (cosim_only && argc > 2) {
        cosim_op = std::atoi(argv[2]);
        if (cosim_op < 0 || cosim_op >= (int)NUM_CLS_POOLS) {
            printf("--cosim-only: pool index %d out of range [0,%u), using 0\n",
                   cosim_op, NUM_CLS_POOLS);
            cosim_op = 0;
        }
    }

    int fail = 0;

    if (cosim_only) {
        const ClsPool &p = CLS_POOLS[cosim_op];
        printf("=== COSIM pool %d: %s ===\n", cosim_op, p.name);
        fail = run_maxpool_config(p.name, p.img_h, p.img_w, p.ch,
                                  /*stride=*/2, /*pad_right=*/0, /*pad_bottom=*/0,
                                  /*seed=*/42, /*border_stress=*/false,
                                  /*pad_to_full_depth=*/true);
        printf(fail ? "COSIM CONFIG FAILED\n" : "COSIM CONFIG PASS\n");
        return fail ? 1 : 0;
    }

    for (unsigned i = 0; i < NUM_CLS_POOLS; i++) {
        const ClsPool &p = CLS_POOLS[i];
        fail |= run_maxpool_config(p.name, p.img_h, p.img_w, p.ch,
                                   /*stride=*/2, /*pad_right=*/0, /*pad_bottom=*/0,
                                   /*seed=*/42 + i);
    }
    // Odd spatial size - a 32x32 ROI after two pools is 8x8, but if the
    // training side ever picks a non-power-of-two edge the pad path runs.
    fail |= run_maxpool_config("extra: odd 15x15x16", 15, 15, 16,
                               /*stride=*/2, /*pad_right=*/0, /*pad_bottom=*/0,
                               /*seed=*/9);

    if (fail) {
        printf("CLASSIFIER MAXPOOL SUITE FAILED\n");
        return 1;
    }
    printf("CLASSIFIER MAXPOOL SUITE PASS (%u shapes)\n", NUM_CLS_POOLS + 1);
    return 0;
}
