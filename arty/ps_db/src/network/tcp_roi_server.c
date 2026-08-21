#include "network/tcp_roi_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static adas_tcp_roi_status_t receive_all(
    int socket_fd,
    uint8_t* buffer,
    size_t size
) {
    size_t received_size = 0;

    while (received_size < size) {
        ssize_t result = recv(
            socket_fd,
            buffer + received_size,
            size - received_size,
            0
        );

        if (result == 0) {
            return ADAS_TCP_ROI_PEER_CLOSED;
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return ADAS_TCP_ROI_SYSTEM_ERROR;
        }

        received_size += (size_t)result;
    }

    return ADAS_TCP_ROI_OK;
}

static adas_tcp_roi_status_t send_all(
    int socket_fd,
    const uint8_t* buffer,
    size_t size
) {
    size_t sent_size = 0;

    while (sent_size < size) {
        ssize_t result = send(
            socket_fd,
            buffer + sent_size,
            size - sent_size,
            MSG_NOSIGNAL
        );

        if (result == 0) {
            return ADAS_TCP_ROI_PEER_CLOSED;
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return ADAS_TCP_ROI_SYSTEM_ERROR;
        }

        sent_size += (size_t)result;
    }

    return ADAS_TCP_ROI_OK;
}

void adas_tcp_roi_server_init(adas_tcp_roi_server_t* server) {
    if (server == NULL){
        return;
    }

    server->listen_fd = -1;
    server->client_fd = -1;
}

adas_tcp_roi_status_t adas_tcp_roi_server_listen(
    adas_tcp_roi_server_t* server,
    const adas_tcp_roi_server_config_t* config
) {
    if (server == NULL || config == NULL || config->backlog <= 0){
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    if (server->listen_fd != -1){
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return ADAS_TCP_ROI_SYSTEM_ERROR;
    }

    int reuse_address = 1;
    if (setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)
    ) < 0){
        close(socket_fd);
        return ADAS_TCP_ROI_SYSTEM_ERROR;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(config->port);

    if (config->bind_address == NULL){
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        int conversion_result = inet_pton(
            AF_INET,
            config->bind_address,
            &address.sin_addr
        );

        if (conversion_result != 1){
            close(socket_fd);
            return ADAS_TCP_ROI_INVALID_ARGUMENT;
        }
    }

    if (bind(
        socket_fd,
        (const struct sockaddr*)&address,
        sizeof(address)
    ) < 0){
        close(socket_fd);
        return ADAS_TCP_ROI_SYSTEM_ERROR;
    }

    if (listen(socket_fd, config->backlog) < 0){
        close(socket_fd);
        return ADAS_TCP_ROI_SYSTEM_ERROR;
    }

    server->listen_fd = socket_fd;

    return ADAS_TCP_ROI_OK;
}

