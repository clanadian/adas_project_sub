#include "capture/FrameSource.hpp"

FrameSource::FrameSource(V4L2Capture& capture) : capture_(capture) {}

FrameSource::~FrameSource() { stop(); }

void FrameSource::start() {
    running_.store(true);
    thread_ = std::thread(&FrameSource::run, this);
}

void FrameSource::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    capture_.requestStop();
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FrameSource::run() {
    cv::Mat frame;
    while (running_.load()) {
        const V4L2Capture::Result result = capture_.captureFrame(frame, 1000);
        if (result == V4L2Capture::Result::Ok) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // clone() - mmap 버퍼를 감싼 cv::Mat일 수 있어 다음
                // DQBUF가 그 메모리를 덮어쓰기 전에 복사해 둔다.
                latest_frame_ = frame.clone();
                ++seq_;
            }
            cv_.notify_one();
        } else if (result == V4L2Capture::Result::Timeout) {
            continue;
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                capture_error_ = (result == V4L2Capture::Result::Error);
            }
            cv_.notify_all();
            break;
        }
    }
}

FrameSource::Status FrameSource::next(cv::Mat& out_bgr, std::uint64_t& last_seq,
                                       int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool woke_for_new_frame = cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [this, last_seq] { return seq_ != last_seq || !running_.load() || capture_error_; });

    if (capture_error_) {
        return Status::Error;
    }
    if (!running_.load() && seq_ == last_seq) {
        return Status::Stopped;
    }
    if (!woke_for_new_frame) {
        return Status::Timeout;
    }

    out_bgr = latest_frame_;
    last_seq = seq_;
    return Status::Ok;
}
