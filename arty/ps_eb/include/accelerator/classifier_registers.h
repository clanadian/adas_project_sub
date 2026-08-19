#ifndef ADAS_CLASSIFIER_REGISTERS_H
#define ADAS_CLASSIFIER_REGISTERS_H

/*
 * EB 가속기의 AXI-Lite 주소·오프셋은 커널 드라이버와 공유해야 하므로
 * driver/ 아래의 하드웨어 계약 헤더 하나에만 둔다. 이 헤더는 사용자 공간
 * 코드가 익숙한 이름으로 쓰라고 있는 얇은 포워딩 계층이다.
 *
 * 값을 여기에 다시 적지 말 것 — 정본이 둘이 되면 반드시 갈라진다.
 */
#include "driver/adas_classifier_eb_hw.h"

#define ADAS_CLASSIFIER_REGISTER_SPAN  ADAS_EB_REGISTER_SPAN

#define ADAS_CLASSIFIER_AP_START_MASK  ADAS_EB_AP_START_MASK
#define ADAS_CLASSIFIER_AP_DONE_MASK   ADAS_EB_AP_DONE_MASK
#define ADAS_CLASSIFIER_AP_IDLE_MASK   ADAS_EB_AP_IDLE_MASK
#define ADAS_CLASSIFIER_AP_READY_MASK  ADAS_EB_AP_READY_MASK

#define ADAS_CLASSIFIER_B_CONV0_COUNT  16u
#define ADAS_CLASSIFIER_B_CONV1_COUNT  32u
#define ADAS_CLASSIFIER_B_CONV2_COUNT  64u

#endif  // ADAS_CLASSIFIER_REGISTERS_H
