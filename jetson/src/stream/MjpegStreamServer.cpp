#include "stream/MjpegStreamServer.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace adas::stream {

namespace {

// manifest.json / PROVENANCE.md 기준 6-class 고정 순서.
constexpr std::array<const char*, 6> kClassNames = {
    "background", "car", "person",
    "sign_warning", "sign_prohibition", "sign_mandatory"
};

std::string classLabel(std::uint32_t class_id) {
    if (class_id == std::numeric_limits<std::uint32_t>::max()) {
        return "invalid";
    }
    if (class_id < kClassNames.size()) {
        return kClassNames[class_id];
    }
    return "class" + std::to_string(class_id);
}

// data/len 전체를 보낼 때까지 반복한다. 소켓에는 이미 SO_SNDTIMEO가
// 짧게 걸려 있으므로, 느리거나 멈춘 클라이언트에서는 send()가
// 무한정 블로킹하는 대신 타임아웃으로 실패한다 - 그러면 이 프레임은
// 포기하고 false를 돌려준다. 절대 재시도하며 더 기다리지 않는다.
bool writeAll(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // EAGAIN/EWOULDBLOCK(SO_SNDTIMEO 만료) 또는 그 외 소켓 오류.
        return false;
    }
    return true;
}

void drawOverlays(cv::Mat& canvas, const std::vector<RoiOverlay>& overlays) {
    for (const auto& ov : overlays) {
        cv::Rect box(static_cast<int>(ov.bbox.x), static_cast<int>(ov.bbox.y),
                     static_cast<int>(ov.bbox.width), static_cast<int>(ov.bbox.height));
        box &= cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (box.width <= 0 || box.height <= 0) {
            continue;
        }

        const bool ok = (ov.status == 0);
        const cv::Scalar color = ok ? cv::Scalar(0, 200, 0) : cv::Scalar(0, 0, 220);  // BGR
        cv::rectangle(canvas, box, color, 2);

        const std::string label = ok
            ? classLabel(ov.class_id) + " " + std::to_string(ov.confidence_ppm / 10000) + "%"
            : "ERR" + std::to_string(ov.status);

        int baseline = 0;
        const cv::Size text_size =
            cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        const cv::Point text_org(box.x, std::max(box.y - 4, text_size.height));
        cv::rectangle(canvas,
                      text_org + cv::Point(0, baseline + 2),
                      text_org + cv::Point(text_size.width, -text_size.height),
                      color, cv::FILLED);
        cv::putText(canvas, label, text_org, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }
}

}  // namespace

MjpegStreamServer::MjpegStreamServer(MjpegServerConfig config) : config_(config) {}

MjpegStreamServer::~MjpegStreamServer() {
    stop();
}

bool MjpegStreamServer::start() {
    if (running_.load()) {
        return true;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }

    const int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 8) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    encoder_thread_ = std::thread(&MjpegStreamServer::encoderLoop, this);
    accept_thread_ = std::thread(&MjpegStreamServer::acceptLoop, this);
    return true;
}

void MjpegStreamServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    frame_cv_.notify_all();
    jpeg_cv_.notify_all();

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    std::vector<std::thread> threads_to_join;
    {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        threads_to_join.swap(client_threads_);
    }
    for (auto& t : threads_to_join) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void MjpegStreamServer::publish(const cv::Mat& bgr_frame, std::vector<RoiOverlay> overlays) {
    if (!running_.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        latest_frame_ = bgr_frame.clone();
        latest_overlays_ = std::move(overlays);
        ++frame_version_;
        has_frame_ = true;
    }
    frame_cv_.notify_all();
}

std::size_t MjpegStreamServer::clientCount() const noexcept {
    return client_count_.load();
}

void MjpegStreamServer::encoderLoop() {
    std::uint64_t last_encoded_version = 0;
    const std::vector<int> jpeg_params{cv::IMWRITE_JPEG_QUALITY, config_.jpeg_quality};

    while (running_.load()) {
        cv::Mat frame;
        std::vector<RoiOverlay> overlays;
        {
            std::unique_lock<std::mutex> lock(frame_mutex_);
            frame_cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                [this, last_encoded_version] {
                    return !running_.load() ||
                           (has_frame_ && frame_version_ != last_encoded_version);
                });
            if (!running_.load()) {
                break;
            }
            if (!has_frame_ || frame_version_ == last_encoded_version) {
                continue;
            }
            last_encoded_version = frame_version_;
            frame = latest_frame_.clone();
            overlays = latest_overlays_;
        }

        drawOverlays(frame, overlays);

        std::vector<std::uint8_t> jpeg_buffer;
        if (!cv::imencode(".jpg", frame, jpeg_buffer, jpeg_params)) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(jpeg_mutex_);
            latest_jpeg_ = std::move(jpeg_buffer);
            ++jpeg_version_;
        }
        jpeg_cv_.notify_all();
    }
}

void MjpegStreamServer::acceptLoop() {
    while (running_.load()) {
        pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, config_.poll_interval_ms);
        if (ready <= 0) {
            continue;
        }

        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        const int client_fd =
            ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            continue;
        }

        timeval timeout{};
        timeout.tv_sec = config_.client_send_timeout_ms / 1000;
        timeout.tv_usec = (config_.client_send_timeout_ms % 1000) * 1000;
        ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        const int one = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        client_threads_.emplace_back(&MjpegStreamServer::clientLoop, this, client_fd);
    }
}

void MjpegStreamServer::clientLoop(int client_fd) {
    ++client_count_;

    // 요청 라인은 파싱하지 않는다 - MJPEG 뷰어는 아무 GET이나 보내고
    // 곧바로 응답 스트림을 읽기 시작한다.
    char discard[512];
    ::recv(client_fd, discard, sizeof(discard), 0);

    static const char kHeader[] =
        "HTTP/1.0 200 OK\r\n"
        "Server: adas-jetson-mjpeg\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, private\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=adasframe\r\n"
        "\r\n";

    if (!writeAll(client_fd, kHeader, sizeof(kHeader) - 1)) {
        ::close(client_fd);
        --client_count_;
        return;
    }

    std::uint64_t last_sent_version = 0;
    while (running_.load()) {
        std::vector<std::uint8_t> jpeg_copy;
        {
            std::unique_lock<std::mutex> lock(jpeg_mutex_);
            jpeg_cv_.wait_for(lock, std::chrono::milliseconds(config_.poll_interval_ms),
                [this, last_sent_version] {
                    return !running_.load() || jpeg_version_ != last_sent_version;
                });
            if (!running_.load()) {
                break;
            }
            if (jpeg_version_ == last_sent_version || latest_jpeg_.empty()) {
                continue;
            }
            last_sent_version = jpeg_version_;
            jpeg_copy = latest_jpeg_;
        }

        char part_header[128];
        const int header_len = std::snprintf(part_header, sizeof(part_header),
            "--adasframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
            jpeg_copy.size());

        const bool sent_ok =
            header_len > 0 &&
            writeAll(client_fd, part_header, static_cast<std::size_t>(header_len)) &&
            writeAll(client_fd, reinterpret_cast<const char*>(jpeg_copy.data()), jpeg_copy.size()) &&
            writeAll(client_fd, "\r\n", 2);

        if (!sent_ok) {
            // 느리거나 끊긴 클라이언트. 이 소켓만 정리하고 끝낸다 -
            // 프로듀서(캡처/분류 루프)는 이 실패를 알 필요도, 기다릴
            // 필요도 없다.
            break;
        }
    }

    ::close(client_fd);
    --client_count_;
}

}  // namespace adas::stream
