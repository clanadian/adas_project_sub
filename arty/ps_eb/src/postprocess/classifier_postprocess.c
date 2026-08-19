#include "postprocess/classifier_postprocess.h"

#include <limits.h>
#include <math.h>

adas_classifier_postprocess_status_t adas_classifier_gap_sum(
    const int8_t pl_output[ADAS_CLASSIFIER_OUTPUT_ELEMENTS],
    int32_t channel_sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS]
) {
    /* 입력이나 결과 배열이 없으면 계산할 수 없습니다. */
    if (pl_output == NULL || channel_sums == NULL) {
        return ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT;
    }

    /* 각 채널의 누적값을 0에서 시작합니다. */
    for (size_t channel = 0u;
         channel < ADAS_CLASSIFIER_OUTPUT_CHANNELS;
         ++channel) {
        channel_sums[channel] = 0;
    }

    /*
     * PL 출력은 NHWC이므로 같은 픽셀의 64채널이 연속해서 놓입니다.
     * (y, x, channel)의 1차원 index를 계산해 해당 채널 합계에 더합니다.
     */
    for (size_t y = 0u; y < ADAS_CLASSIFIER_OUTPUT_HEIGHT; ++y) {
        for (size_t x = 0u; x < ADAS_CLASSIFIER_OUTPUT_WIDTH; ++x) {
            for (size_t channel = 0u;
                 channel < ADAS_CLASSIFIER_OUTPUT_CHANNELS;
                 ++channel) {
                const size_t index =
                    (y * ADAS_CLASSIFIER_OUTPUT_WIDTH + x)
                    * ADAS_CLASSIFIER_OUTPUT_CHANNELS
                    + channel;
                channel_sums[channel] += (int32_t)pl_output[index];
            }
        }
    }

    return ADAS_CLASSIFIER_POSTPROCESS_OK;
}

adas_classifier_postprocess_status_t adas_classifier_fc(
    const int32_t channel_sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS],
    uint32_t feature_scale_divisor,
    const int8_t* weights,
    const int32_t* biases,
    size_t class_count,
    int32_t* logits
) {
    /* FC에 필요한 모든 배열과 0이 아닌 크기/나눗수가 필요합니다. */
    if (channel_sums == NULL
        || feature_scale_divisor == 0u
        || weights == NULL
        || biases == NULL
        || class_count == 0u
        || logits == NULL) {
        return ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT;
    }

    /* 클래스 하나마다 bias에서 시작하여 64개 feature*weight를 더합니다. */
    for (size_t class_index = 0u;
         class_index < class_count;
         ++class_index) {
        /* 중간 합은 INT32 범위 초과를 검사할 수 있도록 INT64로 계산합니다. */
        int64_t accumulator = (int64_t)biases[class_index];

        for (size_t channel = 0u;
             channel < ADAS_CLASSIFIER_OUTPUT_CHANNELS;
             ++channel) {
            /*
             * 현재 기본 규칙은 C의 signed 정수 나눗셈, 즉 0 방향 절삭입니다.
             * export 계약이 평균을 FC scale에 흡수하면 divisor에는 1을 줍니다.
             */
            const int32_t feature =
                channel_sums[channel] / (int32_t)feature_scale_divisor;
            const size_t weight_index =
                class_index * ADAS_CLASSIFIER_OUTPUT_CHANNELS + channel;
            accumulator +=
                (int64_t)feature * (int64_t)weights[weight_index];
        }

        /* 외부 산출물 때문에 결과가 INT32 범위를 넘으면 조용히 자르지 않습니다. */
        if (accumulator < INT32_MIN || accumulator > INT32_MAX) {
            return ADAS_CLASSIFIER_POSTPROCESS_OVERFLOW;
        }

        logits[class_index] = (int32_t)accumulator;
    }

    return ADAS_CLASSIFIER_POSTPROCESS_OK;
}

adas_classifier_postprocess_status_t adas_classifier_argmax(
    const int32_t* logits,
    size_t class_count,
    uint32_t* class_id
) {
    /* 빈 logit 배열에서는 선택할 클래스가 없습니다. */
    if (logits == NULL || class_count == 0u || class_id == NULL) {
        return ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT;
    }

    /* 우선 0번 클래스를 최댓값으로 놓고 나머지와 비교합니다. */
    size_t best_index = 0u;
    for (size_t index = 1u; index < class_count; ++index) {
        /* >만 사용하므로 점수가 같으면 번호가 작은 기존 클래스가 유지됩니다. */
        if (logits[index] > logits[best_index]) {
            best_index = index;
        }
    }

    *class_id = (uint32_t)best_index;
    return ADAS_CLASSIFIER_POSTPROCESS_OK;
}

adas_classifier_postprocess_status_t adas_classifier_confidence_ppm(
    const int32_t* logits,
    size_t class_count,
    uint32_t argmax_class_id,
    float logits_scale,
    uint32_t* confidence_ppm
) {
    /* 잘못된 scale이나 클래스 범위 밖 인덱스는 계산할 수 없습니다. */
    if (logits == NULL
        || class_count == 0u
        || argmax_class_id >= class_count
        || !(logits_scale > 0.0F)
        || confidence_ppm == NULL) {
        return ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT;
    }

    /*
     * 최댓값을 먼저 구해서 뺀 뒤 지수를 취합니다 - 이러면 가장 큰 항은
     * exp(0)=1이 되고 나머지는 항상 exp(음수 또는 0)이라, class_count가
     * 커져도 exp() 오버플로가 나지 않습니다.
     */
    double max_real_logit = -HUGE_VAL;
    for (size_t index = 0u; index < class_count; ++index) {
        const double real_logit = (double)logits[index] * (double)logits_scale;
        if (real_logit > max_real_logit) {
            max_real_logit = real_logit;
        }
    }

    double sum_exp = 0.0;
    double argmax_exp = 0.0;
    for (size_t index = 0u; index < class_count; ++index) {
        const double real_logit = (double)logits[index] * (double)logits_scale;
        const double term = exp(real_logit - max_real_logit);
        sum_exp += term;
        if (index == argmax_class_id) {
            argmax_exp = term;
        }
    }

    /*
     * 1,000,000 = ppm 정의(parts per million) 그 자체이며
     * shared/include/roi_protocol.h의 ADAS_ROI_CONFIDENCE_PPM_MAX와 같은
     * 값입니다. 이 모듈은 네트워크 와이어 포맷을 몰라도 되므로 그 헤더를
     * include하지 않고 리터럴로 둡니다.
     *
     * sum_exp는 argmax_exp(=exp(0)=1)를 포함하므로 항상 1 이상입니다.
     */
    const double probability = argmax_exp / sum_exp;
    double ppm = probability * 1000000.0;
    if (ppm < 0.0) {
        ppm = 0.0;
    } else if (ppm > 1000000.0) {
        ppm = 1000000.0;
    }

    *confidence_ppm = (uint32_t)(ppm + 0.5);
    return ADAS_CLASSIFIER_POSTPROCESS_OK;
}
