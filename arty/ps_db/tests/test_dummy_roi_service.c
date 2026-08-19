#include "app/dummy_roi_service.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int send_exact(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0u;
    while (sent < size) {
        const ssize_t result = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
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

static int receive_exact(int fd, uint8_t* data, size_t size) {
    size_t received = 0u;
    while (received < size) {
        const ssize_t result = recv(fd, data + received, size - received, 0);
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

static int run_client(int fd) {
    const adas_roi_header_t request = {
        .magic = ADAS_ROI_MAGIC,
        .version = ADAS_ROI_VERSION,
        .message_type = ADAS_ROI_MESSAGE_REQUEST,
        .frame_id = 55u,
        .roi_id = 4u,
        .payload_size = ADAS_ROI_IMAGE_PAYLOAD_SIZE
    };

    uint8_t header_bytes[ADAS_ROI_HEADER_SIZE];
    uint8_t image[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
    adas_roi_encode_header(header_bytes, &request);
    for (size_t i = 0u; i < sizeof(image); ++i) {
        image[i] = (uint8_t)(i & 0xffu);
    }

    if (send_exact(fd, header_bytes, sizeof(header_bytes)) != 0
        || send_exact(fd, image, sizeof(image)) != 0) {
        return 1;
    }

    uint8_t response_header_bytes[ADAS_ROI_HEADER_SIZE];
    uint8_t result_bytes[ADAS_ROI_RESULT_PAYLOAD_SIZE];
    if (receive_exact(fd, response_header_bytes, sizeof(response_header_bytes)) != 0
        || receive_exact(fd, result_bytes, sizeof(result_bytes)) != 0) {
        return 2;
    }

    adas_roi_header_t response_header;
    adas_roi_result_t result;
    adas_roi_decode_header(response_header_bytes, &response_header);
    adas_roi_decode_result(result_bytes, &result);

    return adas_roi_is_valid_response_header(&response_header)
        && response_header.frame_id == 55u
        && response_header.roi_id == 4u
        && result.status == ADAS_ROI_STATUS_OK
        && result.class_id == 2u
        && result.confidence_ppm == 875000u
        ? 0 : 3;
}

int main(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        const int status = run_client(sockets[1]);
        close(sockets[1]);
        _exit(status);
    }

    close(sockets[1]);

    adas_tcp_roi_server_t server;
    adas_tcp_roi_server_init(&server);
    server.client_fd = sockets[0];

    assert(adas_dummy_service_handle_one(&server) == ADAS_DUMMY_SERVICE_OK);
    adas_tcp_roi_server_disconnect(&server);

    int child_status = 0;
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);

    puts("PS dummy ROI service test passed");
    return EXIT_SUCCESS;
}
