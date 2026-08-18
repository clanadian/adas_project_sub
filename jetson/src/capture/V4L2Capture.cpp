#include "capture/V4L2Capture.hpp"

#include <linux/videodev2.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>

namespace {

//ioctl은 시그널에 깨질 수 있다. EINTR은 오류가 아니라 "다시 하라"는 뜻이다.
int xioctl(int fd, unsigned long request, void* arg) {
    int result = 0;
    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

void logErrno(const char* what) {
    std::cerr << "V4L2Capture: " << what << " 실패: " << std::strerror(errno) << "\n";
}

void logError(const std::string& what) {
    std::cerr << "V4L2Capture: " << what << "\n";
}

}  // namespace

std::string V4L2Capture::Format::pixelFormatName() const {
    if (pixelformat == 0) {
        return "(none)";
    }
    std::string name(4, ' ');
    for (int i = 0; i < 4; ++i) {
        name[static_cast<size_t>(i)] =
            static_cast<char>((pixelformat >> (8 * i)) & 0xFF);
    }
    return name;
}

double V4L2Capture::Format::fps() const {
    if (fps_numerator == 0) {
        return 0.0;
    }
    return static_cast<double>(fps_denominator) / static_cast<double>(fps_numerator);
}

std::string V4L2Capture::Format::describe() const {
    std::ostringstream out;
    out << pixelFormatName() << " " << width << "x" << height
        << " @" << fps() << "fps"
        << "  bytesperline=" << bytesperline
        << " sizeimage=" << sizeimage;
    return out.str();
}

V4L2Capture::~V4L2Capture() {
    close();
}

bool V4L2Capture::init(const std::string& device) {
    return init(device, Request{});
}

bool V4L2Capture::init(const std::string& device, const Request& request) {
    //재초기화를 허용한다. 이전 상태가 남아 있으면 먼저 정리한다.
    close();

    //O_NONBLOCK으로 열어야 DQBUF가 프레임을 기다리며 멈추지 않는다.
    //기다리는 일은 poll이 맡고, 그래야 종료 신호로 깨울 수 있다.
    fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        logErrno(("open(" + device + ")").c_str());
        return false;
    }

    stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd_ < 0) {
        logErrno("eventfd");
        close();
        return false;
    }

    //아래 단계는 어디서 실패하든 close()가 지금까지 잡은 자원을 되돌린다
    if (!queryCapability(device) ||
        !negotiateFormat(request) ||
        !negotiateFrameRate(request.fps) ||
        !setupBuffers(request.buffer_count) ||
        !startStreaming()) {
        close();
        return false;
    }

    std::cout << "V4L2Capture: " << device << "  " << format_.describe() << "\n";

    //요청과 다르게 협상됐으면 알린다. 오류는 아니지만 화면비가 달라질 수 있다.
    if (format_.width != request.width || format_.height != request.height) {
        std::cout << "V4L2Capture: 요청 " << request.width << "x" << request.height
                  << "과 다른 해상도로 협상됨. 협상 결과를 사용한다\n";
    }
    return true;
}

bool V4L2Capture::queryCapability(const std::string& device) {
    struct v4l2_capability cap = {};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        logErrno("VIDIOC_QUERYCAP");
        return false;
    }

    //device_caps가 있으면 그쪽이 이 노드의 실제 능력이다.
    //요즘 uvcvideo는 메타데이터 노드도 함께 만들기 때문에 구분이 필요하다.
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                              ? cap.device_caps
                              : cap.capabilities;

    if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
        logError(device + "는 video capture 장치가 아니다");
        return false;
    }
    if (!(caps & V4L2_CAP_STREAMING)) {
        logError(device + "는 streaming(mmap)을 지원하지 않는다");
        return false;
    }
    return true;
}

