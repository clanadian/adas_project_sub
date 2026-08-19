#include "preprocess/roi_preprocessor.h"

#include <stddef.h>
#include <string.h>

int8_t adas_roi_quantize_pixel(uint8_t pixel) {
    const uint32_t scaled = (uint32_t)pixel * 127u;
    return (int8_t)((scaled + 127u) / 255u);
}

adas_roi_preprocess_status_t adas_roi_preprocess(
    const uint8_t input[ADAS_ROI_IMAGE_PAYLOAD_SIZE],
    int8_t output[ADAS_PL_INPUT_SIZE]
) {
    if (input == NULL || output == NULL) {
        return ADAS_ROI_PREPROCESS_INVALID_ARGUMENT;
    }

    memset(output, 0, ADAS_PL_INPUT_SIZE * sizeof(output[0]));

    for (size_t y = 0; y < ADAS_ROI_HEIGHT; ++y) {
        for (size_t x = 0; x < ADAS_ROI_WIDTH; ++x) {
            for (size_t channel = 0; channel < ADAS_ROI_CHANNELS; ++channel) {
                const size_t input_index =
                    (y * ADAS_ROI_WIDTH + x) * ADAS_ROI_CHANNELS
                    + channel;

                const size_t output_index =
                    ((y + ADAS_PL_INPUT_BORDER) * ADAS_PL_INPUT_WIDTH
                        + (x + ADAS_PL_INPUT_BORDER))
                    * ADAS_PL_INPUT_CHANNELS
                    + channel;

                output[output_index] =
                    adas_roi_quantize_pixel(input[input_index]);
            }
        }
    }

    return ADAS_ROI_PREPROCESS_OK;
}
