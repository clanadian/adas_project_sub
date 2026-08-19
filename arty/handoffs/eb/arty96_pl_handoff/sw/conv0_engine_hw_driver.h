/*
 * conv0_engine_hw_driver.h — bare-metal register driver for conv0_engine.
 *
 * 2026-08-08 신규. 이게 없어서 `SW/network_run_full.c` 가 레이어 0 을
 * conv_engine 으로 돌리고 있었다 (grep 결과 conv0_engine 참조 0건).
 * 레이어 0 은 프레임의 25% 이고 conv0_engine 이 그 자리에서 2.60배인데
 * 그 이득이 통째로 안 쓰이고 있었다.
 *
 * 오프셋 출처 — 추측이 아니다
 * ---------------------------
 * 전부 Package IP 가 생성한
 *   conv0_engine_prj/solution1/impl/ip/drivers/conv0_engine_v1_0/src/
 *   xconv0_engine_hw.h
 * 에서 그대로 옮겼다. 이 프로젝트는 손으로 짐작한 레지스터 맵 때문에 조용한
 * BD 실패를 낸 전례가 있고(route_concat 의 +0x04 어긋남), conv_engine 쪽
 * 드라이버 헤더도 같은 경고를 달고 있다. 값을 바꿀 일이 생기면 반드시 위
 * 생성 파일을 다시 볼 것.
 *
 * 포인터마다 32비트 레지스터 2개(lo/hi) + 예약 1개로 0x0c 씩 전진하고,
 * 스칼라는 8비트든 16비트든 각자 32비트 슬롯 + 예약 1개로 0x08 씩 전진한다.
 * conv_engine 드라이버와 같은 패턴이다.
 *
 * conv_engine 과 다른 계약 (틀리면 크래시가 아니라 조용히 틀린 결과)
 * ------------------------------------------------------------------
 *  1. `weights` 는 **OIHW** (`[oc][ic][ky][kx]`, 골든 .bin 순서) 다.
 *     conv_engine 의 wpack(`[oc][ky][kx][ic]`) 이 **아니다**. 레이어 0 은
 *     16x3x3x3 = 432 개로 두 순서의 크기가 같아서 잘못 넣어도 아무 신호가
 *     없다. `python/export_sw_headers.py` 가 2026-08-08 부터 레이어 0 만
 *     전치에서 제외한다 — 그 조건문을 지우면 이 계약이 깨진다.
 *  2. `ifmap` 은 **미리 패딩된** 입력이다 (290x514, pad=0 으로 호출).
 *     엔진에 stride/pad 레지스터가 없는 이유다.
 *  3. `ofmap` 은 **int8x4 패킹** 출력이다 (pack4.h). DDR 바이트는 int8
 *     버퍼와 동일하므로 downstream(maxpool)은 그대로 읽으면 된다.
 *  4. `bias` 는 **보정 bias** 여야 한다:
 *     `bias'[oc] = bias[oc] + 128 * sum(w[oc])`, 입력 `s = u - 128` 과 짝.
 *     `python/regen_layer0_contract.py` 가 .bin 에 구워 넣으므로 SW 는
 *     BIAS_BLOB 을 그대로 넘기면 된다 (doc/04_team/layer0-bias-regeneration.md).
 *     weight blob 이 바뀌면 이 보정값이 즉시 무효가 된다 — 크기도 형식도
 *     그대로라 아무것도 알려주지 않는다.
 *  5. accum 스크래치가 **없다**. in_ch=3 은 한 패스라 부분합 이월이 없다.
 */

#pragma once
#include <stdint.h>
#include "xparameters.h"
#include "xil_io.h"
#include "xil_printf.h"

#define C0_BASE      XPAR_CONV0_ENGINE_0_S_AXI_CTRL_BASEADDR
#define C0_REG(off)  (C0_BASE + (uint32_t)(off))

/* ---- ap_ctrl_hs block control (fixed offsets, identical on every HLS IP) */
#define C0_REG_CTRL       0x00
#define C0_CTRL_AP_START  (1u << 0)
#define C0_CTRL_AP_DONE   (1u << 1)
#define C0_CTRL_AP_IDLE   (1u << 2)
#define C0_CTRL_AP_READY  (1u << 3)

