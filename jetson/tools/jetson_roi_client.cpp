#include "capture/V4L2Capture.hpp"
#include "network/TcpRoiClient.hpp"
#include "preprocess/RoiPreprocessor.hpp"
#include "roi/RoiCropper.hpp"
#include "roi/RoiProposer.hpp"
#include "stream/MjpegStreamServer.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

std::atomic_bool stop_requested{false};

void handle_signal(int) {
    stop_requested.store(true);
}

bool parse_port(const char* text, std::uint16_t& port) {
    try {
        const unsigned long value = std::stoul(text);
        if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        port = static_cast<std::uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // argv[5] (mjpeg-port)는 선택 인자다: 안 주면 스트리밍 없이 기존과
    // 동일하게 동작한다.
    if (argc != 5 && argc != 6) {
        std::cerr << "usage: " << argv[0]
                  << " <video-device> <ps-ip> <port> <engine|--full-frame>"
                     " [mjpeg-port]\n";
        return EXIT_FAILURE;
    }

    std::uint16_t port = 0;
    if (!parse_port(argv[3], port)) {
        std::cerr << "invalid TCP port\n";
        return EXIT_FAILURE;
    }
    const bool full_frame_mode = std::string(argv[4]) == "--full-frame";

    std::unique_ptr<adas::stream::MjpegStreamServer> mjpeg_server;
    if (argc == 6) {
        std::uint16_t mjpeg_port = 0;
        if (!parse_port(argv[5], mjpeg_port)) {
            std::cerr << "invalid MJPEG port\n";
            return EXIT_FAILURE;
        }
        adas::stream::MjpegServerConfig mjpeg_config;
        mjpeg_config.port = mjpeg_port;
        mjpeg_server = std::make_unique<adas::stream::MjpegStreamServer>(mjpeg_config);
        if (!mjpeg_server->start()) {
            // 스트리밍은 부가 기능이다 - 못 띄워도 분류 파이프라인은
            // 계속 돌린다.
            std::cerr << "warning: failed to start MJPEG server on port "
                      << mjpeg_port << ", continuing without streaming\n";
            mjpeg_server.reset();
        } else {
            std::cout << "MJPEG stream: http://<jetson-ip>:" << mjpeg_port << "/\n";
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    V4L2Capture capture;
    if (!capture.init(argv[1])) {
        std::cerr << "failed to open camera: " << argv[1] << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "camera: " << capture.format().describe() << '\n';

    adas::network::TcpRoiClient client;
    if (client.connectToServer(argv[2], port)
        != adas::network::TcpClientStatus::Ok) {
        std::cerr << "failed to connect to PS server\n";
        return EXIT_FAILURE;
    }

    adas::roi::ProposerConfig proposer_config;
    if (!full_frame_mode) {
        proposer_config.engine_path = argv[4];
    }
    const adas::roi::RoiProposer proposer(proposer_config);
    const adas::roi::RoiCropper cropper;
    const adas::preprocess::RoiPreprocessor preprocessor;
    std::uint32_t frame_id = 0u;

    while (!stop_requested.load()) {
        cv::Mat frame;
        const V4L2Capture::Result capture_status =
            capture.captureFrame(frame, 1000);
        if (capture_status == V4L2Capture::Result::Timeout) {
            continue;
        }
        if (capture_status != V4L2Capture::Result::Ok) {
            std::cerr << "camera capture stopped or failed\n";
            break;
        }

        std::vector<adas::roi::RoiCandidate> candidates;
        if (full_frame_mode) {
            candidates.push_back({
                frame_id,
                0u,
                {0.0F, 0.0F,
                 static_cast<float>(frame.cols),
                 static_cast<float>(frame.rows)},
                1.0F
            });
        } else {
            candidates = proposer.propose(frame, frame_id);
        }

        // 이번 프레임에서 나온 ROI 분류 결과. MJPEG 서버로 넘겨
        // 화면에 겹쳐 그릴 때만 쓴다 - 분류 루프 자체의 동작에는
        // 영향이 없다.
        std::vector<adas::stream::RoiOverlay> overlays;
        overlays.reserve(candidates.size());

        for (const auto& candidate : candidates) {
            const auto cropped = cropper.crop(frame, candidate);
            if (!cropped) {
                continue;
            }
            const auto prepared = preprocessor.prepare(*cropped);
            if (!prepared) {
                continue;
            }

            adas::network::ClassificationResult result;
            if (client.classify(*prepared, result)
                != adas::network::TcpClientStatus::Ok) {
                std::cerr << "classification request failed\n";
                return EXIT_FAILURE;
            }

            std::cout << "frame=" << result.frame_id
                      << " roi=" << result.roi_id
                      << " status=" << result.status
                      << " class=" << result.class_id
                      << " confidence_ppm=" << result.confidence_ppm
                      << '\n';

            overlays.push_back({
                candidate.object_bbox,
                result.status,
                result.class_id,
                result.confidence_ppm
            });
        }

        // 캡처/분류 루프는 이 호출로 절대 멈추지 않는다 - publish()는
        // 프레임을 clone해서 공유 슬롯에 넣고 바로 반환한다. 실제 JPEG
        // 인코딩과 브라우저로의 전송은 MjpegStreamServer 내부의 별도
        // 스레드에서 일어난다 (JETSON_MJPEG_STREAM_NOTES.md 참고).
        if (mjpeg_server) {
            mjpeg_server->publish(frame, std::move(overlays));
        }

        ++frame_id;
    }

    capture.requestStop();
    client.disconnect();
    return EXIT_SUCCESS;
}
