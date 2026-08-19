#ifndef ADAS_CLASSIFIER_UAPI_H
#define ADAS_CLASSIFIER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#include "adas_classifier_eb_hw.h"

#define ADAS_CLASSIFIER_DEVICE_NAME "adas_classifier"

/*
 * coherent DMA 영역의 배치 — EB 판.
 *
 * DB 와 달리 중간 활성값이 DDR 을 왕복한다. 엔진 6개가 순차로 돌면서
 * act_a / act_b 를 번갈아 쓰기 때문에 **ping-pong 버퍼 두 개**가 필요하다.
 * 크기는 가장 큰 중간 텐서인 conv0 출력(96*96*16)으로 정한다.
 *
 *   conv0 : roi   -> act_a      pool0 : act_a -> act_b
 *   conv1 : act_b -> act_a      pool1 : act_a -> act_b
 *   conv2 : act_b -> act_a      pool2 : act_a -> act_b
 *
 * 그래서 PS 가 읽을 최종 12x12x64 는 **항상 act_b** 에 있다.
 * bias 도 레지스터가 아니라 이 영역에 두고 주소로 넘긴다(EB 계약).
 *
 * 모든 offset 은 4 KiB 정렬이다. 엔진의 m_axi 포트는 4 바이트 정렬만
 * 요구하지만, 버퍼마다 캐시 라인을 독점해야 flush/invalidate 가 옆 버퍼를
 * 건드리지 않는다.
 */
#define ADAS_CLASSIFIER_ROI_OFFSET     0x00000u
#define ADAS_CLASSIFIER_ACT_A_OFFSET   0x08000u
#define ADAS_CLASSIFIER_ACT_B_OFFSET   0x2c000u
#define ADAS_CLASSIFIER_W_CONV0_OFFSET 0x50000u
#define ADAS_CLASSIFIER_W_CONV1_OFFSET 0x51000u
#define ADAS_CLASSIFIER_W_CONV2_OFFSET 0x53000u
#define ADAS_CLASSIFIER_B_CONV0_OFFSET 0x58000u
#define ADAS_CLASSIFIER_B_CONV1_OFFSET 0x59000u
#define ADAS_CLASSIFIER_B_CONV2_OFFSET 0x5a000u
#define ADAS_CLASSIFIER_DMA_SPAN       0x5b000u

/* PS 가 채우는 입력과 PS 가 읽는 출력. 이름은 DB 판과 맞춘다. */
#define ADAS_CLASSIFIER_IFMAP_OFFSET   ADAS_CLASSIFIER_ROI_OFFSET
#define ADAS_CLASSIFIER_OUTPUT_OFFSET  ADAS_CLASSIFIER_ACT_B_OFFSET

#define ADAS_CLASSIFIER_IFMAP_SIZE \
    (ADAS_EB_WIRE_SIZE * ADAS_EB_WIRE_SIZE * ADAS_EB_ROI_IN_CH)
#define ADAS_CLASSIFIER_ACT_SIZE \
    (ADAS_EB_ROI_SIZE * ADAS_EB_ROI_SIZE * ADAS_EB_CONV0_OUT_CH)
#define ADAS_CLASSIFIER_OUTPUT_SIZE \
    (ADAS_EB_GAP_IN_SIZE * ADAS_EB_GAP_IN_SIZE * ADAS_EB_GAP_IN_CH)

/* conv0 는 OIHW(전치 없음), conv1/conv2 는 WPACK. 크기는 같아도 내용이 다르다. */
#define ADAS_CLASSIFIER_W_CONV0_SIZE   (16u * 3u * 3u * 3u)
#define ADAS_CLASSIFIER_W_CONV1_SIZE   (32u * 16u * 3u * 3u)
#define ADAS_CLASSIFIER_W_CONV2_SIZE   (64u * 32u * 3u * 3u)
#define ADAS_CLASSIFIER_B_CONV0_SIZE   (16u * 4u)
#define ADAS_CLASSIFIER_B_CONV1_SIZE   (32u * 4u)
#define ADAS_CLASSIFIER_B_CONV2_SIZE   (64u * 4u)

