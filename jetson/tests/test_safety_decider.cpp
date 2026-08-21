#include "control/SafetyDecider.hpp"

#include "roi_protocol.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using adas::control::RoiObservation;
using adas::control::SafetyDecider;

safety::ClassMap projectClasses() {
    safety::ClassMap classes;
    classes.background       = 0;
    classes.car              = 1;
    classes.person           = 2;
    classes.sign_warning     = 3;
    classes.sign_prohibition = 4;
    classes.sign_mandatory   = 5;
    return classes;
}

SafetyDecider::Config makeConfig() {
    SafetyDecider::Config config;
    config.judge.classes = projectClasses();
    config.latch.hold_ms = 3000u;
    config.latch.release_frames = 3u;
    config.latch.release_ms = 200u;
    return config;
}

/*
 * 정규화 좌표로 관측을 만든다. 640x360 기준이라 픽셀로 되돌려 넣는다 -
 * 판단 계층이 보는 것은 정규화 값이고, 그 변환도 함께 시험된다.
 */
RoiObservation observationAt(float center_x, float bottom_y, float height,
                             std::uint32_t class_id, bool classified = true) {
    RoiObservation observation;
    const float width = height * 0.5F;
    observation.candidate.object_bbox = {
        (center_x - width * 0.5F) * 640.0F,
        (bottom_y - height) * 360.0F,
        width * 640.0F,
        height * 360.0F
    };
    observation.candidate.objectness = 0.9F;
    observation.result.status = classified
        ? ADAS_ROI_STATUS_OK : ADAS_ROI_STATUS_ACCELERATOR_ERROR;
    observation.result.class_id = class_id;
    observation.result.confidence_ppm = 900000u;
    observation.classified = classified;
    return observation;
}

void testStartsStoppedAndClearsWhenEmpty() {
    SafetyDecider decider(makeConfig());
    /* 아무 판단도 하기 전에는 Stop 이다. */
    assert(decider.state() == safety::State::Stop);

    const auto decision = decider.decide({}, 0u);
    assert(decision.state == safety::State::Clear);
}

void testDistanceThresholds() {
    SafetyDecider decider(makeConfig());
    const auto& judge = decider.config().judge;

    /* 경로 안, 가까움 -> Stop */
    auto decision = decider.decide(
        {observationAt(0.5F, 0.9F, judge.stop_height + 0.05F, 1u)}, 0u);
    assert(decision.state == safety::State::Stop);
    assert(decision.hazards == 1u);

    /* 래치를 피하려고 새 인스턴스로 각 경계를 본다. */
    SafetyDecider slow_case(makeConfig());
    decision = slow_case.decide(
        {observationAt(0.5F, 0.9F, judge.slow_height + 0.05F, 1u)}, 0u);
    assert(decision.state == safety::State::Slow);

    SafetyDecider far_case(makeConfig());
    decision = far_case.decide(
        {observationAt(0.5F, 0.9F, judge.slow_height - 0.05F, 1u)}, 0u);
    assert(decision.state == safety::State::Clear);

    /* 경로 밖은 크기와 무관하게 Clear 다. */
    SafetyDecider side_case(makeConfig());
    decision = side_case.decide(
        {observationAt(0.05F, 0.9F, judge.stop_height + 0.2F, 1u)}, 0u);
    assert(decision.state == safety::State::Clear);
}

/*
 * A sign yields Slow once it is big enough, and Clear while it is still far.
 *
 * Before the size gate existed, merely looking at a distant sign stopped the
 * robot. That is the regression this test pins down.
 */
