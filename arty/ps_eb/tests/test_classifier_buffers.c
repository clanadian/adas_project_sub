#include "accelerator/classifier_buffers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* 예약 DDR 시작 주소로 쓸 임의의 4 KiB 정렬 값. */
#define TEST_BASE 0x1f000000u

static void test_rejects_bad_arguments(void) {
    adas_classifier_buffer_layout_t layout;

    assert(adas_classifier_buffers_make_layout(TEST_BASE, ADAS_CLASSIFIER_BUFFER_SPAN, NULL)
           == ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT);
    assert(adas_classifier_buffers_make_layout(0u, ADAS_CLASSIFIER_BUFFER_SPAN, &layout)
           == ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT);
    /* 4 KiB 정렬이 아니면 뒤의 모든 offset 이 정렬을 잃는다. */
    assert(adas_classifier_buffers_make_layout(TEST_BASE + 0x800u,
                                               ADAS_CLASSIFIER_BUFFER_SPAN, &layout)
           == ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT);
    assert(adas_classifier_buffers_make_layout(TEST_BASE,
                                               ADAS_CLASSIFIER_BUFFER_SPAN - 1u, &layout)
           == ADAS_CLASSIFIER_BUFFER_TOO_SMALL);
}

static void test_layout_matches_uapi_offsets(void) {
    adas_classifier_buffer_layout_t layout;

    assert(adas_classifier_buffers_make_layout(TEST_BASE,
                                               ADAS_CLASSIFIER_BUFFER_SPAN, &layout)
           == ADAS_CLASSIFIER_BUFFER_OK);

    /*
     * 커널 드라이버가 같은 uapi offset 으로 주소를 만든다. 사용자 공간이
     * 다른 값을 계산하면 PL 이 엉뚱한 DDR 을 읽는데, 그건 크래시가 아니라
     * 조용히 틀린 결과다.
     */
    assert(layout.addresses.roi     == TEST_BASE + ADAS_CLASSIFIER_ROI_OFFSET);
    assert(layout.addresses.act_a   == TEST_BASE + ADAS_CLASSIFIER_ACT_A_OFFSET);
    assert(layout.addresses.act_b   == TEST_BASE + ADAS_CLASSIFIER_ACT_B_OFFSET);
    assert(layout.addresses.w_conv0 == TEST_BASE + ADAS_CLASSIFIER_W_CONV0_OFFSET);
    assert(layout.addresses.w_conv1 == TEST_BASE + ADAS_CLASSIFIER_W_CONV1_OFFSET);
    assert(layout.addresses.w_conv2 == TEST_BASE + ADAS_CLASSIFIER_W_CONV2_OFFSET);
    assert(layout.addresses.b_conv0 == TEST_BASE + ADAS_CLASSIFIER_B_CONV0_OFFSET);
    assert(layout.addresses.b_conv1 == TEST_BASE + ADAS_CLASSIFIER_B_CONV1_OFFSET);
    assert(layout.addresses.b_conv2 == TEST_BASE + ADAS_CLASSIFIER_B_CONV2_OFFSET);
}

/*
 * 버퍼가 서로 겹치면 op 이 자기가 읽는 중인 데이터를 덮어쓴다. 크래시가
 * 아니라 값만 틀리므로 실행 중에는 드러나지 않는다.
 */
