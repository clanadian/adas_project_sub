#include "accelerator/classifier_accelerator.h"
#include "accelerator/classifier_buffers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_BASE 0x1f000000u

static adas_classifier_buffer_addresses_t make_addresses(void) {
    adas_classifier_buffer_layout_t layout;
    const adas_classifier_buffer_status_t status =
        adas_classifier_buffers_make_layout(
            TEST_BASE, ADAS_CLASSIFIER_BUFFER_SPAN, &layout);
    assert(status == ADAS_CLASSIFIER_BUFFER_OK);
    return layout.addresses;
}

static void test_init_leaves_everything_closed(void) {
    adas_classifier_accelerator_t accelerator;
    adas_classifier_accelerator_init(&accelerator);

    /* 설정 전 실행은 하드웨어를 열었는지와 무관하게 거부돼야 한다. */
    assert(adas_classifier_accelerator_run_op(&accelerator, 0u, 100u)
           == ADAS_CLASSIFIER_NOT_CONFIGURED);
    assert(adas_classifier_accelerator_run(&accelerator, 100u, NULL)
           == ADAS_CLASSIFIER_NOT_CONFIGURED);

    /* NULL 을 넘겨도 죽지 않아야 정리 코드가 단순해진다. */
    adas_classifier_accelerator_init(NULL);
    adas_classifier_accelerator_close(NULL);
    adas_classifier_accelerator_close(&accelerator);
}

