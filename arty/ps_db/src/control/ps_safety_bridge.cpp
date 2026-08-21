#include "control/ps_safety_bridge.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "common/SafetyHazardLatch.hpp"
#include "common/SafetyJudge.hpp"
#include "common/SafetyMessage.hpp"
#include "control/SafetyTransmitter.hpp"
#include "control/UartPort.hpp"

namespace {

constexpr std::uint32_t kDefaultMinConfidencePpm = 600000u;

float ratioFromEnvironment(const char* name, float fallback) {
    const char* const text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }

    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(value)
        || value < 0.0F || value > 1.0F) {
        std::fprintf(stderr,
            "warning: %s must be a ratio in [0,1]; using %.3f\n",
            name, static_cast<double>(fallback));
        return fallback;
    }
    return value;
}

std::uint32_t minConfidenceFromEnvironment() {
    const char* const text = std::getenv("ADAS_MIN_CLASS_CONFIDENCE_PPM");
    if (text == nullptr || *text == '\0') {
        return kDefaultMinConfidencePpm;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > 1000000ul) {
        return kDefaultMinConfidencePpm;
    }
    return static_cast<std::uint32_t>(value);
}

/*
 * 모델의 background=0 때문에 KR260 기본값(car=0)에서 전부 한 칸 밀린다.
 */
safety::ClassMap projectClassMap() {
    safety::ClassMap classes;
    classes.background       = 0;
    classes.car              = 1;
    classes.person           = 2;
    classes.sign_warning     = 3;
    classes.sign_prohibition = 4;
    classes.sign_mandatory   = 5;
    return classes;
}

}  // namespace

struct ps_safety_handle {
    adas::control::PosixUartPort uart;
    adas::control::SteadySafetyClock clock;
    safety::HazardLatch latch;
    safety::JudgeConfig judge_config;

    /* AdapterConfig 대응. Jetson 카메라 포맷(640x360)에 고정돼 있다 -
     * Jetson 쪽과 마찬가지로 지금은 런타임에 안 바뀐다. */
    float min_objectness{0.25F};
    std::uint32_t min_confidence_ppm{kDefaultMinConfidencePpm};
    float frame_width{640.0F};
    float frame_height{360.0F};

    /* 지금 모으는 중인 프레임의 관측. ps_safety_flush_frame()에서
     * HazardLatch::update()에 한 번에 넘기고 비운다.
     * kMaxDetections를 상한으로 쓴다. */
    safety::DetectionRecord pending[safety::kMaxDetections];
    std::size_t pending_count{0u};
    bool pending_uncertain_slow{false};

    safety::State state{safety::State::Stop};
    std::unique_ptr<adas::control::SafetyTransmitter> transmitter;
    std::atomic_bool transmitter_stop{false};
    std::thread transmitter_thread;
};

ps_safety_handle_t* ps_safety_start(const char* uart_port, unsigned baud) {
    if (uart_port == nullptr || uart_port[0] == '\0') {
        return nullptr;
    }

    auto handle = std::make_unique<ps_safety_handle>();
    if (!handle->uart.open(uart_port, baud)) {
        return nullptr;
    }

    handle->judge_config.classes = projectClassMap();
    handle->min_confidence_ppm = minConfidenceFromEnvironment();

    /* 최종 구조에서는 Arty PS가 판단하므로 카메라 의존 gate도 여기서 읽는다. */
    handle->judge_config.sign_slow_height = ratioFromEnvironment(
        "ADAS_SIGN_SLOW_HEIGHT", handle->judge_config.sign_slow_height);
    handle->judge_config.stop_height = ratioFromEnvironment(
        "ADAS_STOP_HEIGHT", handle->judge_config.stop_height);
    handle->judge_config.slow_height = ratioFromEnvironment(
        "ADAS_SLOW_HEIGHT", handle->judge_config.slow_height);
    handle->judge_config.zone_y_min = ratioFromEnvironment(
        "ADAS_ZONE_Y_MIN", handle->judge_config.zone_y_min);
    handle->judge_config.zone_x_min = ratioFromEnvironment(
        "ADAS_ZONE_X_MIN", handle->judge_config.zone_x_min);
    handle->judge_config.zone_x_max = ratioFromEnvironment(
        "ADAS_ZONE_X_MAX", handle->judge_config.zone_x_max);
    handle->judge_config.min_score = ratioFromEnvironment(
        "ADAS_MIN_SCORE", handle->judge_config.min_score);

    if (handle->judge_config.zone_x_min > handle->judge_config.zone_x_max) {
        std::fprintf(stderr,
            "ADAS_ZONE_X_MIN must not exceed ADAS_ZONE_X_MAX\n");
        return nullptr;
    }
    if (handle->judge_config.slow_height > handle->judge_config.stop_height) {
        std::fprintf(stderr,
            "warning: ADAS_SLOW_HEIGHT exceeds ADAS_STOP_HEIGHT; "
            "car/person SLOW is unreachable\n");
    }

    std::fprintf(stderr,
        "safety judge: sign_slow_height=%.3f stop_height=%.3f "
        "slow_height=%.3f zone_x=[%.3f,%.3f] zone_y_min=%.3f "
        "min_score=%.3f min_class_confidence_ppm=%u\n",
        static_cast<double>(handle->judge_config.sign_slow_height),
        static_cast<double>(handle->judge_config.stop_height),
        static_cast<double>(handle->judge_config.slow_height),
        static_cast<double>(handle->judge_config.zone_x_min),
        static_cast<double>(handle->judge_config.zone_x_max),
        static_cast<double>(handle->judge_config.zone_y_min),
        static_cast<double>(handle->judge_config.min_score),
        handle->min_confidence_ppm);

    safety::HazardLatch::Config latch_config;
    latch_config.release_ms = 200u;
    latch_config.release_frames = 3u;
    handle->latch = safety::HazardLatch(latch_config);

    handle->transmitter = std::make_unique<adas::control::SafetyTransmitter>(
        handle->uart, handle->clock,
        adas::control::SafetyTransmitter::Config{});

    ps_safety_handle* raw = handle.release();
    raw->transmitter_thread = std::thread([raw]() {
        raw->transmitter->run(raw->transmitter_stop);
    });
    return raw;
}

