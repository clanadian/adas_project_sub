#include "network/tcp_roi_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int send_exact(int socket_fd, const uint8_t* data, size_t size) {
    size_t sent = 0;

    while (sent < size) {
        const ssize_t result = send(
            socket_fd,
            data + sent,
            size - sent,
            MSG_NOSIGNAL
        );

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }

        sent += (size_t)result;
    }

    return 0;
}

static int receive_exact(int socket_fd, uint8_t* data, size_t size) {
    size_t received = 0;

    while (received < size) {
        const ssize_t result = recv(
            socket_fd,
            data + received,
            size - received,
            0
        );

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }

        received += (size_t)result;
    }

    return 0;
}

static uint8_t test_pixel(size_t index) {
    return (uint8_t)((index * 31u + 7u) & 0xffu);
}

static int run_client(uint16_t server_port) {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return 1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(server_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(
            socket_fd,
            (const struct sockaddr*)&address,
            sizeof(address)
        ) < 0) {
        close(socket_fd);
        return 2;
    }

    const adas_roi_header_t request_header = {
        .magic = ADAS_ROI_MAGIC,
        .version = ADAS_ROI_VERSION,
        .message_type = ADAS_ROI_MESSAGE_REQUEST,
        .frame_id = 1234u,
        .roi_id = 7u,
        .payload_size = ADAS_ROI_REQUEST_PAYLOAD_SIZE
    };

    uint8_t header_bytes[ADAS_ROI_HEADER_SIZE];
    adas_roi_encode_header(header_bytes, &request_header);

    /* TCP fragmentation을 흉내 내기 위해 header를 세 조각으로 보낸다. */
    if (send_exact(socket_fd, header_bytes, 3u) != 0
        || send_exact(socket_fd, header_bytes + 3u, 7u) != 0
        || send_exact(
            socket_fd,
            header_bytes + 10u,
            ADAS_ROI_HEADER_SIZE - 10u
        ) != 0) {
        close(socket_fd);
        return 3;
    }

    const adas_roi_bbox_t request_bbox = {
        .x = 100.0F,
        .y = 50.0F,
        .width = 40.0F,
        .height = 80.0F,
        .objectness = 0.9F,
        .frame_width = 640u,
        .frame_height = 360u
    };
    uint8_t bbox_bytes[ADAS_ROI_BBOX_PAYLOAD_SIZE];
    adas_roi_encode_bbox(bbox_bytes, &request_bbox);
    if (send_exact(socket_fd, bbox_bytes, sizeof(bbox_bytes)) != 0) {
        close(socket_fd);
        return 3;
    }

    uint8_t image_payload[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
    for (size_t i = 0; i < sizeof(image_payload); ++i) {
        image_payload[i] = test_pixel(i);
    }

    /* ROI도 113-byte 조각으로 나눠 전송한다. */
    size_t offset = 0;
    while (offset < sizeof(image_payload)) {
        size_t chunk_size = 113u;
        if (chunk_size > sizeof(image_payload) - offset) {
            chunk_size = sizeof(image_payload) - offset;
        }

        if (send_exact(
                socket_fd,
                image_payload + offset,
                chunk_size
            ) != 0) {
            close(socket_fd);
            return 4;
        }

        offset += chunk_size;
    }

    uint8_t response_header_bytes[ADAS_ROI_HEADER_SIZE];
    uint8_t result_bytes[ADAS_ROI_RESULT_PAYLOAD_SIZE];

    if (receive_exact(
            socket_fd,
            response_header_bytes,
            sizeof(response_header_bytes)
        ) != 0
        || receive_exact(socket_fd, result_bytes, sizeof(result_bytes)) != 0) {
        close(socket_fd);
        return 5;
    }

    adas_roi_header_t response_header;
    adas_roi_result_t result;
    adas_roi_decode_header(response_header_bytes, &response_header);
    adas_roi_decode_result(result_bytes, &result);

    const int response_is_valid =
        adas_roi_is_valid_response_header(&response_header)
        && response_header.frame_id == request_header.frame_id
        && response_header.roi_id == request_header.roi_id
        && result.status == ADAS_ROI_STATUS_OK
        && result.class_id == 2u
        && result.confidence_ppm == 875000u;

    close(socket_fd);
    return response_is_valid ? 0 : 6;
}

static void test_fragmented_request_and_response(void) {
    adas_tcp_roi_server_t server;
    adas_tcp_roi_server_init(&server);

    const adas_tcp_roi_server_config_t config = {
        .bind_address = "127.0.0.1",
        .port = 0u,
        .backlog = 1
    };

    assert(adas_tcp_roi_server_listen(&server, &config) == ADAS_TCP_ROI_OK);

    struct sockaddr_in bound_address;
    socklen_t bound_address_size = sizeof(bound_address);
    memset(&bound_address, 0, sizeof(bound_address));
    assert(getsockname(
        server.listen_fd,
        (struct sockaddr*)&bound_address,
        &bound_address_size
    ) == 0);

    const uint16_t server_port = ntohs(bound_address.sin_port);
    assert(server_port != 0u);

    const pid_t child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        close(server.listen_fd);
        _exit(run_client(server_port));
    }

    assert(adas_tcp_roi_server_accept(&server) == ADAS_TCP_ROI_OK);

    adas_roi_header_t request_header;
    adas_roi_bbox_t received_bbox;
    uint8_t received_image[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
    assert(adas_tcp_roi_server_receive_request(
        &server,
        &request_header,
        &received_bbox,
        received_image
    ) == ADAS_TCP_ROI_OK);

    assert(request_header.frame_id == 1234u);
    assert(request_header.roi_id == 7u);
    assert(received_bbox.x == 100.0F);
    assert(received_bbox.frame_width == 640u);
    for (size_t i = 0; i < sizeof(received_image); ++i) {
        assert(received_image[i] == test_pixel(i));
    }

    const adas_roi_result_t result = {
        .status = ADAS_ROI_STATUS_OK,
        .class_id = 2u,
        .confidence_ppm = 875000u
    };
    assert(adas_tcp_roi_server_send_result(
        &server,
        &request_header,
        &result
    ) == ADAS_TCP_ROI_OK);

    adas_tcp_roi_server_disconnect(&server);
    adas_tcp_roi_server_close(&server);

    int child_status = 0;
    assert(waitpid(child_pid, &child_status, 0) == child_pid);
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);
}

