#ifndef ADAS_TCP_ROI_SERVER_H
#define ADAS_TCP_ROI_SERVER_H

#include <stdint.h>

#include "roi_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADAS_TCP_ROI_DEFAULT_PORT     5000u
#define ADAS_TCP_ROI_DEFAULT_BACKLOG     1

/*
 * 함수의 반환값이다.
 *
 * PEER_CLOSED는 오류라기보다 Jetson이 정상적으로 연결을 끊은 상태다.
 * PROTOCOL_ERROR는 header 또는 payload가 계약과 맞지 않는 경우다.
 */
typedef enum adas_tcp_roi_status {
    ADAS_TCP_ROI_OK = 0,
    ADAS_TCP_ROI_PEER_CLOSED = 1,
    ADAS_TCP_ROI_INVALID_ARGUMENT = -1,
    ADAS_TCP_ROI_SYSTEM_ERROR = -2,
    ADAS_TCP_ROI_PROTOCOL_ERROR = -3
} adas_tcp_roi_status_t;

/*
 * bind_address가 NULL이면 모든 IPv4 인터페이스(0.0.0.0)에서 연결을 받는다.
 * port는 host byte order 값이다.
 */
typedef struct adas_tcp_roi_server_config {
    const char* bind_address;
    uint16_t port;
    int backlog;
} adas_tcp_roi_server_config_t;

/*
 * 소켓 descriptor는 서버 객체가 소유한다.
 * 사용자는 값을 직접 변경하지 말고 아래 함수로만 관리한다.
 */
typedef struct adas_tcp_roi_server {
    int listen_fd;
    int client_fd;
} adas_tcp_roi_server_t;

/* server를 닫힌 초기 상태로 만든다. */
void adas_tcp_roi_server_init(adas_tcp_roi_server_t* server);

/* socket 생성, bind, listen을 수행한다. */
adas_tcp_roi_status_t adas_tcp_roi_server_listen(
    adas_tcp_roi_server_t* server,
    const adas_tcp_roi_server_config_t* config
);

/* Jetson client 한 대의 연결을 기다린다. */
adas_tcp_roi_status_t adas_tcp_roi_server_accept(
    adas_tcp_roi_server_t* server
);


/*
 * 연결된 client socket의 Nagle 알고리즘을 켜고 끈다.
 *
 * 응답은 header 20 B와 result 12 B로 나뉘어 두 번 송신되므로, Nagle이 켜져
 * 있으면 두 번째 조각이 첫 조각의 ACK를 기다리다가 상대의 delayed ACK만큼
 * (Linux 기본 최대 40 ms) 지연될 수 있다. ROI 한 건의 PL 실행이 약 6.6 ms인
 * 것을 생각하면 이 지연은 처리량을 통째로 지배한다.
 *
 * 기본 동작은 바꾸지 않는다 - 켤지 말지는 호출자가 정하고, 측정으로 효과를
 * 확인한 뒤 기본값을 옮기는 것이 순서다.
 *
 * accept() 이후에만 호출할 수 있다.
 */
adas_tcp_roi_status_t adas_tcp_roi_server_set_no_delay(
    adas_tcp_roi_server_t* server,
    int enable
);

/*
 * 20-byte request header와 96x96x3 RGB UINT8 payload를 모두 수신한다.
 * TCP 부분 수신을 내부에서 반복 처리한다.
 * image_payload는 ADAS_ROI_IMAGE_PAYLOAD_SIZE 바이트 이상이어야 한다.
 */
adas_tcp_roi_status_t adas_tcp_roi_server_receive_request(
    adas_tcp_roi_server_t* server,
    adas_roi_header_t* request_header,
    uint8_t image_payload[ADAS_ROI_IMAGE_PAYLOAD_SIZE]
);

/*
 * request와 같은 frame_id/roi_id를 갖는 response header 및 결과를 전송한다.
 * TCP 부분 송신을 내부에서 반복 처리한다.
 */
adas_tcp_roi_status_t adas_tcp_roi_server_send_result(
    adas_tcp_roi_server_t* server,
    const adas_roi_header_t* request_header,
    const adas_roi_result_t* result
);

/* 현재 client 연결만 닫는다. listen socket은 유지한다. */
void adas_tcp_roi_server_disconnect(adas_tcp_roi_server_t* server);

/* client와 listen socket을 모두 닫고 초기 상태로 되돌린다. */
void adas_tcp_roi_server_close(adas_tcp_roi_server_t* server);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_TCP_ROI_SERVER_H