void ps_safety_add_observation(
    ps_safety_handle_t* handle,
    const adas_roi_bbox_t* bbox,
    const adas_roi_result_t* result
) {
    if (handle == nullptr || bbox == nullptr || result == nullptr) {
        return;
    }
    if (handle->pending_count >= safety::kMaxDetections) {
        /* 이미 32개 모였다 - 더 있어도 "가장 위험한 것" 판단은 안 바뀐다. */
        return;
    }

    /*
     * crop이 아니라 원본 bbox를 프레임 크기로 정규화하고,
     * objectness 미달/분류 실패/background를 갈라낸다.
     */
    if (bbox->objectness < handle->min_objectness
        || !(bbox->width > 0.0F) || !(bbox->height > 0.0F)
        || !(handle->frame_width > 0.0F) || !(handle->frame_height > 0.0F)) {
        return;
    }

    safety::DetectionRecord record{};
    record.x1 = bbox->x / handle->frame_width;
    record.y1 = bbox->y / handle->frame_height;
    record.x2 = (bbox->x + bbox->width) / handle->frame_width;
    record.y2 = (bbox->y + bbox->height) / handle->frame_height;
    record.score = bbox->objectness;

    const bool classification_ok = result->status == ADAS_ROI_STATUS_OK;
    const bool usable_class = classification_ok
        && result->confidence_ppm >= handle->min_confidence_ppm;

    /*
     * 분류는 성공했지만 confidence가 낮으면 car/person/sign으로 확정하지
     * 않는다. 그렇다고 가까운 큰 후보를 Clear로 버리면 위험하므로, 주행
     * 경로 안에서 slow_height를 넘는 경우에만 보수적으로 Slow를 만든다.
     * 이 경로는 Stop을 만들지 않는다. 높은 confidence의 확정 객체만 아래
     * 일반 SafetyJudge 규칙으로 들어가 Slow/Stop을 결정한다.
     */
    if (classification_ok && !usable_class) {
        const float center_x = (record.x1 + record.x2) * 0.5F;
        const float height = record.y2 - record.y1;
        if (center_x >= handle->judge_config.zone_x_min
            && center_x <= handle->judge_config.zone_x_max
            && record.y2 >= handle->judge_config.zone_y_min
            && height >= handle->judge_config.slow_height) {
            handle->pending_uncertain_slow = true;
        }
        return;
    }

    if (!usable_class) {
        // 통신/가속기 오류처럼 class 자체를 얻지 못한 경우는 기존 fail-safe
        // 경로를 유지해 person 규칙으로 판단한다.
        record.class_id = handle->judge_config.classes.person;
    } else {
        const std::int32_t class_id =
            static_cast<std::int32_t>(result->class_id);
        const bool is_background =
            handle->judge_config.classes.background >= 0
            && class_id == handle->judge_config.classes.background;
        if (is_background) {
            return;
        }
        record.class_id = class_id;
    }

    handle->pending[handle->pending_count] = record;
    ++handle->pending_count;
}

void ps_safety_flush_frame(ps_safety_handle_t* handle) {
    if (handle == nullptr) {
        return;
    }

    const std::uint64_t now_ms = handle->clock.nowMs();

    /*
     * 후보가 하나도 없어도 update를 부른다. 래치가 Holding 중이면
     * 시간을 흘려보내야 하고,
     * Released 중이면 absent 카운트를 올려야 한다. 여기서 건너뛰면
     * 정지가 영원히 안 풀린다.
     */
    handle->state = handle->latch.update(
        handle->pending_count == 0u ? nullptr : handle->pending,
        handle->pending_count,
        handle->judge_config,
        now_ms
    );
    if (handle->pending_uncertain_slow
        && handle->state == safety::State::Clear) {
        handle->state = safety::State::Slow;
    }
    handle->pending_count = 0u;
    handle->pending_uncertain_slow = false;

    if (handle->transmitter) {
        handle->transmitter->publish(handle->state, now_ms);
    }
}

void ps_safety_force_stop(ps_safety_handle_t* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->pending_count = 0u;
    handle->pending_uncertain_slow = false;
    handle->state = safety::State::Stop;
    if (handle->transmitter) {
        handle->transmitter->publish(handle->state, handle->clock.nowMs());
    }
}

void ps_safety_stop(ps_safety_handle_t* handle) {
    if (handle == nullptr) {
        return;
    }
    ps_safety_force_stop(handle);
    handle->transmitter_stop.store(true);
    if (handle->transmitter_thread.joinable()) {
        handle->transmitter_thread.join();
    }
    delete handle;
}
