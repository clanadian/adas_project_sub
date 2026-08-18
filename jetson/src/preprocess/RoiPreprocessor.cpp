#include "preprocess/RoiPreprocessor.hpp"

#include <opencv2/imgproc.hpp>

namespace adas::preprocess {

std::optional<PreparedRoi> RoiPreprocessor::prepare(
    const roi::CroppedRoi& cropped
) const {
    // TODO 1: cropped.bgr_pixels가 비어 있지 않은지 검사
    // TODO 2: CV_8UC3인지 검사
    // TODO 3: 96x96인지 검사
    // TODO 4: cv::COLOR_BGR2RGB로 변환
    // TODO 5: frame_id, roi_id, object_bbox와 RGB pixels를 반환
    return std::nullopt;
}

}  // namespace adas::preprocess
