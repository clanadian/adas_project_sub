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
