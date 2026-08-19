#define _POSIX_C_SOURCE 200809L

#include "accelerator/classifier_accelerator.h"

#include "accelerator/classifier_buffers.h"

#include <stddef.h>
#include <time.h>

static int region_is_open(const adas_pl_mmio_region_t* region) {
    return region != NULL && region->memory_fd >= 0 && region->base != NULL;
}

static adas_classifier_status_t write_register(
    adas_pl_mmio_region_t* region,
    size_t offset,
    uint32_t value
) {
    return adas_pl_mmio_write32(region, offset, value) == ADAS_PL_MMIO_OK
        ? ADAS_CLASSIFIER_OK
        : ADAS_CLASSIFIER_MMIO_ERROR;
}

static adas_classifier_status_t write_address(
    adas_pl_mmio_region_t* region,
    size_t low_offset,
    size_t high_offset,
    uintptr_t address
) {
    /*
     * 포인터 인자는 low/high 레지스터 두 개다. Zynq-7000 은 32비트 주소라
     * high 는 항상 0 이지만, 계약대로 둘 다 쓴다 — 이전 op 가 남긴 값이
     * 살아 있으면 엉뚱한 주소를 읽는다.
     */
    const uint64_t value = (uint64_t)address;
    const uint32_t low = (uint32_t)(value & UINT32_MAX);
    const uint32_t high = (uint32_t)(value >> 32u);

    if (write_register(region, low_offset, low) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    return write_register(region, high_offset, high);
}

static uint64_t elapsed_milliseconds(
    const struct timespec* start,
    const struct timespec* current
) {
    const int64_t seconds = (int64_t)current->tv_sec - (int64_t)start->tv_sec;
    const int64_t nanoseconds =
        (int64_t)current->tv_nsec - (int64_t)start->tv_nsec;
    return (uint64_t)(seconds * 1000 + nanoseconds / 1000000);
}

/*
 * ap_ctrl 의 특정 비트가 설 때까지 기다린다. idle 과 done 이 같은 모양이라
 * 하나로 합쳤다 — 다른 것은 기다리는 비트뿐이다.
 */
static adas_classifier_status_t wait_for_bit(
    adas_pl_mmio_region_t* region,
    uint32_t mask,
    uint32_t timeout_ms
) {
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }

    const struct timespec polling_delay = { .tv_sec = 0, .tv_nsec = 100000 };

    for (;;) {
        uint32_t control = 0u;
        if (adas_pl_mmio_read32(region, ADAS_EB_REG_CTRL, &control)
            != ADAS_PL_MMIO_OK) {
            return ADAS_CLASSIFIER_MMIO_ERROR;
        }
        if ((control & mask) != 0u) {
            return ADAS_CLASSIFIER_OK;
        }

        struct timespec current;
        if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
            return ADAS_CLASSIFIER_MMIO_ERROR;
        }
        if (elapsed_milliseconds(&start, &current) >= timeout_ms) {
            return ADAS_CLASSIFIER_TIMEOUT;
        }

        (void)nanosleep(&polling_delay, NULL);
    }
}

/* op 인덱스로 그 op 을 실행할 엔진의 제어 창을 고른다. */
static adas_pl_mmio_region_t* region_for_kind(
    adas_classifier_accelerator_t* accelerator,
    unsigned kind
) {
    switch (kind) {
    case ADAS_EB_OP_CONV0:   return &accelerator->conv0_region;
    case ADAS_EB_OP_CONV:    return &accelerator->conv_region;
    case ADAS_EB_OP_MAXPOOL: return &accelerator->maxpool_region;
    default:                 return NULL;
    }
}

/*
 * op 하나의 입력·출력 버퍼.
 *
 * conv0 만 roi 를 읽고, 그 뒤로는 act_a / act_b 를 번갈아 쓴다.
 *   op0: roi   -> act_a
 *   홀수 op   : act_a -> act_b
 *   짝수 op(>0): act_b -> act_a
 * 그래서 op 이 6개(짝수)면 마지막 출력은 항상 act_b 다.
 */
