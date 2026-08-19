#include "roi/RoiCropper.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <opencv2/core.hpp>

namespace {

cv::Mat makeSolidBgrFrame() {
    return cv::Mat(
        200,
        200,
        CV_8UC3,
        cv::Scalar(10, 20, 30)
    );
}

void assertPixelEquals(
    const cv::Mat& image,
    int y,
    int x,
    unsigned char channel0,
    unsigned char channel1,
    unsigned char channel2
) {
    const cv::Vec3b pixel = image.at<cv::Vec3b>(y, x);
    assert(pixel[0] == channel0);
    assert(pixel[1] == channel1);
    assert(pixel[2] == channel2);
}

void testCenteredBoxIsExpandedAndResized() {
    const adas::roi::RoiCropper cropper;
    const cv::Mat frame = makeSolidBgrFrame();

    adas::roi::RoiCandidate candidate;
    candidate.frame_id = 10;
    candidate.roi_id = 2;
    candidate.object_bbox = {70.0F, 80.0F, 40.0F, 20.0F};
    candidate.objectness = 0.9F;

    const auto result = cropper.crop(frame, candidate);

    assert(result.has_value());

    // 15% 여백 적용 후 가로 52, 세로 26이므로 52x52로 확장된다.
    assert(result->crop_window.x == 64);
    assert(result->crop_window.y == 64);
    assert(result->crop_window.width == 52);
    assert(result->crop_window.height == 52);

    assert(result->padding.left == 0);
    assert(result->padding.top == 0);
    assert(result->padding.right == 0);
    assert(result->padding.bottom == 0);

    assert(result->bgr_pixels.rows == 96);
    assert(result->bgr_pixels.cols == 96);
    assert(result->bgr_pixels.type() == CV_8UC3);
    assertPixelEquals(result->bgr_pixels, 0, 0, 10, 20, 30);
    assertPixelEquals(result->bgr_pixels, 48, 48, 10, 20, 30);

    assert(result->frame_id == candidate.frame_id);
    assert(result->roi_id == candidate.roi_id);
    assert(result->object_bbox.x == candidate.object_bbox.x);
    assert(result->object_bbox.y == candidate.object_bbox.y);
    assert(result->object_bbox.width == candidate.object_bbox.width);
    assert(result->object_bbox.height == candidate.object_bbox.height);
}

void testTopLeftOverflowGetsBlackPadding() {
    const adas::roi::RoiCropper cropper;
    const cv::Mat frame = makeSolidBgrFrame();

    adas::roi::RoiCandidate candidate;
    candidate.object_bbox = {0.0F, 0.0F, 20.0F, 20.0F};

    const auto result = cropper.crop(frame, candidate);

    assert(result.has_value());
    assert(result->crop_window.x == -3);
    assert(result->crop_window.y == -3);
    assert(result->crop_window.width == 26);
    assert(result->crop_window.height == 26);

    assert(result->padding.left == 3);
    assert(result->padding.top == 3);
    assert(result->padding.right == 0);
    assert(result->padding.bottom == 0);

    assertPixelEquals(result->bgr_pixels, 0, 0, 0, 0, 0);
    assertPixelEquals(result->bgr_pixels, 95, 95, 10, 20, 30);
}

void testBottomRightOverflowGetsBlackPadding() {
    const adas::roi::RoiCropper cropper;
    const cv::Mat frame = makeSolidBgrFrame();

    adas::roi::RoiCandidate candidate;
    candidate.object_bbox = {180.0F, 180.0F, 20.0F, 20.0F};

    const auto result = cropper.crop(frame, candidate);

    assert(result.has_value());
    assert(result->crop_window.x == 177);
    assert(result->crop_window.y == 177);
    assert(result->crop_window.width == 26);
    assert(result->crop_window.height == 26);

    assert(result->padding.left == 0);
    assert(result->padding.top == 0);
    assert(result->padding.right == 3);
    assert(result->padding.bottom == 3);

    assertPixelEquals(result->bgr_pixels, 0, 0, 10, 20, 30);
    assertPixelEquals(result->bgr_pixels, 95, 95, 0, 0, 0);
}

void testCompletelyOutsideBoxIsRejected() {
    const adas::roi::RoiCropper cropper;
    const cv::Mat frame = makeSolidBgrFrame();

    adas::roi::RoiCandidate candidate;
    candidate.object_bbox = {-100.0F, -100.0F, 20.0F, 20.0F};

    assert(!cropper.crop(frame, candidate).has_value());
}

void testInvalidFramesAreRejected() {
    const adas::roi::RoiCropper cropper;

    adas::roi::RoiCandidate candidate;
    candidate.object_bbox = {10.0F, 10.0F, 20.0F, 20.0F};

    const cv::Mat empty_frame;
    assert(!cropper.crop(empty_frame, candidate).has_value());

    const cv::Mat grayscale_frame(200, 200, CV_8UC1, cv::Scalar(10));
    assert(!cropper.crop(grayscale_frame, candidate).has_value());

    const cv::Mat float_frame(200, 200, CV_32FC3, cv::Scalar(1.0));
    assert(!cropper.crop(float_frame, candidate).has_value());
}

void testInvalidBoxesAreRejected() {
    const adas::roi::RoiCropper cropper;
    const cv::Mat frame = makeSolidBgrFrame();

    adas::roi::RoiCandidate candidate;

    candidate.object_bbox = {10.0F, 10.0F, 0.0F, 20.0F};
    assert(!cropper.crop(frame, candidate).has_value());

    candidate.object_bbox = {10.0F, 10.0F, -1.0F, 20.0F};
    assert(!cropper.crop(frame, candidate).has_value());

    candidate.object_bbox = {
        std::numeric_limits<float>::quiet_NaN(),
        10.0F,
        20.0F,
        20.0F
    };
    assert(!cropper.crop(frame, candidate).has_value());

    candidate.object_bbox = {
        10.0F,
        10.0F,
        std::numeric_limits<float>::infinity(),
        20.0F
    };
    assert(!cropper.crop(frame, candidate).has_value());
}

void testInvalidConfigsThrow() {
    bool negative_margin_threw = false;
    try {
        const adas::roi::RoiCropper cropper({-0.1F, 96});
        (void)cropper;
    } catch (const std::invalid_argument&) {
        negative_margin_threw = true;
    }
    assert(negative_margin_threw);

    bool unsupported_size_threw = false;
    try {
        const adas::roi::RoiCropper cropper({0.15F, 80});
        (void)cropper;
    } catch (const std::invalid_argument&) {
        unsupported_size_threw = true;
    }
    assert(unsupported_size_threw);
}

}  // namespace

int main() {
    testCenteredBoxIsExpandedAndResized();
    testTopLeftOverflowGetsBlackPadding();
    testBottomRightOverflowGetsBlackPadding();
    testCompletelyOutsideBoxIsRejected();
    testInvalidFramesAreRejected();
    testInvalidBoxesAreRejected();
    testInvalidConfigsThrow();

    std::cout << "RoiCropper tests passed\n";
    return 0;
}
