#include "network/TcpRoiClient.hpp"

#include "roi_protocol.h"

#include <cstddef>
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>

namespace adas::network {

TcpRoiClient::~TcpRoiClient() {
    disconnect();
}

TcpClientStatus TcpRoiClient::connectToServer(
    const std::string& server_address,
    std::uint16_t server_port
) {
    if (server_address.empty() || server_port == 0) {
        return TcpClientStatus::InvalidArgument;
    }

    if (isConnected()) {
        return TcpClientStatus::InvalidArgument;
    }

    const int new_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (new_socket < 0) {
        return TcpClientStatus::SystemError;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server_port);

    const int conversion_result = ::inet_pton(
        AF_INET,
        server_address.c_str(),
        &address.sin_addr
    );

    if (conversion_result != 1) {
        ::close(new_socket);
        return TcpClientStatus::InvalidArgument;
    }

    const int connection_result = ::connect(
        new_socket,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address)
    );

    if (connection_result < 0) {
        ::close(new_socket);
        return TcpClientStatus::SystemError;
    }

    socket_fd_ = new_socket;

    return TcpClientStatus::Ok;
}

TcpClientStatus TcpRoiClient::setNoDelay(bool enable) noexcept {
    if (!isConnected()) {
        return TcpClientStatus::NotConnected;
    }

    const int value = enable ? 1 : 0;
    if (::setsockopt(
            socket_fd_,
            IPPROTO_TCP,
            TCP_NODELAY,
            &value,
            sizeof(value)
    ) < 0) {
        return TcpClientStatus::SystemError;
    }

    return TcpClientStatus::Ok;
}

void TcpRoiClient::armQuickAck() noexcept {
#ifdef TCP_QUICKACK
    if (socket_fd_ < 0) {
        return;
    }

    const int value = 1;
    /* 실패해도 동작에는 문제가 없다 - 느려질 뿐이다. */
    (void)::setsockopt(
        socket_fd_,
        IPPROTO_TCP,
        TCP_QUICKACK,
        &value,
        sizeof(value)
    );
#endif
}