bool V4L2Capture::negotiateFormat(const Request& request) {
    struct v4l2_format fmt = {};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = request.width;
    fmt.fmt.pix.height      = request.height;
    fmt.fmt.pix.pixelformat = request.pixelformat;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    //S_FMT는 구조체를 협상 결과로 덮어써서 돌려준다. 요청값이 아니라
    //이 값을 저장해야 한다.
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        logErrno("VIDIOC_S_FMT");
        return false;
    }

    format_.width        = fmt.fmt.pix.width;
    format_.height       = fmt.fmt.pix.height;
    format_.pixelformat  = fmt.fmt.pix.pixelformat;
    format_.bytesperline = fmt.fmt.pix.bytesperline;
    format_.sizeimage    = fmt.fmt.pix.sizeimage;

    if (format_.width == 0 || format_.height == 0) {
        logError("협상된 해상도가 0이다");
        return false;
    }

    //YUYV 외의 포맷은 아직 변환 경로가 없다. 조용히 잘못 해석하느니 멈춘다.
    //MJPEG 입력은 P1-7에서 다룬다.
    if (format_.pixelformat != kPixelFormatYUYV) {
        Format requested;
        requested.pixelformat = request.pixelformat;
        logError(requested.pixelFormatName() + "를 요청했으나 " +
                 format_.pixelFormatName() +
                 "로 협상됐다. 이 포맷은 아직 변환하지 못한다");
        return false;
    }

    //드라이버가 bytesperline을 채우지 않는 경우가 있어 최소값으로 메운다
    const unsigned minimum_stride = format_.width * 2;  //YUYV는 픽셀당 2바이트
    if (format_.bytesperline < minimum_stride) {
        format_.bytesperline = minimum_stride;
    }
    return true;
}

bool V4L2Capture::negotiateFrameRate(unsigned fps) {
    if (fps == 0) {
        return true;
    }

    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    //먼저 현재 값을 읽어 프레임레이트 설정을 지원하는지 본다
    if (xioctl(fd_, VIDIOC_G_PARM, &parm) < 0) {
        logErrno("VIDIOC_G_PARM");
        return false;
    }

    if (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            //프레임레이트는 못 맞춰도 캡처 자체는 된다. 경고만 남긴다.
            logErrno("VIDIOC_S_PARM(경고, 캡처는 계속한다)");
        }
        //설정을 받아줬어도 값이 조정될 수 있으므로 다시 읽는다
        if (xioctl(fd_, VIDIOC_G_PARM, &parm) < 0) {
            logErrno("VIDIOC_G_PARM(재확인)");
            return false;
        }
    } else {
        std::cout << "V4L2Capture: 이 장치는 프레임레이트 설정을 지원하지 않는다\n";
    }

    format_.fps_numerator   = parm.parm.capture.timeperframe.numerator;
    format_.fps_denominator = parm.parm.capture.timeperframe.denominator;
    return true;
}

bool V4L2Capture::setupBuffers(unsigned count) {
    struct v4l2_requestbuffers req = {};
    req.count  = count;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        logErrno("VIDIOC_REQBUFS");
        return false;
    }
    if (req.count == 0) {
        logError("드라이버가 버퍼를 하나도 주지 않았다");
        return false;
    }
    if (req.count < count) {
        std::cout << "V4L2Capture: 버퍼 " << count << "개를 요청했으나 "
                  << req.count << "개를 받았다\n";
    }

    const size_t needed = static_cast<size_t>(format_.bytesperline) * format_.height;

    for (unsigned i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            logErrno("VIDIOC_QUERYBUF");
            return false;
        }

        //협상 결과보다 작은 버퍼를 프레임으로 읽으면 버퍼 밖을 건드린다
        if (buf.length < needed) {
            logError("버퍼가 협상된 프레임보다 작다");
            return false;
        }

        void* ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd_, buf.m.offset);
        if (ptr == MAP_FAILED) {
            logErrno("mmap");
            return false;
        }

        //먼저 기록해야 뒤에서 실패해도 close()가 이 매핑을 되돌린다
        buffers_.push_back(ptr);
        buffer_lengths_.push_back(buf.length);

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            logErrno("VIDIOC_QBUF(최초 등록)");
            return false;
        }
    }
    return true;
}

