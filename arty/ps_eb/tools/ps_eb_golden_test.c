#define _POSIX_C_SOURCE 200809L

/*
 * EB 온보드 골든 테스트.
 *
 * 6-op 을 **하나씩** 돌리고 매 단계 출력을 인계 패키지의 golden 과 바이트로
 * 대조한다. 통째로 돌려 최종만 보면 "틀렸다"는 것만 알고 어디서 갈렸는지는
 * 모른다 - 엔진이 3개고 op 이 6개라 후보가 여섯이다.
 *
 * golden 은 arty/pl_eb/golden/ 이고 **실제 학습 가중치**로 만든 것이다.
 *
 * ⚠️ 가중치는 golden 폴더가 아니라 weights 폴더(= models/roi_classifier_int8_eb
 *    /export/) 것을 쓴다. golden/w_conv*.bin 은 참조 모델용 OIHW 사본이고,
 *    conv1/conv2 는 크기만 같고 레이아웃이 다르다.
 */

#include "driver/classifier_device.h"
#include "model/classifier_model.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PATH 512

/* 단계별 golden 파일과 그 크기. 순서는 6-op 실행 순서와 같다. */
struct golden_step {
    const char* file;
    size_t bytes;
    /* 이 op 의 출력이 act_a 인지 act_b 인지. buffers_for_op 과 같은 규칙. */
    int destination_is_act_b;
};

static const struct golden_step GOLDEN_STEPS[ADAS_EB_NUM_OPS] = {
    { "out_conv0_96x96x16_int8.bin",  96u * 96u * 16u, 0 },
    { "out_pool0_48x48x16_int8.bin",  48u * 48u * 16u, 1 },
    { "out_conv1_48x48x32_int8.bin",  48u * 48u * 32u, 0 },
    { "out_pool1_24x24x32_int8.bin",  24u * 24u * 32u, 1 },
    { "out_conv2_24x24x64_int8.bin",  24u * 24u * 64u, 0 },
    { "out_pl_final_12x12x64_int8.bin", 12u * 12u * 64u, 1 },
};

static const char* OP_NAMES[ADAS_EB_NUM_OPS] = {
    "conv0", "pool0", "conv1", "pool1", "conv2", "pool2"
};

static int join_path(char* out, size_t out_size,
                     const char* directory, const char* name) {
    const int written = snprintf(out, out_size, "%s/%s", directory, name);
    return (written < 0 || (size_t)written >= out_size) ? -1 : 0;
}

/* 정확히 expected 바이트여야 한다. 크기가 다르면 다른 판의 파일이다. */
static int read_exact(const char* path, void* buffer, size_t expected) {
    FILE* const file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "  열 수 없음: %s (%s)\n", path, strerror(errno));
        return -1;
    }
    const size_t read_bytes = fread(buffer, 1u, expected, file);
    int extra = fgetc(file);
    fclose(file);
    if (read_bytes != expected || extra != EOF) {
        fprintf(stderr, "  크기 불일치: %s (기대 %zu B)\n", path, expected);
        return -1;
    }
    return 0;
}

/* 첫 불일치 위치와 총 개수를 함께 낸다. 위치가 곧 어느 채널·행인지의 단서다. */
static size_t compare_bytes(const int8_t* actual, const int8_t* expected,
                            size_t bytes, size_t* first_mismatch) {
    size_t mismatches = 0u;
    *first_mismatch = bytes;
    for (size_t i = 0u; i < bytes; ++i) {
        if (actual[i] != expected[i]) {
            if (mismatches == 0u) {
                *first_mismatch = i;
            }
            ++mismatches;
        }
    }
    return mismatches;
}

static uint64_t elapsed_us(const struct timespec* start,
                           const struct timespec* end) {
    const int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    const int64_t nanoseconds = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    const int64_t total = seconds * 1000000 + nanoseconds / 1000;
    return total > 0 ? (uint64_t)total : 0u;
}

