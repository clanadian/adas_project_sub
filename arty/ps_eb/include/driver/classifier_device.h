#ifndef ADAS_CLASSIFIER_DEVICE_H
#define ADAS_CLASSIFIER_DEVICE_H

#include <stdint.h>

#include "accelerator/classifier_accelerator.h"
#include "driver/adas_classifier_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adas_classifier_device_status {
    ADAS_CLASSIFIER_DEVICE_OK = 0,
    ADAS_CLASSIFIER_DEVICE_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_DEVICE_IO_ERROR = -2,
    ADAS_CLASSIFIER_DEVICE_ABI_MISMATCH = -3
} adas_classifier_device_status_t;

typedef struct adas_classifier_device {
    int fd;
    uint8_t* dma;
    uint32_t dma_span;
} adas_classifier_device_t;

void adas_classifier_device_init(adas_classifier_device_t* device);

adas_classifier_device_status_t adas_classifier_device_open(
    adas_classifier_device_t* device,
    const char* path
);

adas_classifier_device_status_t adas_classifier_device_load_parameters(
    adas_classifier_device_t* device,
    const adas_classifier_parameters_t* parameters
);

adas_classifier_device_status_t adas_classifier_device_run(
    adas_classifier_device_t* device,
    uint32_t timeout_ms
);

/*
 * 6-op 실행 상태를 읽는다. 타임아웃이 났을 때 어느 엔진에서 멈췄는지가
 * 곧 조사 시작점이다 - 세 엔진 중 하나만 죽어도 증상은 "결과가 안 온다"로
 * 똑같이 보인다.
 */
/*
 * op 하나만 실행한다. 단계별 golden 대조와 보드 첫 점등에 쓴다.
 * op_index 는 0..5 (conv0, pool0, conv1, pool1, conv2, pool2).
 */
adas_classifier_device_status_t adas_classifier_device_run_op(
    adas_classifier_device_t* device,
    uint32_t op_index,
    uint32_t timeout_ms
);

adas_classifier_device_status_t adas_classifier_device_status(
    adas_classifier_device_t* device,
    struct adas_classifier_status_uapi* status
);

int8_t* adas_classifier_device_ifmap(adas_classifier_device_t* device);
int8_t* adas_classifier_device_w_conv0(adas_classifier_device_t* device);
int8_t* adas_classifier_device_w_conv1(adas_classifier_device_t* device);
int8_t* adas_classifier_device_w_conv2(adas_classifier_device_t* device);
/*
 * bias 는 EB 에서 DDR 버퍼에 있다. DB 는 AXI-Lite 레지스터로 넣었으므로
 * 이 세 함수가 DB 판에는 없다.
 */
int32_t* adas_classifier_device_b_conv0(adas_classifier_device_t* device);
int32_t* adas_classifier_device_b_conv1(adas_classifier_device_t* device);
int32_t* adas_classifier_device_b_conv2(adas_classifier_device_t* device);
/* 중간 활성값. 단계별 golden 대조에 쓴다. */
int8_t* adas_classifier_device_act_a(adas_classifier_device_t* device);
int8_t* adas_classifier_device_act_b(adas_classifier_device_t* device);
/* 6-op 이 끝나면 최종 12x12x64 는 act_b 에 있다. */
int8_t* adas_classifier_device_output(adas_classifier_device_t* device);

void adas_classifier_device_close(adas_classifier_device_t* device);

#ifdef __cplusplus
}
#endif

#endif
