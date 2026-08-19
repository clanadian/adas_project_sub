#include "preprocess/RoiPreprocessor.hpp"

#include <opencv2/imgproc.hpp>

namespace adas::preprocess {

std::optional<PreparedRoi> RoiPreprocessor::prepare(
    const roi::CroppedRoi& cropped
) const {
    // 1. 96×96 BGR CV_8UC3 입력 검사
    if (cropped.bgr_pixels.empty()
        || cropped.bgr_pixels.type() != CV_8UC3
        || cropped.bgr_pixels.cols != 96
        || cropped.bgr_pixels.rows != 96) {
        return std::nullopt;
    }

    // 2. 결과 메타데이터 복사
    PreparedRoi result;
    result.frame_id = cropped.frame_id;
    result.roi_id = cropped.roi_id;
    result.object_bbox = cropped.object_bbox;

    // 3. BGR → RGB 변환
    cv::cvtColor(
        cropped.bgr_pixels,
        result.rgb_pixels,
        cv::COLOR_BGR2RGB
    );

    return result;
}

}  // namespace adas::preprocess
