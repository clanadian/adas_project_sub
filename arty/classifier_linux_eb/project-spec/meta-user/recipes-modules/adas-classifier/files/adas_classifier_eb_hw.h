#ifndef ADAS_CLASSIFIER_EB_HW_H
#define ADAS_CLASSIFIER_EB_HW_H

/*
 * EB PL 하드웨어 계약 — 커널 드라이버와 사용자 공간이 함께 쓴다.
 *
 * 정본: arty/pl_eb/SW/arty_cls_address_map.h (Vivado 빌드 로그에서
 * 생성) 와 같은 폴더의 엔진별 드라이버 헤더 3종.
 *
 * 표준 라이브러리를 쓰지 않는다. 커널 모듈이 그대로 include 한다.
 *
 * ⚠️ DB 판과 완전히 다른 하드웨어다. DB 는 IP 하나(`classifier_top`)가
 *    한 번의 ap_start 로 전체를 돌리고 bias·requant 를 AXI-Lite 레지스터로
 *    받는다. EB 는 엔진 3개를 PS 가 6번 나눠 기동하고, bias 는 레지스터가
 *    아니라 **DDR 주소**로 넘긴다. 주소맵을 옮겨 쓰면 존재하지 않는
 *    레지스터를 건드린다.
 */

/* --- s_axilite 제어 창 3개 (PS -> 엔진, M_AXI_GP0) ---------------------- */
#define ADAS_EB_CONV_BASE_ADDRESS      0x40000000u  /* conv_engine (conv1/2) */
#define ADAS_EB_CONV0_BASE_ADDRESS     0x40010000u  /* conv0_engine          */
#define ADAS_EB_MAXPOOL_BASE_ADDRESS   0x40020000u  /* maxpool_engine        */
#define ADAS_EB_REGISTER_SPAN          0x00010000u  /* 창 하나당 64 KiB      */

/*
 * ap_ctrl 비트는 세 엔진이 같다. 오프셋 0x00 도 같다.
 * 나머지는 **엔진마다 다르다** — 아래 세 블록을 섞어 쓰지 말 것.
 */
#define ADAS_EB_REG_CTRL               0x00u
#define ADAS_EB_AP_START_MASK          (1u << 0)
#define ADAS_EB_AP_DONE_MASK           (1u << 1)
#define ADAS_EB_AP_IDLE_MASK           (1u << 2)
#define ADAS_EB_AP_READY_MASK          (1u << 3)

/* --- conv_engine (conv1, conv2) ----------------------------------------- */
#define ADAS_EB_CONV_IFMAP_LO          0x10u
#define ADAS_EB_CONV_IFMAP_HI          0x14u
#define ADAS_EB_CONV_WEIGHTS_LO        0x1cu
#define ADAS_EB_CONV_WEIGHTS_HI        0x20u
/*
 * weights_hi 는 "가중치의 상위 절반"이 아니라 **같은 버퍼를 읽는 두 번째
 * 물리 AXI 포트**다. weights 와 같은 주소를 넣는다. 다른 주소를 넣으면
 * 에러 없이 틀린 결과가 나온다.
 */
#define ADAS_EB_CONV_WEIGHTS2_LO       0x28u
#define ADAS_EB_CONV_WEIGHTS2_HI       0x2cu
#define ADAS_EB_CONV_BIAS_LO           0x34u
#define ADAS_EB_CONV_BIAS_HI           0x38u
#define ADAS_EB_CONV_OFMAP_LO          0x40u
#define ADAS_EB_CONV_OFMAP_HI          0x44u
#define ADAS_EB_CONV_IMG_H             0x4cu
#define ADAS_EB_CONV_IMG_W             0x54u
#define ADAS_EB_CONV_IN_CH             0x5cu
#define ADAS_EB_CONV_OUT_CH            0x64u
#define ADAS_EB_CONV_K                 0x6cu
/*
 * STRIDE 는 쓰기 가능하지만 **1 만 동작한다.** RTL 에 분기 하드웨어가 없어
 * 2 를 쓰면 에러 없이 틀린 결과가 나온다. 다운샘플은 maxpool 이 한다.
 */
#define ADAS_EB_CONV_STRIDE            0x74u
#define ADAS_EB_CONV_PAD               0x7cu
#define ADAS_EB_CONV_REQUANT_MUL       0x84u
#define ADAS_EB_CONV_REQUANT_SHIFT     0x8cu
#define ADAS_EB_CONV_LEAKY_ENABLE      0x94u

/* --- conv0_engine (conv0 전용) ------------------------------------------ */
/*
 * 입력이 **미리 패딩된** 98x98x3 이고 pad 포트가 없다. in_ch/out_ch/k 도
 * 하드와이어(3/16/3)라 레지스터가 없다 — 프로그램할 것은 주소·형상·requant 뿐.
 */
