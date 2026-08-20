#ifndef ADAS_ROI_PROTOCOL_H
#define ADAS_ROI_PROTOCOL_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

// Jetson Nano Linux(C++)와 Arty PS Linux(C)가 공통으로 사용한다.
// 모든 다중 바이트 정수는 network byte order(big-endian)로 전송한다.
// 아래 host-side 구조체를 그대로 send()하지 말고 encode 함수를 사용한다.

#define ADAS_ROI_MAGIC             0x524F4931u  // wire bytes: "ROI1"
#define ADAS_ROI_VERSION           2u

#define ADAS_ROI_MESSAGE_REQUEST   1u
#define ADAS_ROI_MESSAGE_RESPONSE  2u

#define ADAS_ROI_WIDTH             96u
#define ADAS_ROI_HEIGHT            96u
#define ADAS_ROI_CHANNELS           3u
#define ADAS_ROI_IMAGE_PAYLOAD_SIZE \
    (ADAS_ROI_WIDTH * ADAS_ROI_HEIGHT * ADAS_ROI_CHANNELS)

// v2 - 안전 판단(zone/거리)이 Arty PS로 옮겨가면서, crop된 96x96 이미지만으론
// bbox 기하 정보가 없어 요청 페이로드 앞에 bbox 블록을 追加했다. crop이 아니라
// 원본 프레임 픽셀 좌표다 - crop은 마진 때문에 실제보다 가까워 보인다
// (docs/JETSON_CONTROL_DESIGN.md §2.2 참고).
#define ADAS_ROI_BBOX_PAYLOAD_SIZE     28u
#define ADAS_ROI_REQUEST_PAYLOAD_SIZE \
    (ADAS_ROI_BBOX_PAYLOAD_SIZE + ADAS_ROI_IMAGE_PAYLOAD_SIZE)

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

// 28-byte bbox 블록의 필드 offset. 요청 페이로드에서 이미지 앞에 온다.
#define ADAS_ROI_BBOX_X_OFFSET              0u
#define ADAS_ROI_BBOX_Y_OFFSET              4u
#define ADAS_ROI_BBOX_WIDTH_OFFSET          8u
#define ADAS_ROI_BBOX_HEIGHT_OFFSET        12u
#define ADAS_ROI_BBOX_OBJECTNESS_OFFSET    16u
#define ADAS_ROI_BBOX_FRAME_WIDTH_OFFSET   20u
#define ADAS_ROI_BBOX_FRAME_HEIGHT_OFFSET  24u

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

// Host byte order 표현이다. x/y/width/height는 crop이 아니라 원본 프레임
// 픽셀 좌표(소수점 보존), objectness는 0.0~1.0.
typedef struct adas_roi_bbox {
    float x;
    float y;
    float width;
    float height;
    float objectness;
    uint32_t frame_width;
    uint32_t frame_height;
} adas_roi_bbox_t;

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

// float 비트 패턴을 uint32로 그대로 옮겨 network byte order로 보낸다.
// 양쪽 다 IEEE-754 리틀엔디안 호스트라 값 손실 없이 재구성된다.
static inline void adas_roi_write_f32(uint8_t* destination, float host_value) {
    uint32_t bits;
    memcpy(&bits, &host_value, sizeof(bits));
    adas_roi_write_u32(destination, bits);
}

static inline float adas_roi_read_f32(const uint8_t* source) {
    const uint32_t bits = adas_roi_read_u32(source);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
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
        && header->payload_size == ADAS_ROI_REQUEST_PAYLOAD_SIZE;
}

static inline void adas_roi_encode_bbox(
    uint8_t destination[ADAS_ROI_BBOX_PAYLOAD_SIZE],
    const adas_roi_bbox_t* bbox
) {
    adas_roi_write_f32(destination + ADAS_ROI_BBOX_X_OFFSET, bbox->x);
    adas_roi_write_f32(destination + ADAS_ROI_BBOX_Y_OFFSET, bbox->y);
    adas_roi_write_f32(destination + ADAS_ROI_BBOX_WIDTH_OFFSET, bbox->width);
    adas_roi_write_f32(destination + ADAS_ROI_BBOX_HEIGHT_OFFSET, bbox->height);
    adas_roi_write_f32(
        destination + ADAS_ROI_BBOX_OBJECTNESS_OFFSET, bbox->objectness
    );
    adas_roi_write_u32(
        destination + ADAS_ROI_BBOX_FRAME_WIDTH_OFFSET, bbox->frame_width
    );
    adas_roi_write_u32(
        destination + ADAS_ROI_BBOX_FRAME_HEIGHT_OFFSET, bbox->frame_height
    );
}

static inline void adas_roi_decode_bbox(
    const uint8_t source[ADAS_ROI_BBOX_PAYLOAD_SIZE],
    adas_roi_bbox_t* bbox
) {
    bbox->x = adas_roi_read_f32(source + ADAS_ROI_BBOX_X_OFFSET);
    bbox->y = adas_roi_read_f32(source + ADAS_ROI_BBOX_Y_OFFSET);
    bbox->width = adas_roi_read_f32(source + ADAS_ROI_BBOX_WIDTH_OFFSET);
    bbox->height = adas_roi_read_f32(source + ADAS_ROI_BBOX_HEIGHT_OFFSET);
    bbox->objectness = adas_roi_read_f32(
        source + ADAS_ROI_BBOX_OBJECTNESS_OFFSET
    );
    bbox->frame_width = adas_roi_read_u32(
        source + ADAS_ROI_BBOX_FRAME_WIDTH_OFFSET
    );
    bbox->frame_height = adas_roi_read_u32(
        source + ADAS_ROI_BBOX_FRAME_HEIGHT_OFFSET
    );
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
