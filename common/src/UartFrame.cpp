#include "common/UartFrame.hpp"

namespace uart_frame {

uint8_t crc8(const uint8_t* data, size_t length) {
    if (data == nullptr) {
        return 0x00;
    }

    //표를 쓰지 않고 비트 단위로 돈다. 3 byte짜리라 속도가 문제되지 않고,
    //256 byte 표를 R5 메모리에 두는 것보다 낫다.
    uint8_t crc = 0x00;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x80u) != 0u) {
                crc = static_cast<uint8_t>((crc << 1) ^ 0x07u);
            } else {
                crc = static_cast<uint8_t>(crc << 1);
            }
        }
    }
    return crc;
}

bool encode(safety::State state, uint8_t* out, size_t out_size) {
    if (out == nullptr || out_size < kFrameSize) {
        return false;
    }

    out[0] = kMagic;
    out[1] = static_cast<uint8_t>(state);
    out[2] = crc8(out, 2);
    return true;
}

}  // namespace uart_frame
