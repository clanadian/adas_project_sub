#pragma once

#include <optional>

#include <opencv2/core/mat.hpp>

#include "roi/RoiTypes.hpp"

namespace adas::roi {

class RoiCropper final {
public:
    explicit RoiCropper(CropConfig config = {});

    // bgr_frame은 V4L2Capture의 BGR CV_8UC3 출력이어야 한다.
    // 반환되는 ROI도 BGR 순서를 유지한다.
    // 정상적인 ROI를 만들 수 없으면 std::nullopt를 반환한다.
    [[nodiscard]]
    std::optional<CroppedRoi> crop(
        const cv::Mat& bgr_frame,
        const RoiCandidate& candidate
    ) const;

    [[nodiscard]]
    const CropConfig& config() const noexcept;

private:
    CropConfig config_;
};

}  // namespace adas::roi