void TcpRoiClient::disconnect() noexcept {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool TcpRoiClient::isConnected() const noexcept {
    return socket_fd_ >= 0;
}

TcpClientStatus TcpRoiClient::classify(
    const preprocess::PreparedRoi& roi,
    ClassificationResult& result
) {
    /* 요청을 보낼 TCP 연결이 먼저 만들어져 있어야 합니다. */
    if (!isConnected()) {
        return TcpClientStatus::NotConnected;
    }

    /*
     * 서버와 약속한 입력 형식을 검사합니다.
     * Jetson 전처리 결과는 96x96, 3채널, 채널당 UINT8인 RGB 영상입니다.
     */
    if (roi.rgb_pixels.empty()
        || roi.rgb_pixels.type() != CV_8UC3
        || roi.rgb_pixels.cols != static_cast<int>(ADAS_ROI_WIDTH)
        || roi.rgb_pixels.rows != static_cast<int>(ADAS_ROI_HEIGHT)) {
        return TcpClientStatus::InvalidArgument;
    }

    /*
     * cv::Mat은 행 사이에 여분 공간(step)이 있을 수 있습니다.
     * TCP에는 픽셀을 빈틈없이 연속 전송해야 하므로 필요할 때 복사합니다.
     */
    cv::Mat continuous_roi;

    if (roi.rgb_pixels.isContinuous()) {
        continuous_roi = roi.rgb_pixels;
    } else {
        continuous_roi = roi.rgb_pixels.clone();
    }

    if (continuous_roi.empty()) {
        return TcpClientStatus::SystemError;
    }

    /*
     * 수신 측이 뒤따르는 바이트의 의미와 길이를 알 수 있도록
     * ROI보다 먼저 고정 길이 요청 헤더를 만듭니다.
     */
    const adas_roi_header_t request_header = {
        ADAS_ROI_MAGIC,
        ADAS_ROI_VERSION,
        ADAS_ROI_MESSAGE_REQUEST,
        roi.frame_id,
        roi.roi_id,
        ADAS_ROI_REQUEST_PAYLOAD_SIZE
    };

    /* C 구조체를 그대로 보내지 않고, 정해진 wire byte 배열로 변환합니다. */
    std::array<std::uint8_t, ADAS_ROI_HEADER_SIZE> request_header_bytes{};

    adas_roi_encode_header(
        request_header_bytes.data(),
        &request_header
    );

    /* 1단계: 요청 헤더 전체를 전송합니다. */
    TcpClientStatus transfer_status = sendAll(
        request_header_bytes.data(),
        request_header_bytes.size()
    );

    if (transfer_status != TcpClientStatus::Ok) {
        return transfer_status;
    }

    /*
     * 2단계: bbox 블록(원본 프레임 좌표 + objectness + 프레임 크기)을
     * 이미지보다 먼저 보냅니다. Arty PS가 안전 판단(zone/거리)을 하려면
     * crop된 이미지만으로는 부족합니다.
     */
    const adas_roi_bbox_t request_bbox = {
        roi.object_bbox.x,
        roi.object_bbox.y,
        roi.object_bbox.width,
        roi.object_bbox.height,
        roi.objectness,
        roi.frame_width,
        roi.frame_height
    };

    std::array<std::uint8_t, ADAS_ROI_BBOX_PAYLOAD_SIZE> request_bbox_bytes{};
    adas_roi_encode_bbox(request_bbox_bytes.data(), &request_bbox);

    transfer_status = sendAll(
        request_bbox_bytes.data(),
        request_bbox_bytes.size()
    );

    if (transfer_status != TcpClientStatus::Ok) {
        return transfer_status;
    }

    /* 3단계: 96x96x3 RGB 픽셀 payload 전체를 전송합니다. */
    transfer_status = sendAll(
        continuous_roi.ptr<std::uint8_t>(0),
        ADAS_ROI_IMAGE_PAYLOAD_SIZE
    );

    if (transfer_status != TcpClientStatus::Ok) {
        return transfer_status;
    }

    /*
     * PS는 응답을 헤더(20B) + 본문(12B) 두 번에 나눠 write 한다. 서버 쪽
     * Nagle이 두 번째 write를 "첫 20B의 ACK이 올 때까지" 붙들기 때문에,
     * 이쪽 delayed-ACK 타이머(~40ms)가 그대로 왕복 시간에 얹힌다.
     *
     * 2026-08-20 tcpdump 실측: 왕복 51.6ms 중 마지막 요청 바이트에서
     * 응답 헤더까지가 8ms(가속기 6.6ms 포함)뿐이고, 헤더에서 본문까지가
     * 40ms였다. 본문은 이쪽 ACK이 도착한 지 0.4ms 만에 나왔다.
     *
     * 근본 수정은 서버가 헤더+본문을 한 번에 write 하는 것이다. 그건
     * Arty PS 쪽 변경이라, 여기서는 받는 쪽이 즉시 ACK을 보내 대기를
     * 없앤다. Linux는 quickack을 몇 번 쓰면 스스로 되돌리므로 요청마다
     * 다시 켠다.
     */
    armQuickAck();

    /* 4단계: PS가 돌려주는 응답 헤더를 정확히 한 개 받습니다. */
    std::array<std::uint8_t, ADAS_ROI_HEADER_SIZE> response_header_bytes{};

    transfer_status = receiveAll(
        response_header_bytes.data(),
        response_header_bytes.size()
    );

    if (transfer_status != TcpClientStatus::Ok) {
        return transfer_status;
    }

    /* 네트워크 byte 배열을 CPU에서 읽을 수 있는 필드로 복호화합니다. */
    adas_roi_header_t response_header{};
    adas_roi_decode_header(
        response_header_bytes.data(),
        &response_header
    );

    /*
     * magic/version/type/길이와 요청 식별자를 확인합니다.
     * 식별자가 다르면 다른 ROI의 결과가 섞인 것이므로 연결을 폐기합니다.
     */
    if (!adas_roi_is_valid_response_header(&response_header)
        || response_header.frame_id != roi.frame_id
        || response_header.roi_id != roi.roi_id) {
        disconnect();
        return TcpClientStatus::ProtocolError;
    }

    /* 5단계: 응답 헤더 뒤의 분류 결과 payload를 전부 받습니다. */
    std::array<std::uint8_t, ADAS_ROI_RESULT_PAYLOAD_SIZE> result_bytes{};

    transfer_status = receiveAll(
        result_bytes.data(),
        result_bytes.size()
    );

    if (transfer_status != TcpClientStatus::Ok) {
        return transfer_status;
    }

    /* 결과 payload도 정수 필드 구조체로 복호화합니다. */
    adas_roi_result_t wire_result{};
    adas_roi_decode_result(
        result_bytes.data(),
        &wire_result
    );

    /* 정의되지 않은 상태값이나 100%를 넘는 confidence는 프로토콜 오류입니다. */
    if (wire_result.status > ADAS_ROI_STATUS_POSTPROCESS_ERROR
        || wire_result.confidence_ppm > ADAS_ROI_CONFIDENCE_PPM_MAX) {
        disconnect();
        return TcpClientStatus::ProtocolError;
    }

    /* 성공 응답에는 반드시 유효한 class_id가 있어야 합니다. */
    if (wire_result.status == ADAS_ROI_STATUS_OK
        && wire_result.class_id == ADAS_ROI_INVALID_CLASS_ID) {
        disconnect();
        return TcpClientStatus::ProtocolError;
    }

    /* 실패 응답은 class_id=INVALID, confidence=0이라는 계약을 지켜야 합니다. */
    if (wire_result.status != ADAS_ROI_STATUS_OK
        && (wire_result.class_id != ADAS_ROI_INVALID_CLASS_ID
            || wire_result.confidence_ppm != 0u)) {
        disconnect();
        return TcpClientStatus::ProtocolError;
    }

    /* 모든 검증을 통과한 경우에만 호출자에게 결과를 복사합니다. */
    result.frame_id = response_header.frame_id;
    result.roi_id = response_header.roi_id;
    result.status = wire_result.status;
    result.class_id = wire_result.class_id;
    result.confidence_ppm = wire_result.confidence_ppm;

    return TcpClientStatus::Ok;
}

TcpClientStatus TcpRoiClient::sendAll(
    const std::uint8_t* data,
    std::size_t size
) {
    if (!isConnected()) {
        return TcpClientStatus::NotConnected;
    }

    if (data == nullptr && size > 0) {
        return TcpClientStatus::InvalidArgument;
    }

    std::size_t sent_size = 0;

    while (sent_size < size) {
        const ssize_t send_result = ::send(
            socket_fd_,
            data + sent_size,
            size - sent_size,
            MSG_NOSIGNAL
        );

        if (send_result == 0) {
            disconnect();
            return TcpClientStatus::PeerClosed;
        }

        if (send_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            disconnect();
            return TcpClientStatus::SystemError;
        }

        sent_size += static_cast<std::size_t>(send_result);
    }

    return TcpClientStatus::Ok;
}

TcpClientStatus TcpRoiClient::receiveAll(
    std::uint8_t* data,
    std::size_t size
) {
    if (!isConnected()) {
        return TcpClientStatus::NotConnected;
    }

    if (data == nullptr && size > 0) {
        return TcpClientStatus::InvalidArgument;
    }

    std::size_t received_size = 0;

    while (received_size < size) {
        const ssize_t receive_result = ::recv(
            socket_fd_,
            data + received_size,
            size - received_size,
            0
        );

        if (receive_result == 0) {
            disconnect();
            return TcpClientStatus::PeerClosed;
        }

        if (receive_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            disconnect();
            return TcpClientStatus::SystemError;
        }

        received_size += static_cast<std::size_t>(receive_result);
    }

    return TcpClientStatus::Ok;
}

}  // namespace adas::network