static void print_usage(const char* program) {
    fprintf(stderr,
        "usage: %s <model-dir> <golden-dir> [device] [timeout-ms]\n"
        "\n"
        "  model-dir  : arty/models/roi_classifier_int8_eb/export\n"
        "  golden-dir : arty/pl_eb/golden\n"
        "  device     : 기본 /dev/adas_classifier\n"
        "\n"
        "requant 값은 golden-dir 이 아니라 model-dir 의 manifest 를 따른다.\n",
        program);
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char* const model_dir = argv[1];
    const char* const golden_dir = argv[2];
    const char* const device_path = (argc >= 4) ? argv[3] : "/dev/adas_classifier";
    const uint32_t timeout_ms = (argc >= 5) ? (uint32_t)strtoul(argv[4], NULL, 10)
                                            : 2000u;
    if (timeout_ms == 0u) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * EB export 의 manifest 값이다. 여기 박아 두는 이유는 이 도구가
     * "지금 올라간 비트스트림이 학습 결과를 재현하는가"만 보기 때문이다 -
     * 값을 인자로 받으면 잘못된 값으로 통과시키는 실행이 생긴다.
     */
    const adas_classifier_model_metadata_t metadata = {
        .class_count = 6u,
        .gap_divisor = 1u,
        .rq_conv0 = { 1545298110, 37u, 1u },
        .rq_conv1 = { 1525725976, 36u, 1u },
        .rq_conv2 = { 1924470265, 39u, 0u }
    };

    adas_classifier_model_t model;
    adas_classifier_model_init(&model);
    if (adas_classifier_model_load(&model, model_dir, &metadata)
        != ADAS_CLASSIFIER_MODEL_OK) {
        fprintf(stderr, "모델 로드 실패: %s\n", model_dir);
        return EXIT_FAILURE;
    }

    adas_classifier_device_t device;
    adas_classifier_device_init(&device);
    if (adas_classifier_device_open(&device, device_path)
        != ADAS_CLASSIFIER_DEVICE_OK) {
        fprintf(stderr, "장치 열기 실패: %s (%s)\n", device_path, strerror(errno));
        fprintf(stderr, "  ABI 불일치라면 DB 드라이버가 올라가 있는 것이다.\n");
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }

    /* 가중치와 bias 를 DMA 버퍼로. EB 는 bias 도 DDR 이다. */
    memcpy(adas_classifier_device_w_conv0(&device), model.w_conv0,
           sizeof(model.w_conv0));
    memcpy(adas_classifier_device_w_conv1(&device), model.w_conv1,
           sizeof(model.w_conv1));
    memcpy(adas_classifier_device_w_conv2(&device), model.w_conv2,
           sizeof(model.w_conv2));
    memcpy(adas_classifier_device_b_conv0(&device), model.b_conv0,
           sizeof(model.b_conv0));
    memcpy(adas_classifier_device_b_conv1(&device), model.b_conv1,
           sizeof(model.b_conv1));
    memcpy(adas_classifier_device_b_conv2(&device), model.b_conv2,
           sizeof(model.b_conv2));

    if (adas_classifier_device_load_parameters(&device, &model.pl_parameters)
        != ADAS_CLASSIFIER_DEVICE_OK) {
        fprintf(stderr, "requant 설정 실패\n");
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }

    /*
     * 입력은 **이미 패딩된** 98x98x3 이다. PS 전처리를 거치지 않고 golden
     * 입력을 그대로 넣는다 - 여기서 보려는 것은 PL 이지 전처리가 아니다.
     */
    char path[MAX_PATH];
    if (join_path(path, sizeof(path), golden_dir, "in_prepad_98x98x3_int8.bin") != 0
        || read_exact(path, adas_classifier_device_ifmap(&device),
                      ADAS_CLASSIFIER_IFMAP_SIZE) != 0) {
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }

    printf("EB golden test: model=%s golden=%s device=%s\n",
           model_dir, golden_dir, device_path);

    int8_t* const expected = (int8_t*)malloc(ADAS_CLASSIFIER_ACT_SIZE);
    if (expected == NULL) {
        fprintf(stderr, "메모리 부족\n");
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }

    int failed = 0;
    uint64_t total_us = 0u;
    for (unsigned i = 0u; i < ADAS_EB_NUM_OPS && !failed; ++i) {
        const struct golden_step* const step = &GOLDEN_STEPS[i];

        if (join_path(path, sizeof(path), golden_dir, step->file) != 0
            || read_exact(path, expected, step->bytes) != 0) {
            failed = 1;
            break;
        }

        struct timespec started;
        struct timespec finished;
        (void)clock_gettime(CLOCK_MONOTONIC, &started);
        const adas_classifier_device_status_t run_status =
            adas_classifier_device_run_op(&device, i, timeout_ms);
        (void)clock_gettime(CLOCK_MONOTONIC, &finished);
        const uint64_t op_us = elapsed_us(&started, &finished);
        total_us += op_us;

        if (run_status != ADAS_CLASSIFIER_DEVICE_OK) {
            printf("  op%u %-6s FAIL  실행 오류 (%s)\n",
                   i, OP_NAMES[i], strerror(errno));
            failed = 1;
            break;
        }

        /* op 의 출력이 어느 ping-pong 버퍼에 있는지는 op 번호로 정해진다. */
        const int8_t* const actual = step->destination_is_act_b
            ? adas_classifier_device_act_b(&device)
            : adas_classifier_device_act_a(&device);

        size_t first = 0u;
        const size_t mismatches =
            compare_bytes(actual, expected, step->bytes, &first);

        if (mismatches == 0u) {
            printf("  op%u %-6s PASS  %7zu B bit-exact  %6" PRIu64 " us\n",
                   i, OP_NAMES[i], step->bytes, op_us);
        } else {
            printf("  op%u %-6s FAIL  %zu/%zu 바이트 불일치, 첫 위치 %zu"
                   "  (기대 %d, 실제 %d)\n",
                   i, OP_NAMES[i], mismatches, step->bytes, first,
                   (int)expected[first], (int)actual[first]);
            /*
             * 여기서 멈추는 것이 요점이다. 앞 단계가 틀린 채로 다음을
             * 돌리면 그 뒤는 전부 틀리고, 원인 단계가 묻힌다.
             */
            failed = 1;
        }
    }

    free(expected);

    if (!failed) {
        printf("PASS: 6-op 전부 bit-exact, PL 합계 %" PRIu64 " us\n", total_us);
    }

    adas_classifier_device_close(&device);
    adas_classifier_model_unload(&model);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
