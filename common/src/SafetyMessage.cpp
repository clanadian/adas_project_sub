#include "common/SafetyMessage.hpp"

#include <atomic>
#include <cstring>

namespace safety {
namespace {

//seqlock은 컴파일러와 CPU가 순서를 바꾸지 않아야 성립한다.
//sequence 증가와 데이터 쓰기 사이에 장벽을 둔다.
//
//실보드에서는 이것만으로 부족할 수 있다. APU와 RPU는 별도 코어라
//cache flush/invalidate가 추가로 필요하다. 그 처리는 공유 버퍼를
//실제로 잡는 계층(P2)에서 하고, 여기서는 순서만 보장한다.
inline void barrier() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

}  // namespace

void publish(SafetyMessage& msg,
             const DetectionRecord* items, size_t count,
             uint64_t capture_time_ns) {
    if (count > kMaxDetections) {
        //NMS가 점수 내림차순으로 주므로 뒤쪽(낮은 점수)이 잘린다
        count = kMaxDetections;
    }

    //홀수로 만들어 "쓰는 중"을 알린다
    msg.sequence += 1;
    barrier();

    msg.magic            = kMessageMagic;
    msg.version          = kMessageVersion;
    msg.count            = static_cast<uint16_t>(count);
    msg.capture_time_ns  = capture_time_ns;

    if (items != nullptr && count > 0) {
        std::memcpy(msg.detections, items, count * sizeof(DetectionRecord));
    }

    barrier();
    //짝수로 되돌려 완료를 알린다
    msg.sequence += 1;
}

bool read(const SafetyMessage& shared, SafetyMessage& out, int max_retries) {
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        const uint32_t before = shared.sequence;
        if ((before & 1u) != 0u) {
            continue;  //쓰는 중이다. 다시 본다
        }

        barrier();
        std::memcpy(&out, &shared, sizeof(SafetyMessage));
        barrier();

        if (shared.sequence != before) {
            continue;  //읽는 사이에 바뀌었다. 찢어진 스냅샷이다
        }

        //내용이 우리가 아는 형식인지 확인한다.
        //메모리를 잘못 짚었거나 아직 초기화 전이면 여기서 걸린다.
        if (out.magic != kMessageMagic) {
            return false;
        }
        if (out.version != kMessageVersion) {
            return false;
        }
        if (out.count > kMaxDetections) {
            return false;
        }
        return true;
    }
    return false;  //계속 쓰는 중이었다. 다음 주기에 다시 시도한다
}

}  // namespace safety
