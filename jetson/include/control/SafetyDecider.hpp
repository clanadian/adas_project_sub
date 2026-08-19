#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/SafetyHazardLatch.hpp"
#include "common/SafetyJudge.hpp"
#include "control/DetectionAdapter.hpp"

/*
 * 한 프레임의 관측을 안전 상태 하나로 줄인다.
 *
 *   관측 -> DetectionRecord 변환 -> 위험 판단 -> 정지 이벤트 래치 -> State
 *
 * 하드웨어를 만지지 않는 순수 계산이라 PC 에서 그대로 시험한다. 시간은
 * 인자로 받는다(내부에서 시계를 읽지 않는다) - 래치의 hold/release 를
 * 테스트가 통제할 수 있어야 한다.
 */
namespace adas::control {

class SafetyDecider final {
public:
    struct Config {
        AdapterConfig adapter;
        safety::JudgeConfig judge;
        safety::HazardLatch::Config latch;
    };

    /* 한 프레임의 결과. 개수는 로그·오버레이·튜닝에 쓴다. */
    struct Decision {
        safety::State state{safety::State::Stop};
        std::size_t hazards{0};
        std::size_t unclassified{0};
        std::size_t background{0};
        std::size_t rejected{0};
    };

    SafetyDecider();
    explicit SafetyDecider(const Config& config);

    /*
     * 분류에 실패한 관측도 **버리지 말고 그대로 넘긴다.** classified=false
     * 인 것은 Unclassified 로 처리되며, 그것이 이 시스템에서 새로 생긴
     * 안전 요구다 (분류기가 네트워크 건너편에 있다).
     */
    Decision decide(
        const std::vector<RoiObservation>& observations,
        std::uint64_t now_ms
    );

    /*
     * 카메라 정지·링크 단절처럼 판단 자체가 불가능한 경우.
     *
     * 래치를 **거치지 않는다** — "링크가 끊겼으니 3초 기다렸다 출발"은
     * fail-safe 로 말이 안 된다. 래치의 내부 시계도 건드리지 않으므로,
     * 복구되면 경과 시간이 저절로 맞다.
     */
    void forceStop(std::uint64_t now_ms);

    [[nodiscard]] safety::State state() const noexcept { return state_; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }

    /* 상태를 처음(Stop)으로 되돌린다. */
    void reset();

private:
    Config config_;
    safety::HazardLatch latch_;
    safety::State state_{safety::State::Stop};
    std::vector<safety::DetectionRecord> records_;
};

}  // namespace adas::control
