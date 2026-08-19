#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

#include "roi/RoiTypes.hpp"

namespace adas::roi {

// 프레임당 PL에 전송할 ROI 후보 수를 제한한다.
// 후보 생성 알고리즘의 세부 임계값은 알고리즘을 정한 후 추가한다.
struct ProposerConfig {
    std::size_t max_candidates{10};
    float confidence_threshold{0.10F};
    float nms_iou_threshold{0.45F};
    std::string engine_path{};
};

class TensorRtProposalEngine;

class RoiProposer final {
public:
    explicit RoiProposer(ProposerConfig config = {});
    ~RoiProposer();

    RoiProposer(const RoiProposer&) = delete;
    RoiProposer& operator=(const RoiProposer&) = delete;

    // bgr_frame은 V4L2Capture의 BGR CV_8UC3 출력이어야 한다.
    // 반환 백터는 objectness 내림차순으로 정렬하고
    // max_candidates 개 이하로 제한한다.
    [[nodiscard]]
    std::vector<RoiCandidate> propose(
        const cv::Mat& bgr_frame,
        std::uint32_t frame_id
    ) const;

    [[nodiscard]]
    const ProposerConfig& config() const noexcept;

private:
    ProposerConfig config_;
    mutable std::unique_ptr<TensorRtProposalEngine> engine_;
};

}  // namespace adas::roi
