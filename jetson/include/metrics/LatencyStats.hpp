#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace adas::metrics {

/*
 * 보고서용 지연 표본 수집기.
 *
 * 측정 중에는 표본을 vector에 넣기만 하고(재할당 외 계산 없음), 백분위수는
 * 종료 시점에 한 번 정렬해서 낸다. 측정 자체가 파이프라인을 느리게 만들면
 * 그 숫자는 보고서에 쓸 수 없기 때문이다.
 *
 * 평균만 보고하지 않는 이유: Nagle/delayed ACK 같은 간헐적 정체는 평균에
 * 묻히고 p95·max 에만 나타난다.
 */
class LatencyStats final {
public:
    void add(std::int64_t microseconds) {
        samples_.push_back(microseconds);
    }

    void reserve(std::size_t capacity) {
        samples_.reserve(capacity);
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return samples_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return samples_.empty();
    }

    [[nodiscard]] std::int64_t sum() const noexcept {
        std::int64_t total = 0;
        for (const std::int64_t sample : samples_) {
            total += sample;
        }
        return total;
    }

    [[nodiscard]] double meanMs() const noexcept {
        if (samples_.empty()) {
            return 0.0;
        }
        return static_cast<double>(sum())
             / static_cast<double>(samples_.size()) / 1000.0;
    }

    /*
     * nearest-rank 백분위수. ratio 는 0.0~1.0 이며 0.5 가 중앙값이다.
     * 표본을 정렬하므로 호출 후 저장 순서는 보존되지 않는다 - 측정이 끝난
     * 뒤에만 부른다.
     */
    [[nodiscard]] double percentileMs(double ratio) {
        if (samples_.empty()) {
            return 0.0;
        }
        std::sort(samples_.begin(), samples_.end());
        const double clamped = ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio);
        // nearest-rank: rank = ceil(ratio * n), 1-based.
        const double rank =
            std::ceil(clamped * static_cast<double>(samples_.size()));
        const std::size_t index = rank < 1.0
            ? 0u
            : static_cast<std::size_t>(rank) - 1u;
        return static_cast<double>(
            samples_[index < samples_.size() ? index : samples_.size() - 1u]
        ) / 1000.0;
    }

    [[nodiscard]] double minMs() {
        return percentileMs(0.0);
    }

    [[nodiscard]] double maxMs() {
        return percentileMs(1.0);
    }

    [[nodiscard]] const std::vector<std::int64_t>& samples() const noexcept {
        return samples_;
    }

private:
    std::vector<std::int64_t> samples_;
};

}  // namespace adas::metrics
