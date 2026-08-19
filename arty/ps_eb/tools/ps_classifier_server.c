#define _POSIX_C_SOURCE 200809L

#include "driver/classifier_device.h"
#include "model/classifier_model.h"
#include "network/tcp_roi_server.h"
#include "postprocess/classifier_postprocess.h"
#include "preprocess/roi_preprocessor.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int parse_u64(const char* text, uint64_t* value) {
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_i32(const char* text, int32_t* value) {
    errno = 0;
    char* end = NULL;
    const long long parsed = strtoll(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0'
        || parsed < INT32_MIN || parsed > INT32_MAX) return -1;
    *value = (int32_t)parsed;
    return 0;
}

/* export manifest.json의 logits_scale처럼 아주 작은 양수 실수를 받습니다. */
static int parse_positive_float(const char* text, float* value) {
    errno = 0;
    char* end = NULL;
    const float parsed = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !(parsed > 0.0F)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

/*
 * ---------------------------------------------------------------------------
 * 보고서용 구간 측정
 * ---------------------------------------------------------------------------
 *
 * Jetson이 재는 왕복 지연(RTT)에서 네트워크 몫을 분리하려면 보드가 실제로
 * 쓴 시간을 따로 알아야 한다. 여기서 재는 것은 요청 payload를 다 받은
 * 시점부터 응답을 보내기 직전까지이며, 그 안을 셋으로 쪼갠다.
 *
 *     네트워크 왕복 = Jetson RTT - server_us
 *
 * 측정은 항상 켜져 있다 - clock_gettime 두 번은 ROI 한 건(수 ms)에 비해
 * 무시할 수 있고, 껐다 켜는 스위치가 있으면 "측정할 때만 느린 경로"라는
 * 의심을 만들기 때문이다. 출력만 환경변수로 조절한다.
 *
 *   ADAS_PS_REPORT_EVERY : 몇 건마다 진행 상황을 한 줄 찍을지 (기본 100, 0=끔)
 *   ADAS_PS_CSV          : 요청 한 건당 한 행을 남길 CSV 경로 (백분위수는
 *                          이 파일로 오프라인 계산한다)
 *   ADAS_TCP_NODELAY     : 1이면 accept 후 client socket의 Nagle을 끈다
 *
 * pl_run_us 는 EB 6-op(conv0/pool0/conv1/pool1/conv2/pool2) 전체 시간이다.
 * op 별 시간은 드라이버의 GET_STATUS 가 마지막 op 것만 들고 있다.
 */

typedef struct request_timing {
    uint64_t preprocess_us;   /* UINT8 -> INT8 양자화 + 1픽셀 zero padding */
    uint64_t pl_run_us;       /* PL 6-op 전체 (엔진 3개, 기동 6회) */
    uint64_t postprocess_us;  /* GAP -> FC -> argmax -> confidence */
    uint64_t server_us;       /* 위 셋을 포함한 요청 처리 전체 */
} request_timing_t;

typedef struct stat_accumulator {
    uint64_t count;
    uint64_t sum_us;
    uint64_t min_us;
    uint64_t max_us;
} stat_accumulator_t;

typedef struct session_metrics {
    stat_accumulator_t preprocess;
    stat_accumulator_t pl_run;
    stat_accumulator_t postprocess;
    stat_accumulator_t server;
    uint64_t ok_count;
    uint64_t error_count;
    struct timespec first_request;
    struct timespec last_request;
} session_metrics_t;

static void monotonic_now(struct timespec* now) {
    (void)clock_gettime(CLOCK_MONOTONIC, now);
}

static uint64_t elapsed_us(const struct timespec* start,
                           const struct timespec* end) {
    const int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    const int64_t nanoseconds =
        (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    const int64_t total = seconds * 1000000 + nanoseconds / 1000;
    return total > 0 ? (uint64_t)total : 0u;
}

static void stat_reset(stat_accumulator_t* stat) {
    stat->count = 0u;
    stat->sum_us = 0u;
    stat->min_us = UINT64_MAX;
    stat->max_us = 0u;
}

static void stat_add(stat_accumulator_t* stat, uint64_t value_us) {
    stat->count += 1u;
    stat->sum_us += value_us;
    if (value_us < stat->min_us) stat->min_us = value_us;
    if (value_us > stat->max_us) stat->max_us = value_us;
}

static double stat_mean_ms(const stat_accumulator_t* stat) {
    if (stat->count == 0u) return 0.0;
    return (double)stat->sum_us / (double)stat->count / 1000.0;
}

static double stat_min_ms(const stat_accumulator_t* stat) {
    if (stat->count == 0u) return 0.0;
    return (double)stat->min_us / 1000.0;
}

static double stat_max_ms(const stat_accumulator_t* stat) {
    return (double)stat->max_us / 1000.0;
}

static void session_metrics_reset(session_metrics_t* metrics) {
    stat_reset(&metrics->preprocess);
    stat_reset(&metrics->pl_run);
    stat_reset(&metrics->postprocess);
    stat_reset(&metrics->server);
    metrics->ok_count = 0u;
    metrics->error_count = 0u;
    metrics->first_request.tv_sec = 0;
    metrics->first_request.tv_nsec = 0;
    metrics->last_request.tv_sec = 0;
    metrics->last_request.tv_nsec = 0;
}

static void session_metrics_add(
    session_metrics_t* metrics,
    const request_timing_t* timing,
    const adas_roi_result_t* result,
    const struct timespec* started,
    const struct timespec* finished
) {
    if (metrics->server.count == 0u) {
        metrics->first_request = *started;
    }
    metrics->last_request = *finished;
    stat_add(&metrics->preprocess, timing->preprocess_us);
    stat_add(&metrics->pl_run, timing->pl_run_us);
    stat_add(&metrics->postprocess, timing->postprocess_us);
    stat_add(&metrics->server, timing->server_us);
    if (result->status == ADAS_ROI_STATUS_OK) {
        metrics->ok_count += 1u;
    } else {
        metrics->error_count += 1u;
    }
}

static void print_stat_row(const char* name, const stat_accumulator_t* stat) {
    printf("  %-14s n=%-7" PRIu64 " mean %7.3f ms  min %7.3f ms"
           "  max %7.3f ms\n",
           name, stat->count, stat_mean_ms(stat),
           stat_min_ms(stat), stat_max_ms(stat));
}

static void print_session_summary(const session_metrics_t* metrics) {
    if (metrics->server.count == 0u) {
        printf("session summary: 처리한 요청 없음\n");
        return;
    }

    const uint64_t span_us =
        elapsed_us(&metrics->first_request, &metrics->last_request);
    const double span_s = (double)span_us / 1000000.0;

    printf("\n=== session summary ===\n");
    printf("  requests       %" PRIu64 " (ok %" PRIu64 ", error %" PRIu64 ")\n",
           metrics->server.count, metrics->ok_count, metrics->error_count);
    if (span_s > 0.0) {
        printf("  wall clock     %.2f s  ->  %.2f ROI/s (수신 간격 포함)\n",
               span_s, (double)metrics->server.count / span_s);
    }
    printf("  보드가 쓴 시간이 곧 처리량 상한이다:"
           " %.2f ROI/s (server_us 평균 기준)\n",
           metrics->server.count > 0u && stat_mean_ms(&metrics->server) > 0.0
               ? 1000.0 / stat_mean_ms(&metrics->server)
               : 0.0);
    print_stat_row("preprocess", &metrics->preprocess);
    print_stat_row("pl_run", &metrics->pl_run);
    print_stat_row("postprocess", &metrics->postprocess);
    print_stat_row("server total", &metrics->server);
    printf("=======================\n\n");
    fflush(stdout);
}

/* 환경변수를 부호 없는 정수로 읽는다. 없거나 형식이 틀리면 기본값. */
static unsigned long env_ulong(const char* name, unsigned long fallback) {
    const char* text = getenv(name);
    if (text == NULL || *text == '\0') return fallback;
    errno = 0;
    char* end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return fallback;
    return value;
}

static adas_roi_result_t failed_result(uint32_t status) {
    const adas_roi_result_t result = {
        .status = status,
        .class_id = ADAS_ROI_INVALID_CLASS_ID,
        .confidence_ppm = 0u
    };
    return result;
}

static adas_roi_result_t classify_one(
    const uint8_t image[ADAS_ROI_IMAGE_PAYLOAD_SIZE],
    adas_classifier_device_t* device,
    const adas_classifier_model_t* model,
    float logits_scale,
    request_timing_t* timing
) {
    /*
     * 실패 경로에서도 timing이 초기화된 값을 갖도록 먼저 0으로 둔다.
     * 재지 못한 구간은 0으로 남고, 그건 CSV에서 그대로 구분된다.
     */
    timing->preprocess_us = 0u;
    timing->pl_run_us = 0u;
    timing->postprocess_us = 0u;

    int8_t* const ifmap = adas_classifier_device_ifmap(device);
    const int8_t* const pl_output = adas_classifier_device_output(device);
    if (ifmap == NULL || pl_output == NULL) {
        return failed_result(ADAS_ROI_STATUS_INVALID_PAYLOAD);
    }

    struct timespec mark_start;
    struct timespec mark_preprocess;
    struct timespec mark_pl;
    struct timespec mark_post;

    monotonic_now(&mark_start);
    const adas_roi_preprocess_status_t preprocess_status =
        adas_roi_preprocess(image, ifmap);
    monotonic_now(&mark_preprocess);
    timing->preprocess_us = elapsed_us(&mark_start, &mark_preprocess);
    if (preprocess_status != ADAS_ROI_PREPROCESS_OK) {
        return failed_result(ADAS_ROI_STATUS_INVALID_PAYLOAD);
    }

    const adas_classifier_device_status_t run_status =
        adas_classifier_device_run(device, 1000u);
    monotonic_now(&mark_pl);
    timing->pl_run_us = elapsed_us(&mark_preprocess, &mark_pl);
    if (run_status != ADAS_CLASSIFIER_DEVICE_OK) {
        return failed_result(ADAS_ROI_STATUS_ACCELERATOR_ERROR);
    }

    int32_t channel_sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS];
    if (adas_classifier_gap_sum(pl_output, channel_sums)
        != ADAS_CLASSIFIER_POSTPROCESS_OK) {
        monotonic_now(&mark_post);
        timing->postprocess_us = elapsed_us(&mark_pl, &mark_post);
        return failed_result(ADAS_ROI_STATUS_POSTPROCESS_ERROR);
    }
    int32_t* logits = (int32_t*)malloc(model->class_count * sizeof(int32_t));
    if (logits == NULL) {
        monotonic_now(&mark_post);
        timing->postprocess_us = elapsed_us(&mark_pl, &mark_post);
        return failed_result(ADAS_ROI_STATUS_POSTPROCESS_ERROR);
    }

    uint32_t class_id = ADAS_ROI_INVALID_CLASS_ID;
    const adas_classifier_postprocess_status_t fc_status = adas_classifier_fc(
        channel_sums, model->gap_divisor, model->fc_weights,
        model->fc_biases, model->class_count, logits);
    const adas_classifier_postprocess_status_t argmax_status =
        fc_status == ADAS_CLASSIFIER_POSTPROCESS_OK
        ? adas_classifier_argmax(logits, model->class_count, &class_id)
        : fc_status;
    if (argmax_status != ADAS_CLASSIFIER_POSTPROCESS_OK) {
        free(logits);
        monotonic_now(&mark_post);
        timing->postprocess_us = elapsed_us(&mark_pl, &mark_post);
        return failed_result(ADAS_ROI_STATUS_POSTPROCESS_ERROR);
    }

    /*
     * confidence 계산 실패는 분류 자체(class_id)를 무효화하지 않습니다 -
     * argmax는 이미 성공했으므로, confidence만 0으로 둔 채 정상 응답을
     * 보냅니다.
     */
    uint32_t confidence_ppm = 0u;
    (void)adas_classifier_confidence_ppm(
        logits, model->class_count, class_id, logits_scale, &confidence_ppm);
    free(logits);

    monotonic_now(&mark_post);
    timing->postprocess_us = elapsed_us(&mark_pl, &mark_post);

    const adas_roi_result_t result = {
        .status = ADAS_ROI_STATUS_OK,
        .class_id = class_id,
        .confidence_ppm = confidence_ppm
    };
    return result;
}

static void print_usage(const char* program) {
    fprintf(stderr,
        "usage: %s <bind|*> <port> <model-dir> <classes> <gap-div> "
        "<rq0-mul> <rq0-shift> <rq0-leaky> "
        "<rq1-mul> <rq1-shift> <rq1-leaky> "
        "<rq2-mul> <rq2-shift> <rq2-leaky> <logits-scale>\n"
        "\n"
        "  rqN-leaky : EB 엔진의 LeakyReLU(13/128) 활성화 여부 (0/1).\n"
        "              manifest 의 activation 과 어긋나면 오류 없이 결과만 틀린다.\n"
        "  model-dir : **_eb** export 여야 한다. _db 모델은 산술이 달라 교환 불가.\n",
        program);
}

int main(int argc, char** argv) {
    if (argc != 16) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    uint64_t numeric[3] = {0};       /* port, classes, gap-div */
    int32_t multipliers[3] = {0};
    uint64_t shifts[3] = {0};
    uint64_t leaky[3] = {0};
    float logits_scale = 0.0F;
    int parse_failed =
        parse_u64(argv[2], &numeric[0]) != 0 || numeric[0] == 0u
        || numeric[0] > UINT16_MAX
        || parse_u64(argv[4], &numeric[1]) != 0 || numeric[1] == 0u
        || numeric[1] > SIZE_MAX || numeric[1] > UINT32_MAX
        || parse_u64(argv[5], &numeric[2]) != 0 || numeric[2] == 0u
        || numeric[2] > UINT32_MAX
        || parse_positive_float(argv[15], &logits_scale) != 0;
    /* conv 3단은 (multiplier, shift, leaky) 3개씩 연속으로 받는다. */
    for (int i = 0; i < 3 && !parse_failed; ++i) {
        const int base = 6 + i * 3;
        parse_failed =
            parse_i32(argv[base], &multipliers[i]) != 0
            || parse_u64(argv[base + 1], &shifts[i]) != 0
            || shifts[i] > UINT8_MAX
            || parse_u64(argv[base + 2], &leaky[i]) != 0
            || leaky[i] > 1u;
    }
    if (parse_failed) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const adas_classifier_model_metadata_t metadata = {
        .class_count = (size_t)numeric[1],
        .gap_divisor = (uint32_t)numeric[2],
        .rq_conv0 = {multipliers[0], (uint8_t)shifts[0], (uint8_t)leaky[0]},
        .rq_conv1 = {multipliers[1], (uint8_t)shifts[1], (uint8_t)leaky[1]},
        .rq_conv2 = {multipliers[2], (uint8_t)shifts[2], (uint8_t)leaky[2]}
    };
    adas_classifier_model_t model;
    adas_classifier_model_init(&model);
    if (adas_classifier_model_load(&model, argv[3], &metadata)
        != ADAS_CLASSIFIER_MODEL_OK) {
        fprintf(stderr, "failed to load model files\n");
        return EXIT_FAILURE;
    }

    adas_classifier_device_t device;
    adas_classifier_device_init(&device);
    if (adas_classifier_device_open(&device, "/dev/adas_classifier")
        != ADAS_CLASSIFIER_DEVICE_OK) {
        perror("failed to open /dev/adas_classifier");
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }
    memcpy(adas_classifier_device_w_conv0(&device),
           model.w_conv0, sizeof(model.w_conv0));
    memcpy(adas_classifier_device_w_conv1(&device),
           model.w_conv1, sizeof(model.w_conv1));
    memcpy(adas_classifier_device_w_conv2(&device),
           model.w_conv2, sizeof(model.w_conv2));
    /*
     * EB 는 bias 도 DDR 로 넘긴다(DB 는 AXI-Lite 레지스터였다).
     * 이 복사를 빠뜨리면 bias 가 전부 0 인 채로 조용히 동작한다.
     */
    memcpy(adas_classifier_device_b_conv0(&device),
           model.b_conv0, sizeof(model.b_conv0));
    memcpy(adas_classifier_device_b_conv1(&device),
           model.b_conv1, sizeof(model.b_conv1));
    memcpy(adas_classifier_device_b_conv2(&device),
           model.b_conv2, sizeof(model.b_conv2));
    if (adas_classifier_device_load_parameters(&device, &model.pl_parameters)
        != ADAS_CLASSIFIER_DEVICE_OK) {
        perror("failed to configure classifier");
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }

    adas_tcp_roi_server_t server;
    adas_tcp_roi_server_init(&server);
    const adas_tcp_roi_server_config_t config = {
        .bind_address = strcmp(argv[1], "*") == 0 ? NULL : argv[1],
        .port = (uint16_t)numeric[0],
        .backlog = ADAS_TCP_ROI_DEFAULT_BACKLOG
    };
    if (adas_tcp_roi_server_listen(&server, &config) != ADAS_TCP_ROI_OK) {
        perror("failed to start classifier server");
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }

    const unsigned long report_every = env_ulong("ADAS_PS_REPORT_EVERY", 100ul);
    const int no_delay_requested = env_ulong("ADAS_TCP_NODELAY", 0ul) != 0ul;

    FILE* csv = NULL;
    const char* const csv_path = getenv("ADAS_PS_CSV");
    if (csv_path != NULL && *csv_path != '\0') {
        csv = fopen(csv_path, "w");
        if (csv == NULL) {
            /*
             * 측정 파일을 못 여는 것으로 분류 서비스를 멈추지는 않는다.
             * 대신 조용히 넘어가지도 않는다 - 없는 줄 모르고 측정하면
             * 그 실행은 통째로 버려진다.
             */
            perror("failed to open ADAS_PS_CSV, continuing without CSV");
        } else {
            fprintf(csv,
                "frame_id,roi_id,status,class_id,confidence_ppm,"
                "preprocess_us,pl_run_us,postprocess_us,server_us\n");
        }
    }

    printf("classifier server listening on port %u\n", (unsigned)config.port);
    printf("measurement: report_every=%lu csv=%s tcp_nodelay=%s\n",
           report_every,
           csv != NULL ? csv_path : "(off)",
           no_delay_requested ? "on" : "off");

    session_metrics_t metrics;
    session_metrics_reset(&metrics);

    for (;;) {
        if (adas_tcp_roi_server_accept(&server) != ADAS_TCP_ROI_OK) {
            perror("accept failed");
            break;
        }

        if (no_delay_requested
            && adas_tcp_roi_server_set_no_delay(&server, 1)
               != ADAS_TCP_ROI_OK) {
            /*
             * 이 실행의 측정값이 "NODELAY 켠 조건"이라는 전제가 깨지므로
             * 반드시 눈에 보여야 한다.
             */
            perror("warning: failed to set TCP_NODELAY");
        }

        session_metrics_reset(&metrics);

        for (;;) {
            adas_roi_header_t request;
            uint8_t image[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
            if (adas_tcp_roi_server_receive_request(
                    &server, &request, image) != ADAS_TCP_ROI_OK) break;

            struct timespec request_start;
            struct timespec request_end;
            request_timing_t timing;

            monotonic_now(&request_start);
            const adas_roi_result_t result =
                classify_one(image, &device, &model, logits_scale, &timing);
            monotonic_now(&request_end);
            timing.server_us = elapsed_us(&request_start, &request_end);

            session_metrics_add(
                &metrics, &timing, &result, &request_start, &request_end);

            if (csv != NULL) {
                fprintf(csv,
                    "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                    ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 "\n",
                    request.frame_id, request.roi_id, result.status,
                    result.class_id, result.confidence_ppm,
                    timing.preprocess_us, timing.pl_run_us,
                    timing.postprocess_us, timing.server_us);
            }

            if (report_every != 0ul
                && metrics.server.count % report_every == 0ull) {
                printf("[%" PRIu64 " requests] server mean %.3f ms"
                       " (pl %.3f / pre %.3f / post %.3f)  errors %" PRIu64
                       "\n",
                       metrics.server.count,
                       stat_mean_ms(&metrics.server),
                       stat_mean_ms(&metrics.pl_run),
                       stat_mean_ms(&metrics.preprocess),
                       stat_mean_ms(&metrics.postprocess),
                       metrics.error_count);
                fflush(stdout);
            }

            if (adas_tcp_roi_server_send_result(&server, &request, &result)
                != ADAS_TCP_ROI_OK) break;
        }

        /*
         * 연결이 끊긴 시점이 곧 한 측정 구간의 끝이다. Jetson 쪽 클라이언트를
         * Ctrl-C로 멈추면 여기가 실행돼 그 세션의 요약이 남는다.
         */
        print_session_summary(&metrics);
        if (csv != NULL) fflush(csv);

        adas_tcp_roi_server_disconnect(&server);
    }

    if (csv != NULL) fclose(csv);
    adas_tcp_roi_server_close(&server);
    adas_classifier_device_close(&device);
    adas_classifier_model_unload(&model);
    return EXIT_FAILURE;
}