static void test_buffers_do_not_overlap(void) {
    adas_classifier_buffer_layout_t layout;
    assert(adas_classifier_buffers_make_layout(TEST_BASE,
                                               ADAS_CLASSIFIER_BUFFER_SPAN, &layout)
           == ADAS_CLASSIFIER_BUFFER_OK);

    const struct { uintptr_t start; size_t bytes; } regions[] = {
        { layout.addresses.roi,     ADAS_CLASSIFIER_IFMAP_SIZE },
        { layout.addresses.act_a,   ADAS_CLASSIFIER_ACT_SIZE },
        { layout.addresses.act_b,   ADAS_CLASSIFIER_ACT_SIZE },
        { layout.addresses.w_conv0, ADAS_CLASSIFIER_W_CONV0_SIZE },
        { layout.addresses.w_conv1, ADAS_CLASSIFIER_W_CONV1_SIZE },
        { layout.addresses.w_conv2, ADAS_CLASSIFIER_W_CONV2_SIZE },
        { layout.addresses.b_conv0, ADAS_CLASSIFIER_B_CONV0_SIZE },
        { layout.addresses.b_conv1, ADAS_CLASSIFIER_B_CONV1_SIZE },
        { layout.addresses.b_conv2, ADAS_CLASSIFIER_B_CONV2_SIZE },
    };
    const size_t count = sizeof(regions) / sizeof(regions[0]);

    for (size_t i = 0u; i < count; ++i) {
        /* 엔진의 m_axi 포트는 32비트다. 정렬이 깨지면 접근 자체가 어긋난다. */
        assert(regions[i].start % 4u == 0u);
        /* 전부 예약 영역 안에 들어와야 한다. */
        assert(regions[i].start >= TEST_BASE);
        assert(regions[i].start + regions[i].bytes
               <= TEST_BASE + ADAS_CLASSIFIER_BUFFER_SPAN);
        for (size_t j = i + 1u; j < count; ++j) {
            const int disjoint =
                regions[i].start + regions[i].bytes <= regions[j].start
                || regions[j].start + regions[j].bytes <= regions[i].start;
            assert(disjoint);
        }
    }
}

/*
 * ping-pong 이 짝수 번 돌아 최종 출력은 act_b 에 있다. uapi 의
 * OUTPUT_OFFSET 과 어긋나면 PS 가 중간 활성값을 결과로 읽는다.
 */
static void test_output_is_act_b(void) {
    adas_classifier_buffer_layout_t layout;
    assert(adas_classifier_buffers_make_layout(TEST_BASE,
                                               ADAS_CLASSIFIER_BUFFER_SPAN, &layout)
           == ADAS_CLASSIFIER_BUFFER_OK);

    assert(ADAS_EB_NUM_OPS % 2u == 0u);
    assert(adas_classifier_buffers_output_address(&layout.addresses)
           == layout.addresses.act_b);
    assert(ADAS_CLASSIFIER_OUTPUT_OFFSET == ADAS_CLASSIFIER_ACT_B_OFFSET);
    assert(adas_classifier_buffers_output_address(NULL) == 0u);
}

/*
 * 가장 큰 중간 텐서(conv0 출력)가 활성 버퍼에 들어가야 한다. 이 검사가
 * 없으면 채널 수가 늘 때 act_a 가 act_b 를 침범한다.
 */
static void test_act_buffer_holds_every_intermediate(void) {
    const size_t intermediates[] = {
        96u * 96u * 16u,  /* conv0 */
        48u * 48u * 16u,  /* pool0 */
        48u * 48u * 32u,  /* conv1 */
        24u * 24u * 32u,  /* pool1 */
        24u * 24u * 64u,  /* conv2 */
        12u * 12u * 64u,  /* pool2 */
    };
    for (size_t i = 0u; i < sizeof(intermediates) / sizeof(intermediates[0]); ++i) {
        assert(intermediates[i] <= ADAS_CLASSIFIER_ACT_SIZE);
    }
    assert(ADAS_CLASSIFIER_OUTPUT_SIZE == 12u * 12u * 64u);
    assert(ADAS_CLASSIFIER_IFMAP_SIZE == 98u * 98u * 3u);
}

int main(void) {
    test_rejects_bad_arguments();
    test_layout_matches_uapi_offsets();
    test_buffers_do_not_overlap();
    test_output_is_act_b();
    test_act_buffer_holds_every_intermediate();
    puts("classifier buffer tests passed");
    return EXIT_SUCCESS;
}
