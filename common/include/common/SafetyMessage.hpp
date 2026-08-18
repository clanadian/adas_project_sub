#pragma once

#include <cstddef>
#include <cstdint>

//APU가 RPU에게 넘기는 detection 메시지.
//
//공유 DDR에 그대로 놓이므로 POD여야 하고 크기가 고정이어야 한다.
//포인터, 가상 함수, std 컨테이너를 넣지 않는다. R5 베어메탈에서도 같은
//선언을 쓴다.
//
//APU와 RPU는 서로 다른 코어라 lock을 걸 수 없다. 대신 sequence를 두 번
//올리는 seqlock 방식으로 찢어진 읽기를 막는다.
namespace safety {

//detection 하나. YoloDecoder의 Detection을 공유 메모리용으로 줄인 것이다.
//좌표는 0~1 정규화이며 카메라 해상도에 의존하지 않는다.
struct DetectionRecord {
    float   x1, y1, x2, y2;
    float   score;
    int32_t class_id;
};

//한 프레임에서 넘길 수 있는 최대 detection 수.
//NMS 이후 값이라 실제로는 훨씬 적다. 고정 크기 배열을 쓰려고 상한을 둔다.
inline constexpr size_t   kMaxDetections = 32;

//구조가 바뀌면 올린다. RPU가 모르는 version이면 처리하지 않는다.
inline constexpr uint16_t kMessageVersion = 1;

//메모리를 잘못 짚었을 때 쓰레기를 유효 메시지로 읽지 않도록 하는 표식
inline constexpr uint32_t kMessageMagic = 0x41445341;  // "ADSA"

struct SafetyMessage {
    uint32_t magic;
    uint16_t version;
    uint16_t count;          //유효한 detections 개수

    //seqlock. 홀수면 쓰는 중, 짝수면 완료다.
    //읽기 전후의 값이 같고 짝수일 때만 신뢰한다.
    uint32_t sequence;

    //APU가 캡처한 시각. RPU가 오래된 데이터를 감지하는 데 쓴다.
    uint64_t capture_time_ns;

    DetectionRecord detections[kMaxDetections];
};

//--- 쓰기 쪽 (APU) --------------------------------------------------------

//detections를 메시지에 담는다. count가 kMaxDetections를 넘으면 앞에서부터 자른다.
//NMS가 점수 내림차순으로 주므로 잘리는 것은 점수가 낮은 쪽이다.
//
//sequence를 올리는 순서가 중요하다. 데이터를 쓰기 전과 후에 각각 올려야
//읽는 쪽이 "쓰는 중"을 구분할 수 있다.
void publish(SafetyMessage& msg,
             const DetectionRecord* items, size_t count,
             uint64_t capture_time_ns);

//--- 읽기 쪽 (RPU) --------------------------------------------------------

//메시지를 안전하게 복사한다.
//  true  = out이 온전한 스냅샷이다
//  false = magic/version이 맞지 않거나, 재시도 후에도 쓰는 중이었다
//
//lock이 없으므로 실패할 수 있다. 실패는 오류가 아니라 "다음 주기에 다시"다.
bool read(const SafetyMessage& shared, SafetyMessage& out, int max_retries = 4);

}  // namespace safety
