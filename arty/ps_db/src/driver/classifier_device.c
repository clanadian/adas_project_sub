#define _GNU_SOURCE

#include "driver/classifier_device.h"

#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

void adas_classifier_device_init(adas_classifier_device_t* device) {
    if (device == NULL) {
        return;
    }
    device->fd = -1;
    device->dma = NULL;
    device->dma_span = 0u;
}

adas_classifier_device_status_t adas_classifier_device_open(
    adas_classifier_device_t* device,
    const char* path
) {
    if (device == NULL || path == NULL || path[0] == '\0'
        || device->fd >= 0 || device->dma != NULL) {
        return ADAS_CLASSIFIER_DEVICE_INVALID_ARGUMENT;
    }

    device->fd = open(path, O_RDWR | O_CLOEXEC);
    if (device->fd < 0) {
        return ADAS_CLASSIFIER_DEVICE_IO_ERROR;
    }

    struct adas_classifier_info_uapi info;
    if (ioctl(device->fd, ADAS_CLASSIFIER_IOC_GET_INFO, &info) != 0) {
        adas_classifier_device_close(device);
        return ADAS_CLASSIFIER_DEVICE_IO_ERROR;
    }
    if (info.abi_version != ADAS_CLASSIFIER_ABI_VERSION
        || info.dma_span != ADAS_CLASSIFIER_DMA_SPAN
        || info.ifmap_offset != ADAS_CLASSIFIER_IFMAP_OFFSET
        || info.output_offset != ADAS_CLASSIFIER_OUTPUT_OFFSET) {
        adas_classifier_device_close(device);
        return ADAS_CLASSIFIER_DEVICE_ABI_MISMATCH;
    }

    void* mapped = mmap(
        NULL, info.dma_span, PROT_READ | PROT_WRITE,
        MAP_SHARED, device->fd, 0);
    if (mapped == MAP_FAILED) {
        adas_classifier_device_close(device);
        return ADAS_CLASSIFIER_DEVICE_IO_ERROR;
    }
    device->dma = (uint8_t*)mapped;
    device->dma_span = info.dma_span;
    return ADAS_CLASSIFIER_DEVICE_OK;
}

adas_classifier_device_status_t adas_classifier_device_load_parameters(
    adas_classifier_device_t* device,
    const adas_classifier_parameters_t* parameters
) {
    if (device == NULL || parameters == NULL || device->fd < 0) {
        return ADAS_CLASSIFIER_DEVICE_INVALID_ARGUMENT;
    }

    struct adas_classifier_parameters_uapi request;
    memset(&request, 0, sizeof(request));
    request.requant[0].multiplier = parameters->rq_conv0.multiplier;
    request.requant[0].shift = parameters->rq_conv0.shift;
    request.requant[1].multiplier = parameters->rq_conv1.multiplier;
    request.requant[1].shift = parameters->rq_conv1.shift;
    request.requant[2].multiplier = parameters->rq_conv2.multiplier;
    request.requant[2].shift = parameters->rq_conv2.shift;
    memcpy(request.bias_conv0, parameters->b_conv0,
           sizeof(request.bias_conv0));
    memcpy(request.bias_conv1, parameters->b_conv1,
           sizeof(request.bias_conv1));
    memcpy(request.bias_conv2, parameters->b_conv2,
           sizeof(request.bias_conv2));

    return ioctl(device->fd, ADAS_CLASSIFIER_IOC_SET_PARAMETERS, &request) == 0
        ? ADAS_CLASSIFIER_DEVICE_OK
        : ADAS_CLASSIFIER_DEVICE_IO_ERROR;
}

adas_classifier_device_status_t adas_classifier_device_run(
    adas_classifier_device_t* device,
    uint32_t timeout_ms
) {
    if (device == NULL || device->fd < 0 || timeout_ms == 0u) {
        return ADAS_CLASSIFIER_DEVICE_INVALID_ARGUMENT;
    }
    const struct adas_classifier_run_uapi request = {
        .timeout_ms = timeout_ms,
        .reserved = 0u
    };
    return ioctl(device->fd, ADAS_CLASSIFIER_IOC_RUN, &request) == 0
        ? ADAS_CLASSIFIER_DEVICE_OK
        : ADAS_CLASSIFIER_DEVICE_IO_ERROR;
}

static int8_t* buffer_at(adas_classifier_device_t* device, uint32_t offset) {
    return device != NULL && device->dma != NULL && offset < device->dma_span
        ? (int8_t*)(device->dma + offset)
        : NULL;
}

int8_t* adas_classifier_device_ifmap(adas_classifier_device_t* device) {
    return buffer_at(device, ADAS_CLASSIFIER_IFMAP_OFFSET);
}

int8_t* adas_classifier_device_w_conv0(adas_classifier_device_t* device) {
    return buffer_at(device, ADAS_CLASSIFIER_W_CONV0_OFFSET);
}

int8_t* adas_classifier_device_w_conv1(adas_classifier_device_t* device) {
    return buffer_at(device, ADAS_CLASSIFIER_W_CONV1_OFFSET);
}

int8_t* adas_classifier_device_w_conv2(adas_classifier_device_t* device) {
    return buffer_at(device, ADAS_CLASSIFIER_W_CONV2_OFFSET);
}

int8_t* adas_classifier_device_output(adas_classifier_device_t* device) {
    return buffer_at(device, ADAS_CLASSIFIER_OUTPUT_OFFSET);
}

void adas_classifier_device_close(adas_classifier_device_t* device) {
    if (device == NULL) {
        return;
    }
    if (device->dma != NULL) {
        munmap(device->dma, device->dma_span);
    }
    if (device->fd >= 0) {
        close(device->fd);
    }
    adas_classifier_device_init(device);
}