void testSignRequiresProximity() {
    const auto config = makeConfig();
    const float gate = config.judge.sign_slow_height;

    /* Far - small in frame. No reaction at all. */
    SafetyDecider far_case(config);
    auto decision = far_case.decide(
        {observationAt(0.5F, 0.3F, gate - 0.05F, 4u)}, 0u);
    assert(decision.state == safety::State::Clear);
    assert(decision.hazards == 1u);

    /* Close - past the gate it slows down. */
    SafetyDecider near_case(config);
    decision = near_case.decide(
        {observationAt(0.5F, 0.3F, gate + 0.05F, 4u)}, 0u);
    assert(decision.state == safety::State::Slow);

    /*
     * Unlike car/person, zone_y_min (ground plane) is still not consulted: a
     * sign is mounted rather than resting on the floor, so it sits high in the
     * frame. bottom_y 0.3 is above zone_y_min (0.55) and it still reacted.
     */

    /* Outside the path column nothing happens, however large it looks. */
    SafetyDecider side_case(config);
    decision = side_case.decide(
        {observationAt(0.05F, 0.3F, gate + 0.2F, 4u)}, 0u);
    assert(decision.state == safety::State::Clear);
}

/*
 * Signs never reach Stop, no matter how large they get.
 *
 * The classifier cannot separate a stop sign from any other sign, so halting
 * on one was wrong more often than right. Slow is the ceiling. A regression
 * here would bring back full braking on every sign the robot drives past.
 */
void testSignNeverStops() {
    const auto config = makeConfig();

    for (std::uint32_t class_id = 3u; class_id <= 5u; ++class_id) {
        for (float height = 0.5F; height <= 1.0F; height += 0.1F) {
            SafetyDecider decider(config);
            const auto decision =
                decider.decide({observationAt(0.5F, 0.3F, height, class_id)}, 0u);
            assert(decision.state != safety::State::Stop);
        }
    }

    /*
     * A sign holds Slow for as long as it stays in view - it never opens a
     * latch event, so there is no hold-then-release cycle to wait out.
     */
    SafetyDecider decider(config);
    const std::vector<RoiObservation> sign = {
        observationAt(0.5F, 0.3F, config.judge.sign_slow_height + 0.1F, 3u)
    };
    assert(decider.decide(sign, 0u).state == safety::State::Slow);
    assert(decider.decide(sign, 5000u).state == safety::State::Slow);
    /* Gone from view means Clear immediately, with no hold to expire. */
    assert(decider.decide({}, 5001u).state == safety::State::Clear);
}

/*
 * 이 시스템에서 새로 생긴 요구. 분류 실패가 Clear 가 되면 안 되고,
 * 동시에 멀리 있는 오탐 하나로 멈춰서도 안 된다.
 */
void testUnclassifiedUsesGeometry() {
    SafetyDecider near_case(makeConfig());
    auto decision = near_case.decide(
        {observationAt(0.5F, 0.9F, 0.5F, 9u, /*classified=*/false)}, 0u);
    assert(decision.state == safety::State::Stop);
    assert(decision.unclassified == 1u);
    assert(decision.hazards == 0u);

    /* 멀거나 경로 밖이면 정체를 몰라도 Clear 다. */
    SafetyDecider far_case(makeConfig());
    decision = far_case.decide(
        {observationAt(0.5F, 0.9F, 0.05F, 9u, false)}, 0u);
    assert(decision.state == safety::State::Clear);

    SafetyDecider side_case(makeConfig());
    decision = side_case.decide(
        {observationAt(0.05F, 0.9F, 0.5F, 9u, false)}, 0u);
    assert(decision.state == safety::State::Clear);
}

void testBackgroundDoesNotStop() {
    SafetyDecider decider(makeConfig());
    const auto decision = decider.decide(
        {observationAt(0.5F, 0.9F, 0.6F, 0u)}, 0u);
    assert(decision.state == safety::State::Clear);
    assert(decision.background == 1u);
}

/*
 * 정지 이벤트: 멈춤 -> hold_ms 유지 -> 해제. 해제에는 프레임 수와 경과
 * 시간이 **둘 다** 필요하다.
 */
