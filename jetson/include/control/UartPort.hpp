#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/*
 * 안전 상태를 내보내는 바이트 출구.
 *
 * 인터페이스로 분리하는 이유는 **하드웨어 없이 시간 동작을 시험하기
 * 위해서다.** 송신 주기, watchdog, STOP 즉시 송신은 실물 UART 로는 확인하기
 * 어렵고, 확인하지 못한 fail-safe 는 없는 것과 같다.
 */
namespace adas::control {

class UartPort {
public:
    virtual ~UartPort() = default;
    /* 전부 쓰거나 실패다. 부분 송신은 프레임을 깨뜨린다. */
    [[nodiscard]] virtual bool write(const std::uint8_t* data, std::size_t size) = 0;
};

/*
 * POSIX termios 구현. Jetson Nano 40핀 헤더의 UART 는 /dev/ttyTHS1 이다.
 *
 * ⚠️ Jetson Nano 는 기본적으로 그 포트에 serial console(nvgetty)이 붙어
 *    있다. 끄지 않으면 콘솔 출력이 프레임 사이에 섞인다.
 *      sudo systemctl disable --now nvgetty
 *
 * 초기 브링업에는 USB-TTL(/dev/ttyUSB0)이 편하다. 포트 이름만 설정값이라
 * 나중에 그대로 바꿔 끼운다.
 */
class PosixUartPort final : public UartPort {
public:
    PosixUartPort() = default;
    ~PosixUartPort() override;

    PosixUartPort(const PosixUartPort&) = delete;
    PosixUartPort& operator=(const PosixUartPort&) = delete;

    /* 8N1, flow control 없음. baud 는 115200 을 쓴다. */
    [[nodiscard]] bool open(const std::string& path, unsigned baud_rate = 115200u);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return fd_ >= 0; }

    [[nodiscard]] bool write(const std::uint8_t* data, std::size_t size) override;

private:
    int fd_{-1};
};

}  // namespace adas::control
