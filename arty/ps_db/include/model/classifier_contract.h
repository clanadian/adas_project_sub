#ifndef ADAS_CLASSIFIER_CONTRACT_H
#define ADAS_CLASSIFIER_CONTRACT_H

#include <stdint.h>

/*
 * PS 와 PL 이 주고받는 자료형과 상수만 모아 둔다.
 *
 * 레지스터를 어떻게 건드리는지는 여기 없다. 운영 경로에서 PL 을 실제로
 * 구동하는 것은 커널 드라이버(`driver/classifier_device.h`)이고, 사용자
 * 공간은 `/dev/adas_classifier` 만 쓴다.
 *
 * 값의 정본은 docs/contracts/ROI_CLASSIFIER_CONTRACT.md 다.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Conv bias 개수. PL 의 채널 수와 같다. */
#define ADAS_CLASSIFIER_B_CONV0_COUNT 16u
#define ADAS_CLASSIFIER_B_CONV1_COUNT 32u
#define ADAS_CLASSIFIER_B_CONV2_COUNT 64u

/* 레이어별 고정소수점 requant 계수. */
typedef struct adas_classifier_requant {
    int32_t multiplier;
    uint8_t shift;
} adas_classifier_requant_t;

/* PL 한 번 실행에 필요한 파라미터 전체. */
typedef struct adas_classifier_parameters {
    int32_t b_conv0[ADAS_CLASSIFIER_B_CONV0_COUNT];
    int32_t b_conv1[ADAS_CLASSIFIER_B_CONV1_COUNT];
    int32_t b_conv2[ADAS_CLASSIFIER_B_CONV2_COUNT];
    adas_classifier_requant_t rq_conv0;
    adas_classifier_requant_t rq_conv1;
    adas_classifier_requant_t rq_conv2;
} adas_classifier_parameters_t;

/* PL 이 읽고 쓰는 DDR 버퍼의 물리 주소. */
typedef struct adas_classifier_buffer_addresses {
    uintptr_t ifmap;
    uintptr_t w_conv0;
    uintptr_t w_conv1;
    uintptr_t w_conv2;
    uintptr_t output;
} adas_classifier_buffer_addresses_t;

#ifdef __cplusplus
}
#endif

#endif  // ADAS_CLASSIFIER_CONTRACT_H
