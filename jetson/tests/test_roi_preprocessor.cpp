#include "preprocess/RoiPreprocessor.hpp"

#include <cassert>
#include <iostream>

#include <opencv2/core.hpp>

namespace {

void testValidBgrInputIsConvertedToRgb() {
    adas::preprocess::RoiPreprocessor preprocessor;

    adas::roi::CroppedRoi cropped;
    cropped.frame_id = 10;
    cropped.roi_id = 3;
    cropped.object_bbox = {20.0F, 30.0F, 40.0F, 50.0F};

    // cv::Scalar의 3채널 순서는 B, G, R이다.
    cropped.bgr_pixels = cv::Mat(
        96,
        96,
        CV_8UC3,
        cv::Scalar(10, 20, 30)
    );

    const auto result = preprocessor.prepare(cropped);

    assert(result.has_value());
    assert(result->rgb_pixels.rows == 96);
    assert(result->rgb_pixels.cols == 96);
    assert(result->rgb_pixels.type() == CV_8UC3);

    // BGR [10, 20, 30]이 RGB [30, 20, 10]으로 바뀌었는지 검사한다.
    const cv::Vec3b rgb_pixel =
        result->rgb_pixels.at<cv::Vec3b>(0, 0);

    assert(rgb_pixel[0] == 30);
    assert(rgb_pixel[1] == 20);
    assert(rgb_pixel[2] == 10);

    // 색상 변환 후에도 ROI 식별자와 원본 bbox는 유지되어야 한다.
    assert(result->frame_id == cropped.frame_id);
    assert(result->roi_id == cropped.roi_id);
    assert(result->object_bbox.x == cropped.object_bbox.x);
    assert(result->object_bbox.y == cropped.object_bbox.y);
    assert(result->object_bbox.width == cropped.object_bbox.width);
    assert(result->object_bbox.height == cropped.object_bbox.height);

    // prepare()가 입력 BGR 이미지를 변경하지 않았는지도 확인한다.
    const cv::Vec3b original_pixel =
        cropped.bgr_pixels.at<cv::Vec3b>(0, 0);

    assert(original_pixel[0] == 10);
    assert(original_pixel[1] == 20);
    assert(original_pixel[2] == 30);
}

void testEmptyInputIsRejected() {
    adas::preprocess::RoiPreprocessor preprocessor;
    const adas::roi::CroppedRoi cropped;

    const auto result = preprocessor.prepare(cropped);

    assert(!result.has_value());
}

void testNonThreeChannelInputIsRejected() {
    adas::preprocess::RoiPreprocessor preprocessor;

    adas::roi::CroppedRoi cropped;
    cropped.bgr_pixels = cv::Mat(
        96,
        96,
        CV_8UC1,
        cv::Scalar(10)
    );

    const auto result = preprocessor.prepare(cropped);

    assert(!result.has_value());
}

void testWrongWidthIsRejected() {
    adas::preprocess::RoiPreprocessor preprocessor;

    adas::roi::CroppedRoi cropped;
    cropped.bgr_pixels = cv::Mat(
        96,
        64,
        CV_8UC3,
        cv::Scalar(10, 20, 30)
    );

    const auto result = preprocessor.prepare(cropped);

    assert(!result.has_value());
}

void testWrongHeightIsRejected() {
    adas::preprocess::RoiPreprocessor preprocessor;

    adas::roi::CroppedRoi cropped;
    cropped.bgr_pixels = cv::Mat(
        64,
        96,
        CV_8UC3,
        cv::Scalar(10, 20, 30)
    );

    const auto result = preprocessor.prepare(cropped);

    assert(!result.has_value());
}

}  // namespace

int main() {
    testValidBgrInputIsConvertedToRgb();
    testEmptyInputIsRejected();
    testNonThreeChannelInputIsRejected();
    testWrongWidthIsRejected();
    testWrongHeightIsRejected();

    std::cout << "RoiPreprocessor tests passed\n";
    return 0;
}
