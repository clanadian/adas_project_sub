#include "accelerator/classifier_buffers.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void test_valid_layout(void) {
    /* 실제 보드 주소 대신 정렬된 가짜 물리 주소로 계산만 검증합니다. */
    const uintptr_t base = (uintptr_t)0x10000000u;
    adas_classifier_buffer_layout_t layout = {0};

    assert(adas_classifier_buffers_make_layout(
        base,
        ADAS_CLASSIFIER_BUFFER_SPAN,
        &layout
    ) == ADAS_CLASSIFIER_BUFFER_OK);

    assert(layout.reserved_base == base);
    assert(layout.reserved_span == ADAS_CLASSIFIER_BUFFER_SPAN);
    assert(layout.addresses.ifmap == base + ADAS_CLASSIFIER_IFMAP_OFFSET);
    assert(layout.addresses.w_conv0 == base + ADAS_CLASSIFIER_W_CONV0_OFFSET);
    assert(layout.addresses.w_conv1 == base + ADAS_CLASSIFIER_W_CONV1_OFFSET);
    assert(layout.addresses.w_conv2 == base + ADAS_CLASSIFIER_W_CONV2_OFFSET);
    assert(layout.addresses.output == base + ADAS_CLASSIFIER_OUTPUT_OFFSET);
}

static void test_invalid_arguments(void) {
    adas_classifier_buffer_layout_t layout = {0};

    /* 0번지는 유효한 예약 DDR 시작 주소로 취급하지 않습니다. */
    assert(adas_classifier_buffers_make_layout(
        0u,
        ADAS_CLASSIFIER_BUFFER_SPAN,
        &layout
    ) == ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT);

    /* 4 KiB로 정렬되지 않은 시작 주소도 거부합니다. */
    assert(adas_classifier_buffers_make_layout(
        (uintptr_t)0x10000001u,
        ADAS_CLASSIFIER_BUFFER_SPAN,
        &layout
    ) == ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT);

    /* 결과를 쓸 곳이 없으면 계산 결과를 반환할 수 없습니다. */
    assert(adas_classifier_buffers_make_layout(
        (uintptr_t)0x10000000u,
        ADAS_CLASSIFIER_BUFFER_SPAN,
        NULL
    ) == ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT);
}

static void test_small_region_and_overflow(void) {
    adas_classifier_buffer_layout_t layout = {0};

    /* 필요한 0x13000보다 한 바이트라도 작으면 마지막 버퍼가 잘립니다. */
    assert(adas_classifier_buffers_make_layout(
        (uintptr_t)0x10000000u,
        ADAS_CLASSIFIER_BUFFER_SPAN - 1u,
        &layout
    ) == ADAS_CLASSIFIER_BUFFER_TOO_SMALL);

    /* 주소 공간 끝을 넘어가는 base + offset 계산도 거부합니다. */
    const uintptr_t near_end =
        UINTPTR_MAX - (uintptr_t)ADAS_CLASSIFIER_BUFFER_ALIGNMENT + 1u;
    assert(near_end % ADAS_CLASSIFIER_BUFFER_ALIGNMENT == 0u);
    assert(adas_classifier_buffers_make_layout(
        near_end,
        ADAS_CLASSIFIER_BUFFER_SPAN,
        &layout
    ) == ADAS_CLASSIFIER_BUFFER_ADDRESS_OVERFLOW);
}

int main(void) {
    test_valid_layout();
    test_invalid_arguments();
    test_small_region_and_overflow();

    puts("Classifier buffer layout tests passed");
    return EXIT_SUCCESS;
}
