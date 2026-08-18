#pragma once

#include <cstddef>
#include <cstdint>

#include "common/SafetyJudge.hpp"

//RPU → RPi safety state frame 생성.
//
//정본은 platform/handoff/turtlebot/UART_PROTOCOL_v0.md다. 여기서 값을
//바꾸면 이미 전달한 test vector와 어긋나므로 문서를 먼저 고친다.
//
//  offset 0   magic  0xA5
//  offset 1   state  0x00 CLEAR / 0x01 SLOW / 0x02 STOP
//  offset 2   crc8   CRC-8/SMBUS, 대상 범위는 offset 0..1
//
//버퍼를 호출부가 주는 이유는 R5에서 힙을 쓰지 않기 위해서다.
namespace uart_frame {

inline constexpr uint8_t kMagic     = 0xA5;
inline constexpr size_t  kFrameSize = 3;

//CRC-8/SMBUS. poly 0x07, init 0x00, refin/refout false, xorout 0x00.
//구현 검증값은 "123456789" -> 0xF4다. 이 값이 다르면 파라미터가 틀린 것이다.
uint8_t crc8(const uint8_t* data, size_t length);

//out에 3 byte를 쓴다. out이 null이면 false.
//
//정의되지 않은 state는 만들지 않는다. 수신 측이 알 수 없는 값을 받으면
//즉시 STOP으로 가도록 규격에 정해져 있는데, 송신 측이 그런 값을 보낼
//이유가 없다.
bool encode(safety::State state, uint8_t* out, size_t out_size);

}  // namespace uart_frame
