#include "capture/V4L2Capture.hpp"
#include "network/TcpRoiClient.hpp"
#include "preprocess/RoiPreprocessor.hpp"
#include "roi/RoiCropper.hpp"
#include "roi/RoiProposer.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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
    if (argc != 5) {
        std::cerr << "usage: " << argv[0]
                  << " <video-device> <ps-ip> <port> <engine|--full-frame>\n";
        return EXIT_FAILURE;
    }

    std::uint16_t port = 0;
    if (!parse_port(argv[3], port)) {
        std::cerr << "invalid TCP port\n";
        return EXIT_FAILURE;
    }
    const bool full_frame_mode = std::string(argv[4]) == "--full-frame";

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
        }
        ++frame_id;
    }

    capture.requestStop();
    client.disconnect();
    return EXIT_SUCCESS;
}
