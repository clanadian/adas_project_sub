#include "app/dummy_roi_service.h"

#include "preprocess/roi_preprocessor.h"

#include <stdint.h>

#define ADAS_DUMMY_CLASS_ID        2u
#define ADAS_DUMMY_CONFIDENCE_PPM  875000u

static adas_dummy_service_status_t convert_network_status(
    adas_tcp_roi_status_t status
) {
    if (status == ADAS_TCP_ROI_PEER_CLOSED) {
        return ADAS_DUMMY_SERVICE_PEER_CLOSED;
    }
    return ADAS_DUMMY_SERVICE_NETWORK_ERROR;
}

adas_dummy_service_status_t adas_dummy_service_handle_one(
    adas_tcp_roi_server_t* server
) {
    if (server == NULL) {
        return ADAS_DUMMY_SERVICE_INVALID_ARGUMENT;
    }

    uint8_t image_payload[ADAS_ROI_IMAGE_PAYLOAD_SIZE];
    int8_t pl_input[ADAS_PL_INPUT_SIZE];
    adas_roi_header_t request_header;
    adas_roi_bbox_t bbox;

    const adas_tcp_roi_status_t receive_status =
        adas_tcp_roi_server_receive_request(
            server,
            &request_header,
            &bbox,
            image_payload
        );
    (void)bbox;
    if (receive_status != ADAS_TCP_ROI_OK) {
        return convert_network_status(receive_status);
    }

    if (adas_roi_preprocess(image_payload, pl_input)
        != ADAS_ROI_PREPROCESS_OK) {
        return ADAS_DUMMY_SERVICE_PREPROCESS_ERROR;
    }

    /*
     * pl_input은 의도적으로 생성만 한다. 실제 classifier가 연결되면
     * 이 위치에서 DDR 기록과 PL 실행으로 교체한다.
     */
    (void)pl_input;

    const adas_roi_result_t result = {
        .status = ADAS_ROI_STATUS_OK,
        .class_id = ADAS_DUMMY_CLASS_ID,
        .confidence_ppm = ADAS_DUMMY_CONFIDENCE_PPM
    };

    const adas_tcp_roi_status_t send_status =
        adas_tcp_roi_server_send_result(server, &request_header, &result);
    if (send_status != ADAS_TCP_ROI_OK) {
        return convert_network_status(send_status);
    }

    return ADAS_DUMMY_SERVICE_OK;
}
