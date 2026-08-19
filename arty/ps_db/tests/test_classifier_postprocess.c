#include "postprocess/classifier_postprocess.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void test_gap_nhwc_channel_sums(void) {
    int8_t output[ADAS_CLASSIFIER_OUTPUT_ELEMENTS] = {0};
    int32_t sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS] = {0};

    /* 모든 위치에서 channel c의 값을 c-32로 만들어 예상 합계를 단순화합니다. */
    for (size_t position = 0u;
         position < ADAS_CLASSIFIER_GAP_AREA;
         ++position) {
        for (size_t channel = 0u;
             channel < ADAS_CLASSIFIER_OUTPUT_CHANNELS;
             ++channel) {
            output[position * ADAS_CLASSIFIER_OUTPUT_CHANNELS + channel] =
                (int8_t)((int32_t)channel - 32);
        }
    }

    assert(adas_classifier_gap_sum(output, sums)
        == ADAS_CLASSIFIER_POSTPROCESS_OK);

    for (size_t channel = 0u;
         channel < ADAS_CLASSIFIER_OUTPUT_CHANNELS;
         ++channel) {
        assert(sums[channel]
            == ((int32_t)channel - 32) * (int32_t)ADAS_CLASSIFIER_GAP_AREA);
    }
}

static void test_fc_and_argmax(void) {
    int32_t sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS] = {0};
    int8_t weights[3u * ADAS_CLASSIFIER_OUTPUT_CHANNELS] = {0};
    const int32_t biases[3] = {10, 20, 30};
    int32_t logits[3] = {0};
    uint32_t class_id = UINT32_MAX;

    /* 평균 특징을 2로 만들고 클래스별 weight를 1, 2, -1로 설정합니다. */
    for (size_t channel = 0u;
         channel < ADAS_CLASSIFIER_OUTPUT_CHANNELS;
         ++channel) {
        sums[channel] = 2 * (int32_t)ADAS_CLASSIFIER_GAP_AREA;
        weights[0u * ADAS_CLASSIFIER_OUTPUT_CHANNELS + channel] = 1;
        weights[1u * ADAS_CLASSIFIER_OUTPUT_CHANNELS + channel] = 2;
        weights[2u * ADAS_CLASSIFIER_OUTPUT_CHANNELS + channel] = -1;
    }

    assert(adas_classifier_fc(
        sums,
        ADAS_CLASSIFIER_GAP_AREA,
        weights,
        biases,
        3u,
        logits
    ) == ADAS_CLASSIFIER_POSTPROCESS_OK);

    assert(logits[0] == 10 + 64 * 2);
    assert(logits[1] == 20 + 64 * 4);
    assert(logits[2] == 30 - 64 * 2);

    assert(adas_classifier_argmax(logits, 3u, &class_id)
        == ADAS_CLASSIFIER_POSTPROCESS_OK);
    assert(class_id == 1u);
}

static void test_invalid_arguments(void) {
    int8_t output[ADAS_CLASSIFIER_OUTPUT_ELEMENTS] = {0};
    int32_t sums[ADAS_CLASSIFIER_OUTPUT_CHANNELS] = {0};
    int8_t weights[ADAS_CLASSIFIER_OUTPUT_CHANNELS] = {0};
    int32_t bias = 0;
    int32_t logit = 0;
    uint32_t class_id = 0u;

    assert(adas_classifier_gap_sum(NULL, sums)
        == ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT);
    assert(adas_classifier_gap_sum(output, NULL)
        == ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT);
    assert(adas_classifier_fc(sums, 0u, weights, &bias, 1u, &logit)
        == ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT);
    assert(adas_classifier_argmax(NULL, 1u, &class_id)
        == ADAS_CLASSIFIER_POSTPROCESS_INVALID_ARGUMENT);
}

int main(void) {
    test_gap_nhwc_channel_sums();
    test_fc_and_argmax();
    test_invalid_arguments();

    puts("Classifier postprocess tests passed");
    return EXIT_SUCCESS;
}
