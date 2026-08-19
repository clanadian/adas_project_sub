#define _POSIX_C_SOURCE 200809L

#include "accelerator/classifier_accelerator.h"

#include <stddef.h>
#include <time.h>

static int region_is_open(const adas_pl_mmio_region_t* region) {
    /* /dev/mem과 가상 주소가 모두 준비됐는지 확인하는 공통 검사입니다. */
    return region != NULL && region->memory_fd >= 0 && region->base != NULL;
}

static adas_classifier_status_t write_register(
    adas_pl_mmio_region_t* region,
    size_t offset,
    uint32_t value
) {
    /* MMIO 계층의 오류를 가속기 계층의 오류 코드로 바꿉니다. */
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
     * HLS 포인터 인자는 64비트 레지스터 두 개(low/high)로 표현됩니다.
     * 32비트 Zynq 주소에서는 high가 보통 0이지만 계약대로 둘 다 씁니다.
     */
    const uint64_t value = (uint64_t)address;
    const uint32_t low = (uint32_t)(value & UINT32_MAX);
    const uint32_t high = (uint32_t)(value >> 32u);

    if (write_register(region, low_offset, low) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    return write_register(region, high_offset, high);
}

static adas_classifier_status_t write_requant(
    adas_pl_mmio_region_t* region,
    size_t low_offset,
    size_t high_offset,
    const adas_classifier_requant_t* requant
) {
    /* multiplier는 low 레지스터, shift는 그 다음 레지스터에 기록합니다. */
    if (write_register(
            region,
            low_offset,
            (uint32_t)requant->multiplier
        ) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }
    return write_register(region, high_offset, (uint32_t)requant->shift);
}

static adas_classifier_status_t write_biases(
    adas_pl_mmio_region_t* region,
    size_t base_offset,
    const int32_t* biases,
    size_t count
) {
    /* 각 출력 채널의 INT32 bias를 연속된 AXI-Lite 레지스터에 씁니다. */
    for (size_t i = 0u; i < count; ++i) {
        if (write_register(
                region,
                base_offset + i * sizeof(uint32_t),
                (uint32_t)biases[i]
            ) != ADAS_CLASSIFIER_OK) {
            return ADAS_CLASSIFIER_MMIO_ERROR;
        }
    }
    return ADAS_CLASSIFIER_OK;
}

static uint64_t elapsed_milliseconds(
    const struct timespec* start,
    const struct timespec* current
) {
    /* CLOCK_MONOTONIC 두 시각의 차이를 timeout 비교용 ms로 변환합니다. */
    const int64_t seconds = (int64_t)current->tv_sec - (int64_t)start->tv_sec;
    const int64_t nanoseconds =
        (int64_t)current->tv_nsec - (int64_t)start->tv_nsec;
    return (uint64_t)(seconds * 1000 + nanoseconds / 1000000);
}

void adas_classifier_accelerator_init(
    adas_classifier_accelerator_t* accelerator
) {
    /* 아직 어떤 MMIO 영역도 열지 않은 안전한 초기 상태로 만듭니다. */
    if (accelerator == NULL) {
        return;
    }

    adas_pl_mmio_init(&accelerator->args_region);
    adas_pl_mmio_init(&accelerator->exec_region);
}

adas_classifier_status_t adas_classifier_accelerator_open(
    adas_classifier_accelerator_t* accelerator
) {
    /* NULL이나 이미 열린 객체를 다시 여는 사용 오류를 막습니다. */
    if (accelerator == NULL
        || region_is_open(&accelerator->args_region)
        || region_is_open(&accelerator->exec_region)) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    /* 첫 AXI-Lite 영역: DDR 입력/가중치/출력 주소 인자 레지스터입니다. */
    if (adas_pl_mmio_open(
            &accelerator->args_region,
            ADAS_CLASSIFIER_ARGS_BASE_ADDRESS,
            ADAS_CLASSIFIER_REGISTER_SPAN
        ) != ADAS_PL_MMIO_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }

    /* 두 번째 AXI-Lite 영역: 시작/완료, requant, bias 레지스터입니다. */
    if (adas_pl_mmio_open(
            &accelerator->exec_region,
            ADAS_CLASSIFIER_EXEC_BASE_ADDRESS,
            ADAS_CLASSIFIER_REGISTER_SPAN
        ) != ADAS_PL_MMIO_OK) {
        /* 두 번째 mmap 실패 시 먼저 연 첫 영역도 되돌립니다. */
        adas_pl_mmio_close(&accelerator->args_region);
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }

    return ADAS_CLASSIFIER_OK;
}

adas_classifier_status_t adas_classifier_accelerator_configure_buffers(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_buffer_addresses_t* addresses
) {
    /* 주소 레지스터 영역이 열린 뒤에만 설정할 수 있습니다. */
    if (accelerator == NULL || addresses == NULL
        || !region_is_open(&accelerator->args_region)) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    /* 모든 PL 접근 주소가 존재하고 최소 32비트 정렬인지 한 번에 검사합니다. */
    const uintptr_t values[] = {
        addresses->ifmap,
        addresses->w_conv0,
        addresses->w_conv1,
        addresses->w_conv2,
        addresses->output
    };
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i] == 0u || values[i] % sizeof(uint32_t) != 0u) {
            return ADAS_CLASSIFIER_INVALID_ARGUMENT;
        }
    }

    /*
     * DDR에 데이터를 복사하는 부분이 아닙니다.
     * PL이 DDR 어디를 읽고 쓸지 알려주는 물리 주소 숫자만 기록합니다.
     */
    if (write_address(
            &accelerator->args_region,
            ADAS_CLASSIFIER_ARGS_IFMAP_LOW,
            ADAS_CLASSIFIER_ARGS_IFMAP_HIGH,
            addresses->ifmap
        ) != ADAS_CLASSIFIER_OK
        || write_address(
            &accelerator->args_region,
            ADAS_CLASSIFIER_ARGS_W_CONV0_LOW,
            ADAS_CLASSIFIER_ARGS_W_CONV0_HIGH,
            addresses->w_conv0
        ) != ADAS_CLASSIFIER_OK
        || write_address(
            &accelerator->args_region,
            ADAS_CLASSIFIER_ARGS_W_CONV1_LOW,
            ADAS_CLASSIFIER_ARGS_W_CONV1_HIGH,
            addresses->w_conv1
        ) != ADAS_CLASSIFIER_OK
        || write_address(
            &accelerator->args_region,
            ADAS_CLASSIFIER_ARGS_W_CONV2_LOW,
            ADAS_CLASSIFIER_ARGS_W_CONV2_HIGH,
            addresses->w_conv2
        ) != ADAS_CLASSIFIER_OK
        || write_address(
            &accelerator->args_region,
            ADAS_CLASSIFIER_ARGS_OUTPUT_LOW,
            ADAS_CLASSIFIER_ARGS_OUTPUT_HIGH,
            addresses->output
        ) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }

    return ADAS_CLASSIFIER_OK;
}

