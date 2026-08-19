#include "preprocess/roi_preprocessor.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static size_t input_index(size_t y, size_t x, size_t channel) {
    return (y * ADAS_ROI_WIDTH + x) * ADAS_ROI_CHANNELS + channel;
}

static size_t output_index(size_t y, size_t x, size_t channel) {
    return (y * ADAS_PL_INPUT_WIDTH + x) * ADAS_PL_INPUT_CHANNELS + channel;
}

static void test_quantization_boundaries(void) {
    assert(adas_roi_quantize_pixel(0u) == 0);
    assert(adas_roi_quantize_pixel(1u) == 0);
    assert(adas_roi_quantize_pixel(2u) == 1);
    assert(adas_roi_quantize_pixel(128u) == 64);
    assert(adas_roi_quantize_pixel(254u) == 127);
    assert(adas_roi_quantize_pixel(255u) == 127);
}

static void test_invalid_arguments(void) {
    static uint8_t input[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
    static int8_t output[ADAS_PL_INPUT_SIZE];

    assert(adas_roi_preprocess(NULL, output)
        == ADAS_ROI_PREPROCESS_INVALID_ARGUMENT);
    assert(adas_roi_preprocess(input, NULL)
        == ADAS_ROI_PREPROCESS_INVALID_ARGUMENT);
}

static void test_padding_and_nhwc_mapping(void) {
    static uint8_t input[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
    static int8_t output[ADAS_PL_INPUT_SIZE];

    for (size_t i = 0; i < ADAS_ROI_IMAGE_PAYLOAD_SIZE; ++i) {
        input[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
    }

    assert(adas_roi_preprocess(input, output) == ADAS_ROI_PREPROCESS_OK);

    /* 상단과 하단의 1-pixel border는 모든 채널이 0이어야 한다. */
    for (size_t x = 0; x < ADAS_PL_INPUT_WIDTH; ++x) {
        for (size_t c = 0; c < ADAS_PL_INPUT_CHANNELS; ++c) {
            assert(output[output_index(0u, x, c)] == 0);
            assert(output[output_index(ADAS_PL_INPUT_HEIGHT - 1u, x, c)] == 0);
        }
    }

    /* 좌측과 우측의 1-pixel border도 모든 채널이 0이어야 한다. */
    for (size_t y = 0; y < ADAS_PL_INPUT_HEIGHT; ++y) {
        for (size_t c = 0; c < ADAS_PL_INPUT_CHANNELS; ++c) {
            assert(output[output_index(y, 0u, c)] == 0);
            assert(output[output_index(y, ADAS_PL_INPUT_WIDTH - 1u, c)] == 0);
        }
    }

    /* 입력 (y,x,c)가 출력 (y+1,x+1,c)에 양자화되어 저장되는지 전수 검사한다. */
    for (size_t y = 0; y < ADAS_ROI_HEIGHT; ++y) {
        for (size_t x = 0; x < ADAS_ROI_WIDTH; ++x) {
            for (size_t c = 0; c < ADAS_ROI_CHANNELS; ++c) {
                const uint8_t source = input[input_index(y, x, c)];
                const int8_t expected = adas_roi_quantize_pixel(source);
                const int8_t actual = output[output_index(y + 1u, x + 1u, c)];
                assert(actual == expected);
            }
        }
    }
}

int main(void) {
    test_quantization_boundaries();
    test_invalid_arguments();
    test_padding_and_nhwc_mapping();

    puts("ROI preprocessor tests passed");
    return EXIT_SUCCESS;
}
