#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <opencv2/core/mat.hpp>

#include "roi/RoiTypes.hpp"

namespace adas::stream {

// MJPEG 오버레이 한 개 = ROI 하나의 분류 결과.
// roi_protocol.h의 status/class_id 규약을 그대로 따른다
// (status==0이 성공, class_id==UINT32_MAX는 무효/미분류 sentinel).
struct RoiOverlay {
    adas::roi::BoundingBox bbox{};
    std::uint32_t status{0};
    std::uint32_t class_id{0};
    std::uint32_t confidence_ppm{0};
};

struct MjpegServerConfig {
    std::uint16_t port{8080};
    int jpeg_quality{80};

    // 클라이언트 소켓 send()에 거는 타임아웃(SO_SNDTIMEO).
    // 이 시간 안에 못 보내면 그 프레임은 건너뛴다.
    // 절대 이 값을 늘려서 "기다리게" 만들지 않는다 -
    // JETSON_MJPEG_STREAM_NOTES.md의 핵심 요구사항이다.
    int client_send_timeout_ms{200};

    // accept()/조건변수 대기가 종료 신호(running_ == false)를
    // 주기적으로 확인하도록 거는 상한 시간.
    int poll_interval_ms{500};
};

// 카메라 프레임 + ROI 분류 결과를 MJPEG-over-HTTP로 스트리밍한다.
//
// 반드시 지킬 것 (JETSON_MJPEG_STREAM_NOTES.md):
//   - 분류 루프와 절대 같은 스레드에 있으면 안 된다.
//   - publish()는 최신 프레임을 mutex로 보호된 슬롯에 복사만 하고
//     즉시 반환한다. JPEG 인코딩·네트워크 전송은 전부 이 클래스가
//     내부적으로 띄우는 인코더 스레드 / 클라이언트 스레드에서 일어난다.
//   - 클라이언트로의 소켓 쓰기는 짧은 타임아웃을 걸어, 느리거나 멈춘
//     브라우저가 분류 루프(프로듀서)를 절대 기다리게 하지 않는다.
//
// 스레드 구조:
//   producer(캡처/분류 루프) --publish()--> [frame_mutex_ 슬롯]
//                                              |
//                                     encoder_thread_ (JPEG 인코딩)
//                                              |
//                                          [jpeg_mutex_ 슬롯]
//                                        (여러 클라이언트가 공유)
//                              client_thread #1      client_thread #2 ...
//                              (넌블로킹/타임아웃 write)
class MjpegStreamServer final {
public:
    explicit MjpegStreamServer(MjpegServerConfig config = {});
    ~MjpegStreamServer();

    MjpegStreamServer(const MjpegStreamServer&) = delete;
    MjpegStreamServer& operator=(const MjpegStreamServer&) = delete;

    // 리스너 스레드 + 인코더 스레드를 띄운다. 실패하면 false.
    [[nodiscard]] bool start();

    // 모든 스레드를 정리하고 소켓을 닫는다. 소멸자도 이걸 부른다.
    void stop();

    // 캡처/분류 루프가 프레임마다 호출한다. bgr_frame을 clone해서
    // 공유 슬롯에 넣고 즉시 반환한다 - 블로킹 소켓 IO가 전혀 없다.
    // overlays는 이번 프레임에서 나온 ROI 분류 결과 (없으면 빈 벡터).
    void publish(const cv::Mat& bgr_frame, std::vector<RoiOverlay> overlays);

    [[nodiscard]] std::size_t clientCount() const noexcept;

private:
    void encoderLoop();
    void acceptLoop();
    void clientLoop(int client_fd);

    MjpegServerConfig config_;

    int listen_fd_{-1};
    std::atomic_bool running_{false};

    std::thread accept_thread_;
    std::thread encoder_thread_;
    mutable std::mutex client_threads_mutex_;
    std::vector<std::thread> client_threads_;
    std::atomic_size_t client_count_{0};

    // producer -> encoder
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    cv::Mat latest_frame_;
    std::vector<RoiOverlay> latest_overlays_;
    std::uint64_t frame_version_{0};
    bool has_frame_{false};

    // encoder -> clients
    std::mutex jpeg_mutex_;
    std::condition_variable jpeg_cv_;
    std::vector<std::uint8_t> latest_jpeg_;
    std::uint64_t jpeg_version_{0};
};

}  // namespace adas::stream
