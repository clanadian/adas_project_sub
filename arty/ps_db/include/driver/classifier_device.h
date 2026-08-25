#ifndef ADAS_CLASSIFIER_DEVICE_H
#define ADAS_CLASSIFIER_DEVICE_H

#include <stdint.h>

#include "driver/adas_classifier_uapi.h"
#include "model/classifier_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adas_classifier_device_status {
    ADAS_CLASSIFIER_DEVICE_OK = 0,
    ADAS_CLASSIFIER_DEVICE_INVALID_ARGUMENT = -1,
    ADAS_CLASSIFIER_DEVICE_IO_ERROR = -2,
    ADAS_CLASSIFIER_DEVICE_ABI_MISMATCH = -3
} adas_classifier_device_status_t;

typedef struct adas_classifier_device {
    int fd;
    uint8_t* dma;
    uint32_t dma_span;
} adas_classifier_device_t;

void adas_classifier_device_init(adas_classifier_device_t* device);

adas_classifier_device_status_t adas_classifier_device_open(
    adas_classifier_device_t* device,
    const char* path
);

adas_classifier_device_status_t adas_classifier_device_load_parameters(
    adas_classifier_device_t* device,
    const adas_classifier_parameters_t* parameters
);

adas_classifier_device_status_t adas_classifier_device_run(
    adas_classifier_device_t* device,
    uint32_t timeout_ms
);

int8_t* adas_classifier_device_ifmap(adas_classifier_device_t* device);
int8_t* adas_classifier_device_w_conv0(adas_classifier_device_t* device);
int8_t* adas_classifier_device_w_conv1(adas_classifier_device_t* device);
int8_t* adas_classifier_device_w_conv2(adas_classifier_device_t* device);
int8_t* adas_classifier_device_output(adas_classifier_device_t* device);

void adas_classifier_device_close(adas_classifier_device_t* device);

#ifdef __cplusplus
}
#endif

#endif
