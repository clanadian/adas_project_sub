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
    /*
     * DB 드라이버(ABI 1)가 올라가 있으면 여기서 걸린다. 주소맵과 실행
     * 순서가 완전히 달라 그대로 돌리면 존재하지 않는 레지스터를 건드린다.
     */
    if (info.abi_version != ADAS_CLASSIFIER_ABI_VERSION
        || info.dma_span != ADAS_CLASSIFIER_DMA_SPAN
        || info.ifmap_offset != ADAS_CLASSIFIER_IFMAP_OFFSET
        || info.output_offset != ADAS_CLASSIFIER_OUTPUT_OFFSET
        || info.act_a_offset != ADAS_CLASSIFIER_ACT_A_OFFSET
        || info.act_b_offset != ADAS_CLASSIFIER_ACT_B_OFFSET
        || info.num_ops != ADAS_EB_NUM_OPS) {
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
    const adas_classifier_requant_t* const source[ADAS_EB_NUM_CONVS] = {
        &parameters->rq_conv0, &parameters->rq_conv1, &parameters->rq_conv2
    };
    for (unsigned i = 0u; i < ADAS_EB_NUM_CONVS; ++i) {
        request.requant[i].multiplier = source[i]->multiplier;
        request.requant[i].shift = source[i]->shift;
        /* EB 엔진은 requant 이전에 LeakyReLU 를 적용한다. DB 에 없던 필드다. */
        request.requant[i].leaky = source[i]->leaky;
    }

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

adas_classifier_device_status_t adas_classifier_device_run_op(
    adas_classifier_device_t* device,
    uint32_t op_index,
    uint32_t timeout_ms
) {
    if (device == NULL || device->fd < 0 || timeout_ms == 0u
        || op_index >= ADAS_EB_NUM_OPS) {
        return ADAS_CLASSIFIER_DEVICE_INVALID_ARGUMENT;
    }
    const struct adas_classifier_run_op_uapi request = {
        .op_index = op_index,
        .timeout_ms = timeout_ms
    };
    return ioctl(device->fd, ADAS_CLASSIFIER_IOC_RUN_OP, &request) == 0
        ? ADAS_CLASSIFIER_DEVICE_OK
        : ADAS_CLASSIFIER_DEVICE_IO_ERROR;
}

adas_classifier_device_status_t adas_classifier_device_status(
    adas_classifier_device_t* device,
    struct adas_classifier_status_uapi* status
) {
    if (device == NULL || status == NULL || device->fd < 0) {
        return ADAS_CLASSIFIER_DEVICE_INVALID_ARGUMENT;
    }
    return ioctl(device->fd, ADAS_CLASSIFIER_IOC_GET_STATUS, status) == 0
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

static int32_t* bias_at(adas_classifier_device_t* device, uint32_t offset) {
    /* bias 는 INT32 배열이다. 정렬은 4 KiB offset 이 보장한다. */
    int8_t* const bytes = buffer_at(device, offset);
    return (int32_t*)(void*)bytes;
}

int32_t* adas_classifier_device_b_conv0(adas_classifier_device_t* device) {
    return bias_at(device, ADAS_CLASSIFIER_B_CONV0_OFFSET);
}

int32_t* adas_classifier_device_b_conv1(adas_classifier_device_t* device) {
    return bias_at(device, ADAS_CLASSIFIER_B_CONV1_OFFSET);
}

int32_t* adas_classifier_device_b_conv2(adas_classifier_device_t* device) {
    return bias_at(device, ADAS_CLASSIFIER_B_CONV2_OFFSET);
}

int8_t* adas_classifier_device_act_a(adas_classifier_device_t* device) {
    return buffer_at(device, ADAS_CLASSIFIER_ACT_A_OFFSET);
}

int8_t* adas_classifier_device_act_b(adas_classifier_device_t* device) {
    return buffer_at(device, ADAS_CLASSIFIER_ACT_B_OFFSET);
}

int8_t* adas_classifier_device_output(adas_classifier_device_t* device) {
    /* op 6개가 짝수라 마지막 출력은 act_b 다 (classifier_buffers.c 참고). */
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
