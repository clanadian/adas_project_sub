#ifndef ADAS_CLASSIFIER_MODEL_H
#define ADAS_CLASSIFIER_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "accelerator/classifier_accelerator.h"
#include "accelerator/classifier_buffers.h"
#include "postprocess/classifier_postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adas_classifier_model_status {
    ADAS_CLASSIFIER_MODEL_OK = 0,
    ADAS_CLASSIFIER_MODEL_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_MODEL_IO_ERROR = -2,
    ADAS_CLASSIFIER_MODEL_SIZE_MISMATCH = -3,
    ADAS_CLASSIFIER_MODEL_OUT_OF_MEMORY = -4
} adas_classifier_model_status_t;

/*
 * manifest 형식이 확정되기 전까지 호출자가 넘기는 모델 메타데이터입니다.
 * requant 값은 PL 레지스터에 기록되고 GAP divisor는 PS 후처리에 사용됩니다.
 *
 * requant 의 `leaky` 는 EB 전용이며 manifest 의 activation 을 그대로 옮긴다
 * (conv0/conv1 = leaky, conv2 = linear). 값이 어긋나면 오류 없이 결과만 틀린다.
 */
typedef struct adas_classifier_model_metadata {
    size_t class_count;
    uint32_t gap_divisor;
    adas_classifier_requant_t rq_conv0;
    adas_classifier_requant_t rq_conv1;
    adas_classifier_requant_t rq_conv2;
} adas_classifier_model_metadata_t;

/*
 * 디스크에서 읽은 모델 한 세트입니다.
 * Conv weight/bias는 PL 실행에, FC weight/bias는 PS 후처리에 사용됩니다.
 */
typedef struct adas_classifier_model {
    int8_t w_conv0[ADAS_CLASSIFIER_W_CONV0_SIZE];
    int8_t w_conv1[ADAS_CLASSIFIER_W_CONV1_SIZE];
    int8_t w_conv2[ADAS_CLASSIFIER_W_CONV2_SIZE];
    /*
     * EB 는 bias 를 AXI-Lite 레지스터가 아니라 DDR 로 넘긴다. 그래서
     * pl_parameters 가 아니라 모델 쪽에 두고, 호출자가 DMA 버퍼로 복사한다.
     */
    int32_t b_conv0[ADAS_CLASSIFIER_B_CONV0_COUNT];
    int32_t b_conv1[ADAS_CLASSIFIER_B_CONV1_COUNT];
    int32_t b_conv2[ADAS_CLASSIFIER_B_CONV2_COUNT];
    adas_classifier_parameters_t pl_parameters;
    int8_t* fc_weights;
    int32_t* fc_biases;
    size_t class_count;
    uint32_t gap_divisor;
} adas_classifier_model_t;

/* 동적 포인터가 없는 빈 상태로 초기화합니다. */
void adas_classifier_model_init(adas_classifier_model_t* model);

/*
 * directory에서 아래 파일을 정확한 크기로 읽습니다.
 *   w_conv0.bin, b_conv0.bin
 *   w_conv1.bin, b_conv1.bin
 *   w_conv2.bin, b_conv2.bin
 *   fc_weight.bin, fc_bias.bin
 *
 * FC 배열 순서는 weight[class][64], bias[class]를 가정합니다.
 * Conv bias 는 model->b_conv* 로 들어오며, DDR 로 복사하는 것은 호출자 몫입니다.
 * TODO(model contract): 최종 manifest가 확정되면 metadata JSON 로더를 추가합니다.
 */
adas_classifier_model_status_t adas_classifier_model_load(
    adas_classifier_model_t* model,
    const char* directory,
    const adas_classifier_model_metadata_t* metadata
);

/* FC 동적 메모리를 해제하고 다시 빈 상태로 만듭니다. */
void adas_classifier_model_unload(adas_classifier_model_t* model);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_CLASSIFIER_MODEL_H
