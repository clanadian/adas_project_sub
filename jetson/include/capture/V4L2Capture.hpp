#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

//V4L2 MMAP 기반 캡처.
//
//요청 포맷은 어디까지나 요청이다. 드라이버는 다른 값을 돌려줄 수 있으므로
//프레임 해석은 항상 협상 결과인 format()을 쓴다. 해상도를 코드에 박아두면
//fallback 웹캠처럼 다른 값이 협상됐을 때 버퍼를 잘못 읽는다.
//
//docs/webcam_spec.md 기준:
//  통합 목표  Logitech C920   YUYV 640x360@30. 모델 입력 512x288과 같은 16:9라
//                             crop/stretch/letterbox가 필요 없다
//  fallback   USB2.0 PC CAM   YUYV 640x480. 16:9를 지원하지 않는다
//
//제품명이나 /dev/video0에 의존하지 않는다. 장치 경로는 호출부가 넘긴다.
//V4L2 pixel format은 4글자 코드다. linux/videodev2.h를 공개 헤더로
//끌어들이지 않으려고 직접 만든다. 클래스 밖에 두는 이유는 클래스가 완성되기
//전에는 자기 멤버 함수를 상수식으로 부를 수 없기 때문이다.
namespace v4l2_capture {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return  static_cast<uint32_t>(static_cast<unsigned char>(a))
         | (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8)
         | (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16)
         | (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

constexpr uint32_t kPixelFormatYUYV = fourcc('Y', 'U', 'Y', 'V');

}  // namespace v4l2_capture

class V4L2Capture {
public:
    static constexpr uint32_t kPixelFormatYUYV = v4l2_capture::kPixelFormatYUYV;

    //드라이버에 요청할 값. 기본값은 통합 목표인 C920 설정이다.
    struct Request {
        unsigned width        = 640;
        unsigned height       = 360;
        uint32_t pixelformat  = v4l2_capture::kPixelFormatYUYV;
        unsigned fps          = 30;
        unsigned buffer_count = 4;
    };

    //드라이버가 실제로 협상해준 값. 프레임 해석은 전부 이 값을 쓴다.
    struct Format {
        unsigned width           = 0;
        unsigned height          = 0;
        uint32_t pixelformat     = 0;

        //한 행의 실제 바이트 수. YUYV라도 width*2와 다를 수 있어서
        //cv::Mat의 step으로 그대로 넘겨야 한다.
        unsigned bytesperline    = 0;
        unsigned sizeimage       = 0;

        //V4L2의 timeperframe은 "프레임당 초"라 fps = denominator / numerator다
        unsigned fps_numerator   = 0;
        unsigned fps_denominator = 0;

        std::string pixelFormatName() const;
        double fps() const;
        std::string describe() const;
    };

    enum class Result {
        Ok,        //out_bgr에 프레임이 들어왔다
        Timeout,   //제한 시간 안에 프레임이 오지 않았다. 오류가 아니다
        Stopped,   //requestStop()으로 깨어났다
        Error,     //장치 오류. 호출부가 캡처를 중단해야 한다
    };

    V4L2Capture() = default;
    ~V4L2Capture();

    //버퍼 소유권이 얽히므로 복사를 막는다
    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    //실패하면 이미 잡은 fd와 mmap을 스스로 정리하고 false를 돌려준다.
    //성공 후 협상 결과는 format()으로 확인한다.
    //
    //기본 인자로 Request{}를 쓰지 않는 이유는 중첩 구조체의 기본값을
    //바깥 클래스가 완성되기 전에 쓸 수 없기 때문이다. 오버로드로 나눈다.
    bool init(const std::string& device);
    bool init(const std::string& device, const Request& request);

    //timeout_ms 동안 프레임을 기다린다. 반환값을 반드시 확인한다.
    Result captureFrame(cv::Mat& out_bgr, int timeout_ms = 1000);

    //다른 스레드에서 불러 캡처 대기를 즉시 깨운다.
    //이게 없으면 종료할 때 DQBUF에서 프레임을 기다리며 멈춰 있는다.
    void requestStop();

    //여러 번 불러도 안전하다. 소멸자도 이걸 부른다.
    void close();

    const Format& format() const { return format_; }
    bool isOpen() const { return fd_ >= 0; }

private:
    bool queryCapability(const std::string& device);
    bool negotiateFormat(const Request& request);
    bool negotiateFrameRate(unsigned fps);
    bool setupBuffers(unsigned count);
    bool startStreaming();

    //poll로 프레임이나 종료 신호를 기다린다
    Result waitForEvent(int timeout_ms);

    int  fd_        = -1;
    int  stop_fd_   = -1;      //eventfd. poll을 즉시 깨우는 용도
    bool streaming_ = false;

    Format format_;
    std::vector<void*>  buffers_;
    std::vector<size_t> buffer_lengths_;
};
