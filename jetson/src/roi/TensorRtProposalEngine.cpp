#include "roi/TensorRtProposalEngine.hpp"

#ifdef ADAS_HAS_TENSORRT

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace adas::roi {
namespace {

class Logger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override {
        if (severity <= Severity::kWARNING) {
            std::fprintf(stderr, "TensorRT: %s\n", message);
        }
    }
};

template <typename T>
void destroy_trt(T*& object) {
    if (object != nullptr) {
        object->destroy();
        object = nullptr;
    }
}

std::size_t tensor_elements(const nvinfer1::Dims& dimensions) {
    std::size_t count = 1;
    for (int index = 0; index < dimensions.nbDims; ++index) {
        if (dimensions.d[index] <= 0) {
            throw std::runtime_error("TensorRT engine has a dynamic/invalid shape");
        }
        count *= static_cast<std::size_t>(dimensions.d[index]);
    }
    return count;
}

void check_cuda(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + ": " + cudaGetErrorString(status)
        );
    }
}

}  // namespace

class TensorRtProposalEngine::Impl {
public:
    explicit Impl(const std::string& engine_path) {
        std::ifstream file(engine_path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("cannot open TensorRT engine: " + engine_path);
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            throw std::runtime_error("TensorRT engine is empty");
        }
        file.seekg(0, std::ios::beg);
        std::vector<char> serialized(static_cast<std::size_t>(size));
        if (!file.read(serialized.data(), size)) {
            throw std::runtime_error("cannot read TensorRT engine");
        }

        runtime_ = nvinfer1::createInferRuntime(logger_);
        if (runtime_ == nullptr) {
            throw std::runtime_error("cannot create TensorRT runtime");
        }
        engine_ = runtime_->deserializeCudaEngine(
            serialized.data(), serialized.size(), nullptr);
        if (engine_ == nullptr) {
            throw std::runtime_error("cannot deserialize TensorRT engine");
        }
        context_ = engine_->createExecutionContext();
        if (context_ == nullptr) {
            throw std::runtime_error("cannot create TensorRT execution context");
        }

        input_index_ = engine_->getBindingIndex("images");
        output_index_ = engine_->getBindingIndex("output0");
        if (input_index_ < 0 || output_index_ < 0
            || !engine_->bindingIsInput(input_index_)
            || engine_->bindingIsInput(output_index_)) {
            throw std::runtime_error("unexpected TensorRT binding names");
        }
        input_elements_ = tensor_elements(engine_->getBindingDimensions(input_index_));
        output_elements_ = tensor_elements(engine_->getBindingDimensions(output_index_));
        if (input_elements_ != 1u * 3u * 320u * 320u
            || output_elements_ != 1u * 5u * 2100u) {
            throw std::runtime_error("unexpected TensorRT input/output shape");
        }

        bindings_.resize(static_cast<std::size_t>(engine_->getNbBindings()), nullptr);
        check_cuda(cudaMalloc(&bindings_[input_index_], input_elements_ * sizeof(float)),
                   "cudaMalloc(input)");
        check_cuda(cudaMalloc(&bindings_[output_index_], output_elements_ * sizeof(float)),
                   "cudaMalloc(output)");
        check_cuda(cudaStreamCreate(&stream_), "cudaStreamCreate");
    }

    ~Impl() {
        if (stream_ != nullptr) {
            cudaStreamDestroy(stream_);
        }
        if (input_index_ >= 0 && !bindings_.empty()) {
            cudaFree(bindings_[input_index_]);
        }
        if (output_index_ >= 0 && !bindings_.empty()) {
            cudaFree(bindings_[output_index_]);
        }
        destroy_trt(context_);
        destroy_trt(engine_);
        destroy_trt(runtime_);
    }

    std::vector<float> infer(const std::vector<float>& input) {
        if (input.size() != input_elements_) {
            throw std::invalid_argument("proposal input must be 1x3x320x320");
        }
        std::vector<float> output(output_elements_);
        check_cuda(cudaMemcpyAsync(
            bindings_[input_index_], input.data(), input.size() * sizeof(float),
            cudaMemcpyHostToDevice, stream_), "cudaMemcpyAsync(input)");
        if (!context_->enqueueV2(bindings_.data(), stream_, nullptr)) {
            throw std::runtime_error("TensorRT enqueueV2 failed");
        }
        check_cuda(cudaMemcpyAsync(
            output.data(), bindings_[output_index_], output.size() * sizeof(float),
            cudaMemcpyDeviceToHost, stream_), "cudaMemcpyAsync(output)");
        check_cuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize");
        return output;
    }

private:
    Logger logger_;
    nvinfer1::IRuntime* runtime_{nullptr};
    nvinfer1::ICudaEngine* engine_{nullptr};
    nvinfer1::IExecutionContext* context_{nullptr};
    cudaStream_t stream_{nullptr};
    std::vector<void*> bindings_;
    int input_index_{-1};
    int output_index_{-1};
    std::size_t input_elements_{0};
    std::size_t output_elements_{0};
};

TensorRtProposalEngine::TensorRtProposalEngine(const std::string& engine_path)
    : impl_(std::make_unique<Impl>(engine_path)) {}

TensorRtProposalEngine::~TensorRtProposalEngine() = default;

std::vector<float> TensorRtProposalEngine::infer(const std::vector<float>& input) {
    return impl_->infer(input);
}

}  // namespace adas::roi

#else

#include <stdexcept>

namespace adas::roi {

class TensorRtProposalEngine::Impl {};

TensorRtProposalEngine::TensorRtProposalEngine(const std::string&)
    : impl_(std::make_unique<Impl>()) {
    throw std::runtime_error("this build has no TensorRT support");
}

TensorRtProposalEngine::~TensorRtProposalEngine() = default;

std::vector<float> TensorRtProposalEngine::infer(const std::vector<float>&) {
    throw std::runtime_error("this build has no TensorRT support");
}

}  // namespace adas::roi

#endif