adas_classifier_status_t adas_classifier_accelerator_load_parameters(
    adas_classifier_accelerator_t* accelerator,
    const adas_classifier_parameters_t* parameters
) {
    /* 실행/파라미터 레지스터 영역이 열린 뒤에만 기록할 수 있습니다. */
    if (accelerator == NULL || parameters == NULL
        || !region_is_open(&accelerator->exec_region)) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    /* 세 Conv의 requant 값과 채널별 bias를 PL 레지스터에 적재합니다. */
    if (write_requant(
            &accelerator->exec_region,
            ADAS_CLASSIFIER_EXEC_RQ_CONV0_LOW,
            ADAS_CLASSIFIER_EXEC_RQ_CONV0_HIGH,
            &parameters->rq_conv0
        ) != ADAS_CLASSIFIER_OK
        || write_requant(
            &accelerator->exec_region,
            ADAS_CLASSIFIER_EXEC_RQ_CONV1_LOW,
            ADAS_CLASSIFIER_EXEC_RQ_CONV1_HIGH,
            &parameters->rq_conv1
        ) != ADAS_CLASSIFIER_OK
        || write_requant(
            &accelerator->exec_region,
            ADAS_CLASSIFIER_EXEC_RQ_CONV2_LOW,
            ADAS_CLASSIFIER_EXEC_RQ_CONV2_HIGH,
            &parameters->rq_conv2
        ) != ADAS_CLASSIFIER_OK
        || write_biases(
            &accelerator->exec_region,
            ADAS_CLASSIFIER_EXEC_B_CONV0_BASE,
            parameters->b_conv0,
            ADAS_CLASSIFIER_B_CONV0_COUNT
        ) != ADAS_CLASSIFIER_OK
        || write_biases(
            &accelerator->exec_region,
            ADAS_CLASSIFIER_EXEC_B_CONV1_BASE,
            parameters->b_conv1,
            ADAS_CLASSIFIER_B_CONV1_COUNT
        ) != ADAS_CLASSIFIER_OK
        || write_biases(
            &accelerator->exec_region,
            ADAS_CLASSIFIER_EXEC_B_CONV2_BASE,
            parameters->b_conv2,
            ADAS_CLASSIFIER_B_CONV2_COUNT
        ) != ADAS_CLASSIFIER_OK) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }

    return ADAS_CLASSIFIER_OK;
}

