#ifndef ADAS_CLASSIFIER_ACCELERATOR_H
#define ADAS_CLASSIFIER_ACCELERATOR_H

#include <stdint.h>

#include "accelerator/classifier_registers.h"
#include "accelerator/pl_mmio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adas_classifier_status {
    ADAS_CLASSIFIER_OK = 0,
    ADAS_CLASSIFIER_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_MMIO_ERROR = -2,
    ADAS_CLASSIFIER_TIMEOUT = -3,
    ADAS_CLASSIFIER_NOT_CONFIGURED = -4
} adas_classifier_status_t;

/*
 * PL 이 접근할 DDR 물리 주소들.
 *
 * DB 판은 입력·가중치·출력 5개였지만, EB 는 중간 활성값이 DDR 을 왕복하므로
 * ping-pong 버퍼 두 개와 bias 세 개가 더 필요하다. bias 가 주소인 것이
 * DB 와의 가장 큰 계약 차이다 — DB 는 AXI-Lite 레지스터로 받았다.
 */
typedef struct adas_classifier_buffer_addresses {
    uintptr_t roi;      /* 98x98x3, PS 가 zero border 까지 채워 넣는다 */
    uintptr_t act_a;    /* ping-pong A */
    uintptr_t act_b;    /* ping-pong B — 6-op 이 끝나면 여기 최종 출력이 있다 */
    uintptr_t w_conv0;  /* OIHW, 전치 없음 */
    uintptr_t w_conv1;  /* WPACK */
    uintptr_t w_conv2;  /* WPACK */
    uintptr_t b_conv0;
    uintptr_t b_conv1;
    uintptr_t b_conv2;
} adas_classifier_buffer_addresses_t;

/*
 * conv 한 단의 requant. `leaky` 는 EB 전용이다 — 엔진이 requant 이전에
 * LeakyReLU(13/128)를 적용할지 레지스터로 받는다. 모델 manifest 의
 * activation 과 어긋나면 오류 없이 값만 틀린다.
 */
typedef struct adas_classifier_requant {
    int32_t multiplier;
    uint8_t shift;
    uint8_t leaky;
} adas_classifier_requant_t;

typedef struct adas_classifier_parameters {
    adas_classifier_requant_t rq_conv0;
    adas_classifier_requant_t rq_conv1;
    adas_classifier_requant_t rq_conv2;
} adas_classifier_parameters_t;

/*
 * 엔진 3개의 제어 창과, op 마다 다시 써야 하는 설정값을 들고 있다.
 *
 * DB 는 주소·파라미터를 한 번 쓰고 ap_start 한 번이면 끝이었다. EB 는 op
 * 6개가 각자 주소·형상·requant 를 자기 엔진에 다시 써야 하므로, 설정을
 * 객체에 보관했다가 run 시점에 프로그램한다.
 */
typedef struct adas_classifier_accelerator {
    adas_pl_mmio_region_t conv_region;
    adas_pl_mmio_region_t conv0_region;
    adas_pl_mmio_region_t maxpool_region;
    adas_classifier_buffer_addresses_t addresses;
    adas_classifier_parameters_t parameters;
    int addresses_valid;
    int parameters_valid;
} adas_classifier_accelerator_t;

void adas_classifier_accelerator_init(
    adas_classifier_accelerator_t* accelerator
);

/* 세 엔진의 제어 창을 /dev/mem 으로 mmap 한다. 하나라도 실패하면 전부 되돌린다. */
adas_classifier_status_t adas_classifier_accelerator_open(
    adas_classifier_accelerator_t* accelerator
);

/*
 * 주소를 레지스터에 바로 쓰지 않고 보관만 한다. op 마다 대상 엔진이
 * 다르고 ifmap/ofmap 이 ping-pong 으로 바뀌기 때문이다.
 */
adas_classifier_status_t adas_classifier_accelerator_configure_buffers(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_buffer_addresses_t* addresses
);

adas_classifier_status_t adas_classifier_accelerator_load_parameters(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_parameters_t* parameters
);

/*
 * op 하나의 입력·출력 버퍼를 계산한다.
 *
 * 공개하는 이유는 이 규칙이 **조용히 틀리는** 종류이기 때문이다. ping-pong
 * 방향이 어긋나면 op 이 자기가 읽는 버퍼를 덮어써서, 크래시 없이 값만
 * 틀린다. 같은 규칙이 커널 드라이버에도 있어 단위 테스트로 고정해 둔다.
 */
adas_classifier_status_t adas_classifier_accelerator_op_buffers(
    const adas_classifier_buffer_addresses_t* addresses,
    unsigned op_index,
    uintptr_t* source,
    uintptr_t* destination
);

/*
 * op 하나만 실행한다. 보드 첫 점등에서 엔진을 하나씩 확인할 때 쓴다
 * (pl_eb/HANDOFF.md §3-b 의 순서).
 * op_index 는 0..5 이며 ADAS_EB_OP_TABLE_INITIALIZER 의 순서를 따른다.
 */
adas_classifier_status_t adas_classifier_accelerator_run_op(
    adas_classifier_accelerator_t* accelerator,
    unsigned op_index,
    uint32_t timeout_ms
);

/*
 * 6-op 전체를 순서대로 실행한다. 완료 후 최종 12x12x64 는 act_b 에 있다.
 * 실패한 op 의 인덱스를 알고 싶으면 failed_op 에 포인터를 준다(선택).
 */
adas_classifier_status_t adas_classifier_accelerator_run(
    adas_classifier_accelerator_t* accelerator,
    uint32_t timeout_ms,
    unsigned* failed_op
);

void adas_classifier_accelerator_close(
    adas_classifier_accelerator_t* accelerator
);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_CLASSIFIER_ACCELERATOR_H
