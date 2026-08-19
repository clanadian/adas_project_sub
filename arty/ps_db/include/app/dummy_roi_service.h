#ifndef ADAS_DUMMY_ROI_SERVICE_H
#define ADAS_DUMMY_ROI_SERVICE_H

#include "network/tcp_roi_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adas_dummy_service_status {
    ADAS_DUMMY_SERVICE_OK = 0,
    ADAS_DUMMY_SERVICE_PEER_CLOSED = 1,
    ADAS_DUMMY_SERVICE_INVALID_ARGUMENT = -1,
    ADAS_DUMMY_SERVICE_NETWORK_ERROR = -2,
    ADAS_DUMMY_SERVICE_PREPROCESS_ERROR = -3
} adas_dummy_service_status_t;

/* 연결된 client의 ROI 요청 하나를 처리하고 고정 분류 결과를 회신한다. */
adas_dummy_service_status_t adas_dummy_service_handle_one(
    adas_tcp_roi_server_t* server
);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_DUMMY_ROI_SERVICE_H
