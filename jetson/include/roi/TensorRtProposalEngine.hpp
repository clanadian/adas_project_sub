#pragma once

#include <memory>
#include <string>
#include <vector>

namespace adas::roi {

// TensorRT engine 파일의 로드, CUDA buffer 관리와 단일 추론만 담당한다.
class TensorRtProposalEngine final {
public:
    explicit TensorRtProposalEngine(const std::string& engine_path);
    ~TensorRtProposalEngine();

    TensorRtProposalEngine(const TensorRtProposalEngine&) = delete;
    TensorRtProposalEngine& operator=(const TensorRtProposalEngine&) = delete;

    // input은 1x3x320x320 FP32 NCHW, 반환값은 1x5x2100이다.
    [[nodiscard]]
    std::vector<float> infer(const std::vector<float>& input);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adas::roi
