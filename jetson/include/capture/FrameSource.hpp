#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <opencv2/opencv.hpp>

#include "capture/V4L2Capture.hpp"

/*
 * V4L2Capture::captureFrame()을 별도 스레드에서 계속 돌리고, 소비자(분류
 * 루프)는 그때그때 최신 프레임만 받아간다.
 *
 * 기존 구조는 캡처 대기(카메라 30fps 한계로 ~33ms)와 처리(YOLO propose +
 * TCP 왕복, ROI 1~2개 기준 ~20ms)가 한 스레드에서 순차로 일어나 프레임당
 * 시간이 둘의 합이었다(DB_EB_VERIFICATION_SUMMARY.md 실측 14.6~14.8
 * ROI/s). 캡처를 별도 스레드로 빼면 소비자가 이전 프레임을 처리하는
 * 동안 다음 프레임을 미리 받아둘 수 있어, 프레임당 시간이
 * max(캡처, 처리)에 가까워진다 - 카메라 fps가 진짜 하한이 된다.
 *
 * "모든 프레임을 처리"에서 "최신 프레임만 처리"로 바뀌는 게 트레이드
 * 오프다. 소비자가 느리면 중간 프레임은 버려진다. 실시간 로봇 제어에는
 * 오래된 프레임을 쌓아 순서대로 처리하는 것보다 최신 프레임을 처리하는
 * 쪽이 낫다.
 */
class FrameSource {
public:
    enum class Status {
        Ok,       // out_bgr에 새 프레임이 들어왔다
        Timeout,  // timeout_ms 동안 새 프레임이 안 왔다. 오류가 아니다
        Stopped,  // stop()으로 깨어났다
        Error,    // 캡처 스레드가 장치 오류로 멈췄다
    };

    // capture는 이미 init() 된 상태로 넘긴다. FrameSource는 소유하지
    // 않고 참조만 한다 - 열고 닫는 책임은 호출부에 남긴다.
    explicit FrameSource(V4L2Capture& capture);
    ~FrameSource();

    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    // 캡처 스레드를 띄운다. 한 번만 부른다.
    void start();

    // 캡처 스레드를 멈추고 join한다. capture.requestStop()도 같이
    // 불러 DQBUF 대기 중이어도 즉시 깨운다. 여러 번 불러도 안전하다.
    void stop();

    // last_seq보다 새 프레임이 생기면 즉시 반환한다. 처음 호출할 때는
    // last_seq를 0으로 둔다 - 성공 시 그 프레임의 시퀀스 번호로
    // 갱신되므로 다음 호출에 그대로 넘기면 된다.
    Status next(cv::Mat& out_bgr, std::uint64_t& last_seq, int timeout_ms);

private:
    void run();

    V4L2Capture& capture_;
    std::thread thread_;
    std::atomic_bool running_{false};

    std::mutex mutex_;
    std::condition_variable cv_;
    cv::Mat latest_frame_;
    std::uint64_t seq_{0};
    bool capture_error_{false};
};
