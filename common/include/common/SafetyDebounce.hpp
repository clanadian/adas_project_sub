#pragma once

#include <cstdint>

#include "common/SafetyJudge.hpp"

//판단 결과의 진동을 막는다.
//
//detection은 프레임마다 흔들린다. 사람이 경로 경계에 서 있으면 잡혔다
//안 잡혔다를 반복하고, 그대로 내보내면 로봇이 덜컥거린다.
//
//핵심은 **비대칭**이다.
//  위험해지는 쪽  즉시 반영한다. 늦으면 못 멈춘다
//  안전해지는 쪽  연속 N 프레임과 T ms를 모두 채워야 한다
//
//TURTLEBOT_CONTROL_PLAN_v0.md의 "연속 N frame과 T ms 이상 위험 없음이
//모두 확인된 뒤 CLEAR를 송신한다"를 구현한 것이다. N과 T는 팀 협의
//항목이라 설정값으로 둔다.
namespace safety {

class Debounce {
public:
    struct Config {
        //안전 쪽으로 내려가기 위해 필요한 연속 프레임 수
        uint32_t release_frames = 10;

        //같은 조건이 유지되어야 하는 시간. 프레임 수만 보면 카메라가
        //빠를 때 너무 쉽게 풀린다. 둘 다 채워야 한다.
        uint32_t release_hold_ms = 500;
    };

    //초기 상태는 Stop이다. 아무것도 모르는 상태에서 움직이게 두지 않는다.
    //수신 측 초기 상태(expected.json의 initial_state)와도 같다.
    //
    //기본 인자로 Config{}를 쓰지 않는 이유는 중첩 구조체의 기본값을
    //바깥 클래스가 완성되기 전에 쓸 수 없기 때문이다. 오버로드로 나눈다.
    Debounce();
    explicit Debounce(const Config& config);

    //raw는 이번 프레임의 판단 결과, now_ms는 단조 증가 시각이다.
    //반환값이 실제로 송신할 상태다.
    State update(State raw, uint64_t now_ms);

    State state() const { return state_; }

    //현재 완화 조건을 얼마나 채웠는지. 로그와 테스트용이다.
    uint32_t pendingFrames() const { return pending_frames_; }

    //판단을 처음부터 다시 시작한다. 상태는 Stop으로 돌아간다.
    void reset();

private:
    Config   config_;
    State    state_          = State::Stop;

    //완화 후보 상태와 그것이 연속으로 관측된 횟수·시작 시각
    State    pending_        = State::Stop;
    uint32_t pending_frames_ = 0;
    uint64_t pending_since_  = 0;
};

}  // namespace safety
