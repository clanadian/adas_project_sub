#include "roi/RoiProposer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <opencv2/imgcodecs.hpp>

namespace {

struct ExpectedProposal {
    float x;
    float y;
    float width;
    float height;
    float score;
};

constexpr std::array<ExpectedProposal, 8> kExpected{{
    {119.37236F, 0.0F, 86.72412F, 161.11890F, 0.891880F},
    {32.97283F, 154.92570F, 95.00764F, 98.00513F, 0.877332F},
    {286.65424F, 41.69487F, 67.55487F, 170.82280F, 0.874281F},
    {380.46359F, 58.20201F, 96.75192F, 178.46307F, 0.867646F},
    {243.52165F, 61.63186F, 64.89497F, 134.06552F, 0.847416F},
    {144.60437F, 169.60635F, 103.45374F, 101.90244F, 0.825596F},
    {302.91132F, 192.68115F, 80.77902F, 95.31885F, 0.804148F},
    {176.51115F, 152.62117F, 65.61119F, 48.88969F, 0.295911F}
}};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <engine> <golden-image>\n";
        return EXIT_FAILURE;
    }

    const cv::Mat image = cv::imread(argv[2], cv::IMREAD_COLOR);
    if (image.empty()) {
        std::cerr << "cannot read golden image\n";
        return EXIT_FAILURE;
    }

    adas::roi::ProposerConfig config;
    config.engine_path = argv[1];
    const adas::roi::RoiProposer proposer(config);
    const auto proposals = proposer.propose(image, 7u);

    if (proposals.size() != kExpected.size()) {
        std::cerr << "proposal count mismatch: expected " << kExpected.size()
                  << ", got " << proposals.size() << '\n';
        return EXIT_FAILURE;
    }

    float maximum_pixel_error = 0.0F;
    float maximum_score_error = 0.0F;
    for (std::size_t index = 0; index < proposals.size(); ++index) {
        const auto& actual = proposals[index];
        const auto& expected = kExpected[index];
        maximum_pixel_error = std::max({
            maximum_pixel_error,
            std::abs(actual.object_bbox.x - expected.x),
            std::abs(actual.object_bbox.y - expected.y),
            std::abs(actual.object_bbox.width - expected.width),
            std::abs(actual.object_bbox.height - expected.height)
        });
        maximum_score_error = std::max(
            maximum_score_error,
            std::abs(actual.objectness - expected.score)
        );
        std::cout << index
                  << ": x=" << actual.object_bbox.x
                  << " y=" << actual.object_bbox.y
                  << " w=" << actual.object_bbox.width
                  << " h=" << actual.object_bbox.height
                  << " score=" << actual.objectness << '\n';
    }

    std::cout << "max_pixel_error=" << maximum_pixel_error
              << " max_score_error=" << maximum_score_error << '\n';
    if (maximum_pixel_error > 2.0F || maximum_score_error > 0.02F) {
        std::cerr << "TensorRT result is outside FP16 golden tolerance\n";
        return EXIT_FAILURE;
    }
    std::cout << "proposal TensorRT golden check passed\n";
    return EXIT_SUCCESS;
}