/* ---- m_axi base addresses (confirmed against xconv0_engine_hw.h) */
#define C0_REG_IFMAP_ADDR_LO    0x10
#define C0_REG_IFMAP_ADDR_HI    0x14
/* 0x18 reserved */
#define C0_REG_WEIGHTS_ADDR_LO  0x1c
#define C0_REG_WEIGHTS_ADDR_HI  0x20
/* 0x24 reserved */
#define C0_REG_BIAS_ADDR_LO     0x28
#define C0_REG_BIAS_ADDR_HI     0x2c
/* 0x30 reserved */
#define C0_REG_OFMAP_ADDR_LO    0x34
#define C0_REG_OFMAP_ADDR_HI    0x38
/* 0x3c reserved */

/* ---- scalars (confirmed against xconv0_engine_hw.h) */
#define C0_REG_IMG_H               0x40
/* 0x44 reserved */
#define C0_REG_IMG_W               0x48
/* 0x4c reserved */
#define C0_REG_REQUANT_MULTIPLIER  0x50
/* 0x54 reserved */
#define C0_REG_REQUANT_SHIFT       0x58
/* 0x5c reserved */
#define C0_REG_LEAKY_RELU_ENABLE   0x60
/* 0x64 reserved */

/* Program layer 0's DDR addresses. Call once before conv0_engine_start().
 * Unlike conv_engine_set_addrs() there is no accum argument (contract 5 above). */
static inline void conv0_engine_set_addrs(uint64_t ifmap, uint64_t weights,
                                          uint64_t bias, uint64_t ofmap)
{
    Xil_Out32(C0_REG(C0_REG_IFMAP_ADDR_LO),   (uint32_t)(ifmap        & 0xffffffffu));
    Xil_Out32(C0_REG(C0_REG_IFMAP_ADDR_HI),   (uint32_t)(ifmap   >> 32));
    Xil_Out32(C0_REG(C0_REG_WEIGHTS_ADDR_LO), (uint32_t)(weights      & 0xffffffffu));
    Xil_Out32(C0_REG(C0_REG_WEIGHTS_ADDR_HI), (uint32_t)(weights >> 32));
    Xil_Out32(C0_REG(C0_REG_BIAS_ADDR_LO),    (uint32_t)(bias         & 0xffffffffu));
    Xil_Out32(C0_REG(C0_REG_BIAS_ADDR_HI),    (uint32_t)(bias    >> 32));
    Xil_Out32(C0_REG(C0_REG_OFMAP_ADDR_LO),   (uint32_t)(ofmap        & 0xffffffffu));
    Xil_Out32(C0_REG(C0_REG_OFMAP_ADDR_HI),   (uint32_t)(ofmap   >> 32));
}

/* img_h/img_w are the **padded** dimensions (layer 0 = 290x514).
 * The output is therefore (img_h-2) x (img_w-2) x 16. */
static inline void conv0_engine_set_shape(uint16_t img_h, uint16_t img_w)
{
    Xil_Out32(C0_REG(C0_REG_IMG_H), (uint32_t)img_h);
    Xil_Out32(C0_REG(C0_REG_IMG_W), (uint32_t)img_w);
}

static inline void conv0_engine_set_quant(int32_t requant_multiplier,
                                          uint8_t requant_shift,
                                          uint8_t leaky_relu_enable)
{
    Xil_Out32(C0_REG(C0_REG_REQUANT_MULTIPLIER), (uint32_t)requant_multiplier);
    Xil_Out32(C0_REG(C0_REG_REQUANT_SHIFT),      (uint32_t)requant_shift);
    Xil_Out32(C0_REG(C0_REG_LEAKY_RELU_ENABLE),  (uint32_t)leaky_relu_enable);
}

static inline void conv0_engine_start(void)
{
    Xil_Out32(C0_REG(C0_REG_CTRL), C0_CTRL_AP_START);
}

static inline int conv0_engine_wait_idle(const char *where)
{
    uint32_t tmo = 200000000u;
    while ((Xil_In32(C0_REG(C0_REG_CTRL)) & C0_CTRL_AP_IDLE) == 0u) {
        if (--tmo == 0u) {
            xil_printf("%s: conv0_engine ap_idle timeout\r\n", where);
            return -1;
        }
    }
    return 0;
}

static inline int conv0_engine_wait_done(const char *where)
{
    uint32_t tmo = 200000000u;
    while ((Xil_In32(C0_REG(C0_REG_CTRL)) & C0_CTRL_AP_DONE) == 0u) {
        if (--tmo == 0u) {
            xil_printf("%s: conv0_engine ap_done timeout\r\n", where);
            return -1;
        }
    }
    return 0;
}
