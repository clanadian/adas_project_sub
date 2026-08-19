#ifndef ADAS_ROI_PROTOCOL_H
#define ADAS_ROI_PROTOCOL_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

// Jetson Nano Linux(C++)와 Arty PS Linux(C)가 공통으로 사용한다.
// 모든 다중 바이트 정수는 network byte order(big-endian)로 전송한다.
// 아래 host-side 구조체를 그대로 send()하지 말고 encode 함수를 사용한다.

#define ADAS_ROI_MAGIC             0x524F4931u  // wire bytes: "ROI1"
#define ADAS_ROI_VERSION           1u

#define ADAS_ROI_MESSAGE_REQUEST   1u
#define ADAS_ROI_MESSAGE_RESPONSE  2u

#define ADAS_ROI_WIDTH             96u
#define ADAS_ROI_HEIGHT            96u
#define ADAS_ROI_CHANNELS           3u
#define ADAS_ROI_IMAGE_PAYLOAD_SIZE \
    (ADAS_ROI_WIDTH * ADAS_ROI_HEIGHT * ADAS_ROI_CHANNELS)

#define ADAS_ROI_HEADER_SIZE           20u
#define ADAS_ROI_RESULT_PAYLOAD_SIZE   12u

#define ADAS_ROI_STATUS_OK                 0u
#define ADAS_ROI_STATUS_INVALID_HEADER     1u
#define ADAS_ROI_STATUS_INVALID_PAYLOAD    2u
#define ADAS_ROI_STATUS_ACCELERATOR_ERROR  3u
#define ADAS_ROI_STATUS_POSTPROCESS_ERROR  4u

#define ADAS_ROI_INVALID_CLASS_ID       UINT32_MAX
#define ADAS_ROI_CONFIDENCE_PPM_MAX     1000000u

// 20-byte wire header의 필드 offset.
#define ADAS_ROI_HEADER_MAGIC_OFFSET        0u
#define ADAS_ROI_HEADER_VERSION_OFFSET      4u
#define ADAS_ROI_HEADER_TYPE_OFFSET         6u
#define ADAS_ROI_HEADER_FRAME_ID_OFFSET     8u
#define ADAS_ROI_HEADER_ROI_ID_OFFSET      12u
#define ADAS_ROI_HEADER_PAYLOAD_SIZE_OFFSET 16u

// 12-byte response payload의 필드 offset.
#define ADAS_ROI_RESULT_STATUS_OFFSET          0u
#define ADAS_ROI_RESULT_CLASS_ID_OFFSET        4u
#define ADAS_ROI_RESULT_CONFIDENCE_PPM_OFFSET  8u

// Host byte order 표현이다. Wire layout과 구조체 padding은 무관하다.
typedef struct adas_roi_header {
    uint32_t magic;
    uint16_t version;
    uint16_t message_type;
    uint32_t frame_id;
    uint32_t roi_id;
    uint32_t payload_size;
} adas_roi_header_t;

// Host byte order 표현이다. confidence_ppm은 0..1,000,000이다.
typedef struct adas_roi_result {
    uint32_t status;
    uint32_t class_id;
    uint32_t confidence_ppm;
} adas_roi_result_t;

static inline void adas_roi_write_u16(
    uint8_t* destination,
    uint16_t host_value
) {
    const uint16_t network_value = htons(host_value);
    memcpy(destination, &network_value, sizeof(network_value));
}

static inline void adas_roi_write_u32(
    uint8_t* destination,
    uint32_t host_value
) {
    const uint32_t network_value = htonl(host_value);
    memcpy(destination, &network_value, sizeof(network_value));
}

static inline uint16_t adas_roi_read_u16(const uint8_t* source) {
    uint16_t network_value = 0;
    memcpy(&network_value, source, sizeof(network_value));
    return ntohs(network_value);
}

static inline uint32_t adas_roi_read_u32(const uint8_t* source) {
    uint32_t network_value = 0;
    memcpy(&network_value, source, sizeof(network_value));
    return ntohl(network_value);
}

