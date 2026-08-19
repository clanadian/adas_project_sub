#include "control/SafetyDecider.hpp"

namespace adas::control {

SafetyDecider::SafetyDecider() : SafetyDecider(Config{}) {}

SafetyDecider::SafetyDecider(const Config& config)
    : config_(config), latch_(config.latch) {
    /*
     * 판단 계층이 쓰는 class 배치는 adapter 와 반드시 같아야 한다.
     * 둘이 갈라지면 adapter 가 person 으로 채운 Unclassified 를 판단
     * 계층이 다른 규칙으로 읽는다.
     */
    records_.reserve(32u);
}

SafetyDecider::Decision SafetyDecider::decide(
    const std::vector<RoiObservation>& observations,
    std::uint64_t now_ms
) {
    Decision decision;
    records_.clear();

    for (const RoiObservation& observation : observations) {
        safety::DetectionRecord record{};
        const AdaptResult result =
            adapt(observation, config_.adapter, config_.judge.classes, record);

        switch (result) {
        case AdaptResult::Hazard:
            ++decision.hazards;
            records_.push_back(record);
            break;
        case AdaptResult::Unclassified:
            ++decision.unclassified;
            records_.push_back(record);
            break;
        case AdaptResult::Background:
            ++decision.background;
            break;
        case AdaptResult::Rejected:
            ++decision.rejected;
            break;
        }
    }

    /*
     * 후보가 하나도 남지 않아도 update 를 부른다. 래치가 Holding 중이면
     * 시간을 흘려보내야 하고, Released 중이면 absent 카운트를 올려야 한다.
     * 여기서 건너뛰면 정지가 영원히 풀리지 않는다.
     */
    state_ = latch_.update(
        records_.empty() ? nullptr : records_.data(),
        records_.size(),
        config_.judge,
        now_ms
    );
    decision.state = state_;
    return decision;
}

void SafetyDecider::forceStop(std::uint64_t now_ms) {
    (void)now_ms;
    state_ = safety::State::Stop;
}

void SafetyDecider::reset() {
    latch_.reset();
    state_ = safety::State::Stop;
}

}  // namespace adas::control