static void test_peer_closed_during_receive(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    adas_tcp_roi_server_t server;
    adas_tcp_roi_server_init(&server);
    server.client_fd = sockets[0];

    close(sockets[1]);

    adas_roi_header_t request_header;
    adas_roi_bbox_t bbox;
    uint8_t image_payload[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
    assert(adas_tcp_roi_server_receive_request(
        &server,
        &request_header,
        &bbox,
        image_payload
    ) == ADAS_TCP_ROI_PEER_CLOSED);

    adas_tcp_roi_server_close(&server);
}

/*
 * TCP_NODELAY 토글. 응답이 header 20 B + result 12 B로 두 번 나뉘어 나가므로
 * Nagle이 켜져 있으면 두 번째 조각이 상대의 delayed ACK를 기다린다. 그 스위치가
 * 실제로 socket까지 도달하는지 getsockopt로 되읽어 확인한다.
 */
static void test_no_delay_toggle(void) {
    adas_tcp_roi_server_t server;
    adas_tcp_roi_server_init(&server);

    assert(adas_tcp_roi_server_set_no_delay(NULL, 1)
           == ADAS_TCP_ROI_INVALID_ARGUMENT);
    /* 연결 전에는 적용할 client socket이 없다. */
    assert(adas_tcp_roi_server_set_no_delay(&server, 1)
           == ADAS_TCP_ROI_INVALID_ARGUMENT);

    const adas_tcp_roi_server_config_t config = {
        .bind_address = "127.0.0.1",
        .port = 0u,
        .backlog = 1
    };
    assert(adas_tcp_roi_server_listen(&server, &config) == ADAS_TCP_ROI_OK);

    struct sockaddr_in bound_address;
    socklen_t bound_address_size = sizeof(bound_address);
    memset(&bound_address, 0, sizeof(bound_address));
    assert(getsockname(
        server.listen_fd,
        (struct sockaddr*)&bound_address,
        &bound_address_size
    ) == 0);

    const int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_fd >= 0);
    assert(connect(
        client_fd,
        (const struct sockaddr*)&bound_address,
        sizeof(bound_address)
    ) == 0);
    assert(adas_tcp_roi_server_accept(&server) == ADAS_TCP_ROI_OK);

    int value = 0;
    socklen_t value_size = sizeof(value);
    assert(adas_tcp_roi_server_set_no_delay(&server, 1) == ADAS_TCP_ROI_OK);
    assert(getsockopt(
        server.client_fd, IPPROTO_TCP, TCP_NODELAY, &value, &value_size
    ) == 0);
    assert(value != 0);

    value = 1;
    value_size = sizeof(value);
    assert(adas_tcp_roi_server_set_no_delay(&server, 0) == ADAS_TCP_ROI_OK);
    assert(getsockopt(
        server.client_fd, IPPROTO_TCP, TCP_NODELAY, &value, &value_size
    ) == 0);
    assert(value == 0);

    close(client_fd);
    adas_tcp_roi_server_disconnect(&server);
    adas_tcp_roi_server_close(&server);
}

int main(void) {
    test_fragmented_request_and_response();
    test_peer_closed_during_receive();
    test_no_delay_toggle();

    puts("TCP ROI server tests passed");
    return EXIT_SUCCESS;
}
