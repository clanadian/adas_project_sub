#include "accelerator/classifier_buffers.h"

#include <stdint.h>

/* base + offset 계산이 uintptr_t 범위를 넘는지 먼저 검사한다. */
static int address_add_would_overflow(uintptr_t base, size_t offset) {
    return offset > (size_t)(UINTPTR_MAX - base);
}

adas_classifier_buffer_status_t adas_classifier_buffers_make_layout(
    uintptr_t reserved_base,
    size_t reserved_span,
    adas_classifier_buffer_layout_t* layout
) {
    if (layout == NULL || reserved_base == 0u) {
        return ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT;
    }

    if (reserved_base % ADAS_CLASSIFIER_BUFFER_ALIGNMENT != 0u) {
        return ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT;
    }

    if (reserved_span < ADAS_CLASSIFIER_BUFFER_SPAN) {
        return ADAS_CLASSIFIER_BUFFER_TOO_SMALL;
    }

    /* 가장 뒤의 주소까지 더할 수 있으면 앞의 offset 들도 안전하다. */
    if (address_add_would_overflow(
            reserved_base,
            ADAS_CLASSIFIER_BUFFER_SPAN - 1u
        )) {
        return ADAS_CLASSIFIER_BUFFER_ADDRESS_OVERFLOW;
    }

    layout->reserved_base = reserved_base;
    layout->reserved_span = reserved_span;

    /*
     * PL 은 포인터가 아니라 DDR 물리 주소 숫자를 받는다. 엔진의 AXI master 가
     * 그 주소를 직접 읽고 쓴다.
     */
    layout->addresses.roi     = reserved_base + ADAS_CLASSIFIER_ROI_OFFSET;
    layout->addresses.act_a   = reserved_base + ADAS_CLASSIFIER_ACT_A_OFFSET;
    layout->addresses.act_b   = reserved_base + ADAS_CLASSIFIER_ACT_B_OFFSET;
    layout->addresses.w_conv0 = reserved_base + ADAS_CLASSIFIER_W_CONV0_OFFSET;
    layout->addresses.w_conv1 = reserved_base + ADAS_CLASSIFIER_W_CONV1_OFFSET;
    layout->addresses.w_conv2 = reserved_base + ADAS_CLASSIFIER_W_CONV2_OFFSET;
    layout->addresses.b_conv0 = reserved_base + ADAS_CLASSIFIER_B_CONV0_OFFSET;
    layout->addresses.b_conv1 = reserved_base + ADAS_CLASSIFIER_B_CONV1_OFFSET;
    layout->addresses.b_conv2 = reserved_base + ADAS_CLASSIFIER_B_CONV2_OFFSET;

    return ADAS_CLASSIFIER_BUFFER_OK;
}

uintptr_t adas_classifier_buffers_output_address(
    const adas_classifier_buffer_addresses_t* addresses
) {
    /*
     * conv0 이 roi -> act_a 로 시작하고 op 마다 ping-pong 하므로,
     * op 개수가 짝수면 마지막 출력은 act_b 다. 지금은 6개다.
     */
    if (addresses == NULL) {
        return 0u;
    }
    return (ADAS_EB_NUM_OPS % 2u == 0u) ? addresses->act_b : addresses->act_a;
}
