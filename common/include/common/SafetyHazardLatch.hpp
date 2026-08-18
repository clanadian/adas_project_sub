#pragma once

#include <cstdint>

#include "common/SafetyJudge.hpp"

//"표지판/사람/차를 봤으면 정지 → T ms 유지 → 출발 → 같은 대상을 다시
//트리거로 세지 않음(N프레임 연속으로 안 보일 때까지) → 다음은 새 이벤트"를
//구현한다. 팀 결정: 세 클래스 전부 이 방식으로 동일하게 다룬다.
//
//judge()/judgeOne()과 다른 층이다. 그쪽은 "이번 프레임에 뭐가 보이는가"만
//보는 순수 함수고, 이 클래스는 "이미 처리한 이벤트인가"를 기억한다.
//
//한 번에 이벤트 하나만 기억한다(전역 latch). object tracking이 없어서
//"같은 사람인지 다른 사람인지"조차 구분 못 하므로, class 단위가 이 시스템이
//낼 수 있는 최선의 근사다 — 같은 class의 다른 개체가 latch 기간에 나타나면
//역시 무시된다.
//
//주의: person/car에도 이 방식을 그대로 쓰면, 출발 후 그 대상이 실제로는
//그 자리에 그대로 있어도 N프레임 동안은 무시한다(표지판과 달리 사람/차는
//움직이지 않았을 수 있다). 팀 논의 후 승인된 트레이드오프다 — 데모에서
//대상 등장을 컨베이어/랙피니언으로 직접 통제하기 때문에 감수하기로 했다.
namespace safety {

class HazardLatch {
public:
    struct Config {
        //Stop을 무조건 유지하는 시간(T). 이 동안은 detection이 뭐라 해도
        //Stop을 낸다.
        uint64_t hold_ms = 3000;

        //래치를 풀기 위해 latched class가 연속으로 안 보여야 하는 프레임 수(N).
        uint32_t release_frames = 10;
    };

    HazardLatch();
    explicit HazardLatch(const Config& config);

    //이번 프레임의 raw detection과 판단 기준을 받아 실제로 낼 state를 정한다.
    //stale(APU 정지) 같은 상위 fail-safe는 이 함수를 부르지 않고 SafetyLoop가
    //직접 Stop을 낸다 — "T초 기다렸다 출발"이 fail-safe에 섞이면 안 된다.
    State update(const DetectionRecord* items, size_t count, const JudgeConfig& config,
                uint64_t now_ms);

    State state() const { return last_state_; }

    //처음부터 다시 시작한다. Idle로 돌아간다.
    void reset();

private:
    enum class Phase { Idle, Holding, Released };

    Config   config_;
    Phase    phase_          = Phase::Idle;
    int32_t  latched_class_  = -1;
    uint64_t hold_until_ms_  = 0;
    uint32_t absent_frames_  = 0;
    State    last_state_     = State::Clear;
};

}  // namespace safety
