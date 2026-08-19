#ifndef ADAS_CLASSIFIER_BUFFERS_H
#define ADAS_CLASSIFIER_BUFFERS_H

#include <stddef.h>
#include <stdint.h>

#include "accelerator/classifier_accelerator.h"
#include "driver/adas_classifier_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 버퍼 크기·offset 의 정본은 driver/adas_classifier_uapi.h 다.
 * 커널 드라이버가 같은 값을 쓰기 때문이며, 여기서 다시 정의하면
 * 사용자 공간과 커널이 갈라진다. 이 헤더는 이름만 이어 준다.
 */
#define ADAS_CLASSIFIER_IFMAP_SIZE_BYTES   ADAS_CLASSIFIER_IFMAP_SIZE
#define ADAS_CLASSIFIER_OUTPUT_SIZE_BYTES  ADAS_CLASSIFIER_OUTPUT_SIZE

/*
 * 예약 DDR 시작 주소는 4 KiB 정렬이어야 한다. 그래야 uapi 의 모든 offset 이
 * 그대로 4 KiB 정렬을 유지한다(엔진 요구는 4 바이트지만, 버퍼가 캐시 라인을
 * 독점해야 flush/invalidate 가 옆 버퍼를 건드리지 않는다).
 */
#define ADAS_CLASSIFIER_BUFFER_ALIGNMENT 0x1000u
#define ADAS_CLASSIFIER_BUFFER_SPAN      ADAS_CLASSIFIER_DMA_SPAN

typedef enum adas_classifier_buffer_status {
    ADAS_CLASSIFIER_BUFFER_OK = 0,
    ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_BUFFER_TOO_SMALL = -2,
    ADAS_CLASSIFIER_BUFFER_ADDRESS_OVERFLOW = -3
} adas_classifier_buffer_status_t;

typedef struct adas_classifier_buffer_layout {
    uintptr_t reserved_base;
    size_t reserved_span;
    adas_classifier_buffer_addresses_t addresses;
} adas_classifier_buffer_layout_t;

/*
 * 예약 DDR 시작 주소에서 9개 버퍼의 물리 주소를 계산한다.
 * mmap 이나 복사는 하지 않으므로 PC 단위 테스트로 검증할 수 있다.
 */
adas_classifier_buffer_status_t adas_classifier_buffers_make_layout(
    uintptr_t reserved_base,
    size_t reserved_span,
    adas_classifier_buffer_layout_t* layout
);

/*
 * 6-op 이 끝났을 때 최종 12x12x64 가 들어 있는 버퍼의 주소.
 *
 * ping-pong 이 짝수 번(6번) 돌아 항상 act_b 로 끝난다. 이 함수를 두는 이유는
 * "왜 act_b 인가"를 한 곳에만 적어 두기 위해서다 — op 개수가 홀수로 바뀌면
 * 여기만 고치면 된다.
 */
uintptr_t adas_classifier_buffers_output_address(
    const adas_classifier_buffer_addresses_t* addresses
);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_CLASSIFIER_BUFFERS_H
