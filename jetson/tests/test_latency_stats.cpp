#include "metrics/LatencyStats.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

bool nearlyEqual(double left, double right) {
    return std::fabs(left - right) < 1e-9;
}

void testEmpty() {
    adas::metrics::LatencyStats stats;
    assert(stats.empty());
    assert(stats.count() == 0u);
    // 표본이 없을 때 0을 돌려주는 것은 계약이다 - 요약 출력이 빈 구간에서도
    // 죽지 않아야 한다.
    assert(nearlyEqual(stats.meanMs(), 0.0));
    assert(nearlyEqual(stats.percentileMs(0.5), 0.0));
    assert(nearlyEqual(stats.maxMs(), 0.0));
}

void testSingleSample() {
    adas::metrics::LatencyStats stats;
    stats.add(7000);
    assert(stats.count() == 1u);
    assert(nearlyEqual(stats.meanMs(), 7.0));
    assert(nearlyEqual(stats.percentileMs(0.5), 7.0));
    assert(nearlyEqual(stats.percentileMs(0.95), 7.0));
    assert(nearlyEqual(stats.minMs(), 7.0));
    assert(nearlyEqual(stats.maxMs(), 7.0));
}

void testNearestRankPercentiles() {
    adas::metrics::LatencyStats stats;
    // 1 ms .. 100 ms를 역순으로 넣는다. 정렬이 실제로 일어나는지 확인한다.
    for (int value = 100; value >= 1; --value) {
        stats.add(static_cast<std::int64_t>(value) * 1000);
    }
    assert(stats.count() == 100u);
    assert(nearlyEqual(stats.meanMs(), 50.5));
    assert(nearlyEqual(stats.percentileMs(0.5), 50.0));
    assert(nearlyEqual(stats.percentileMs(0.95), 95.0));
    assert(nearlyEqual(stats.minMs(), 1.0));
    assert(nearlyEqual(stats.maxMs(), 100.0));
    // 범위를 벗어난 비율은 양 끝으로 잘린다.
    assert(nearlyEqual(stats.percentileMs(-1.0), 1.0));
    assert(nearlyEqual(stats.percentileMs(2.0), 100.0));
}

void testTailIsNotHiddenByMean() {
    /*
     * 이 검사가 이 클래스의 존재 이유다. Nagle/delayed ACK 같은 간헐적
     * 정체는 평균에 거의 안 나타나고 p95에만 나타난다. 평균만 보고했다면
     * 아래 40 ms 꼬리를 놓친다.
     */
    adas::metrics::LatencyStats stats;
    for (int i = 0; i < 95; ++i) {
        stats.add(8000);   // 8 ms
    }
    for (int i = 0; i < 5; ++i) {
        stats.add(48000);  // 40 ms 지연이 붙은 건
    }
    assert(nearlyEqual(stats.meanMs(), 10.0));
    assert(nearlyEqual(stats.percentileMs(0.5), 8.0));
    assert(nearlyEqual(stats.percentileMs(0.95), 8.0));
    assert(nearlyEqual(stats.maxMs(), 48.0));
    // p96부터 꼬리가 드러난다.
    assert(nearlyEqual(stats.percentileMs(0.96), 48.0));
}

}  // namespace

int main() {
    testEmpty();
    testSingleSample();
    testNearestRankPercentiles();
    testTailIsNotHiddenByMean();
    std::cout << "LatencyStats tests passed\n";
    return EXIT_SUCCESS;
}
