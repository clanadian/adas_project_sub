#ifndef ADAS_CLASSIFIER_BUFFERS_H
#define ADAS_CLASSIFIER_BUFFERS_H

#include <stddef.h>
#include <stdint.h>

#include "accelerator/classifier_accelerator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PL에 전달할 데이터 크기입니다.
 * 입력은 PS 전처리가 만든 98x98x3 INT8이고,
 * 출력은 PL의 마지막 pool 결과인 12x12x64 INT8입니다.
 */
#define ADAS_CLASSIFIER_IFMAP_SIZE   (98u * 98u * 3u)
#define ADAS_CLASSIFIER_W_CONV0_SIZE (16u * 3u * 3u * 3u)
#define ADAS_CLASSIFIER_W_CONV1_SIZE (32u * 16u * 3u * 3u)
#define ADAS_CLASSIFIER_W_CONV2_SIZE (64u * 32u * 3u * 3u)
#define ADAS_CLASSIFIER_OUTPUT_SIZE  (12u * 12u * 64u)

/*
 * 각 버퍼를 4 KiB 경계에 배치합니다.
 * 데이터끼리 겹치지 않게 하면서 mmap/캐시 관리 단위를 알아보기 쉽게 합니다.
 */
#define ADAS_CLASSIFIER_BUFFER_ALIGNMENT 0x1000u
#define ADAS_CLASSIFIER_IFMAP_OFFSET     0x00000u
#define ADAS_CLASSIFIER_W_CONV0_OFFSET   0x08000u
#define ADAS_CLASSIFIER_W_CONV1_OFFSET   0x09000u
#define ADAS_CLASSIFIER_W_CONV2_OFFSET   0x0b000u
#define ADAS_CLASSIFIER_OUTPUT_OFFSET    0x10000u
#define ADAS_CLASSIFIER_BUFFER_SPAN      0x13000u

typedef enum adas_classifier_buffer_status {
    ADAS_CLASSIFIER_BUFFER_OK = 0,
    ADAS_CLASSIFIER_BUFFER_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_BUFFER_TOO_SMALL = -2,
    ADAS_CLASSIFIER_BUFFER_ADDRESS_OVERFLOW = -3
} adas_classifier_buffer_status_t;

/*
 * reserved_base: Linux가 다른 용도로 사용하지 않는 예약 DDR의 물리 시작 주소
 * reserved_span: 그 예약 영역의 전체 크기
 * addresses: 위 영역에서 계산한, PL에 실제로 전달할 물리 주소들
 */
typedef struct adas_classifier_buffer_layout {
    uintptr_t reserved_base;
    size_t reserved_span;
    adas_classifier_buffer_addresses_t addresses;
} adas_classifier_buffer_layout_t;

/*
 * 예약 DDR의 시작 주소를 기준으로 입력/가중치/출력 버퍼 주소를 계산합니다.
 * 실제 mmap이나 파일 복사는 하지 않으므로 PC 단위 테스트도 가능합니다.
 */
adas_classifier_buffer_status_t adas_classifier_buffers_make_layout(
    uintptr_t reserved_base,
    size_t reserved_span,
    adas_classifier_buffer_layout_t* layout
);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_CLASSIFIER_BUFFERS_H