static inline void adas_roi_encode_header(
    uint8_t destination[ADAS_ROI_HEADER_SIZE],
    const adas_roi_header_t* header
) {
    adas_roi_write_u32(
        destination + ADAS_ROI_HEADER_MAGIC_OFFSET,
        header->magic
    );
    adas_roi_write_u16(
        destination + ADAS_ROI_HEADER_VERSION_OFFSET,
        header->version
    );
    adas_roi_write_u16(
        destination + ADAS_ROI_HEADER_TYPE_OFFSET,
        header->message_type
    );
    adas_roi_write_u32(
        destination + ADAS_ROI_HEADER_FRAME_ID_OFFSET,
        header->frame_id
    );
    adas_roi_write_u32(
        destination + ADAS_ROI_HEADER_ROI_ID_OFFSET,
        header->roi_id
    );
    adas_roi_write_u32(
        destination + ADAS_ROI_HEADER_PAYLOAD_SIZE_OFFSET,
        header->payload_size
    );
}

static inline void adas_roi_decode_header(
    const uint8_t source[ADAS_ROI_HEADER_SIZE],
    adas_roi_header_t* header
) {
    header->magic = adas_roi_read_u32(
        source + ADAS_ROI_HEADER_MAGIC_OFFSET
    );
    header->version = adas_roi_read_u16(
        source + ADAS_ROI_HEADER_VERSION_OFFSET
    );
    header->message_type = adas_roi_read_u16(
        source + ADAS_ROI_HEADER_TYPE_OFFSET
    );
    header->frame_id = adas_roi_read_u32(
        source + ADAS_ROI_HEADER_FRAME_ID_OFFSET
    );
    header->roi_id = adas_roi_read_u32(
        source + ADAS_ROI_HEADER_ROI_ID_OFFSET
    );
    header->payload_size = adas_roi_read_u32(
        source + ADAS_ROI_HEADER_PAYLOAD_SIZE_OFFSET
    );
}

static inline int adas_roi_is_valid_request_header(
    const adas_roi_header_t* header
) {
    return header->magic == ADAS_ROI_MAGIC
        && header->version == ADAS_ROI_VERSION
        && header->message_type == ADAS_ROI_MESSAGE_REQUEST
        && header->payload_size == ADAS_ROI_IMAGE_PAYLOAD_SIZE;
}

static inline int adas_roi_is_valid_response_header(
    const adas_roi_header_t* header
) {
    return header->magic == ADAS_ROI_MAGIC
        && header->version == ADAS_ROI_VERSION
        && header->message_type == ADAS_ROI_MESSAGE_RESPONSE
        && header->payload_size == ADAS_ROI_RESULT_PAYLOAD_SIZE;
}

static inline void adas_roi_encode_result(
    uint8_t destination[ADAS_ROI_RESULT_PAYLOAD_SIZE],
    const adas_roi_result_t* result
) {
    adas_roi_write_u32(
        destination + ADAS_ROI_RESULT_STATUS_OFFSET,
        result->status
    );
    adas_roi_write_u32(
        destination + ADAS_ROI_RESULT_CLASS_ID_OFFSET,
        result->class_id
    );
    adas_roi_write_u32(
        destination + ADAS_ROI_RESULT_CONFIDENCE_PPM_OFFSET,
        result->confidence_ppm
    );
}

static inline void adas_roi_decode_result(
    const uint8_t source[ADAS_ROI_RESULT_PAYLOAD_SIZE],
    adas_roi_result_t* result
) {
    result->status = adas_roi_read_u32(
        source + ADAS_ROI_RESULT_STATUS_OFFSET
    );
    result->class_id = adas_roi_read_u32(
        source + ADAS_ROI_RESULT_CLASS_ID_OFFSET
    );
    result->confidence_ppm = adas_roi_read_u32(
        source + ADAS_ROI_RESULT_CONFIDENCE_PPM_OFFSET
    );
}

#endif  // ADAS_ROI_PROTOCOL_H
