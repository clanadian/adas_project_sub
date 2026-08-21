#ifndef ADAS_PS_SAFETY_BRIDGE_H
#define ADAS_PS_SAFETY_BRIDGE_H

#include <stdint.h>

#include "roi_protocol.h"

/*
 * ps_classifier_server.c(순수 C)에서 안전 판단 + UART 송신을 쓰기 위한 C
 * 경계다. 판단 로직은 common/SafetyJudge.hpp·SafetyHazardLatch.hpp를,
 * 송신은 같은 Arty PS 디렉터리의 SafetyTransmitter·UartPort를 쓴다.
 *
 * bbox 정규화, 분류 실패 처리, background 제외도 이 계층이 담당한다.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_safety_handle ps_safety_handle_t;

/*
 * uart_port가 NULL이거나 빈 문자열이면 제어 계층을 켜지 않고 NULL을
 * 돌려준다 - 이 경우 호출자는 계속 분류만 하는 기존 동작으로 진행한다.
 * 포트를 열지 못하면(장치 없음 등) 마찬가지로 NULL을 돌려준다 - 조용히
 * 켜지지 않은 채로 로봇이 제어 없이 움직이는 상태를 만들지 않기 위해서다.
 * 이 경우 호출자는 실행을 멈춰야 한다(Jetson 쪽과 같은 정책).
 */
ps_safety_handle_t* ps_safety_start(const char* uart_port, unsigned baud);

/*
 * 분류 결과 하나를 "지금 모으는 중인 프레임"의 관측 버퍼에 追加한다.
 * 판단은 여기서 하지 않는다 - ps_safety_flush_frame()이 프레임 전체를
 * 모아 한 번에 판단해야, 한 프레임에 ROI가 여러 개일 때 "가장 위험한
 * 것 하나"를 제대로 고를 수 있다.
 *
 * bbox는 crop이 아니라 원본 프레임 좌표(Jetson이 요청에 실어 보낸 것,
 * adas_roi_bbox_t 그대로)여야 한다. 버퍼가 꽉 찼으면(공통 상한
 * safety::kMaxDetections) 조용히 버린다 - 이미 32개나 모였으면 그 이상은
 * "가장 위험한 것" 판단에 영향을 주기 어렵다.
 */
void ps_safety_add_observation(
    ps_safety_handle_t* handle,
    const adas_roi_bbox_t* bbox,
    const adas_roi_result_t* result
);

/*
 * 지금까지 모은 프레임의 관측을 한 번에 판단하고, 최신 안전 상태를 송신
 * 스레드에 넘긴다(실제 UART 전송은 20ms 주기 송신 스레드가 한다 - 이
 * 호출은 값만 갱신하고 바로 리턴한다). 호출 뒤 버퍼는 비워진다.
 *
 * 호출 시점: 새 frame_id의 첫 요청이 왔을 때(그 앞 frame_id는 끝난
 * 것이므로) 그리고 연결이 끊기기 직전(마지막 프레임을 흘리지 않도록)이다.
 *
 * ROI가 0개인 프레임(Jetson이 아예 요청을 안 보낸 경우)은 여기서 잡히지
 * 않는다 - PS는 그런 프레임이 있었는지 자체를 모른다. release_ms(시간
 * 조건)가 같이 걸려 있어 안전 쪽으로 위험하진 않지만, "N프레임 연속
 * 없음" 카운트의 정확도는 그만큼 떨어진다.
 */
void ps_safety_flush_frame(ps_safety_handle_t* handle);

/*
 * 연속 분류 실패처럼 판단 자체가 불가능할 때 쓴다. 래치를 거치지 않고
 * 곧바로 Stop을 낸다 - "링크가 끊겼으니 기다렸다 출발"은 fail-safe로
 * 말이 안 된다.
 */
void ps_safety_force_stop(ps_safety_handle_t* handle);

/* 송신 스레드를 멈추고 join한 뒤 UART를 닫는다. handle은 이후 못 쓴다. */
void ps_safety_stop(ps_safety_handle_t* handle);

#ifdef __cplusplus
}
#endif

#endif  /* ADAS_PS_SAFETY_BRIDGE_H */
