#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

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
void destroyTrt(T*& object) {
    if (object != nullptr) {
        object->destroy();
        object = nullptr;
    }
}

void checkCuda(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + ": " + cudaGetErrorString(status));
    }
}

std::size_t elementSize(nvinfer1::DataType type) {
    switch (type) {
    case nvinfer1::DataType::kFLOAT: return 4u;
    case nvinfer1::DataType::kHALF:  return 2u;
    case nvinfer1::DataType::kINT8:  return 1u;
    case nvinfer1::DataType::kINT32: return 4u;
    case nvinfer1::DataType::kBOOL:  return 1u;
    }
    throw std::runtime_error("unsupported TensorRT binding dtype");
}

std::size_t bindingBytes(const nvinfer1::ICudaEngine& engine, int index) {
    const nvinfer1::Dims dims = engine.getBindingDimensions(index);
    std::size_t elements = 1u;
    for (int axis = 0; axis < dims.nbDims; ++axis) {
        if (dims.d[axis] <= 0) {
            throw std::runtime_error(
                "dynamic binding shape is unsupported; export fixed batch=1 graphs");
        }
        elements *= static_cast<std::size_t>(dims.d[axis]);
    }
    return elements * elementSize(engine.getBindingDataType(index));
}

class EngineRunner {
public:
    explicit EngineRunner(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("cannot open engine: " + path);
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            throw std::runtime_error("empty engine: " + path);
        }
        file.seekg(0, std::ios::beg);
        std::vector<char> bytes(static_cast<std::size_t>(size));
        if (!file.read(bytes.data(), size)) {
            throw std::runtime_error("cannot read engine: " + path);
        }

        runtime_ = nvinfer1::createInferRuntime(logger_);
        if (runtime_ == nullptr) {
            throw std::runtime_error("createInferRuntime failed");
        }
        engine_ = runtime_->deserializeCudaEngine(bytes.data(), bytes.size(), nullptr);
        if (engine_ == nullptr) {
            throw std::runtime_error("deserializeCudaEngine failed: " + path);
        }
        context_ = engine_->createExecutionContext();
        if (context_ == nullptr) {
            throw std::runtime_error("createExecutionContext failed");
        }

        bindings_.resize(static_cast<std::size_t>(engine_->getNbBindings()), nullptr);
        for (int index = 0; index < engine_->getNbBindings(); ++index) {
            const std::size_t bytes_count = bindingBytes(*engine_, index);
            checkCuda(cudaMalloc(&bindings_[static_cast<std::size_t>(index)], bytes_count),
                      "cudaMalloc(binding)");
            checkCuda(cudaMemset(bindings_[static_cast<std::size_t>(index)], 0, bytes_count),
                      "cudaMemset(binding)");
        }
    }

    ~EngineRunner() {
        for (void* binding : bindings_) {
            if (binding != nullptr) {
                cudaFree(binding);
            }
        }
        destroyTrt(context_);
        destroyTrt(engine_);
        destroyTrt(runtime_);
    }

    void enqueue(cudaStream_t stream) {
        if (!context_->enqueueV2(bindings_.data(), stream, nullptr)) {
            throw std::runtime_error("TensorRT enqueueV2 failed");
        }
    }

private:
    Logger logger_;
    nvinfer1::IRuntime* runtime_{nullptr};
    nvinfer1::ICudaEngine* engine_{nullptr};
    nvinfer1::IExecutionContext* context_{nullptr};
    std::vector<void*> bindings_;
};

struct Summary {
    double median{0.0};
    double mean{0.0};
    double p95{0.0};
    double maximum{0.0};
};

Summary summarize(std::vector<float> values) {
    if (values.empty()) {
        throw std::invalid_argument("cannot summarize zero samples");
    }
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double ratio) {
        const std::size_t index = static_cast<std::size_t>(
            ratio * static_cast<double>(values.size() - 1u));
        return static_cast<double>(values[index]);
    };
    return {percentile(0.50), sum / static_cast<double>(values.size()),
            percentile(0.95), static_cast<double>(values.back())};
}

void printSummary(const char* name, const std::vector<float>& values) {
    const Summary result = summarize(values);
    std::printf("%-28s n=%zu median=%8.4f mean=%8.4f p95=%8.4f max=%8.4f ms\n",
                name, values.size(), result.median, result.mean,
                result.p95, result.maximum);
}

unsigned parseCount(const char* text, const char* name) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0ul || value > 10000000ul) {
        throw std::invalid_argument(std::string("invalid ") + name + ": " + text);
    }
    return static_cast<unsigned>(value);
}

