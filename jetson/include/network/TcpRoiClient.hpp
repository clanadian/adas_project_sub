#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "preprocess/RoiPreprocessor.hpp"

namespace adas::network {

enum class TcpClientStatus {
    Ok,
    InvalidArgument,
    NotConnected,
    SystemError,
    PeerClosed,
    ProtocolError
};

struct ClassificationResult {
    std::uint32_t frame_id{0};
    std::uint32_t roi_id{0};
    std::uint32_t status{0};
    std::uint32_t class_id{0};
    std::uint32_t confidence_ppm{0};
};

class TcpRoiClient final {
public:
    TcpRoiClient() = default;
    ~TcpRoiClient();

    TcpRoiClient(const TcpRoiClient&) = delete;
    TcpRoiClient& operator=(const TcpRoiClient&) = delete;

    [[nodiscard]]
    TcpClientStatus connectToServer(
        const std::string& server_address,
        std::uint16_t server_port
    );

    /*
     * 연결된 socket의 Nagle 알고리즘을 켜고 끈다. connectToServer 이후에만
     * 호출할 수 있다.
     *
     * 요청은 header 20 B와 픽셀 27,648 B로 나뉘어 두 번 송신되고, 응답도
     * header 20 B와 result 12 B로 나뉘어 온다. Nagle이 켜져 있으면 뒤따르는
     * 작은 조각이 앞 조각의 ACK를 기다리므로, 상대의 delayed ACK만큼
     * (Linux 기본 최대 40 ms) ROI 한 건이 통째로 지연될 수 있다.
     *
     * 기본값은 바꾸지 않는다 - 켠 조건과 끈 조건을 각각 측정해서 효과를
     * 확인한 뒤에 기본값을 정하는 것이 순서다.
     */
    [[nodiscard]]
    TcpClientStatus setNoDelay(bool enable) noexcept;

    void disconnect() noexcept;

    [[nodiscard]]
    bool isConnected() const noexcept;

    [[nodiscard]]
    TcpClientStatus classify(
        const preprocess::PreparedRoi& roi,
        ClassificationResult& result
    );

private:
    [[nodiscard]]
    TcpClientStatus sendAll(const std::uint8_t* data, std::size_t size);

    [[nodiscard]]
    TcpClientStatus receiveAll(std::uint8_t* data, std::size_t size);

    int socket_fd_{-1};
};

}  // namespace adas::network
