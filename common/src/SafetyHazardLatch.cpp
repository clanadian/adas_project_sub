#include "common/SafetyHazardLatch.hpp"

namespace safety {

HazardLatch::HazardLatch() : config_(Config{}) {}

HazardLatch::HazardLatch(const Config& config) : config_(config) {}

void HazardLatch::reset() {
    phase_          = Phase::Idle;
    latched_class_  = -1;
    hold_until_ms_  = 0;
    absent_frames_  = 0;
    absent_since_ms_ = 0;
    last_state_     = State::Clear;
}

State HazardLatch::update(const DetectionRecord* items, size_t count,
                          const JudgeConfig& config, uint64_t now_ms) {
    //1. Holding 중이면 시간이 될 때까지 무조건 Stop이다. 그 동안은
    //   detection이 뭐라 하든(더 위험한 게 와도, 안 보여도) 보지 않는다 —
    //   T초는 T초다.
    if (phase_ == Phase::Holding) {
        if (now_ms < hold_until_ms_) {
            last_state_ = State::Stop;
            return last_state_;
        }
        phase_          = Phase::Released;
        absent_frames_  = 0;
        absent_since_ms_ = now_ms;
    }

    //2. Released 중이면 latched class가 이번 프레임에도 보이는지로
    //   absent_frames_를 갱신한다. N프레임 연속으로 안 보이면 Idle로
    //   돌아가 완전히 새 이벤트를 받을 수 있게 한다.
    if (phase_ == Phase::Released) {
        if (classPresent(items, count, latched_class_, config)) {
            absent_frames_  = 0;
            absent_since_ms_ = now_ms;
        } else {
            ++absent_frames_;
            const bool frames_ok = absent_frames_ >= config_.release_frames;
            const bool time_ok =
                config_.release_ms == 0 ||
                (now_ms >= absent_since_ms_ &&
                 now_ms - absent_since_ms_ >= config_.release_ms);
            //프레임 수와 경과 시간을 **둘 다** 만족해야 푼다.
            if (frames_ok && time_ok) {
                phase_         = Phase::Idle;
                latched_class_ = -1;
            }
        }
    }

    //3. latched class(Released 중이면)를 제외하고 가장 위험한 것을 본다.
    //   Idle이면 exclude가 -1이라 아무것도 제외되지 않는다.
    const int32_t exclude = (phase_ == Phase::Released) ? latched_class_ : -1;
    int32_t worst_class = -1;
    const State worst = judgeWorst(items, count, config, exclude, &worst_class);

    //4. Stop급이 새로 나오면(Idle에서 처음이든, Released 중 다른 class가
    //   끼어들든) 새 이벤트를 시작한다.
    if (worst == State::Stop) {
        phase_         = Phase::Holding;
        latched_class_ = worst_class;
        hold_until_ms_ = now_ms + config_.hold_ms;
        last_state_    = State::Stop;
        return last_state_;
    }

    last_state_ = worst;
    return last_state_;
}

}  // namespace safety