float timeOne(EngineRunner& engine, cudaStream_t stream,
              cudaEvent_t begin, cudaEvent_t end) {
    checkCuda(cudaEventRecord(begin, stream), "cudaEventRecord(begin)");
    engine.enqueue(stream);
    checkCuda(cudaEventRecord(end, stream), "cudaEventRecord(end)");
    checkCuda(cudaEventSynchronize(end), "cudaEventSynchronize(end)");
    float milliseconds = 0.0F;
    checkCuda(cudaEventElapsedTime(&milliseconds, begin, end),
              "cudaEventElapsedTime");
    return milliseconds;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::fprintf(stderr,
            "usage: %s <classifier.engine> [yolo.engine] [iterations] [warmup]\n",
            argv[0]);
        return EXIT_FAILURE;
    }

    try {
        const unsigned iterations = argc >= 4 ? parseCount(argv[3], "iterations") : 1000u;
        const unsigned warmup = argc >= 5 ? parseCount(argv[4], "warmup") : 100u;
        EngineRunner classifier(argv[1]);
        EngineRunner* yolo = nullptr;
        if (argc >= 3 && std::string(argv[2]) != "-") {
            yolo = new EngineRunner(argv[2]);
        }

        cudaStream_t stream = nullptr;
        cudaEvent_t e0 = nullptr;
        cudaEvent_t e1 = nullptr;
        cudaEvent_t e2 = nullptr;
        checkCuda(cudaStreamCreate(&stream), "cudaStreamCreate");
        checkCuda(cudaEventCreate(&e0), "cudaEventCreate(e0)");
        checkCuda(cudaEventCreate(&e1), "cudaEventCreate(e1)");
        checkCuda(cudaEventCreate(&e2), "cudaEventCreate(e2)");

        for (unsigned index = 0; index < warmup; ++index) {
            if (yolo != nullptr) {
                yolo->enqueue(stream);
            }
            classifier.enqueue(stream);
        }
        checkCuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize(warmup)");

        std::vector<float> classifier_alone;
        classifier_alone.reserve(iterations);
        for (unsigned index = 0; index < iterations; ++index) {
            classifier_alone.push_back(timeOne(classifier, stream, e0, e1));
        }
        std::puts("\nB. classifier alone (GPU compute only; no H2D/D2H)");
        printSummary("classifier", classifier_alone);

        if (yolo != nullptr) {
            std::vector<float> yolo_alone;
            std::vector<float> paired_yolo;
            std::vector<float> paired_classifier;
            std::vector<float> paired_total;
            yolo_alone.reserve(iterations);
            paired_yolo.reserve(iterations);
            paired_classifier.reserve(iterations);
            paired_total.reserve(iterations);

            for (unsigned index = 0; index < iterations; ++index) {
                yolo_alone.push_back(timeOne(*yolo, stream, e0, e1));
            }
            for (unsigned index = 0; index < iterations; ++index) {
                checkCuda(cudaEventRecord(e0, stream), "cudaEventRecord(pair begin)");
                yolo->enqueue(stream);
                checkCuda(cudaEventRecord(e1, stream), "cudaEventRecord(pair middle)");
                classifier.enqueue(stream);
                checkCuda(cudaEventRecord(e2, stream), "cudaEventRecord(pair end)");
                checkCuda(cudaEventSynchronize(e2), "cudaEventSynchronize(pair)");
                float yolo_ms = 0.0F;
                float classifier_ms = 0.0F;
                float total_ms = 0.0F;
                checkCuda(cudaEventElapsedTime(&yolo_ms, e0, e1), "elapsed(yolo)");
                checkCuda(cudaEventElapsedTime(&classifier_ms, e1, e2),
                          "elapsed(classifier)");
                checkCuda(cudaEventElapsedTime(&total_ms, e0, e2), "elapsed(total)");
                paired_yolo.push_back(yolo_ms);
                paired_classifier.push_back(classifier_ms);
                paired_total.push_back(total_ms);
            }

            std::puts("\nC. YOLO -> classifier on one CUDA stream (GPU compute only)");
            printSummary("YOLO alone", yolo_alone);
            printSummary("paired YOLO", paired_yolo);
            printSummary("paired classifier", paired_classifier);
            printSummary("paired total", paired_total);
        }

        std::puts("\nA baseline: Arty PL=6.62 ms/ROI; PS+PL=6.88 ms/ROI; RTT=8.24 ms/ROI");
        std::puts("Use classifier_features.engine for a PL-core endpoint comparison.");
        std::puts("Use classifier_full.engine for an end-to-end classification comparison.");

        cudaEventDestroy(e2);
        cudaEventDestroy(e1);
        cudaEventDestroy(e0);
        cudaStreamDestroy(stream);
        delete yolo;
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "classifier_gpu_benchmark: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