adas_classifier_status_t adas_classifier_accelerator_start(
    adas_classifier_accelerator_t* accelerator
) {
    /* ap_start 비트 0을 1로 써서 PL 계산을 시작시킵니다. */
    if (accelerator == NULL || !region_is_open(&accelerator->exec_region)) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    return write_register(
        &accelerator->exec_region,
        ADAS_CLASSIFIER_EXEC_AP_CTRL,
        ADAS_CLASSIFIER_AP_START_MASK
    );
}

adas_classifier_status_t adas_classifier_accelerator_wait_done(
    adas_classifier_accelerator_t* accelerator,
    uint32_t timeout_ms
) {
    /* 무한 대기를 피하려고 0이 아닌 제한 시간을 반드시 받습니다. */
    if (accelerator == NULL || timeout_ms == 0u
        || !region_is_open(&accelerator->exec_region)) {
        return ADAS_CLASSIFIER_INVALID_ARGUMENT;
    }

    /* 시스템 시간이 바뀌어도 영향 없는 monotonic clock으로 시작 시각을 잡습니다. */
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return ADAS_CLASSIFIER_MMIO_ERROR;
    }

    /* CPU를 계속 점유하지 않도록 완료 확인 사이에 100 us 쉽니다. */
    const struct timespec polling_delay = {
        .tv_sec = 0,
        .tv_nsec = 100000
    };

    /* 인터럽트 대신 ap_done 비트를 반복해서 읽는 polling 방식입니다. */
    for (;;) {
        uint32_t control = 0u;
        if (adas_pl_mmio_read32(
                &accelerator->exec_region,
                ADAS_CLASSIFIER_EXEC_AP_CTRL,
                &control
            ) != ADAS_PL_MMIO_OK) {
            return ADAS_CLASSIFIER_MMIO_ERROR;
        }

        /* PL이 ap_done을 올렸으면 한 번의 추론이 끝난 것입니다. */
        if ((control & ADAS_CLASSIFIER_AP_DONE_MASK) != 0u) {
            return ADAS_CLASSIFIER_OK;
        }

        struct timespec current;
        if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
            return ADAS_CLASSIFIER_MMIO_ERROR;
        }
        /* 제한 시간 안에 완료되지 않으면 상위 코드가 복구할 수 있게 반환합니다. */
        if (elapsed_milliseconds(&start, &current) >= timeout_ms) {
            return ADAS_CLASSIFIER_TIMEOUT;
        }

        (void)nanosleep(&polling_delay, NULL);
    }
}

void adas_classifier_accelerator_close(
    adas_classifier_accelerator_t* accelerator
) {
    /* NULL close는 아무 일도 하지 않게 하여 정리 코드를 단순하게 합니다. */
    if (accelerator == NULL) {
        return;
    }

    /* mmap을 해제하고 /dev/mem fd를 닫습니다. close 내부에서 다시 초기화됩니다. */
    adas_pl_mmio_close(&accelerator->exec_region);
    adas_pl_mmio_close(&accelerator->args_region);
}