static void buffers_for_op(
    const adas_classifier_buffer_addresses_t* addresses,
    unsigned op_index,
    uintptr_t* source,
    uintptr_t* destination
) {
    if (op_index == 0u) {
        *source = addresses->roi;
        *destination = addresses->act_a;
        return;
    }
    if (op_index % 2u == 1u) {
        *source = addresses->act_a;
        *destination = addresses->act_b;
    } else {
        *source = addresses->act_b;
        *destination = addresses->act_a;
    }
}

static void weights_for_conv(
    const adas_classifier_buffer_addresses_t* addresses,
    unsigned conv_index,
    uintptr_t* weights,
    uintptr_t* bias
) {
    switch (conv_index) {
    case 0u: *weights = addresses->w_conv0; *bias = addresses->b_conv0; break;
    case 1u: *weights = addresses->w_conv1; *bias = addresses->b_conv1; break;
    default: *weights = addresses->w_conv2; *bias = addresses->b_conv2; break;
    }
}

static const adas_classifier_requant_t* requant_for_conv(
    const adas_classifier_parameters_t* parameters,
    unsigned conv_index
) {
    switch (conv_index) {
    case 0u: return &parameters->rq_conv0;
    case 1u: return &parameters->rq_conv1;
    default: return &parameters->rq_conv2;
    }
}

void adas_classifier_accelerator_init(
    adas_classifier_accelerator_t* accelerator
) {
    if (accelerator == NULL) {
        return;
    }

    adas_pl_mmio_init(&accelerator->conv_region);
    adas_pl_mmio_init(&accelerator->conv0_region);
    adas_pl_mmio_init(&accelerator->maxpool_region);
    accelerator->addresses_valid = 0;
    accelerator->parameters_valid = 0;
}

adas_classifier_status_t adas_classifier_accelerator_open(
    adas_classifier_accelerator_t* accelerator
) {
    if (accelerator == NULL
        || region_is_open(&accelerator->conv_region)
        || region_is_open(&accelerator->conv0_region)
        || region_is_open(&accelerator->maxpool_region)) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    /* 셋 중 하나라도 실패하면 앞서 연 것을 전부 되돌린다. */
    if (adas_pl_mmio_open(
            &accelerator->conv_region,
            ADAS_EB_CONV_BASE_ADDRESS,
            ADAS_EB_REGISTER_SPAN
        ) != ADAS_PL_MMIO_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    if (adas_pl_mmio_open(
            &accelerator->conv0_region,
            ADAS_EB_CONV0_BASE_ADDRESS,
            ADAS_EB_REGISTER_SPAN
        ) != ADAS_PL_MMIO_OK) {
        adas_pl_mmio_close(&accelerator->conv_region);
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    if (adas_pl_mmio_open(
            &accelerator->maxpool_region,
            ADAS_EB_MAXPOOL_BASE_ADDRESS,
            ADAS_EB_REGISTER_SPAN
        ) != ADAS_PL_MMIO_OK) {
        adas_pl_mmio_close(&accelerator->conv0_region);
        adas_pl_mmio_close(&accelerator->conv_region);
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }

    return ADAS_CLASSIFIER_OK;
}

adas_classifier_status_t adas_classifier_accelerator_configure_buffers(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_buffer_addresses_t* addresses
) {
    if (accelerator == NULL || addresses == NULL) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    /* 9개 주소가 모두 존재하고 최소 32비트 정렬인지 한 번에 검사한다. */
    const uintptr_t values[] = {
        addresses->roi,
        addresses->act_a,
        addresses->act_b,
        addresses->w_conv0,
        addresses->w_conv1,
        addresses->w_conv2,
        addresses->b_conv0,
        addresses->b_conv1,
        addresses->b_conv2
    };
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i] == 0u || values[i] % sizeof(uint32_t) != 0u) {
            return ADAS_CLASSIFIER_INVALID_ARGUMENT;
        }
    }

    /* act_a 와 act_b 가 같으면 op 이 자기가 읽는 버퍼를 덮어쓴다. */
    if (addresses->act_a == addresses->act_b) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    accelerator->addresses = *addresses;
    accelerator->addresses_valid = 1;
    return ADAS_CLASSIFIER_OK;
}