bool V4L2Capture::startStreaming() {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        logErrno("VIDIOC_STREAMON");
        return false;
    }
    streaming_ = true;
    return true;
}

V4L2Capture::Result V4L2Capture::waitForEvent(int timeout_ms) {
    struct pollfd fds[2] = {};
    fds[0].fd     = fd_;
    fds[0].events = POLLIN;
    fds[1].fd     = stop_fd_;
    fds[1].events = POLLIN;

    for (;;) {
        const int ready = poll(fds, 2, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;  //시그널로 깨어난 것뿐이다
            }
            logErrno("poll");
            return Result::Error;
        }
        if (ready == 0) {
            return Result::Timeout;
        }

        //종료 신호를 프레임보다 먼저 본다. eventfd는 읽지 않고 두어
        //한 번 요청하면 이후 호출도 계속 Stopped를 돌려주게 한다.
        if (fds[1].revents & POLLIN) {
            return Result::Stopped;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            logError("장치가 사라졌거나 오류 상태다");
            return Result::Error;
        }
        if (fds[0].revents & POLLIN) {
            return Result::Ok;
        }
        return Result::Timeout;
    }
}

V4L2Capture::Result V4L2Capture::captureFrame(cv::Mat& out_bgr, int timeout_ms) {
    if (fd_ < 0 || !streaming_) {
        logError("초기화되지 않은 상태로 captureFrame이 호출됐다");
        return Result::Error;
    }

    const Result waited = waitForEvent(timeout_ms);
    if (waited != Result::Ok) {
        return waited;
    }

    struct v4l2_buffer buf = {};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        //poll이 깨워도 실제로는 아직 준비가 안 됐을 수 있다. 오류가 아니다.
        if (errno == EAGAIN) {
            return Result::Timeout;
        }
        logErrno("VIDIOC_DQBUF");
        return Result::Error;
    }

    if (buf.index >= buffers_.size()) {
        logError("드라이버가 범위를 벗어난 버퍼 인덱스를 돌려줬다");
        return Result::Error;
    }

    //해상도를 박아두지 않고 협상 결과로 해석한다.
    //bytesperline을 step으로 넘겨야 행 끝에 패딩이 있어도 어긋나지 않는다.
    const cv::Mat yuyv(static_cast<int>(format_.height),
                       static_cast<int>(format_.width),
                       CV_8UC2,
                       buffers_[buf.index],
                       format_.bytesperline);
    cv::cvtColor(yuyv, out_bgr, cv::COLOR_YUV2BGR_YUYV);

    //변환이 끝났으면 버퍼를 곧바로 돌려줘야 드라이버가 다시 채울 수 있다
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        logErrno("VIDIOC_QBUF(반납)");
        return Result::Error;
    }
    return Result::Ok;
}

void V4L2Capture::requestStop() {
    if (stop_fd_ < 0) {
        return;
    }
    const uint64_t one = 1;
    //실패해도 할 수 있는 게 없다. 종료 경로에서 예외를 던지지 않는다.
    if (write(stop_fd_, &one, sizeof(one)) < 0) {
        logErrno("requestStop write");
    }
}

void V4L2Capture::close() {
    if (streaming_ && fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        //종료 경로라 실패해도 되돌릴 방법이 없다
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    streaming_ = false;

    for (size_t i = 0; i < buffers_.size(); ++i) {
        munmap(buffers_[i], buffer_lengths_[i]);
    }
    buffers_.clear();
    buffer_lengths_.clear();

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (stop_fd_ >= 0) {
        ::close(stop_fd_);
        stop_fd_ = -1;
    }
    format_ = Format{};
}