/*
 * conv 한 단의 requant 파라미터.
 *
 * DB 판과 달리 `leaky` 가 있다. EB 엔진은 requant **이전** 에 LeakyReLU
 * (13/128)를 적용하고 활성화 여부를 레지스터로 받는다. 모델 manifest 의
 * activation 과 어긋나면 오류 없이 값만 틀린다.
 */
struct adas_classifier_requant_uapi {
    __s32 multiplier;
    __u32 shift;
    __u32 leaky;
    __u32 reserved;
};

/*
 * bias 는 여기 없다 — DDR 의 B_CONV*_OFFSET 에 사용자 공간이 직접 쓰고,
 * 드라이버는 그 주소만 엔진에 넘긴다. DB 판은 bias 를 AXI-Lite 레지스터로
 * 받았으므로 이 구조체가 서로 호환되지 않는다.
 */
struct adas_classifier_parameters_uapi {
    struct adas_classifier_requant_uapi requant[ADAS_EB_NUM_CONVS];
};

struct adas_classifier_run_uapi {
    __u32 timeout_ms;
    __u32 reserved;
};

/*
 * op 하나만 실행한다. 보드 첫 점등에서 엔진을 하나씩 확인하고, 단계별
 * golden 과 대조해 **어느 op 에서 갈렸는지** 짚기 위한 것이다.
 * 6-op 을 통째로 돌리면 최종 출력만 보이고 그 정보가 사라진다.
 */
struct adas_classifier_run_op_uapi {
    __u32 op_index;   /* 0..5 */
    __u32 timeout_ms;
};

/*
 * 6-op 중 어디까지 갔는지 돌려준다. 타임아웃이 났을 때 어느 엔진이
 * 멈췄는지가 곧 조사 시작점이다.
 */
struct adas_classifier_status_uapi {
    __u32 last_op;          /* 0..5, 마지막으로 시작한 op */
    __u32 completed_ops;    /* ap_done 까지 확인한 op 수 */
    __u32 last_op_us;       /* 마지막 op 의 소요 시간 */
    __u32 total_us;         /* RUN 전체 소요 시간 */
};

struct adas_classifier_info_uapi {
    __u32 abi_version;
    __u32 dma_span;
    __u32 ifmap_offset;
    __u32 output_offset;
    __u32 act_a_offset;
    __u32 act_b_offset;
    __u32 num_ops;
    __u32 reserved;
};

/*
 * ABI 2 = EB. DB(ABI 1)와 구조체 레이아웃이 다르므로 버전을 올린다.
 * 사용자 공간이 GET_INFO 로 확인하고, 다르면 실행하지 않는다 — 반대쪽
 * 드라이버 위에서 조용히 틀린 결과를 내는 것을 막는 유일한 방어선이다.
 */
#define ADAS_CLASSIFIER_ABI_VERSION 2u
#define ADAS_CLASSIFIER_IOC_MAGIC 'A'
#define ADAS_CLASSIFIER_IOC_GET_INFO \
    _IOR(ADAS_CLASSIFIER_IOC_MAGIC, 0x00, struct adas_classifier_info_uapi)
#define ADAS_CLASSIFIER_IOC_SET_PARAMETERS \
    _IOW(ADAS_CLASSIFIER_IOC_MAGIC, 0x01, struct adas_classifier_parameters_uapi)
#define ADAS_CLASSIFIER_IOC_RUN \
    _IOW(ADAS_CLASSIFIER_IOC_MAGIC, 0x02, struct adas_classifier_run_uapi)
#define ADAS_CLASSIFIER_IOC_GET_STATUS \
    _IOR(ADAS_CLASSIFIER_IOC_MAGIC, 0x03, struct adas_classifier_status_uapi)
#define ADAS_CLASSIFIER_IOC_RUN_OP \
    _IOW(ADAS_CLASSIFIER_IOC_MAGIC, 0x04, struct adas_classifier_run_op_uapi)

#endif
