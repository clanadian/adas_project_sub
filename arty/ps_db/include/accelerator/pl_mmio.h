#ifndef ADAS_PL_MMIO_H
#define ADAS_PL_MMIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum adas_pl_mmio_status {
    ADAS_PL_MMIO_OK = 0,
    ADAS_PL_MMIO_INVALID_ARGUMENT = -1,
    ADAS_PL_MMIO_SYSTEM_ERROR = -2,
    ADAS_PL_MMIO_OUT_OF_RANGE = -3
} adas_pl_mmio_status_t;

typedef struct adas_pl_mmio_region {
    int memory_fd;
    void* mapping;
    size_t mapping_length;
    volatile uint8_t* base;
    size_t span;
} adas_pl_mmio_region_t;

void adas_pl_mmio_init(adas_pl_mmio_region_t* region);

adas_pl_mmio_status_t adas_pl_mmio_open(
    adas_pl_mmio_region_t* region,
    uintptr_t physical_address,
    size_t span
);

adas_pl_mmio_status_t adas_pl_mmio_read32(
    const adas_pl_mmio_region_t* region,
    size_t offset,
    uint32_t* value
);

adas_pl_mmio_status_t adas_pl_mmio_write32(
    adas_pl_mmio_region_t* region,
    size_t offset,
    uint32_t value
);

void adas_pl_mmio_close(adas_pl_mmio_region_t* region);

#ifdef __cplusplus
}
#endif

#endif  // ADAS_PL_MMIO_H