adas_classifier_status_t adas_classifier_accelerator_load_parameters(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_parameters_t* parameters
) {
    if (accelerator == NULL || parameters == NULL) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    /*
     * multiplier 0 은 "아직 manifest 값을 안 넣었다"는 뜻이다. 그대로
     * 실행하면 출력이 전부 0 이 되고, 그것은 배선 오류처럼 보인다.
     */
    const adas_classifier_requant_t* const requants[] = {
        &parameters->rq_conv0, &parameters->rq_conv1, &parameters->rq_conv2
    };
    for (size_t i = 0u; i < ADAS_EB_NUM_CONVS; ++i) {
        if (requants[i]->multiplier == 0 || requants[i]->shift > 63u) {
            return ADAS_CLASSIFIER_INVALID_ARGUMENT;
        }
    }

    accelerator->parameters = *parameters;
    accelerator->parameters_valid = 1;
    return ADAS_CLASSIFIER_OK;
}

/* conv0_engine: 미리 패딩된 입력, pad 포트 없음, 가중치는 OIHW. */
static adas_classifier_status_t program_conv0(
    adas_pl_mmio_region_t* region,
    const struct adas_eb_op* op,
    uintptr_t source,
    uintptr_t destination,
    uintptr_t weights,
    uintptr_t bias,
    const adas_classifier_requant_t* requant
) {
    if (write_address(region, ADAS_EB_CONV0_IFMAP_LO,
                      ADAS_EB_CONV0_IFMAP_HI, source) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_CONV0_WEIGHTS_LO,
                         ADAS_EB_CONV0_WEIGHTS_HI, weights) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_CONV0_BIAS_LO,
                         ADAS_EB_CONV0_BIAS_HI, bias) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_CONV0_OFMAP_LO,
                         ADAS_EB_CONV0_OFMAP_HI, destination) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV0_IMG_H, op->img_h) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV0_IMG_W, op->img_w) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV0_REQUANT_MUL,
                          (uint32_t)requant->multiplier) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV0_REQUANT_SHIFT,
                          requant->shift) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV0_LEAKY_ENABLE,
                          requant->leaky) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    return ADAS_CLASSIFIER_OK;
}

/* conv_engine: conv1/conv2 공용. weights_hi 는 같은 버퍼의 두 번째 읽기 포트다. */
static adas_classifier_status_t program_conv(
    adas_pl_mmio_region_t* region,
    const struct adas_eb_op* op,
    uintptr_t source,
    uintptr_t destination,
    uintptr_t weights,
    uintptr_t bias,
    const adas_classifier_requant_t* requant
) {
    if (write_address(region, ADAS_EB_CONV_IFMAP_LO,
                      ADAS_EB_CONV_IFMAP_HI, source) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_CONV_WEIGHTS_LO,
                         ADAS_EB_CONV_WEIGHTS_HI, weights) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_CONV_WEIGHTS2_LO,
                         ADAS_EB_CONV_WEIGHTS2_HI, weights) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_CONV_BIAS_LO,
                         ADAS_EB_CONV_BIAS_HI, bias) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_CONV_OFMAP_LO,
                         ADAS_EB_CONV_OFMAP_HI, destination) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_IMG_H, op->img_h) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_IMG_W, op->img_w) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_IN_CH, op->in_ch) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_OUT_CH, op->out_ch) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_K, op->k) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_STRIDE, op->stride) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_PAD, op->pad) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_REQUANT_MUL,
                          (uint32_t)requant->multiplier) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_REQUANT_SHIFT,
                          requant->shift) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_CONV_LEAKY_ENABLE,
                          requant->leaky) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    return ADAS_CLASSIFIER_OK;
}

static adas_classifier_status_t program_maxpool(
    adas_pl_mmio_region_t* region,
    const struct adas_eb_op* op,
    uintptr_t source,
    uintptr_t destination
) {
    if (write_address(region, ADAS_EB_MAXPOOL_IFMAP_LO,
                      ADAS_EB_MAXPOOL_IFMAP_HI, source) != ADAS_CLASSIFIER_OK
        || write_address(region, ADAS_EB_MAXPOOL_OFMAP_LO,
                         ADAS_EB_MAXPOOL_OFMAP_HI, destination) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_MAXPOOL_IMG_H, op->img_h) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_MAXPOOL_IMG_W, op->img_w) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_MAXPOOL_CH, op->in_ch) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_MAXPOOL_STRIDE, op->stride) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_MAXPOOL_PAD_RIGHT, 0u) != ADAS_CLASSIFIER_OK
        || write_register(region, ADAS_EB_MAXPOOL_PAD_BOTTOM, 0u) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    return ADAS_CLASSIFIER_OK;
}

