#pragma once

#include <cstdint>

#include <opencv2/core/mat.hpp>

namespace adas::roi {

// 좌표 기준:
// - x, y: 객체 bbox의 좌상단
// - width, height: bbox 크기
// - 영역은 [x, x + width), [y, y + height)
// detector 출력의 소수점 좌표를 보존한다.
struct BoundingBox {
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};
};

// margin과 정사각형 확장을 완료한 픽셀 단위 crop 영역.
// 프레임 밖으로 나갈 수 있으므로 x, y는 음수일 수 있다.
struct PixelRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

// crop 영역이 프레임을 벗어났을 때 추가할 검은색 padding 크기.
struct Padding {
    int left{0};
    int top{0};
    int right{0};
    int bottom{0};
};

// RoiProposer가 RoiCropper에 전달하는 객체 후보.
// objectness는 클래스가 아니라 "객체일 가능성"을 의미한다.
struct RoiCandidate {
    std::uint32_t frame_id{0};
    std::uint32_t roi_id{0};

    BoundingBox object_bbox{};
    float objectness{0.0F};
};

// ROI 전처리 설정.
//
// margin_ratio=0.15:
// bbox의 왼쪽·오른쪽에 각각 width의 15%,
// 위·아래에 각각 height의 15%를 추가한다.
struct CropConfig {
    float margin_ratio{0.15F};
    int output_size{96};
};

// RoiCropper의 결과.
//
// bgr_pixels는 V4L2Capture가 만든 BGR CV_8UC3 채널 순서를
// 그대로 유지한다. BGR→RGB 변환은 후속 RoiPreprocessor,
// uint8→INT8 양자화는 Arty PS에서 수행한다.
struct CroppedRoi {
    std::uint32_t frame_id{0};
    std::uint32_t roi_id{0};

    BoundingBox object_bbox{};
    PixelRect crop_window{};
    Padding padding{};

    cv::Mat bgr_pixels;
};

}  // namespace adas::roi
