#include "roi/RoiProposer.hpp"
#include "roi/TensorRtProposalEngine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace adas::roi {
namespace {

constexpr int kModelSize = 320;
constexpr std::size_t kPredictionCount = 2100u;

struct DecodedBox {
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
};

float intersection_over_union(const DecodedBox& a, const DecodedBox& b) {
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    const float intersection =
        std::max(0.0F, x2 - x1) * std::max(0.0F, y2 - y1);
    const float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    const float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    const float denominator = area_a + area_b - intersection;
    return denominator > 0.0F ? intersection / denominator : 0.0F;
}

}  // namespace

RoiProposer::RoiProposer(ProposerConfig config)
    : config_(config) {
    if (config_.max_candidates == 0) {
        throw std::invalid_argument(
            "max_candidates must be greater than zero"
        );
    }
    if (config_.confidence_threshold < 0.0F
        || config_.confidence_threshold > 1.0F
        || config_.nms_iou_threshold < 0.0F
        || config_.nms_iou_threshold > 1.0F) {
        throw std::invalid_argument("proposal thresholds must be in [0,1]");
    }
#ifdef ADAS_HAS_TENSORRT
    if (!config_.engine_path.empty()) {
        engine_ = std::make_unique<TensorRtProposalEngine>(config_.engine_path);
    }
#else
    if (!config_.engine_path.empty()) {
        throw std::runtime_error("this build has no TensorRT support");
    }
#endif
}

RoiProposer::~RoiProposer() = default;

std::vector<RoiCandidate> RoiProposer::propose(
    const cv::Mat& bgr_frame,
    std::uint32_t frame_id
) const {
    if (bgr_frame.empty() || bgr_frame.type() != CV_8UC3 || engine_ == nullptr) {
        return {};
    }

    const float scale = std::min(
        static_cast<float>(kModelSize) / static_cast<float>(bgr_frame.cols),
        static_cast<float>(kModelSize) / static_cast<float>(bgr_frame.rows));
    const int resized_width = static_cast<int>(
        std::nearbyint(static_cast<float>(bgr_frame.cols) * scale));
    const int resized_height = static_cast<int>(
        std::nearbyint(static_cast<float>(bgr_frame.rows) * scale));
    const float pad_x_float = (kModelSize - resized_width) / 2.0F;
    const float pad_y_float = (kModelSize - resized_height) / 2.0F;
    const int pad_x = static_cast<int>(std::nearbyint(pad_x_float - 0.1F));
    const int pad_y = static_cast<int>(std::nearbyint(pad_y_float - 0.1F));

    cv::Mat resized;
    cv::resize(bgr_frame, resized, {resized_width, resized_height},
               0.0, 0.0, cv::INTER_LINEAR);
    cv::Mat canvas(kModelSize, kModelSize, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(pad_x, pad_y, resized_width, resized_height)));

    std::vector<float> input(3u * kModelSize * kModelSize);
    for (int y = 0; y < kModelSize; ++y) {
        for (int x = 0; x < kModelSize; ++x) {
            const cv::Vec3b pixel = canvas.at<cv::Vec3b>(y, x);
            const std::size_t offset = static_cast<std::size_t>(y * kModelSize + x);
            input[0u * kModelSize * kModelSize + offset] = pixel[2] / 255.0F;
            input[1u * kModelSize * kModelSize + offset] = pixel[1] / 255.0F;
            input[2u * kModelSize * kModelSize + offset] = pixel[0] / 255.0F;
        }
    }

    const std::vector<float> output = engine_->infer(input);
    std::vector<DecodedBox> boxes;
    boxes.reserve(kPredictionCount);
    for (std::size_t index = 0; index < kPredictionCount; ++index) {
        const float score = output[4u * kPredictionCount + index];
        if (score < config_.confidence_threshold) {
            continue;
        }
        const float cx = output[0u * kPredictionCount + index];
        const float cy = output[1u * kPredictionCount + index];
        const float width = output[2u * kPredictionCount + index];
        const float height = output[3u * kPredictionCount + index];
        DecodedBox box{
            (cx - width / 2.0F - pad_x) / scale,
            (cy - height / 2.0F - pad_y) / scale,
            (cx + width / 2.0F - pad_x) / scale,
            (cy + height / 2.0F - pad_y) / scale,
            score
        };
        box.x1 = std::clamp(box.x1, 0.0F, static_cast<float>(bgr_frame.cols));
        box.x2 = std::clamp(box.x2, 0.0F, static_cast<float>(bgr_frame.cols));
        box.y1 = std::clamp(box.y1, 0.0F, static_cast<float>(bgr_frame.rows));
        box.y2 = std::clamp(box.y2, 0.0F, static_cast<float>(bgr_frame.rows));
        if (box.x2 > box.x1 && box.y2 > box.y1) {
            boxes.push_back(box);
        }
    }

    std::sort(boxes.begin(), boxes.end(), [](const DecodedBox& a, const DecodedBox& b) {
        return a.score > b.score;
    });

    std::vector<RoiCandidate> proposals;
    for (const DecodedBox& box : boxes) {
        bool suppressed = false;
        for (const RoiCandidate& kept : proposals) {
            const DecodedBox kept_box{
                kept.object_bbox.x,
                kept.object_bbox.y,
                kept.object_bbox.x + kept.object_bbox.width,
                kept.object_bbox.y + kept.object_bbox.height,
                kept.objectness
            };
            if (intersection_over_union(box, kept_box) > config_.nms_iou_threshold) {
                suppressed = true;
                break;
            }
        }
        if (suppressed) {
            continue;
        }
        proposals.push_back({
            frame_id,
            static_cast<std::uint32_t>(proposals.size()),
            {box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1},
            box.score
        });
        if (proposals.size() == config_.max_candidates) {
            break;
        }
    }
    return proposals;
}

const ProposerConfig& RoiProposer::config() const noexcept {
    return config_;
}

}  // namespace adas::roi
