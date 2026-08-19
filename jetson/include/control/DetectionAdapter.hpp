#pragma once

#include <cstdint>

#include "common/SafetyJudge.hpp"
#include "common/SafetyMessage.hpp"
#include "network/TcpRoiClient.hpp"
#include "roi/RoiTypes.hpp"

/*
 * Jetson 이 가진 bbox 와 Arty 가 준 class 를 하나로 합친다.
 *
 * 이 시스템에서 detection 한 건은 **두 곳에서 반씩** 온다. proposal 모델은
 * "여기 물체가 있다"까지만 말하고 class 는 모르며, 분류기는 96x96 crop 만
 * 받아 그것이 화면 어디에 있었는지 모른다. 안전 판단은 둘 다 필요하다.
 */
namespace adas::control {

/* 한 ROI 에 대해 이번 프레임에서 알아낸 전부. */
struct RoiObservation {
    adas::roi::RoiCandidate candidate;
    adas::network::ClassificationResult result;

    /*
     * TCP 왕복이 성공했는지. 실패(연결 끊김·타임아웃)면 result 는 의미가
     * 없다. 이 경우에도 candidate 는 유효하므로 버리지 않는다 - "물체가
     * 있다"는 정보는 남아 있다.
     */
    bool classified{false};
};

struct AdapterConfig {
    /* 정규화 기준. RoiProposer 가 받은 프레임 크기와 같아야 한다. */
    int frame_width{640};
    int frame_height{360};

    /*
     * 안전 판단에 쓸 최소 objectness. RoiProposer 의 conf(0.10)와 별개다 -
     * "후보로 볼 것"과 "제동 근거로 쓸 것"의 기준은 같을 이유가 없다.
     */
    float min_objectness{0.25F};

    /*
     * 분류 신뢰도 게이트. 0 이면 끈 것이다.
     *
     * 처음부터 켜지 않는 것을 권한다: 값을 잘못 잡으면 전부 Unclassified 가
     * 되어 과잉 정지처럼 보이는데, 원인이 게이트라는 것이 밖에서 안 보인다.
     * 먼저 끄고 confidence 분포를 로그로 모은 뒤 정한다.
     */
    std::uint32_t min_confidence_ppm{0u};
};

enum class AdaptResult {
    Hazard,        /* class 를 얻었고 위험 대상 후보다 */
    Background,    /* 분류기가 "물체 아님"이라고 했다 - 버린다 */
    Unclassified,  /* class 를 못 얻었다. 기하만으로 판단한다 */
    Rejected       /* objectness 미달이거나 bbox 가 성립하지 않는다 */
};

/*
 * observation 을 판단 계층이 쓰는 DetectionRecord 로 바꾼다.
 *
 * Unclassified 는 **person 의 class id 로 채워 내보낸다.** 이유는 이렇다:
 * 판단 계층은 class 로 규칙을 고르는데, car/person 규칙(바닥에 닿는 대상,
 * 박스 높이를 거리 대용으로)이 정체 모를 물체에 정확히 맞는 규칙이다.
 * 표지판 규칙(거리 무관 즉시 Stop)을 쓰면 화면 구석 오탐 하나로 멈춘다.
 * 원래 class 정보는 호출자가 observation 에 그대로 갖고 있으므로,
 * 로그·오버레이는 여기 값이 아니라 그쪽을 쓴다.
 *
 * out 은 AdaptResult 가 Hazard 또는 Unclassified 일 때만 채워진다.
 */
AdaptResult adapt(
    const RoiObservation& observation,
    const AdapterConfig& config,
    const safety::ClassMap& classes,
    safety::DetectionRecord& out
);

}  // namespace adas::control
