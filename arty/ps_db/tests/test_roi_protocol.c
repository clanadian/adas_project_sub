#include "roi_protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_request_header_wire_bytes_and_round_trip(void) {
    const adas_roi_header_t input = {
        ADAS_ROI_MAGIC,
        ADAS_ROI_VERSION,
        ADAS_ROI_MESSAGE_REQUEST,
        0x01020304u,
        0x11223344u,
        ADAS_ROI_REQUEST_PAYLOAD_SIZE
    };

    uint8_t wire[ADAS_ROI_HEADER_SIZE] = {0};
    adas_roi_encode_header(wire, &input);

    assert(wire[0] == 'R');
    assert(wire[1] == 'O');
    assert(wire[2] == 'I');
    assert(wire[3] == '1');
    assert(wire[4] == 0);
    assert(wire[5] == 2);
    assert(wire[6] == 0);
    assert(wire[7] == 1);
    assert(wire[8] == 0x01);
    assert(wire[9] == 0x02);
    assert(wire[10] == 0x03);
    assert(wire[11] == 0x04);

    adas_roi_header_t output = {0u, 0u, 0u, 0u, 0u, 0u};
    adas_roi_decode_header(wire, &output);

    assert(output.magic == input.magic);
    assert(output.version == input.version);
    assert(output.message_type == input.message_type);
    assert(output.frame_id == input.frame_id);
    assert(output.roi_id == input.roi_id);
    assert(output.payload_size == input.payload_size);
    assert(adas_roi_is_valid_request_header(&output));
    assert(!adas_roi_is_valid_response_header(&output));
}

static void test_response_header_and_result_round_trip(void) {
    const adas_roi_header_t input_header = {
        ADAS_ROI_MAGIC,
        ADAS_ROI_VERSION,
        ADAS_ROI_MESSAGE_RESPONSE,
        7u,
        3u,
        ADAS_ROI_RESULT_PAYLOAD_SIZE
    };

    uint8_t header_wire[ADAS_ROI_HEADER_SIZE] = {0};
    adas_roi_encode_header(header_wire, &input_header);

    adas_roi_header_t output_header = {0u, 0u, 0u, 0u, 0u, 0u};
    adas_roi_decode_header(header_wire, &output_header);

    assert(adas_roi_is_valid_response_header(&output_header));
    assert(output_header.frame_id == 7u);
    assert(output_header.roi_id == 3u);

    const adas_roi_result_t input_result = {
        ADAS_ROI_STATUS_OK,
        2u,
        875000u
    };

    uint8_t result_wire[ADAS_ROI_RESULT_PAYLOAD_SIZE] = {0};
    adas_roi_encode_result(result_wire, &input_result);

    adas_roi_result_t output_result = {0u, 0u, 0u};
    adas_roi_decode_result(result_wire, &output_result);

    assert(output_result.status == ADAS_ROI_STATUS_OK);
    assert(output_result.class_id == 2u);
    assert(output_result.confidence_ppm == 875000u);
}

static void test_invalid_request_header_is_rejected(void) {
    adas_roi_header_t header = {
        ADAS_ROI_MAGIC,
        ADAS_ROI_VERSION,
        ADAS_ROI_MESSAGE_REQUEST,
        1u,
        2u,
        ADAS_ROI_REQUEST_PAYLOAD_SIZE
    };

    assert(adas_roi_is_valid_request_header(&header));

    header.magic = 0u;
    assert(!adas_roi_is_valid_request_header(&header));

    header.magic = ADAS_ROI_MAGIC;
    header.payload_size = 1u;
    assert(!adas_roi_is_valid_request_header(&header));
}

static void test_bbox_round_trip(void) {
    const adas_roi_bbox_t input = {
        123.5F, 45.25F, 80.0F, 96.75F, 0.875F, 640u, 360u
    };

    uint8_t wire[ADAS_ROI_BBOX_PAYLOAD_SIZE] = {0};
    adas_roi_encode_bbox(wire, &input);

    adas_roi_bbox_t output = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0u, 0u};
    adas_roi_decode_bbox(wire, &output);

    assert(output.x == input.x);
    assert(output.y == input.y);
    assert(output.width == input.width);
    assert(output.height == input.height);
    assert(output.objectness == input.objectness);
    assert(output.frame_width == input.frame_width);
    assert(output.frame_height == input.frame_height);
}

int main(void) {
    test_request_header_wire_bytes_and_round_trip();
    test_response_header_and_result_round_trip();
    test_invalid_request_header_is_rejected();
    test_bbox_round_trip();

    puts("ROI protocol tests passed");
    return 0;
}
