#include "common/SafetyJudge.hpp"

namespace safety {

bool isHazardClass(int32_t class_id) {
    return class_id == kClassCar || class_id == kClassPerson;
}

bool isSignClass(int32_t class_id) {
    return class_id == kClassSignWarning || class_id == kClassSignProhibition ||
           class_id == kClassSignMandatory;
}

State judgeOne(const DetectionRecord& det, const JudgeConfig& config) {
    const bool is_sign = isSignClass(det.class_id);
    if (!is_sign && !isHazardClass(det.class_id)) {
        return State::Clear;
    }
    if (det.score < config.min_score) {
        return State::Clear;
    }

    //박스가 뒤집혀 들어오면 높이 계산이 음수가 된다. 판단하지 않는다.
    const float height = det.y2 - det.y1;
    if (height <= 0.0f || det.x2 <= det.x1) {
        return State::Clear;
    }

    //가로는 중심으로 본다. 화면 가장자리에 걸친 박스는 좌표가 0~1을
    //벗어날 수 있는데, 중심을 쓰면 그 영향이 절반으로 준다.
    const float center_x = (det.x1 + det.x2) * 0.5f;
    if (center_x < config.zone_x_min || center_x > config.zone_x_max) {
        return State::Clear;
    }

    if (is_sign) {
        //표지판은 zone_y_min(ground-plane) 게이트를 안 쓴다. car/person처럼
        //바닥에 발·바퀴가 닿는 대상이 아니라 세워두는 물건이라 화면 아래쪽에
        //찍힐 이유가 없다. 높이 임계값도 안 쓴다 — 경로 안에 보이면 곧바로
        //Stop이다. "표지판을 봤으면 일단 멈춘다"는 의도이고, 반복 트리거
        //방지·일정 시간 뒤 재출발은 HazardLatch가 이 위에서 처리한다.
        return State::Stop;
    }

    //car/person: 아랫변을 기준으로 삼는다. 사람의 발이 닿는 지점이 실제
    //위치에 가깝고, 박스 중심을 쓰면 키 큰 대상이 실제보다 멀리 있는 것처럼
    //잡힌다. 박스 높이는 거리 대용이다.
    if (det.y2 < config.zone_y_min) {
        return State::Clear;
    }
    if (height >= config.stop_height) {
        return State::Stop;
    }
    if (height >= config.slow_height) {
        return State::Slow;
    }
    return State::Clear;
}

State judge(const DetectionRecord* items, size_t count, const JudgeConfig& config) {
    return judgeWorst(items, count, config, /*exclude_class=*/-1, /*out_class=*/nullptr);
}

State judgeWorst(const DetectionRecord* items, size_t count, const JudgeConfig& config,
                 int32_t exclude_class, int32_t* out_class) {
    if (out_class != nullptr) {
        *out_class = -1;
    }
    if (items == nullptr || count == 0) {
        return State::Clear;
    }

    State worst = State::Clear;
    for (size_t i = 0; i < count; ++i) {
        //class_id는 항상 0 이상이라 exclude_class가 기본값 -1이면
        //아무것도 걸러지지 않는다. judge()가 이 함수를 그대로 재사용하는 이유다.
        if (items[i].class_id == exclude_class) {
            continue;
        }

        const State level = judgeOne(items[i], config);

        //Stop이 나오면 더 볼 필요가 없다. 가장 위험한 값이다.
        if (level == State::Stop) {
            if (out_class != nullptr) {
                *out_class = items[i].class_id;
            }
            return State::Stop;
        }
        if (static_cast<uint8_t>(level) > static_cast<uint8_t>(worst)) {
            worst = level;
            if (out_class != nullptr) {
                *out_class = items[i].class_id;
            }
        }
    }
    return worst;
}

bool classPresent(const DetectionRecord* items, size_t count, int32_t target_class,
                  const JudgeConfig& config) {
    if (items == nullptr || target_class < 0) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (items[i].class_id == target_class && judgeOne(items[i], config) != State::Clear) {
            return true;
        }
    }
    return false;
}

}  // namespace safety
