#pragma once

#include <cstddef>
#include <cstdint>

#include "common/SafetyMessage.hpp"

//detection에서 안전 상태를 판단한다.
//
//힙을 쓰지 않고 std 컨테이너도 받지 않는다. R5 베어메탈에서 같은 코드가
//돌아야 하기 때문이다. 입력은 포인터와 개수로 받는다.
namespace safety {

//UART_PROTOCOL_v0.md의 state 값과 같은 순서다.
//숫자가 클수록 위험하다. 이 순서에 비교 연산이 의존한다.
enum class State : uint8_t {
    Clear = 0x00,
    Slow  = 0x01,
    Stop  = 0x02,
};

//클래스 ID 매핑.
//
//프로젝트마다 모델의 클래스 순서가 다르다. KR260 은 car=0 부터 시작하고
//background 가 없지만, Jetson-Arty 판은 background=0 이 앞에 붙어 전체가
//한 칸씩 밀린다. 상수를 소스에 박으면 반대쪽에서 **오류 없이 car 를 person
//으로 판단한다** - 실행 중에는 드러나지 않는 종류다.
//
//기본값은 KR260 값이라 그쪽 동작은 바뀌지 않는다.
struct ClassMap {
    int32_t car              = 0;
    int32_t person           = 1;
    int32_t sign_warning     = 2;
    int32_t sign_prohibition = 3;
    int32_t sign_mandatory   = 4;
    //모델에 background 클래스가 없으면 -1. 있으면 그 id 는 위험 대상이
    //아니라 "물체 아님"이므로 판단에서 제외한다.
    int32_t background       = -1;
};

//판단 기준. 임계값은 실보드에서 조정한다.
//
//카메라에 depth가 없어 거리를 직접 알 수 없다. 대신 박스 높이를 거리
//대용으로 쓴다. 가까울수록 크게 잡히기 때문이다. 정확한 거리가 아니라
//"가까운가"만 판단하는 용도다.
struct JudgeConfig {
    //진행 경로 영역. 화면 하단 가운데가 로봇 바로 앞이다.
    //옆 인도에 서 있는 사람 때문에 멈추지 않도록 가로를 좁힌다.
    float zone_x_min = 0.25f;
    float zone_x_max = 0.75f;

    //박스 아랫변이 이 아래에 있어야 경로 안으로 본다.
    //위쪽에 찍히는 것은 멀리 있는 것이다.
    float zone_y_min = 0.55f;

    //박스 높이 임계값. 이보다 크면 가깝다고 본다.
    float stop_height = 0.45f;
    float slow_height = 0.25f;

    //Sign-only distance gate. A sign at or above this box width yields Slow.
    //
    //**Signs never produce Stop.** Team decision: the classifier only
    //separates signs into broad categories, not individual signs, so there is
    //no way to tell "stop sign" from any other sign - and reacting to all of
    //them with a full stop was wrong far more often than it was right. A sign
    //that is misjudged now costs a needless slowdown instead of a needless
    //halt, which is the cheaper failure by a wide margin.
    //
    //Width, not height. Until 2026-08-21 this gate was sign_slow_height=0.50
    //and signs that plainly warranted a Slow never reached it.
    //
    //**When re-tuning, note that the two axes are not interchangeable
    //numbers.** Each ratio is normalised against its own axis of a 640x360
    //frame, so a square box - which is what these signs produce - has a height
    //ratio 640/360 = 1.78x its width ratio. The old gate therefore demanded
    //180px of sign while width >= 0.20 asks for 128px, and that drop is what
    //actually changed the behaviour. Whether width is *also* the more robust
    //axis (camera tilt foreshortening the vertical) was suspected but never
    //measured - do not carry it forward as established.
    //
    //**Also rule out zone_x before blaming this gate.** judgeOne() rejects
    //anything whose centre falls outside [zone_x_min, zone_x_max] before it
    //ever looks at size, so a sign off to the side stays Clear at any width.
    //A capture that shows no reaction may be failing the position test rather
    //than this one.
    //
    //0.20 came from measuring a single capture by eye - a starting point, not
    //a calibrated value. Re-derive it from logged bbox values after redeploy.
    //It is overridden by the **Arty PS** via ADAS_SIGN_SLOW_WIDTH
    //(arty/ps_db/src/control/ps_safety_bridge.cpp), which is where tuning
    //happens - the judgement layer runs on the PS, not on the Jetson.
    float sign_slow_width = 0.20f;

    //이 점수 미만은 무시한다. decode 단계의 threshold와 별개로,
    //안전 판단에서는 더 확실한 것만 보고 싶을 수 있다.
    float min_score = 0.25f;

    //모델의 클래스 ID 배치. 기본값은 KR260 순서다.
    ClassMap classes;
};

//위험 대상 클래스의 기본 ID. model_io_v0.json(KR260)의 순서다.
//새 코드는 JudgeConfig::classes 를 쓴다 - 이 상수들은 기본값의 출처로만 남는다.
inline constexpr int32_t kClassCar             = 0;
inline constexpr int32_t kClassPerson          = 1;
inline constexpr int32_t kClassSignWarning     = 2;
inline constexpr int32_t kClassSignProhibition = 3;
inline constexpr int32_t kClassSignMandatory   = 4;

//car/person만 해당한다. 거리 대용(박스 높이)으로 Stop/Slow를 가른다.
bool isHazardClass(int32_t class_id, const ClassMap& classes);
bool isHazardClass(int32_t class_id);

//The three sign categories. The model separates signs only by category, not
//by individual sign, so "stop sign" cannot be told apart from the rest.
//
//A sign inside the path (zone_x) whose box width reaches sign_slow_width
//yields **Slow, never Stop**. Unlike car/person there is no zone_y_min
//(ground-plane) gate - a sign is mounted on a wall or post rather than
//resting on the ground, so it has no reason to appear low in the frame, and
//requiring that would miss a sign directly ahead.
//
//Because signs never reach Stop, they never open a HazardLatch event either:
//the latch triggers on Stop only. A sign therefore holds Slow for as long as
//it stays big enough in the path, rather than firing once and releasing.
bool isSignClass(int32_t class_id, const ClassMap& classes);
bool isSignClass(int32_t class_id);

//detection 하나가 어느 수준인지 본다. 경로 밖이거나 점수가 낮으면 Clear다.
State judgeOne(const DetectionRecord& det, const JudgeConfig& config);

//전체에서 가장 위험한 것을 고른다. 하나라도 Stop이면 Stop이다.
//items가 null이거나 count가 0이면 Clear다.
State judge(const DetectionRecord* items, size_t count, const JudgeConfig& config);

//judge()와 같지만 exclude_class(보통 -1: 아무것도 제외 안 함)를 건너뛰고,
//가장 위험한 detection의 class_id를 out_class에 남긴다(없으면 -1).
//HazardLatch가 "이미 처리한 class를 빼고 다시 판단"할 때 쓴다.
State judgeWorst(const DetectionRecord* items, size_t count, const JudgeConfig& config,
                 int32_t exclude_class, int32_t* out_class);

//items 중 target_class가 경로 안에서 판단 게이트(점수·zone)를 통과해
//하나라도 있으면 true. target_class < 0이면 항상 false.
bool classPresent(const DetectionRecord* items, size_t count, int32_t target_class,
                  const JudgeConfig& config);

}  // namespace safety