static void test_rejects_bad_addresses(void) {
    adas_classifier_accelerator_t accelerator;
    adas_classifier_accelerator_init(&accelerator);

    assert(adas_classifier_accelerator_configure_buffers(&accelerator, NULL)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    adas_classifier_buffer_addresses_t addresses = make_addresses();
    assert(adas_classifier_accelerator_configure_buffers(&accelerator, &addresses)
           == ADAS_CLASSIFIER_OK);

    /* 0 주소는 "아직 안 채웠다"는 뜻이다. 그대로 쓰면 PL 이 0번지를 읽는다. */
    adas_classifier_buffer_addresses_t zeroed = addresses;
    zeroed.b_conv1 = 0u;
    assert(adas_classifier_accelerator_configure_buffers(&accelerator, &zeroed)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    /* m_axi 포트가 32비트라 4바이트 정렬이 아니면 접근이 어긋난다. */
    adas_classifier_buffer_addresses_t misaligned = addresses;
    misaligned.act_a += 1u;
    assert(adas_classifier_accelerator_configure_buffers(&accelerator, &misaligned)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    /* act_a == act_b 면 op 이 자기가 읽는 버퍼를 덮어쓴다. */
    adas_classifier_buffer_addresses_t aliased = addresses;
    aliased.act_b = aliased.act_a;
    assert(adas_classifier_accelerator_configure_buffers(&accelerator, &aliased)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);
}

/*
 * multiplier 0 은 "manifest 값을 아직 안 넣었다"는 뜻이다. 그대로 돌리면
 * 출력이 전부 0 이 되고, 그 증상은 배선 오류와 구분되지 않는다.
 */
static void test_rejects_unset_requant(void) {
    adas_classifier_accelerator_t accelerator;
    adas_classifier_accelerator_init(&accelerator);

    adas_classifier_parameters_t parameters = {
        .rq_conv0 = { 1545298110, 37u, 1u },
        .rq_conv1 = { 1525725976, 36u, 1u },
        .rq_conv2 = { 1924470265, 39u, 0u }
    };
    assert(adas_classifier_accelerator_load_parameters(&accelerator, &parameters)
           == ADAS_CLASSIFIER_OK);

    adas_classifier_parameters_t unset = parameters;
    unset.rq_conv1.multiplier = 0;
    assert(adas_classifier_accelerator_load_parameters(&accelerator, &unset)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    /* shift 는 64비트 중간값에 대한 오른쪽 시프트라 63을 넘길 수 없다. */
    adas_classifier_parameters_t huge_shift = parameters;
    huge_shift.rq_conv2.shift = 64u;
    assert(adas_classifier_accelerator_load_parameters(&accelerator, &huge_shift)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    assert(adas_classifier_accelerator_load_parameters(&accelerator, NULL)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);
}

/*
 * ping-pong 규칙. 커널 드라이버에도 같은 규칙이 있으므로 여기서 고정한다.
 *   op0        : roi   -> act_a
 *   홀수 op    : act_a -> act_b
 *   짝수 op(>0): act_b -> act_a
 */
static void test_ping_pong_sequence(void) {
    const adas_classifier_buffer_addresses_t addresses = make_addresses();
    uintptr_t source = 0u;
    uintptr_t destination = 0u;

    const struct { uintptr_t source; uintptr_t destination; } expected[] = {
        { addresses.roi,   addresses.act_a },  /* conv0 */
        { addresses.act_a, addresses.act_b },  /* pool0 */
        { addresses.act_b, addresses.act_a },  /* conv1 */
        { addresses.act_a, addresses.act_b },  /* pool1 */
        { addresses.act_b, addresses.act_a },  /* conv2 */
        { addresses.act_a, addresses.act_b },  /* pool2 */
    };

    for (unsigned i = 0u; i < ADAS_EB_NUM_OPS; ++i) {
        assert(adas_classifier_accelerator_op_buffers(&addresses, i,
                                                      &source, &destination)
               == ADAS_CLASSIFIER_OK);
        assert(source == expected[i].source);
        assert(destination == expected[i].destination);
        /* 어떤 op 도 자기 입력 버퍼에 쓰면 안 된다. */
        assert(source != destination);
    }

    /* 마지막 op 의 출력이 곧 PS 가 읽을 곳이다. */
    assert(destination == adas_classifier_buffers_output_address(&addresses));

    assert(adas_classifier_accelerator_op_buffers(&addresses, ADAS_EB_NUM_OPS,
                                                  &source, &destination)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);
    assert(adas_classifier_accelerator_op_buffers(NULL, 0u, &source, &destination)
           == ADAS_CLASSIFIER_INVALID_ARGUMENT);
}

/*
 * op 표가 계약과 맞는지 본다. 형상이 하나만 틀려도 엔진은 에러 없이
 * 다른 크기를 쓰고, 다음 op 이 그걸 자기 형상으로 읽는다.
 */
static void test_op_table_matches_contract(void) {
    static const struct adas_eb_op ops[] = ADAS_EB_OP_TABLE_INITIALIZER;

    assert(sizeof(ops) / sizeof(ops[0]) == ADAS_EB_NUM_OPS);

    /* conv0 는 미리 패딩된 98x98 을 받고 pad 포트가 없다. */
    assert(ops[0].kind == ADAS_EB_OP_CONV0);
    assert(ops[0].img_h == 98u && ops[0].img_w == 98u);
    assert(ops[0].pad == 0u);
    assert(ops[0].in_ch == 3u && ops[0].out_ch == 16u);

    /* conv1/conv2 는 자기 입력 크기를 받고 엔진이 pad=1 을 처리한다. */
    assert(ops[2].kind == ADAS_EB_OP_CONV && ops[2].pad == 1u);
    assert(ops[4].kind == ADAS_EB_OP_CONV && ops[4].pad == 1u);

    /* stride 2 는 RTL 에 없다. conv 는 전부 1 이어야 한다. */
    assert(ops[0].stride == 1u && ops[2].stride == 1u && ops[4].stride == 1u);
    /* maxpool 만 stride 2 다. */
    assert(ops[1].stride == 2u && ops[3].stride == 2u && ops[5].stride == 2u);

    /* 공간 크기 사슬: 98 -> 96 -> 48 -> 48 -> 24 -> 24 -> 12 */
    assert(ops[1].img_h == 96u && ops[2].img_h == 48u);
    assert(ops[3].img_h == 48u && ops[4].img_h == 24u && ops[5].img_h == 24u);

    /* conv 3단이 requant 인덱스 0,1,2 를 순서대로 쓴다. */
    assert(ops[0].conv_index == 0u);
    assert(ops[2].conv_index == 1u);
    assert(ops[4].conv_index == 2u);
}

int main(void) {
    test_init_leaves_everything_closed();
    test_rejects_bad_addresses();
    test_rejects_unset_requant();
    test_ping_pong_sequence();
    test_op_table_matches_contract();
    puts("classifier accelerator tests passed");
    return EXIT_SUCCESS;
}
