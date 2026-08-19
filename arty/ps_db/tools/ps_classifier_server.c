#define _POSIX_C_SOURCE 200809L

#include "driver/classifier_device.h"
#include "model/classifier_model.h"
#include "network/tcp_roi_server.h"
#include "postprocess/classifier_postprocess.h"
#include "preprocess/roi_preprocessor.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    float logits_scale
) {
    int8_t* const ifmap = adas_classifier_device_ifmap(device);
    const int8_t* const pl_output = adas_classifier_device_output(device);
    if (ifmap == NULL || pl_output == NULL
        || adas_roi_preprocess(image, ifmap) != ADAS_ROI_PREPROCESS_OK) {
        return failed_result(ADAS_ROI_STATUS_INVALID_PAYLOAD);
    }
    if (adas_classifier_device_run(device, 1000u)
        != ADAS_CLASSIFIER_DEVICE_OK) {
        return failed_result(ADAS_ROI_STATUS_ACCELERATOR_ERROR);
    }

    int32_t channel_sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS];
    if (adas_classifier_gap_sum(pl_output, channel_sums)
        != ADAS_CLASSIFIER_POSTPROCESS_OK) {
        return failed_result(ADAS_ROI_STATUS_POSTPROCESS_ERROR);
    }
    int32_t* logits = (int32_t*)malloc(model->class_count * sizeof(int32_t));
    if (logits == NULL) return failed_result(ADAS_ROI_STATUS_POSTPROCESS_ERROR);

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
        "<rq0-mul> <rq0-shift> <rq1-mul> <rq1-shift> "
        "<rq2-mul> <rq2-shift> <logits-scale>\n", program);
}

int main(int argc, char** argv) {
    if (argc != 13) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    uint64_t numeric[6] = {0};
    int32_t multipliers[3] = {0};
    float logits_scale = 0.0F;
    if (parse_u64(argv[2], &numeric[0]) != 0 || numeric[0] == 0u
        || numeric[0] > UINT16_MAX
        || parse_u64(argv[4], &numeric[1]) != 0 || numeric[1] == 0u
        || numeric[1] > SIZE_MAX || numeric[1] > UINT32_MAX
        || parse_u64(argv[5], &numeric[2]) != 0 || numeric[2] == 0u
        || numeric[2] > UINT32_MAX
        || parse_i32(argv[6], &multipliers[0]) != 0
        || parse_u64(argv[7], &numeric[3]) != 0 || numeric[3] > UINT8_MAX
        || parse_i32(argv[8], &multipliers[1]) != 0
        || parse_u64(argv[9], &numeric[4]) != 0 || numeric[4] > UINT8_MAX
        || parse_i32(argv[10], &multipliers[2]) != 0
        || parse_u64(argv[11], &numeric[5]) != 0 || numeric[5] > UINT8_MAX
        || parse_positive_float(argv[12], &logits_scale) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const adas_classifier_model_metadata_t metadata = {
        .class_count = (size_t)numeric[1],
        .gap_divisor = (uint32_t)numeric[2],
        .rq_conv0 = {multipliers[0], (uint8_t)numeric[3]},
        .rq_conv1 = {multipliers[1], (uint8_t)numeric[4]},
        .rq_conv2 = {multipliers[2], (uint8_t)numeric[5]}
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

    printf("classifier server listening on port %u\n", (unsigned)config.port);
    for (;;) {
        if (adas_tcp_roi_server_accept(&server) != ADAS_TCP_ROI_OK) {
            perror("accept failed");
            break;
        }
        for (;;) {
            adas_roi_header_t request;
            uint8_t image[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
            if (adas_tcp_roi_server_receive_request(
                    &server, &request, image) != ADAS_TCP_ROI_OK) break;
            const adas_roi_result_t result =
                classify_one(image, &device, &model, logits_scale);
            if (adas_tcp_roi_server_send_result(&server, &request, &result)
                != ADAS_TCP_ROI_OK) break;
        }
        adas_tcp_roi_server_disconnect(&server);
    }

    adas_tcp_roi_server_close(&server);
    adas_classifier_device_close(&device);
    adas_classifier_model_unload(&model);
    return EXIT_FAILURE;
}
