#include "control/SafetyTransmitter.hpp"

#include "common/UartFrame.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using adas::control::SafetyClock;
using adas::control::SafetyTransmitter;
using adas::control::UartPort;

/* 보낸 바이트를 그대로 모아 둔다. 프레임 내용까지 검사하기 위해서다. */
class RecordingPort final : public UartPort {
public:
    bool write(const std::uint8_t* data, std::size_t size) override {
        if (fail_next) {
            ++failures;
            return false;
        }
        bytes.insert(bytes.end(), data, data + size);
        ++writes;
        return true;
    }

    [[nodiscard]] std::size_t frameCount() const {
        return bytes.size() / uart_frame::kFrameSize;
    }

    [[nodiscard]] safety::State frameAt(std::size_t index) const {
        const std::size_t base = index * uart_frame::kFrameSize;
        assert(bytes[base] == uart_frame::kMagic);
        /* CRC 까지 확인해야 "보냈다"가 "제대로 보냈다"가 된다. */
        const std::uint8_t crc = uart_frame::crc8(&bytes[base], 2u);
        assert(crc == bytes[base + 2u]);
        return static_cast<safety::State>(bytes[base + 1u]);
    }

    std::vector<std::uint8_t> bytes;
    std::size_t writes{0};
    std::size_t failures{0};
    bool fail_next{false};
};

/* 시간을 테스트가 통제한다. 실물 UART 로는 확인할 수 없는 것들을 본다. */
class FakeClock final : public SafetyClock {
public:
    [[nodiscard]] std::uint64_t nowMs() const override { return now; }
    void sleepMs(std::uint32_t milliseconds) override {
        now += milliseconds;
        ++sleeps;
        total_sleep_ms += milliseconds;
    }

    std::uint64_t now{0};
    std::size_t sleeps{0};
    std::uint64_t total_sleep_ms{0};
};

SafetyTransmitter::Config makeConfig() {
    SafetyTransmitter::Config config;
    config.period_ms = 20u;
    config.decision_timeout_ms = 500u;
    return config;
}

/*
 * 첫 판단이 오기 전에는 Stop 이다. 이 한 줄이 "부팅 직후 로봇이 움직이지
 * 않는다"를 보장한다.
 */
void testStartsWithStop() {
    RecordingPort port;
    FakeClock clock;
    SafetyTransmitter transmitter(port, clock, makeConfig());

    assert(transmitter.tick() == safety::State::Stop);
    assert(port.frameCount() == 1u);
    assert(port.frameAt(0) == safety::State::Stop);
}

/* 유효 프레임은 셋뿐이다. 바이트가 규격과 맞는지 직접 본다. */
void testFrameBytesMatchProtocol() {
    RecordingPort port;
    FakeClock clock;
    SafetyTransmitter transmitter(port, clock, makeConfig());

    transmitter.publish(safety::State::Clear, clock.nowMs());
    transmitter.tick();
    transmitter.publish(safety::State::Slow, clock.nowMs());
    transmitter.tick();

    const std::vector<std::uint8_t> expected = {
        0xA5u, 0x00u, 0x59u,   /* CLEAR */
        0xA5u, 0x01u, 0x5Eu,   /* SLOW  */
    };
    assert(port.bytes == expected);
}

/*
 * 판단이 멎으면 마지막 Clear 를 계속 보내면 안 된다 - 로봇이 달리는 채로
 * 시스템이 멈추는 상황이다. 이 검사가 watchdog 의 존재 이유다.
 */
void testWatchdogForcesStop() {
    RecordingPort port;
    FakeClock clock;
    SafetyTransmitter transmitter(port, clock, makeConfig());

    transmitter.publish(safety::State::Clear, clock.nowMs());
    assert(transmitter.tick() == safety::State::Clear);

    /* 아직 timeout 전 */
    clock.now = 499u;
    assert(transmitter.tick() == safety::State::Clear);

    /* timeout 도달 */
    clock.now = 500u;
    assert(transmitter.tick() == safety::State::Stop);
    assert(transmitter.stats().stale_events == 1u);

    /* 판단이 돌아오면 다시 따른다. */
    transmitter.publish(safety::State::Clear, clock.now);
    assert(transmitter.tick() == safety::State::Clear);
}

/*
 * STOP 진입은 주기를 기다리지 않는다. 지연이 곧 제동 거리다.
 * Clear 진입은 늦어도 위험하지 않으므로 주기에 맡긴다.
 */
void testStopIsSentImmediately() {
    RecordingPort port;
    FakeClock clock;
    SafetyTransmitter transmitter(port, clock, makeConfig());

    transmitter.publish(safety::State::Clear, clock.nowMs());
    const std::size_t before = port.frameCount();

    transmitter.publish(safety::State::Stop, clock.nowMs());
    assert(port.frameCount() == before + 1u);
    assert(port.frameAt(port.frameCount() - 1u) == safety::State::Stop);
    assert(transmitter.stats().immediate_stops == 1u);

    /* 이미 Stop 인데 또 Stop 이면 추가 송신은 없다 - 주기 송신만 계속된다. */
    transmitter.publish(safety::State::Stop, clock.nowMs());
    assert(port.frameCount() == before + 1u);

    /* Clear 진입은 즉시 보내지 않는다. */
    const std::size_t after_stop = port.frameCount();
    transmitter.publish(safety::State::Clear, clock.nowMs());
    assert(port.frameCount() == after_stop);
}

/* 송신 실패는 세어 두고 다음 주기에 다시 시도한다 - 멈추지 않는다. */
void testSendFailureIsCountedNotFatal() {
    RecordingPort port;
    FakeClock clock;
    SafetyTransmitter transmitter(port, clock, makeConfig());

    port.fail_next = true;
    transmitter.tick();
    assert(transmitter.stats().send_failures == 1u);
    assert(transmitter.stats().frames_sent == 0u);

    port.fail_next = false;
    transmitter.tick();
    assert(transmitter.stats().frames_sent == 1u);
}

/*
 * 주기 유지. 처리에 쓴 시간을 빼고 자야 간격이 밀리지 않는다.
 * FakeClock 은 sleep 이 그대로 시간을 흘리므로 5회면 100 ms 다.
 */
void testRunKeepsPeriod() {
    RecordingPort port;
    FakeClock clock;
    SafetyTransmitter transmitter(port, clock, makeConfig());

    std::atomic_bool stop{false};
    /* tick 이 5번 돌면 멈추도록 포트에서 세는 대신 시계로 멈춘다. */
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t started = clock.nowMs();
        transmitter.tick();
        const std::uint64_t spent = clock.nowMs() - started;
        clock.sleepMs(static_cast<std::uint32_t>(20u - spent));
    }
    assert(port.frameCount() == 5u);
    assert(clock.now == 100u);

    /* run() 도 같은 규칙으로 돈다. stop 이 서면 즉시 나온다. */
    stop.store(true);
    transmitter.run(stop);
    assert(port.frameCount() == 5u);
}

}  // namespace

int main() {
    testStartsWithStop();
    testFrameBytesMatchProtocol();
    testWatchdogForcesStop();
    testStopIsSentImmediately();
    testSendFailureIsCountedNotFatal();
    testRunKeepsPeriod();
    std::cout << "SafetyTransmitter tests passed\n";
    return EXIT_SUCCESS;
}
