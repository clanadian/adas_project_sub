#include "network/TcpRoiClient.hpp"
#include "network/tcp_roi_server.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sys/socket.h>
#include <thread>

namespace {

std::uint8_t testPixel(std::size_t index) {
    return static_cast<std::uint8_t>((index * 29u + 13u) & 0xffu);
}

void runServer(adas_tcp_roi_server_t* server) {
    assert(adas_tcp_roi_server_accept(server) == ADAS_TCP_ROI_OK);

    adas_roi_header_t request_header{};
    adas_roi_bbox_t request_bbox{};
    std::uint8_t image_payload[ADAS_ROI_IMAGE_PAYLOAD_SIZE]{};

    assert(adas_tcp_roi_server_receive_request(
        server,
        &request_header,
        &request_bbox,
        image_payload
    ) == ADAS_TCP_ROI_OK);

    assert(request_header.frame_id == 42u);
    assert(request_header.roi_id == 9u);
    for (std::size_t i = 0; i < ADAS_ROI_IMAGE_PAYLOAD_SIZE; ++i) {
        assert(image_payload[i] == testPixel(i));
    }

    const adas_roi_result_t response = {
        ADAS_ROI_STATUS_OK,
        3u,
        900000u
    };

    assert(adas_tcp_roi_server_send_result(
        server,
        &request_header,
        &response
    ) == ADAS_TCP_ROI_OK);

    adas_tcp_roi_server_disconnect(server);
}

void testClientServerRoundTrip() {
    adas_tcp_roi_server_t server;
    adas_tcp_roi_server_init(&server);

    const adas_tcp_roi_server_config_t config = {
        "127.0.0.1",
        0u,
        1
    };
    assert(adas_tcp_roi_server_listen(&server, &config) == ADAS_TCP_ROI_OK);

    sockaddr_in bound_address{};
    socklen_t bound_address_size = sizeof(bound_address);
    assert(::getsockname(
        server.listen_fd,
        reinterpret_cast<sockaddr*>(&bound_address),
        &bound_address_size
    ) == 0);

    const std::uint16_t server_port = ntohs(bound_address.sin_port);
    assert(server_port != 0u);

    std::thread server_thread(runServer, &server);

    adas::network::TcpRoiClient client;
    // 연결 전에는 적용할 socket이 없다.
    assert(client.setNoDelay(true)
        == adas::network::TcpClientStatus::NotConnected);
    assert(client.connectToServer("127.0.0.1", server_port)
        == adas::network::TcpClientStatus::Ok);
    assert(client.isConnected());
    // 연결 후에는 양방향으로 토글된다. socket에 실제로 반영되는지는
    // arty/ps_db/tests/test_tcp_roi_server.c 가 getsockopt로 확인한다.
    assert(client.setNoDelay(true) == adas::network::TcpClientStatus::Ok);
    assert(client.setNoDelay(false) == adas::network::TcpClientStatus::Ok);

    adas::preprocess::PreparedRoi roi;
    roi.frame_id = 42u;
    roi.roi_id = 9u;
    roi.rgb_pixels = cv::Mat(
        static_cast<int>(ADAS_ROI_HEIGHT),
        static_cast<int>(ADAS_ROI_WIDTH),
        CV_8UC3
    );

    assert(roi.rgb_pixels.isContinuous());
    for (std::size_t i = 0; i < ADAS_ROI_IMAGE_PAYLOAD_SIZE; ++i) {
        roi.rgb_pixels.data[i] = testPixel(i);
    }

    adas::network::ClassificationResult result;
    assert(client.classify(roi, result)
        == adas::network::TcpClientStatus::Ok);

    assert(result.frame_id == roi.frame_id);
    assert(result.roi_id == roi.roi_id);
    assert(result.status == ADAS_ROI_STATUS_OK);
    assert(result.class_id == 3u);
    assert(result.confidence_ppm == 900000u);

    client.disconnect();
    assert(!client.isConnected());

    server_thread.join();
    adas_tcp_roi_server_close(&server);
}

}  // namespace

int main() {
    testClientServerRoundTrip();
    std::cout << "TCP ROI client/server round-trip test passed\n";
    return EXIT_SUCCESS;
}
