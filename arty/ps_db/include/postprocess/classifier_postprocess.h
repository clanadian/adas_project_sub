#ifndef ADAS_CLASSIFIER_POSTPROCESS_H
#define ADAS_CLASSIFIER_POSTPROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PL 마지막 maxpool 출력 형상: 12x12x64 INT8 NHWC. */
#define ADAS_CLASSIFIER_OUTPUT_WIDTH     12u
#define ADAS_CLASSIFIER_OUTPUT_HEIGHT    12u
#define ADAS_CLASSIFIER_OUTPUT_CHANNELS  64u
#define ADAS_CLASSIFIER_OUTPUT_ELEMENTS \
    (ADAS_CLASSIFIER_OUTPUT_WIDTH \
        * ADAS_CLASSIFIER_OUTPUT_HEIGHT \
        * ADAS_CLASSIFIER_OUTPUT_CHANNELS)
#define ADAS_CLASSIFIER_GAP_AREA \
    (ADAS_CLASSIFIER_OUTPUT_WIDTH * ADAS_CLASSIFIER_OUTPUT_HEIGHT)

typedef enum adas_classifier_postprocess_status {
    ADAS_CLASSIFIER_POSTPROCESS_OK = 0,
    ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_POSTPROCESS_OVERFLOW = -2
} adas_classifier_postprocess_status_t;

/*
 * 12x12x64 NHWC 출력에서 채널별 12x12 값 144개를 더합니다.
 * 아직 학습/export 계약에서 GAP 나눗셈 규칙이 확정되지 않았으므로
 * 여기서는 정보 손실이 없는 합계까지만 계산합니다.
 */
adas_classifier_postprocess_status_t adas_classifier_gap_sum(
    const int8_t pl_output[ADAS_CLASSIFIER_OUTPUT_ELEMENTS],
    int32_t channel_sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS]
);

/*
 * 채널별 GAP 특징 64개와 FC 가중치로 클래스별 logit을 계산합니다.
 * weight 배열 순서는 [class][channel], bias는 [class]입니다.
 * feature_scale_divisor가 1이면 합계를 그대로 사용하고,
 * 144이면 C 정수 나눗셈(0 방향 절삭)으로 평균을 사용합니다.
 *
 * TODO(model contract): 최종 export의 GAP rounding 규칙이 확정되면
 * divisor 처리 규칙을 그 계약에 맞춰 고정해야 합니다.
 */
adas_classifier_postprocess_status_t adas_classifier_fc(
    const int32_t channel_sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS],
    uint32_t feature_scale_divisor,
    const int8_t* weights,
    const int32_t* biases,
    size_t class_count,
    int32_t* logits
);

/* 가장 큰 logit의 인덱스를 반환합니다. 동점이면 앞쪽 클래스를 선택합니다. */
adas_classifier_postprocess_status_t adas_classifier_argmax(
    const int32_t* logits,
    size_t class_count,
    uint32_t* class_id
);

/*
 * logits(정수)에 export manifest.json의 logits_scale을 곱해 실수 logit으로
 * 되돌린 뒤 softmax를 적용하고, argmax 클래스의 확률을
 * confidence_ppm(0..1,000,000)으로 반환합니다.
 *
 * argmax_class_id는 이미 adas_classifier_argmax()로 구한 값을 그대로
 * 넘깁니다 - 여기서 다시 최댓값을 찾지 않고, softmax 확률 중 그 클래스의
 * 값만 계산에 사용합니다 (같은 로직을 두 번 만들지 않기 위함).
 *
 * 오버플로/언더플로 없이 exp()가 계산되도록 최댓값을 뺀 뒤 지수를 취하는
 * 표준 softmax 안정화 기법을 씁니다.
 */
adas_classifier_postprocess_status_t adas_classifier_confidence_ppm(
    const int32_t* logits,
    size_t class_count,
    uint32_t argmax_class_id,
    float logits_scale,
    uint32_t* confidence_ppm
);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_CLASSIFIER_POSTPROCESS_H
