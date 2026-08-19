#ifndef ADAS_ROI_PREPROCESSOR_H
#define ADAS_ROI_PREPROCESSOR_H

#include <stdint.h>

#include "roi_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADAS_PL_INPUT_BORDER      1u
#define ADAS_PL_INPUT_WIDTH       (ADAS_ROI_WIDTH + 2u * ADAS_PL_INPUT_BORDER)
#define ADAS_PL_INPUT_HEIGHT      (ADAS_ROI_HEIGHT + 2u * ADAS_PL_INPUT_BORDER)
#define ADAS_PL_INPUT_CHANNELS    ADAS_ROI_CHANNELS
#define ADAS_PL_INPUT_SIZE \
    (ADAS_PL_INPUT_WIDTH * ADAS_PL_INPUT_HEIGHT * ADAS_PL_INPUT_CHANNELS)

typedef enum adas_roi_preprocess_status {
    ADAS_ROI_PREPROCESS_OK = 0,
    ADAS_ROI_PREPROCESS_INVALID_ARGUMENT = -1
} adas_roi_preprocess_status_t;

/* UINT8 pixel 0..255를 symmetric signed INT8 값 0..127로 변환한다. */
int8_t adas_roi_quantize_pixel(uint8_t pixel);

/*
 * 96x96x3 RGB UINT8 NHWC 입력을 98x98x3 signed INT8 NHWC로 변환한다.
 * 출력의 상하좌우 1픽셀 테두리는 INT8 0이다.
 */
adas_roi_preprocess_status_t adas_roi_preprocess(
    const uint8_t input[ADAS_ROI_IMAGE_PAYLOAD_SIZE],
    int8_t output[ADAS_PL_INPUT_SIZE]
);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_ROI_PREPROCESSOR_H
