#ifndef ADAS_CLASSIFIER_ACCELERATOR_H
#define ADAS_CLASSIFIER_ACCELERATOR_H

#include <stdint.h>

#include "accelerator/classifier_registers.h"
#include "accelerator/pl_mmio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adas_classifier_status {
    ADAS_CLASSIFIER_OK = 0,
    ADAS_CLASSIFIER_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_MMIO_ERROR = -2,
    ADAS_CLASSIFIER_TIMEOUT = -3
} adas_classifier_status_t;

typedef struct adas_classifier_buffer_addresses {
    uintptr_t ifmap;
    uintptr_t w_conv0;
    uintptr_t w_conv1;
    uintptr_t w_conv2;
    uintptr_t output;
} adas_classifier_buffer_addresses_t;

typedef struct adas_classifier_requant {
    int32_t multiplier;
    uint8_t shift;
} adas_classifier_requant_t;

typedef struct adas_classifier_parameters {
    int32_t b_conv0[ADAS_CLASSIFIER_B_CONV0_COUNT];
    int32_t b_conv1[ADAS_CLASSIFIER_B_CONV1_COUNT];
    int32_t b_conv2[ADAS_CLASSIFIER_B_CONV2_COUNT];
    adas_classifier_requant_t rq_conv0;
    adas_classifier_requant_t rq_conv1;
    adas_classifier_requant_t rq_conv2;
} adas_classifier_parameters_t;

typedef struct adas_classifier_accelerator {
    adas_pl_mmio_region_t args_region;
    adas_pl_mmio_region_t exec_region;
} adas_classifier_accelerator_t;

void adas_classifier_accelerator_init(
    adas_classifier_accelerator_t* accelerator
);

adas_classifier_status_t adas_classifier_accelerator_open(
    adas_classifier_accelerator_t* accelerator
);

adas_classifier_status_t adas_classifier_accelerator_configure_buffers(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_buffer_addresses_t* addresses
);

adas_classifier_status_t adas_classifier_accelerator_load_parameters(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_parameters_t* parameters
);

adas_classifier_status_t adas_classifier_accelerator_start(
    adas_classifier_accelerator_t* accelerator
);

adas_classifier_status_t adas_classifier_accelerator_wait_done(
    adas_classifier_accelerator_t* accelerator,
    uint32_t timeout_ms
);

void adas_classifier_accelerator_close(
    adas_classifier_accelerator_t* accelerator
);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_CLASSIFIER_ACCELERATOR_H
