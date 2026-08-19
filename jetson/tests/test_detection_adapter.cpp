#include "control/DetectionAdapter.hpp"

#include "roi_protocol.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

/*
 * 이 프로젝트의 클래스 배치. KR260 과 한 칸씩 다르다 - background 가 앞에
 * 붙었기 때문이다. 이 값을 틀리면 car 를 person 으로 판단하고, 그건 오류
 * 없이 결과만 틀리는 종류다.
 */
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

adas::control::RoiObservation makeObservation(
    float x, float y, float width, float height,
    float objectness,
    std::uint32_t class_id,
    std::uint32_t confidence_ppm,
    bool classified = true,
    std::uint32_t status = ADAS_ROI_STATUS_OK
) {
    adas::control::RoiObservation observation;
    observation.candidate.object_bbox = {x, y, width, height};
    observation.candidate.objectness = objectness;
    observation.result.status = status;
    observation.result.class_id = class_id;
    observation.result.confidence_ppm = confidence_ppm;
    observation.classified = classified;
    return observation;
}

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 1e-5F;
}

void testNormalizesAgainstFrameSize() {
    const adas::control::AdapterConfig config;   /* 640x360 기본 */
    safety::DetectionRecord record{};

    const auto observation = makeObservation(160.0F, 90.0F, 320.0F, 180.0F,
                                             0.9F, 2u, 900000u);
    assert(adas::control::adapt(observation, config, projectClasses(), record)
           == adas::control::AdaptResult::Hazard);

    assert(near(record.x1, 0.25F));
    assert(near(record.y1, 0.25F));
    assert(near(record.x2, 0.75F));
    assert(near(record.y2, 0.75F));
    /* 판단 계층의 min_score 기준은 objectness 다. */
    assert(near(record.score, 0.9F));
    assert(record.class_id == 2);
}

void testRejectsLowObjectnessAndDegenerateBoxes() {
    adas::control::AdapterConfig config;
    config.min_objectness = 0.25F;
    safety::DetectionRecord record{};

    const auto weak = makeObservation(10.0F, 10.0F, 50.0F, 50.0F,
                                      0.10F, 2u, 900000u);
    assert(adas::control::adapt(weak, config, projectClasses(), record)
           == adas::control::AdaptResult::Rejected);

    /* 폭·높이가 0 이면 거리 대용값이 성립하지 않는다. */
    const auto flat = makeObservation(10.0F, 10.0F, 50.0F, 0.0F,
                                      0.9F, 2u, 900000u);
    assert(adas::control::adapt(flat, config, projectClasses(), record)
           == adas::control::AdaptResult::Rejected);
}

void testBackgroundIsDropped() {
    const adas::control::AdapterConfig config;
    safety::DetectionRecord record{};

    const auto background = makeObservation(100.0F, 100.0F, 100.0F, 100.0F,
                                            0.9F, 0u, 900000u);
    assert(adas::control::adapt(background, config, projectClasses(), record)
           == adas::control::AdaptResult::Background);
}

/*
 * 분류에 실패한 관측이 Clear 로 흘러가면 안 된다. proposal 이 이미
 * "물체가 있다"고 말했으므로, class 를 못 얻은 것은 정보 부족이지
 * 안전 근거가 아니다.
 */
void testUnclassifiedPathsAllConverge() {
    adas::control::AdapterConfig config;
    config.min_confidence_ppm = 600000u;
    safety::DetectionRecord record{};
    const safety::ClassMap classes = projectClasses();

    /* (1) TCP 왕복 실패 */
    const auto no_round_trip = makeObservation(100.0F, 100.0F, 100.0F, 100.0F,
                                               0.9F, 2u, 900000u, false);
    assert(adas::control::adapt(no_round_trip, config, classes, record)
           == adas::control::AdaptResult::Unclassified);
    /* 정체 모를 물체는 car/person 규칙으로 본다 - person id 로 채워진다. */
    assert(record.class_id == classes.person);

    /* (2) 가속기·후처리 오류 */
    const auto accelerator_error =
        makeObservation(100.0F, 100.0F, 100.0F, 100.0F, 0.9F, 2u, 900000u,
                        true, ADAS_ROI_STATUS_ACCELERATOR_ERROR);
    assert(adas::control::adapt(accelerator_error, config, classes, record)
           == adas::control::AdaptResult::Unclassified);

    /* (3) 신뢰도 미달 */
    const auto low_confidence = makeObservation(100.0F, 100.0F, 100.0F, 100.0F,
                                                0.9F, 2u, 100000u);
    assert(adas::control::adapt(low_confidence, config, classes, record)
           == adas::control::AdaptResult::Unclassified);

    /* 게이트를 끄면 같은 관측이 Hazard 로 통과한다 - 기본값이 0 인 이유다. */
    config.min_confidence_ppm = 0u;
    assert(adas::control::adapt(low_confidence, config, classes, record)
           == adas::control::AdaptResult::Hazard);
}

/*
 * KR260 배치를 그대로 쓰면 어떤 일이 벌어지는지 고정해 둔다.
 * class_id=1 은 이 프로젝트에서 car 지만 KR260 배치에서는 person 이다.
 */
void testClassMapIsNotInterchangeable() {
    const adas::control::AdapterConfig config;
    safety::DetectionRecord record{};

    const auto car = makeObservation(100.0F, 100.0F, 100.0F, 100.0F,
                                     0.9F, 1u, 900000u);

    assert(adas::control::adapt(car, config, projectClasses(), record)
           == adas::control::AdaptResult::Hazard);
    assert(record.class_id == projectClasses().car);

    const safety::ClassMap kr260;   /* 기본값 = KR260 배치 */
    assert(adas::control::adapt(car, config, kr260, record)
           == adas::control::AdaptResult::Hazard);
    /* 같은 바이트가 person 으로 읽힌다. */
    assert(record.class_id == kr260.person);
}

}  // namespace

int main() {
    testNormalizesAgainstFrameSize();
    testRejectsLowObjectnessAndDegenerateBoxes();
    testBackgroundIsDropped();
    testUnclassifiedPathsAllConverge();
    testClassMapIsNotInterchangeable();
    std::cout << "DetectionAdapter tests passed\n";
    return EXIT_SUCCESS;
}