void testHoldAndRelease() {
    SafetyDecider decider(makeConfig());
    const std::vector<RoiObservation> stopper = {
        observationAt(0.5F, 0.9F, 0.6F, 1u)
    };

    assert(decider.decide(stopper, 1000u).state == safety::State::Stop);

    /* 대상이 사라져도 hold_ms 동안은 Stop 이다. */
    assert(decider.decide({}, 2000u).state == safety::State::Stop);
    assert(decider.decide({}, 3999u).state == safety::State::Stop);

    /*
     * hold 가 끝나면 내려갈 수 있다. 다만 래치는 아직 Released 이고,
     * 프레임 3개 + 200 ms 를 둘 다 채워야 Idle 로 돌아간다.
     */
    assert(decider.decide({}, 4001u).state == safety::State::Clear);
    assert(decider.decide({}, 4002u).state == safety::State::Clear);

    /*
     * 같은 class 가 다시 나타나도 Released 중에는 재트리거하지 않는다.
     * 표지판을 지나치는 동안 계속 멈추지 않게 하는 장치다.
     */
    assert(decider.decide(stopper, 4003u).state == safety::State::Clear);

    /* 다른 class 가 새로 위험 수준이면 즉시 새 이벤트다. */
    const auto config = makeConfig();
    SafetyDecider other(config);
    assert(other.decide(stopper, 1000u).state == safety::State::Stop);
    assert(other.decide({}, 4001u).state == safety::State::Clear);
    /*
     * person (2), not a sign: signs cap at Slow and so can never open a latch
     * event. The interrupting class has to be one that can actually reach Stop.
     */
    const float gate = config.judge.stop_height;
    assert(other.decide({observationAt(0.5F, 0.9F, gate + 0.05F, 2u)}, 4002u)
               .state == safety::State::Stop);
}

/*
 * release_frames 만 쓰면 판단 주기가 느릴 때 해제가 한없이 늘어지고,
 * release_ms 만 쓰면 판단이 멈춘 사이에 저절로 풀린다. 둘 다 필요하다.
 */
void testReleaseNeedsBothFramesAndTime() {
    SafetyDecider decider(makeConfig());   /* frames=3, ms=200 */
    const std::vector<RoiObservation> stopper = {
        observationAt(0.5F, 0.9F, 0.6F, 1u)
    };

    assert(decider.decide(stopper, 0u).state == safety::State::Stop);
    /* hold 종료 -> Released 진입. absent 계수는 여기서 시작한다. */
    assert(decider.decide({}, 3001u).state == safety::State::Clear);

    /* 프레임은 3개를 채웠지만 200 ms 가 안 됐다 -> 아직 래치 중이다. */
    decider.decide({}, 3002u);   /* absent 1 */
    decider.decide({}, 3003u);   /* absent 2 */
    decider.decide({}, 3004u);   /* absent 3, 경과 3 ms */
    /* 같은 class 가 다시 나타나도 새 이벤트가 되지 않는다 = 아직 래치 중. */
    assert(decider.decide(stopper, 3005u).state == safety::State::Clear);

    /*
     * 위 등장이 absent 계수를 되돌렸다. 이번에는 프레임 3개를 200 ms 넘게
     * 걸쳐 채운다 - 그러면 래치가 풀리고 다음 등장이 새 이벤트가 된다.
     */
    decider.decide({}, 3200u);   /* absent 1 */
    decider.decide({}, 3300u);   /* absent 2 */
    decider.decide({}, 3400u);   /* absent 3, 경과 395 ms -> 해제 */
    assert(decider.decide(stopper, 3401u).state == safety::State::Stop);
}

/* 카메라·링크 장애는 래치를 거치지 않는다. */
void testForceStopBypassesLatch() {
    SafetyDecider decider(makeConfig());
    assert(decider.decide({}, 0u).state == safety::State::Clear);
    decider.forceStop(1u);
    assert(decider.state() == safety::State::Stop);
    /* 다음 정상 프레임은 곧바로 판단 결과를 따른다 - 3초 기다리지 않는다. */
    assert(decider.decide({}, 2u).state == safety::State::Clear);
}

}  // namespace

int main() {
    testStartsStoppedAndClearsWhenEmpty();
    testDistanceThresholds();
    testSignRequiresProximity();
    testSignNeverStops();
    testUnclassifiedUsesGeometry();
    testBackgroundDoesNotStop();
    testHoldAndRelease();
    testReleaseNeedsBothFramesAndTime();
    testForceStopBypassesLatch();
    std::cout << "SafetyDecider tests passed\n";
    return EXIT_SUCCESS;
}
