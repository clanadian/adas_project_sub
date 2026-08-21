#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "common/SafetyJudge.hpp"
#include "control/UartPort.hpp"

/*
 * 안전 상태를 20 ms 주기로 RPi 에 내보낸다.
 *
 * 분류 루프와 **주기를 분리하는 것**이 이 클래스의 존재 이유다. 분류
 * 루프는 프레임당 ROI 개수에 따라 주기가 흔들리고 TCP 왕복에 막힌다.
 * 거기서 직접 송신하면 느린 프레임 한 번에 RPi 가 통신 두절로 판단한다.
 *
 * watchdog 을 여기 두는 것도 같은 이유다 — 분류 스레드가 멈추면 스스로
 * 그 사실을 보고할 수 없다. 밖에서 보는 쪽이 있어야 한다.
 */
namespace adas::control {

/* 시간을 주입한다. 테스트가 가짜 시계로 주기·watchdog 을 통제한다. */
class SafetyClock {
public:
    virtual ~SafetyClock() = default;
    [[nodiscard]] virtual std::uint64_t nowMs() const = 0;
    virtual void sleepMs(std::uint32_t milliseconds) = 0;
};

class SteadySafetyClock final : public SafetyClock {
public:
    [[nodiscard]] std::uint64_t nowMs() const override;
    void sleepMs(std::uint32_t milliseconds) override;
};

class SafetyTransmitter final {
public:
    struct Config {
        /* 프로토콜 주기. RPi 수신 timeout(100 ms)의 1/5 이다. */
        std::uint32_t period_ms{20u};

        /*
         * 판단이 이 시간 이상 갱신되지 않으면 Stop 을 보낸다.
         *
         * RPi 의 100 ms 와는 다른 것을 막는다. 이쪽은 **Arty 판단이 멈춘
         * 것**을 잡는다. 송신 스레드가 링크를 계속 살려 두므로 둘이 경쟁하지
         * 않는다. 최악 프레임 시간(캡처 33 + proposal 13.6 + ROI 10개x8
         * ≈ 127 ms)의 약 4배로 잡는다 - 크게 잡아 손해는 "판단 정지를 늦게
         * 발견"뿐이고, 작게 잡으면 정상 동작 중에 오작동한다.
         */
        std::uint64_t decision_timeout_ms{500u};
    };

    struct Stats {
        std::uint64_t frames_sent{0};
        std::uint64_t send_failures{0};
        std::uint64_t stale_events{0};
        std::uint64_t immediate_stops{0};
    };

    SafetyTransmitter(UartPort& port, SafetyClock& clock, const Config& config);

    /*
     * 분류 스레드가 프레임마다 부른다.
     *
     * Clear/Slow -> Stop 전이일 때는 주기를 기다리지 않고 **즉시 한 번 더**
     * 보낸다(프로토콜 규정). 지연이 곧 제동 거리이기 때문이다. Clear 는
     * 늦게 반영돼도 위험하지 않으므로 주기에 맡긴다.
     */
    void publish(safety::State state, std::uint64_t decided_at_ms);

    /* 한 주기. watchdog 검사 후 현재 상태를 송신한다. 보낸 상태를 돌려준다. */
    safety::State tick();

    /* stop 이 설 때까지 주기를 맞춰 돈다. 스레드 본체로 쓴다. */
    void run(const std::atomic_bool& stop);

    [[nodiscard]] safety::State lastSent() const;
    [[nodiscard]] Stats stats() const;

private:
    bool sendLocked(safety::State state);

    UartPort& port_;
    SafetyClock& clock_;
    Config config_;

    mutable std::mutex mutex_;
    /* 아무것도 모르는 상태에서 움직이게 두지 않는다. */
    safety::State published_{safety::State::Stop};
    safety::State last_sent_{safety::State::Stop};
    std::uint64_t decided_at_ms_{0};
    bool decision_seen_{false};
    Stats stats_;
};

}  // namespace adas::control