#define ADAS_EB_CONV0_IFMAP_LO         0x10u
#define ADAS_EB_CONV0_IFMAP_HI         0x14u
#define ADAS_EB_CONV0_WEIGHTS_LO       0x1cu
#define ADAS_EB_CONV0_WEIGHTS_HI       0x20u
#define ADAS_EB_CONV0_BIAS_LO          0x28u
#define ADAS_EB_CONV0_BIAS_HI          0x2cu
#define ADAS_EB_CONV0_OFMAP_LO         0x34u
#define ADAS_EB_CONV0_OFMAP_HI         0x38u
#define ADAS_EB_CONV0_IMG_H            0x40u
#define ADAS_EB_CONV0_IMG_W            0x48u
#define ADAS_EB_CONV0_REQUANT_MUL      0x50u
#define ADAS_EB_CONV0_REQUANT_SHIFT    0x58u
#define ADAS_EB_CONV0_LEAKY_ENABLE     0x60u

/* --- maxpool_engine ------------------------------------------------------ */
#define ADAS_EB_MAXPOOL_IFMAP_LO       0x10u
#define ADAS_EB_MAXPOOL_IFMAP_HI       0x14u
#define ADAS_EB_MAXPOOL_OFMAP_LO       0x1cu
#define ADAS_EB_MAXPOOL_OFMAP_HI       0x20u
#define ADAS_EB_MAXPOOL_IMG_H          0x28u
#define ADAS_EB_MAXPOOL_IMG_W          0x30u
#define ADAS_EB_MAXPOOL_CH             0x38u
#define ADAS_EB_MAXPOOL_STRIDE         0x40u
#define ADAS_EB_MAXPOOL_PAD_RIGHT      0x48u
#define ADAS_EB_MAXPOOL_PAD_BOTTOM     0x50u

/* --- 형상 (arty/pl_eb/HW/classifier_net.h) -------------------------------- */
#define ADAS_EB_ROI_SIZE               96u
#define ADAS_EB_ROI_IN_CH              3u
#define ADAS_EB_CONV0_PAD_PIXELS       1u
#define ADAS_EB_WIRE_SIZE              (ADAS_EB_ROI_SIZE + 2u * ADAS_EB_CONV0_PAD_PIXELS)
#define ADAS_EB_CONV0_OUT_CH           16u
#define ADAS_EB_GAP_IN_SIZE            12u
#define ADAS_EB_GAP_IN_CH              64u

/* --- 6-op 실행 순서 ------------------------------------------------------ */
/*
 * 96 -> 96 -> 48 -> 48 -> 24 -> 24 -> 12
 *   3 -> 16 -> 16 -> 32 -> 32 -> 64 -> 64
 *
 * conv0 의 img_h/img_w 는 **이미 패딩된** 98x98 이고 pad 는 0 이다.
 * 나머지 conv 는 자기 입력 크기를 받고 엔진이 pad 를 처리한다.
 */
#define ADAS_EB_OP_CONV0    0u
#define ADAS_EB_OP_CONV     1u
#define ADAS_EB_OP_MAXPOOL  2u
#define ADAS_EB_NUM_OPS     6u
#define ADAS_EB_NUM_CONVS   3u

struct adas_eb_op {
    unsigned kind;
    unsigned img_h;
    unsigned img_w;
    unsigned in_ch;
    unsigned out_ch;
    unsigned k;
    unsigned stride;
    unsigned pad;
    /* conv 계열이면 requant/가중치 인덱스(0..2), pool 이면 무시한다. */
    unsigned conv_index;
    const char* name;
};

/*
 * 배열이 아니라 initializer 매크로로 두는 이유: 헤더에 `static const` 배열을
 * 두면 이 헤더를 include 하고 쓰지 않는 번역 단위마다 unused 경고가 난다.
 * 쓰는 쪽이 함수 안에서 자기 배열을 만든다.
 */
#define ADAS_EB_OP_TABLE_INITIALIZER {                                        \
    { ADAS_EB_OP_CONV0,   98u, 98u,  3u, 16u, 3u, 1u, 0u, 0u, "conv0" },      \
    { ADAS_EB_OP_MAXPOOL, 96u, 96u, 16u, 16u, 0u, 2u, 0u, 0u, "pool0" },      \
    { ADAS_EB_OP_CONV,    48u, 48u, 16u, 32u, 3u, 1u, 1u, 1u, "conv1" },      \
    { ADAS_EB_OP_MAXPOOL, 48u, 48u, 32u, 32u, 0u, 2u, 0u, 0u, "pool1" },      \
    { ADAS_EB_OP_CONV,    24u, 24u, 32u, 64u, 3u, 1u, 1u, 2u, "conv2" },      \
    { ADAS_EB_OP_MAXPOOL, 24u, 24u, 64u, 64u, 0u, 2u, 0u, 0u, "pool2" }       \
}

#endif  /* ADAS_CLASSIFIER_EB_HW_H */
