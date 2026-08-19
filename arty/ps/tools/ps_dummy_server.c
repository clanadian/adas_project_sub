#include "app/dummy_roi_service.h"
#include "network/tcp_roi_server.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_port(const char* text, uint16_t* port) {
    if (text == NULL || port == NULL || *text == '\0') {
        return -1;
    }

    errno = 0;
    char* end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0u
        || value > UINT16_MAX) {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 3) {
        fprintf(stderr, "usage: %s [bind-address|*] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* bind_address = NULL;
    if (argc >= 2 && strcmp(argv[1], "*") != 0) {
        bind_address = argv[1];
    }

    uint16_t port = ADAS_TCP_ROI_DEFAULT_PORT;
    if (argc >= 3 && parse_port(argv[2], &port) != 0) {
        fprintf(stderr, "invalid TCP port: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    adas_tcp_roi_server_t server;
    adas_tcp_roi_server_init(&server);

    const adas_tcp_roi_server_config_t config = {
        .bind_address = bind_address,
        .port = port,
        .backlog = ADAS_TCP_ROI_DEFAULT_BACKLOG
    };

    if (adas_tcp_roi_server_listen(&server, &config) != ADAS_TCP_ROI_OK) {
        perror("failed to start dummy ROI server");
        return EXIT_FAILURE;
    }

    printf(
        "dummy ROI server listening on %s:%u\n",
        bind_address == NULL ? "0.0.0.0" : bind_address,
        (unsigned)port
    );

    for (;;) {
        if (adas_tcp_roi_server_accept(&server) != ADAS_TCP_ROI_OK) {
            perror("accept failed");
            adas_tcp_roi_server_close(&server);
            return EXIT_FAILURE;
        }

        puts("Jetson client connected");

        for (;;) {
            const adas_dummy_service_status_t status =
                adas_dummy_service_handle_one(&server);
            if (status == ADAS_DUMMY_SERVICE_OK) {
                continue;
            }
            if (status != ADAS_DUMMY_SERVICE_PEER_CLOSED) {
                fprintf(stderr, "request failed: status=%d\n", (int)status);
            }
            break;
        }

        adas_tcp_roi_server_disconnect(&server);
        puts("Jetson client disconnected");
    }
}
