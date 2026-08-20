#include "control/ps_safety_bridge.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>

#include "common/SafetyHazardLatch.hpp"
#include "common/SafetyJudge.hpp"
#include "common/SafetyMessage.hpp"
#include "control/SafetyTransmitter.hpp"
#include "control/UartPort.hpp"

namespace {

/*
 * docs/JETSON_CONTROL_DESIGN.md 부록 A와 동일한 클래스 배치다 - 모델의
 * background=0 때문에 KR260 기본값(car=0)에서 전부 한 칸 밀린다.
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
    std::uint32_t min_confidence_ppm{0u};
    float frame_width{640.0F};
    float frame_height{360.0F};

    /* 지금 모으는 중인 프레임의 관측. ps_safety_flush_frame()에서
     * HazardLatch::update()에 한 번에 넘기고 비운다.
     * SafetyDecider::records_와 같은 이유로 kMaxDetections를 상한으로
     * 쓴다. */
    safety::DetectionRecord pending[safety::kMaxDetections];
    std::size_t pending_count{0u};

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
     * DetectionAdapter::adapt()와 같은 규칙이다(jetson/src/control/
     * DetectionAdapter.cpp) - crop이 아니라 원본 bbox를 프레임 크기로
     * 정규화하고, objectness 미달/분류 실패/background를 갈라낸다.
     * Hazard/Unclassified만 버퍼에 남긴다 - Background/Rejected는
     * SafetyDecider::decide()도 세기만 하고 판단에는 안 넣는다.
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

    const bool usable_class =
        result->status == ADAS_ROI_STATUS_OK
        && result->confidence_ppm >= handle->min_confidence_ppm;

    if (!usable_class) {
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
     * 후보가 하나도 없어도 update를 부른다(SafetyDecider::decide()와
     * 같은 이유) - 래치가 Holding 중이면 시간을 흘려보내야 하고,
     * Released 중이면 absent 카운트를 올려야 한다. 여기서 건너뛰면
     * 정지가 영원히 안 풀린다.
     */
    handle->state = handle->latch.update(
        handle->pending_count == 0u ? nullptr : handle->pending,
        handle->pending_count,
        handle->judge_config,
        now_ms
    );
    handle->pending_count = 0u;

    if (handle->transmitter) {
        handle->transmitter->publish(handle->state, now_ms);
    }
}

void ps_safety_force_stop(ps_safety_handle_t* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->pending_count = 0u;
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
