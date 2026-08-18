#include "common/SafetyDebounce.hpp"

namespace safety {
namespace {

//State는 숫자가 클수록 위험하다는 전제로 비교한다
inline bool moreDangerous(State a, State b) {
    return static_cast<uint8_t>(a) > static_cast<uint8_t>(b);
}

}  // namespace

Debounce::Debounce() : config_(Config{}) {}

Debounce::Debounce(const Config& config) : config_(config) {}

void Debounce::reset() {
    state_          = State::Stop;
    pending_        = State::Stop;
    pending_frames_ = 0;
    pending_since_  = 0;
}

State Debounce::update(State raw, uint64_t now_ms) {
    //1. 더 위험해졌으면 즉시 반영한다.
    //   여기서 기다리면 못 멈춘다. 안전 쪽으로 치우치는 것이 맞다.
    if (moreDangerous(raw, state_)) {
        state_          = raw;
        pending_        = raw;
        pending_frames_ = 0;
        pending_since_  = now_ms;
        return state_;
    }

    //2. 같은 상태면 완화 후보를 초기화한다.
    //   위험이 계속 관측되는 동안에는 내려갈 이유가 없다.
    if (raw == state_) {
        pending_        = state_;
        pending_frames_ = 0;
        pending_since_  = now_ms;
        return state_;
    }

    //3. 여기부터는 raw가 현재보다 안전한 경우다.

    //후보가 바뀌었으면 처음부터 센다.
    //예: Stop 상태에서 Clear가 몇 번 오다가 Slow가 오면 Slow로 다시 시작한다.
    if (raw != pending_) {
        pending_        = raw;
        pending_frames_ = 1;
        pending_since_  = now_ms;
        return state_;
    }

    ++pending_frames_;

    //4. 프레임 수와 시간을 **둘 다** 채워야 내려간다.
    //   프레임만 보면 카메라가 빠를 때 순식간에 풀리고,
    //   시간만 보면 프레임이 끊긴 구간에서도 풀린다.
    const bool enough_frames = pending_frames_ >= config_.release_frames;
    const uint64_t elapsed   = (now_ms >= pending_since_) ? (now_ms - pending_since_) : 0;
    const bool enough_time   = elapsed >= config_.release_hold_ms;

    if (enough_frames && enough_time) {
        state_          = pending_;
        pending_frames_ = 0;
        pending_since_  = now_ms;
    }
    return state_;
}

}  // namespace safety
