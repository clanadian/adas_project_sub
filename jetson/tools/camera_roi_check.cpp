#include "capture/V4L2Capture.hpp"
#include "roi/RoiCropper.hpp"
#include "roi/RoiProposer.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace {

std::string numbered_name(const std::string& prefix, unsigned number) {
    std::ostringstream stream;
    stream << prefix << std::setw(3) << std::setfill('0') << number << ".jpg";
    return stream.str();
}

cv::Rect clipped_rect(const adas::roi::BoundingBox& box, const cv::Size& size) {
    const int x1 = std::clamp(static_cast<int>(box.x), 0, size.width);
    const int y1 = std::clamp(static_cast<int>(box.y), 0, size.height);
    const int x2 = std::clamp(
        static_cast<int>(box.x + box.width), 0, size.width);
    const int y2 = std::clamp(
        static_cast<int>(box.y + box.height), 0, size.height);
    return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

std::string output_path(const std::string& directory, const std::string& name) {
    return directory + "/" + name;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: " << argv[0]
                  << " <video-device> <engine> [output-dir] [capture-count]\n";
        return EXIT_FAILURE;
    }

    const std::string output_dir =
        argc >= 4 ? argv[3] : "camera_roi_output";
    const unsigned capture_count =
        argc >= 5 ? static_cast<unsigned>(std::stoul(argv[4])) : 1U;
    if (capture_count == 0U) {
        std::cerr << "capture-count must be greater than zero\n";
        return EXIT_FAILURE;
    }

    if (::mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "failed to create output directory: " << output_dir << '\n';
        return EXIT_FAILURE;
    }

    V4L2Capture capture;
    if (!capture.init(argv[1])) {
        std::cerr << "failed to initialize camera\n";
        return EXIT_FAILURE;
    }

    adas::roi::ProposerConfig proposer_config;
    proposer_config.engine_path = argv[2];
    const adas::roi::RoiProposer proposer(proposer_config);
    const adas::roi::RoiCropper cropper;

    for (unsigned frame_id = 0; frame_id < capture_count; ++frame_id) {
        cv::Mat frame;
        const auto status = capture.captureFrame(frame, 2000);
        if (status != V4L2Capture::Result::Ok) {
            std::cerr << "failed to capture frame " << frame_id << '\n';
            return EXIT_FAILURE;
        }

        const auto candidates = proposer.propose(frame, frame_id);
        cv::Mat annotated = frame.clone();
        unsigned saved_rois = 0;

        for (const auto& candidate : candidates) {
            const auto cropped = cropper.crop(frame, candidate);
            if (!cropped) {
                continue;
            }

            const std::string crop_name =
                "frame_" + std::to_string(frame_id) + "_roi_"
                + std::to_string(candidate.roi_id) + ".jpg";
            cv::imwrite(output_path(output_dir, crop_name), cropped->bgr_pixels);
            ++saved_rois;

            const cv::Rect rect = clipped_rect(candidate.object_bbox, frame.size());
            if (rect.width > 0 && rect.height > 0) {
                cv::rectangle(annotated, rect, cv::Scalar(0, 255, 0), 2);
                std::ostringstream label;
                label << "roi " << candidate.roi_id << "  " << std::fixed
                      << std::setprecision(2) << candidate.objectness;
                cv::putText(
                    annotated,
                    label.str(),
                    cv::Point(rect.x, std::max(16, rect.y - 4)),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.5,
                    cv::Scalar(0, 255, 0),
                    1
                );
            }
        }

        const std::string frame_name = numbered_name("frame_", frame_id);
        cv::imwrite(output_path(output_dir, frame_name), annotated);
        std::cout << "frame=" << frame_id
                  << " size=" << frame.cols << 'x' << frame.rows
                  << " proposals=" << candidates.size()
                  << " crops=" << saved_rois << '\n';
    }

    std::cout << "saved to " << output_dir << '\n';
    return EXIT_SUCCESS;
}
