#pragma once

#include <cstdint>
#include <optional>

#include <opencv2/core/mat.hpp>

#include "roi/RoiTypes.hpp"

namespace adas::preprocess {

// Jetson이 Ethernet으로 전송할 96x96 RGB uint8 ROI.
// INT8 양자화와 98x98 pre-padding은 Arty PS의 역할이다.
struct PreparedRoi {
    std::uint32_t frame_id{0};
    std::uint32_t roi_id{0};

    roi::BoundingBox object_bbox{};
    cv::Mat rgb_pixels;
};

class RoiPreprocessor final {
public:
    // cropped.bgr_pixels를 RGB CV_8UC3로 변환한다.
    // 입력이 96x96 BGR CV_8UC3이 아니면 std::nullopt를 반환한다.
    [[nodiscard]]
    std::optional<PreparedRoi> prepare(
        const roi::CroppedRoi& cropped
    ) const;
};

}  // namespace adas::preprocess