adas_classifier_status_t adas_classifier_accelerator_op_buffers(
    const adas_classifier_buffer_addresses_t* addresses,
    unsigned op_index,
    uintptr_t* source,
    uintptr_t* destination
) {
    if (addresses == NULL || source == NULL || destination == NULL
        || op_index >= ADAS_EB_NUM_OPS) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }
    buffers_for_op(addresses, op_index, source, destination);
    return ADAS_CLASSIFIER_OK;
}

adas_classifier_status_t adas_classifier_accelerator_run_op(
    adas_classifier_accelerator_t* accelerator,
    unsigned op_index,
    uint32_t timeout_ms
) {
    static const struct adas_eb_op ops[] = ADAS_EB_OP_TABLE_INITIALIZER;

    if (accelerator == NULL || op_index >= ADAS_EB_NUM_OPS || timeout_ms == 0u) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }
    if (!accelerator->addresses_valid || !accelerator->parameters_valid) {
        return ADAS_CLASSIFIER_NOT_CONFIGURED;
    }

    const struct adas_eb_op* const op = &ops[op_index];
    adas_pl_mmio_region_t* const region = region_for_kind(accelerator, op->kind);
    if (region == NULL || !region_is_open(region)) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    uintptr_t source = 0u;
    uintptr_t destination = 0u;
    buffers_for_op(&accelerator->addresses, op_index, &source, &destination);

    adas_classifier_status_t status;
    if (op->kind == ADAS_EB_OP_MAXPOOL) {
        status = program_maxpool(region, op, source, destination);
    } else {
        uintptr_t weights = 0u;
        uintptr_t bias = 0u;
        weights_for_conv(&accelerator->addresses, op->conv_index,
                         &weights, &bias);
        const adas_classifier_requant_t* const requant =
            requant_for_conv(&accelerator->parameters, op->conv_index);
        status = (op->kind == ADAS_EB_OP_CONV0)
            ? program_conv0(region, op, source, destination, weights, bias, requant)
            : program_conv(region, op, source, destination, weights, bias, requant);
    }
    if (status != ADAS_CLASSIFIER_OK) {
        return status;
    }

    /*
     * 시작 전에 idle 을 확인한다. 앞 op 이 아직 도는 중이면 ap_start 가
     * 무시되고, 그러면 이번 op 의 출력 버퍼에 옛 내용이 남는다.
     */
    status = wait_for_bit(region, ADAS_EB_AP_IDLE_MASK, timeout_ms);
    if (status != ADAS_CLASSIFIER_OK) {
        return status;
    }

    status = write_register(region, ADAS_EB_REG_CTRL, ADAS_EB_AP_START_MASK);
    if (status != ADAS_CLASSIFIER_OK) {
        return status;
    }

    return wait_for_bit(region, ADAS_EB_AP_DONE_MASK, timeout_ms);
}

adas_classifier_status_t adas_classifier_accelerator_run(
    adas_classifier_accelerator_t* accelerator,
    uint32_t timeout_ms,
    unsigned* failed_op
) {
    if (failed_op != NULL) {
        *failed_op = ADAS_EB_NUM_OPS;
    }
    if (accelerator == NULL || timeout_ms == 0u) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    for (unsigned i = 0u; i < ADAS_EB_NUM_OPS; ++i) {
        const adas_classifier_status_t status =
            adas_classifier_accelerator_run_op(accelerator, i, timeout_ms);
        if (status != ADAS_CLASSIFIER_OK) {
            if (failed_op != NULL) {
                *failed_op = i;
            }
            return status;
        }
    }

    return ADAS_CLASSIFIER_OK;
}

void adas_classifier_accelerator_close(
    adas_classifier_accelerator_t* accelerator
) {
    if (accelerator == NULL) {
        return;
    }

    adas_pl_mmio_close(&accelerator->maxpool_region);
    adas_pl_mmio_close(&accelerator->conv0_region);
    adas_pl_mmio_close(&accelerator->conv_region);
    accelerator->addresses_valid = 0;
    accelerator->parameters_valid = 0;
}
