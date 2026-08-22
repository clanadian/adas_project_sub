#include "common/SafetyHazardLatch.hpp"
#include "common/SafetyJudge.hpp"
#include "common/SafetyMessage.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

/*
 * 안전 판단 계층(SafetyJudge + HazardLatch)의 회귀 시험.
 *
 * 이 시험은 원래 젯슨의 `test_safety_decider.cpp` 였으나 판단 계층이 Arty PS
 * 로 옮겨질 때(`ab22bf2`) 구현과 함께 지워졌다. 판단 로직만 남기고 현재
 * API(`judge` / `HazardLatch::update`)에 맞춰 되살린 것이다.
 *
 * `common/` 은 외부 의존이 없어 보드 없이 호스트에서 그대로 돈다.
 */
namespace {

using safety::DetectionRecord;
using safety::State;

safety::JudgeConfig makeConfig() {
    safety::JudgeConfig config;
    /* 배포 구성과 같은 배치다 - background=0 때문에 한 칸씩 밀린다
     * (ps_safety_bridge.cpp 의 projectClassMap()). */
    config.classes.background       = 0;
    config.classes.car              = 1;
    config.classes.person           = 2;
    config.classes.sign_warning     = 3;
    config.classes.sign_prohibition = 4;
    config.classes.sign_mandatory   = 5;
    return config;
}

safety::HazardLatch::Config makeLatchConfig() {
    safety::HazardLatch::Config config;
    config.hold_ms        = 3000u;
    config.release_frames = 3u;
    config.release_ms     = 200u;
    return config;
}

/*
 * 정규화 좌표(0~1)로 관측 하나를 만든다. 판단 계층이 보는 것이 이 값이다.
 * 픽셀 -> 정규화 변환은 ps_safety_bridge 의 몫이라 여기서 다루지 않는다.
 */
DetectionRecord recordAt(float center_x, float bottom_y,
                         float width, float height,
                         std::int32_t class_id, float score = 0.9F) {
    DetectionRecord record{};
    record.x1       = center_x - width * 0.5F;
    record.x2       = center_x + width * 0.5F;
    record.y1       = bottom_y - height;
    record.y2       = bottom_y;
    record.score    = score;
    record.class_id = class_id;
    return record;
}

State judgeOf(const DetectionRecord& record, const safety::JudgeConfig& config) {
    return safety::judge(&record, 1u, config);
}

/* 아무것도 없으면 Clear 다. null 과 count=0 둘 다 받는다. */
void testEmptyIsClear() {
    const auto config = makeConfig();
    assert(safety::judge(nullptr, 0u, config) == State::Clear);

    safety::HazardLatch latch(makeLatchConfig());
    assert(latch.update(nullptr, 0u, config, 0u) == State::Clear);
}

/* car/person 은 아랫변 위치와 높이로 Stop/Slow/Clear 를 가른다. */
void testCarPersonDistanceThresholds() {
    const auto config = makeConfig();

    assert(judgeOf(recordAt(0.5F, 0.9F, 0.3F, config.stop_height + 0.05F,
                            config.classes.car), config) == State::Stop);
    assert(judgeOf(recordAt(0.5F, 0.9F, 0.3F, config.slow_height + 0.05F,
                            config.classes.car), config) == State::Slow);
    assert(judgeOf(recordAt(0.5F, 0.9F, 0.3F, config.slow_height - 0.05F,
                            config.classes.car), config) == State::Clear);

    /* 경로 밖은 크기와 무관하게 Clear 다. */
    assert(judgeOf(recordAt(0.05F, 0.9F, 0.3F, config.stop_height + 0.2F,
                            config.classes.person), config) == State::Clear);

    /* 아랫변이 zone_y_min 위쪽이면 멀리 있는 것으로 본다. */
    assert(judgeOf(recordAt(0.5F, config.zone_y_min - 0.05F, 0.3F,
                            config.stop_height + 0.2F,
                            config.classes.person), config) == State::Clear);
}

/*
 * 표지판 게이트는 **높이가 아니라 폭**이다 (2026-08-21 변경).
 *
 * 두 비율은 서로 축이 달라 바꿔 쓸 수 없다. 640x360 프레임에서 각 축으로
 * 따로 정규화되므로, 정사각형에 가까운 표지판 박스는 height 비율이 width
 * 비율의 640/360 = 1.78 배로 나온다. 아래 두 번째 경우가 그 차이를 고정한다
 * - 폭은 게이트 미달인데 높이는 옛 게이트(0.50)를 한참 넘는 박스다.
 */
void testSignUsesWidthNotHeight() {
    const auto config = makeConfig();

    /* 폭이 게이트를 넘으면 높이가 작아도 Slow 다. */
    assert(judgeOf(recordAt(0.5F, 0.5F, config.sign_slow_width + 0.05F, 0.10F,
                            config.classes.sign_mandatory), config)
           == State::Slow);

    /* 폭이 모자라면 높이가 아무리 커도 Clear 다.
     * 옛 height >= 0.50 게이트에서는 이것이 Slow 였다. */
    assert(judgeOf(recordAt(0.5F, 0.95F, config.sign_slow_width - 0.05F, 0.90F,
                            config.classes.sign_mandatory), config)
           == State::Clear);
}

/*
 * 표지판에는 지면(zone_y_min) 조건이 없다. 벽·기둥에 달려 있어 화면 위쪽에
 * 잡히는 것이 정상이고, 지면 조건을 요구하면 정면의 표지판을 놓친다.
 */
void testSignIgnoresGroundPlane() {
    const auto config = makeConfig();
    const float high_in_frame = config.zone_y_min - 0.2F;

    assert(judgeOf(recordAt(0.5F, high_in_frame, config.sign_slow_width + 0.05F,
                            0.2F, config.classes.sign_warning), config)
           == State::Slow);
}

/*
 * 표지판도 zone_x 는 지킨다. 크기 검사보다 **먼저** 걸러진다.
 *
 * 2026-08-21 데모 캡처의 표지판이 중심 x ~0.85 로 추정됐다 - 경로 밖이라
 * 폭 게이트를 낮춰도 그 구도에서는 Clear 다. 크기 게이트를 재조정할 때
 * 이걸 원인으로 오해하기 쉬워 시험으로 고정해 둔다.
 */
void testSignRespectsZoneX() {
    const auto config = makeConfig();

    assert(judgeOf(recordAt(0.85F, 0.5F, config.sign_slow_width + 0.3F, 0.4F,
                            config.classes.sign_mandatory), config)
           == State::Clear);
}

/*
 * 표지판은 아무리 커도 Stop 을 만들지 않는다.
 *
 * 분류기가 정지표지판을 다른 표지판과 구분하지 못하므로 모든 표지판에
 * 정지를 거는 것은 맞을 때보다 틀릴 때가 많았다. Slow 가 상한이다.
 */
void testSignNeverStops() {
    const auto config = makeConfig();

    for (std::int32_t class_id = config.classes.sign_warning;
         class_id <= config.classes.sign_mandatory; ++class_id) {
        for (float width = 0.2F; width <= 1.0F; width += 0.1F) {
            const auto record = recordAt(0.5F, 0.5F, width, 0.4F, class_id);
            assert(judgeOf(record, config) != State::Stop);
        }
    }

    /*
     * 표지판은 래치를 열지 않으므로 보이는 동안만 Slow 이고, 사라지면
     * hold 를 기다리지 않고 바로 Clear 다.
     */
    safety::HazardLatch latch(makeLatchConfig());
    const DetectionRecord sign = recordAt(
        0.5F, 0.5F, config.sign_slow_width + 0.1F, 0.4F,
        config.classes.sign_prohibition);

    assert(latch.update(&sign, 1u, config, 0u) == State::Slow);
    assert(latch.update(&sign, 1u, config, 5000u) == State::Slow);
    assert(latch.update(nullptr, 0u, config, 5001u) == State::Clear);
}

/*
 * background 와 매핑에 없는 class 는 판단 대상이 아니다.
 *
 * background 를 "미확정 장애물"로 승격하는 규칙은 판단 계층이 아니라
 * ps_safety_bridge 에 있다 - 여기서는 Clear 여야 한다.
 */
void testBackgroundAndUnknownAreClear() {
    const auto config = makeConfig();

    assert(judgeOf(recordAt(0.5F, 0.9F, 0.5F, 0.6F,
                            config.classes.background), config) == State::Clear);
    assert(judgeOf(recordAt(0.5F, 0.9F, 0.5F, 0.6F, 99), config)
           == State::Clear);
}

/* 점수 미달은 크기·위치와 무관하게 Clear 다. */
void testLowScoreIsIgnored() {
    const auto config = makeConfig();

    assert(judgeOf(recordAt(0.5F, 0.9F, 0.3F, config.stop_height + 0.2F,
                            config.classes.person,
                            config.min_score - 0.05F), config) == State::Clear);
}

/*
 * 정지 이벤트: 멈춤 -> hold_ms 유지 -> 해제. 해제에는 프레임 수와 경과
 * 시간이 **둘 다** 필요하다.
 */
void testHoldAndRelease() {
    const auto config = makeConfig();
    const DetectionRecord stopper =
        recordAt(0.5F, 0.9F, 0.3F, config.stop_height + 0.15F,
                 config.classes.car);

    safety::HazardLatch latch(makeLatchConfig());
    assert(latch.update(&stopper, 1u, config, 1000u) == State::Stop);

    /* 대상이 사라져도 hold_ms 동안은 Stop 이다. */
    assert(latch.update(nullptr, 0u, config, 2000u) == State::Stop);
    assert(latch.update(nullptr, 0u, config, 3999u) == State::Stop);

    /* hold 가 끝나면 내려가지만 래치는 아직 Released 다. */
    assert(latch.update(nullptr, 0u, config, 4001u) == State::Clear);
    assert(latch.update(nullptr, 0u, config, 4002u) == State::Clear);

    /* 같은 class 가 다시 나타나도 Released 중에는 재트리거하지 않는다. */
    assert(latch.update(&stopper, 1u, config, 4003u) == State::Clear);

    /* 다른 class 가 새로 위험 수준이면 즉시 새 이벤트다. */
    safety::HazardLatch other(makeLatchConfig());
    assert(other.update(&stopper, 1u, config, 1000u) == State::Stop);
    assert(other.update(nullptr, 0u, config, 4001u) == State::Clear);

    /* 끼어드는 class 는 Stop 에 도달할 수 있는 것이어야 한다 - 표지판은
     * Slow 가 상한이라 래치를 열지 못한다. */
    const DetectionRecord person =
        recordAt(0.5F, 0.9F, 0.3F, config.stop_height + 0.05F,
                 config.classes.person);
    assert(other.update(&person, 1u, config, 4002u) == State::Stop);
}

/*
 * release_frames 만 쓰면 판단 주기가 느릴 때 해제가 한없이 늘어지고,
 * release_ms 만 쓰면 판단이 멈춘 사이에 저절로 풀린다. 둘 다 필요하다.
 */
void testReleaseNeedsBothFramesAndTime() {
    const auto config = makeConfig();
    const DetectionRecord stopper =
        recordAt(0.5F, 0.9F, 0.3F, config.stop_height + 0.15F,
                 config.classes.car);

    safety::HazardLatch latch(makeLatchConfig());   /* frames=3, ms=200 */
    assert(latch.update(&stopper, 1u, config, 0u) == State::Stop);
    /* hold 종료 -> Released 진입. absent 계수는 여기서 시작한다. */
    assert(latch.update(nullptr, 0u, config, 3001u) == State::Clear);

    /* 프레임 3개는 채웠지만 200 ms 가 안 됐다 -> 아직 래치 중이다. */
    latch.update(nullptr, 0u, config, 3002u);   /* absent 1 */
    latch.update(nullptr, 0u, config, 3003u);   /* absent 2 */
    latch.update(nullptr, 0u, config, 3004u);   /* absent 3, 경과 3 ms */
    /* 같은 class 가 다시 나타나도 새 이벤트가 되지 않는다 = 아직 래치 중. */
    assert(latch.update(&stopper, 1u, config, 3005u) == State::Clear);

    /* 위 등장이 absent 계수를 되돌렸다. 이번에는 200 ms 를 넘겨 채운다. */
    latch.update(nullptr, 0u, config, 3200u);   /* absent 1 */
    latch.update(nullptr, 0u, config, 3300u);   /* absent 2 */
    latch.update(nullptr, 0u, config, 3400u);   /* absent 3, 경과 395 ms */
    assert(latch.update(&stopper, 1u, config, 3401u) == State::Stop);
}

/* 여러 개가 섞이면 가장 위험한 것을 고른다. */
void testWorstOfFrameWins() {
    const auto config = makeConfig();
    const std::vector<DetectionRecord> items = {
        recordAt(0.5F, 0.5F, config.sign_slow_width + 0.05F, 0.2F,
                 config.classes.sign_warning),
        recordAt(0.5F, 0.9F, 0.3F, config.stop_height + 0.05F,
                 config.classes.car),
    };
    assert(safety::judge(items.data(), items.size(), config) == State::Stop);
}

}  // namespace

int main() {
    testEmptyIsClear();
    testCarPersonDistanceThresholds();
    testSignUsesWidthNotHeight();
    testSignIgnoresGroundPlane();
    testSignRespectsZoneX();
    testSignNeverStops();
    testBackgroundAndUnknownAreClear();
    testLowScoreIsIgnored();
    testHoldAndRelease();
    testReleaseNeedsBothFramesAndTime();
    testWorstOfFrameWins();
    std::cout << "SafetyJudge/HazardLatch tests passed\n";
    return EXIT_SUCCESS;
}