adas_tcp_roi_status_t adas_tcp_roi_server_accept(
    adas_tcp_roi_server_t* server
) {
    if (server == NULL || server->listen_fd < 0) {
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    if (server->client_fd >= 0) {
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    int client_fd = accept(server->listen_fd, NULL, NULL);
    if (client_fd < 0) {
        return ADAS_TCP_ROI_SYSTEM_ERROR;
    }

    server->client_fd = client_fd;

    return ADAS_TCP_ROI_OK;
}

adas_tcp_roi_status_t adas_tcp_roi_server_set_no_delay(
    adas_tcp_roi_server_t* server,
    int enable
) {
    if (server == NULL || server->client_fd < 0) {
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    const int value = enable != 0 ? 1 : 0;
    if (setsockopt(
            server->client_fd,
            IPPROTO_TCP,
            TCP_NODELAY,
            &value,
            sizeof(value)
    ) < 0) {
        return ADAS_TCP_ROI_SYSTEM_ERROR;
    }

    return ADAS_TCP_ROI_OK;
}

adas_tcp_roi_status_t adas_tcp_roi_server_receive_request(
    adas_tcp_roi_server_t* server,
    adas_roi_header_t* request_header,
    adas_roi_bbox_t* out_bbox,
    uint8_t image_payload[ADAS_ROI_IMAGE_PAYLOAD_SIZE]
) {
    if (server == NULL
        || request_header == NULL
        || out_bbox == NULL
        || image_payload == NULL
        || server->client_fd < 0) {
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    uint8_t header_buffer[ADAS_ROI_HEADER_SIZE];

    adas_tcp_roi_status_t status = receive_all(
        server->client_fd,
        header_buffer,
        sizeof(header_buffer)
    );

    if (status != ADAS_TCP_ROI_OK) {
        return status;
    }

    adas_roi_decode_header(header_buffer, request_header);

    if (!adas_roi_is_valid_request_header(request_header)) {
        return ADAS_TCP_ROI_PROTOCOL_ERROR;
    }

    uint8_t bbox_buffer[ADAS_ROI_BBOX_PAYLOAD_SIZE];

    status = receive_all(
        server->client_fd,
        bbox_buffer,
        sizeof(bbox_buffer)
    );

    if (status != ADAS_TCP_ROI_OK) {
        return status;
    }

    adas_roi_decode_bbox(bbox_buffer, out_bbox);

    status = receive_all(
        server->client_fd,
        image_payload,
        ADAS_ROI_IMAGE_PAYLOAD_SIZE
    );

    if (status != ADAS_TCP_ROI_OK) {
        return status;
    }

    return ADAS_TCP_ROI_OK;
}

adas_tcp_roi_status_t adas_tcp_roi_server_send_result(
    adas_tcp_roi_server_t* server,
    const adas_roi_header_t* request_header,
    const adas_roi_result_t* result
) {
    if (server == NULL
        || request_header == NULL
        || result == NULL
        || server->client_fd < 0) {
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    if (result->confidence_ppm > ADAS_ROI_CONFIDENCE_PPM_MAX) {
        return ADAS_TCP_ROI_INVALID_ARGUMENT;
    }

    adas_roi_header_t response_header = {
        .magic = ADAS_ROI_MAGIC,
        .version = ADAS_ROI_VERSION,
        .message_type = ADAS_ROI_MESSAGE_RESPONSE,
        .frame_id = request_header->frame_id,
        .roi_id = request_header->roi_id,
        .payload_size = ADAS_ROI_RESULT_PAYLOAD_SIZE
    };

    /*
     * 헤더(20B)와 결과(12B)를 한 버퍼에 담아 write 한 번으로 보낸다.
     *
     * 두 번에 나눠 보내면 Nagle 이 두 번째 작은 write 를 "첫 20B 의 ACK 이
     * 올 때까지" 붙들고, 받는 쪽 delayed-ACK 가 최대 40ms 뒤에 오므로 그
     * 시간이 통째로 왕복에 얹힌다. 2026-08-20 실측에서 왕복 51.6ms 중
     * 43ms 가 이 대기였다 (PS_TCP_RESPONSE_FIX.md).
     */
    uint8_t response_buffer[ADAS_ROI_HEADER_SIZE + ADAS_ROI_RESULT_PAYLOAD_SIZE];

    adas_roi_encode_header(response_buffer, &response_header);
    adas_roi_encode_result(response_buffer + ADAS_ROI_HEADER_SIZE, result);

    return send_all(
        server->client_fd,
        response_buffer,
        sizeof(response_buffer)
    );
}

void adas_tcp_roi_server_disconnect(adas_tcp_roi_server_t* server) {
    if (server == NULL) {
        return;
    }

    if (server->client_fd >= 0) {
        close(server->client_fd);
        server->client_fd = -1;
    }
}

void adas_tcp_roi_server_close(adas_tcp_roi_server_t* server) {
    if (server == NULL) {
        return;
    }

    adas_tcp_roi_server_disconnect(server);
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
}
