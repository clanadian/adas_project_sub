#include "control/DetectionAdapter.hpp"

#include "roi_protocol.h"

namespace adas::control {

namespace {

/*
 * crop 영역이 아니라 proposal 원본 bbox 를 쓴다. RoiCropper 가 만드는 crop
 * 은 15% 여백과 정사각 확장이 들어가 있어 실제 물체보다 크고, 그걸 거리
 * 대용값으로 쓰면 실제보다 가깝다고 판단한다.
 */
bool normalizeBox(
    const adas::roi::BoundingBox& box,
    const AdapterConfig& config,
    safety::DetectionRecord& out
) {
    if (config.frame_width <= 0 || config.frame_height <= 0) {
        return false;
    }
    if (!(box.width > 0.0F) || !(box.height > 0.0F)) {
        return false;
    }

    const float width = static_cast<float>(config.frame_width);
    const float height = static_cast<float>(config.frame_height);

    out.x1 = box.x / width;
    out.y1 = box.y / height;
    out.x2 = (box.x + box.width) / width;
    out.y2 = (box.y + box.height) / height;
    return true;
}

}  // namespace

AdaptResult adapt(
    const RoiObservation& observation,
    const AdapterConfig& config,
    const safety::ClassMap& classes,
    safety::DetectionRecord& out
) {
    if (observation.candidate.objectness < config.min_objectness) {
        return AdaptResult::Rejected;
    }
    if (!normalizeBox(observation.candidate.object_bbox, config, out)) {
        return AdaptResult::Rejected;
    }

    /* 판단 계층의 min_score 는 "물체일 가능성"에 대한 기준이다. */
    out.score = observation.candidate.objectness;

    /*
     * class 를 신뢰할 수 없는 경우들. 전부 Unclassified 로 모은다 -
     * 원인은 달라도 "물체는 있는데 정체를 모른다"는 상태는 같다.
     */
    const bool usable_class =
        observation.classified
        && observation.result.status == ADAS_ROI_STATUS_OK
        && observation.result.confidence_ppm >= config.min_confidence_ppm;

    if (!usable_class) {
        out.class_id = classes.person;
        return AdaptResult::Unclassified;
    }

    const std::int32_t class_id =
        static_cast<std::int32_t>(observation.result.class_id);

    /* 분류기가 명시적으로 "물체 아님"이라고 한 것은 버린다. */
    if (classes.background >= 0 && class_id == classes.background) {
        return AdaptResult::Background;
    }

    out.class_id = class_id;
    return AdaptResult::Hazard;
}

}  // namespace adas::control
