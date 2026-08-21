#include "control/SafetyTransmitter.hpp"

#include <chrono>
#include <thread>

#include "common/UartFrame.hpp"

namespace adas::control {

std::uint64_t SteadySafetyClock::nowMs() const {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void SteadySafetyClock::sleepMs(std::uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

SafetyTransmitter::SafetyTransmitter(
    UartPort& port,
    SafetyClock& clock,
    const Config& config
)
    : port_(port), clock_(clock), config_(config) {}

bool SafetyTransmitter::sendLocked(safety::State state) {
    std::uint8_t frame[uart_frame::kFrameSize] = {};
    if (!uart_frame::encode(state, frame, sizeof(frame))) {
        ++stats_.send_failures;
        return false;
    }
    if (!port_.write(frame, sizeof(frame))) {
        ++stats_.send_failures;
        return false;
    }
    ++stats_.frames_sent;
    last_sent_ = state;
    return true;
}

void SafetyTransmitter::publish(safety::State state, std::uint64_t decided_at_ms) {
    std::lock_guard<std::mutex> guard(mutex_);

    const bool entering_stop =
        state == safety::State::Stop && published_ != safety::State::Stop;

    published_ = state;
    decided_at_ms_ = decided_at_ms;
    decision_seen_ = true;

    if (entering_stop) {
        /*
         * 즉시 1회 추가 송신. 주기 송신은 그대로 계속되므로 수신 측에서는
         * 프레임 하나가 더 오는 것뿐이고 별도 처리가 필요 없다.
         */
        ++stats_.immediate_stops;
        (void)sendLocked(state);
    }
}

safety::State SafetyTransmitter::tick() {
    std::lock_guard<std::mutex> guard(mutex_);

    safety::State state = published_;

    /*
     * 판단이 멎었는지 본다. 여기서 보지 않으면 분류 스레드가 죽은 뒤에도
     * 마지막 Clear 를 50 Hz 로 계속 보내게 된다 - 로봇이 달리는 채로
     * 시스템이 멈추는 상황이다.
     *
     * 첫 판단이 오기 전(decision_seen_ == false)도 Stop 이다.
     */
    const std::uint64_t now = clock_.nowMs();
    const bool stale =
        !decision_seen_
        || (now >= decided_at_ms_
            && now - decided_at_ms_ >= config_.decision_timeout_ms);
    if (stale) {
        if (state != safety::State::Stop) {
            ++stats_.stale_events;
        }
        state = safety::State::Stop;
    }

    (void)sendLocked(state);
    return state;
}

void SafetyTransmitter::run(const std::atomic_bool& stop) {
    while (!stop.load()) {
        const std::uint64_t started = clock_.nowMs();
        (void)tick();

        /* 이번 처리에 쓴 시간을 빼서 간격이 밀리지 않게 한다. */
        const std::uint64_t spent = clock_.nowMs() - started;
        if (spent < config_.period_ms) {
            clock_.sleepMs(static_cast<std::uint32_t>(config_.period_ms - spent));
        }
    }
}

safety::State SafetyTransmitter::lastSent() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return last_sent_;
}

SafetyTransmitter::Stats SafetyTransmitter::stats() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return stats_;
}

}  // namespace adas::control
