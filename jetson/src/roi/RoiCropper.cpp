#include "roi/RoiCropper.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <opencv2/core/types.hpp>
#include <opencv2/imgproc.hpp>

namespace adas::roi {

namespace {

bool isSupportedOutputSize(int size){
    return size == 64 || size == 96 || size == 128;
}

bool isFiniteBox(const BoundingBox& box){
    return std::isfinite(box.x)
        && std::isfinite(box.y)
        && std::isfinite(box.width)
        && std::isfinite(box.height);
}
} // namespace

RoiCropper::RoiCropper(CropConfig config)
    : config_(config){
    if (!std::isfinite(config_.margin_ratio)
    || config_.margin_ratio < 0.0F){
        throw std::invalid_argument(
            "margin_ratio must be a finite non-negative value"
        );
    }
    if (!isSupportedOutputSize(config_.output_size)){
        throw std::invalid_argument(
            "output_size must be 64, 96, or 128"
        );
    }
}

std::optional<CroppedRoi> RoiCropper::crop(
    const cv::Mat& bgr_frame,
    const RoiCandidate& candidate
) const
{
    // 1. V4L2Capture가 반환한 BGR CV_8UC3 입력인지 검사한다.
    // cv::Mat 자체에는 BGR/RGB 메타데이터가 없으므로 이름과 계약으로 구분한다.
    if (bgr_frame.empty() || bgr_frame.type() != CV_8UC3){
        return std::nullopt;
    }

    const BoundingBox& box = candidate.object_bbox;

    // 2. bbox 값 검사
    if (!isFiniteBox(box)
        || box.width  <= 0.0F
        || box.height <= 0.0F){
            return std::nullopt;
    }

    // 3. bbox 중심과 margin 적용 후 크기 계산
    const float center_x = box.x + box.width  * 0.5F;
    const float center_y = box.y + box.height * 0.5F;

    const float expanded_width = 
        box.width * (1.0F + 2.0F * config_.margin_ratio);

    const float expanded_height = 
        box.height * (1.0F + 2.0F * config_.margin_ratio);

    // 긴 변을 기준으로 정사각형 크기를 정한다.
    const float square_size = 
        std::max(expanded_width, expanded_height);

    if (!std::isfinite(square_size) || square_size <= 0.0F){
        return std::nullopt;
    }

    // 4. 실수 좌표를 객체 전체가 포함되도록 정수 좌표로 변환
    int left = static_cast<int>(
        std::floor(center_x - square_size * 0.5F)
    );
    int top = static_cast<int>(
        std::floor(center_y - square_size * 0.5F)
    );
    int right = static_cast<int>(
        std::ceil(center_x + square_size * 0.5F)
    );
    int bottom = static_cast<int>(
        std::ceil(center_y + square_size * 0.5F)
    );

    const int initial_width  = right - left;
    const int initial_height = bottom - top;
    const int pixel_size = std::max(initial_width, initial_height);

    if (pixel_size <= 0){
        return std::nullopt;
    }

    // 정수 반올림 때문에 가로/세로 픽셀이 1픽셀 정도 다를 수 있으므로
    // 짧은 쪽을 늘려 정확한 정사각형으로 만든다.
    if (initial_width < pixel_size){
        const int difference = pixel_size - initial_width;
        left -= difference / 2;
        right = left + pixel_size;
    }

    if (initial_height < pixel_size) {
        const int difference = pixel_size - initial_height;
        top -= difference / 2;
        bottom = top + pixel_size;
    }

    const PixelRect crop_window{
        left,
        top,
        pixel_size,
        pixel_size
    };

    // 5. 요청 영역과 실제 프레임 영역의 교집합 계산
    const cv::Rect requested_rect{
        crop_window.x,
        crop_window.y,
        crop_window.width,
        crop_window.height
    };

    const cv::Rect frame_rect{
        0,
        0,
        bgr_frame.cols,
        bgr_frame.rows
    };

    const cv::Rect valid_rect = requested_rect & frame_rect;

    // ROI 전체가 화면 밖이면 만들 수 없다.
    if (valid_rect.empty()){
        return std::nullopt;
    }

    // 6. 요청 영역이 프레임을 벗어난 만큼 padding 크기를 계산한다.
    const Padding padding{
        valid_rect.x - requested_rect.x,
        valid_rect.y - requested_rect.y,
        requested_rect.br().x - valid_rect.br().x,
        requested_rect.br().y - valid_rect.br().y
    };

    const cv::Mat visible_crop = bgr_frame(valid_rect).clone();

    // 7. 화면 밖 영역을 검은색으로 채운다.
    cv::Mat padded_crop;

    cv::copyMakeBorder(
        visible_crop,
        padded_crop,
        padding.top,
        padding.bottom,
        padding.left,
        padding.right,
        cv::BORDER_CONSTANT,
        cv::Scalar::all(0)
    );

    cv::Mat resized_crop;

    cv::resize(
        padded_crop,
        resized_crop,
        cv::Size(config_.output_size, config_.output_size),
        0.0,
        0.0,
        cv::INTER_LINEAR
    );

    // 9. 결과 구성
    CroppedRoi result;
    result.frame_id = candidate.frame_id;
    result.roi_id = candidate.roi_id;
    result.object_bbox = box;
    result.crop_window = crop_window;
    result.padding = padding;
    result.objectness = candidate.objectness;
    result.frame_width = static_cast<std::uint32_t>(bgr_frame.cols);
    result.frame_height = static_cast<std::uint32_t>(bgr_frame.rows);
    result.bgr_pixels = resized_crop;

    return result;

}

const CropConfig& RoiCropper::config() const noexcept {
    return config_;
}

}  // namespace adas::roi
