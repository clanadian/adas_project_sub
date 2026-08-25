#include "accelerator/classifier_buffers.h"

#include <stdint.h>

/* base + offset 계산이 uintptr_t 범위를 넘는지 먼저 검사합니다. */
static int address_add_would_overflow(uintptr_t base, size_t offset) {
    return offset > (size_t)(UINTPTR_MAX - base);
}

adas_classifier_buffer_status_t adas_classifier_buffers_make_layout(
    uintptr_t reserved_base,
    size_t reserved_span,
    adas_classifier_buffer_layout_t* layout
) {
    /* 결과를 기록할 구조체와 실제 DDR 시작 주소가 반드시 필요합니다. */
    if (layout == NULL || reserved_base == 0u) {
        return ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT;
    }

    /*
     * 시작 주소가 4 KiB 경계에 있어야 아래의 모든 고정 offset도
     * 자연스럽게 4 KiB 정렬을 유지합니다.
     */
    if (reserved_base % ADAS_CLASSIFIER_BUFFER_ALIGNMENT != 0u) {
        return ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT;
    }

    /* 예약 DDR이 전체 레이아웃을 담기에 충분한지 확인합니다. */
    if (reserved_span < ADAS_CLASSIFIER_BUFFER_SPAN) {
        return ADAS_CLASSIFIER_BUFFER_TOO_SMALL;
    }

    /* 가장 뒤의 주소까지 더할 수 있다면 앞의 offset들도 안전합니다. */
    if (address_add_would_overflow(
            reserved_base,
            ADAS_CLASSIFIER_BUFFER_SPAN - 1u
        )) {
        return ADAS_CLASSIFIER_BUFFER_ADDRESS_OVERFLOW;
    }

    /* 호출자가 준 예약 영역 자체의 정보도 보존합니다. */
    layout->reserved_base = reserved_base;
    layout->reserved_span = reserved_span;

    /*
     * PL은 포인터 자체를 전달받는 것이 아니라 DDR 물리 주소 숫자를 받습니다.
     * 운영 경로에서 이 값을 AXI-Lite 레지스터에 기록하는 것은 커널 드라이버
     * (adas_classifier_drv)이고, PL의 AXI master가 그 주소의 DDR을 읽고 씁니다.
     */
    layout->addresses.ifmap =
        reserved_base + ADAS_CLASSIFIER_IFMAP_OFFSET;
    layout->addresses.w_conv0 =
        reserved_base + ADAS_CLASSIFIER_W_CONV0_OFFSET;
    layout->addresses.w_conv1 =
        reserved_base + ADAS_CLASSIFIER_W_CONV1_OFFSET;
    layout->addresses.w_conv2 =
        reserved_base + ADAS_CLASSIFIER_W_CONV2_OFFSET;
    layout->addresses.output =
        reserved_base + ADAS_CLASSIFIER_OUTPUT_OFFSET;

    return ADAS_CLASSIFIER_BUFFER_OK;
}
