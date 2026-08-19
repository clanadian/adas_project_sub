#include "accelerator/pl_mmio.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

void adas_pl_mmio_init(adas_pl_mmio_region_t* region) {
    if (region == NULL) {
        return;
    }

    region->memory_fd = -1;
    region->mapping = NULL;
    region->mapping_length = 0u;
    region->base = NULL;
    region->span = 0u;
}

adas_pl_mmio_status_t adas_pl_mmio_open(
    adas_pl_mmio_region_t* region,
    uintptr_t physical_address,
    size_t span
) {
    if (region == NULL || span == 0u) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    if (region->memory_fd >= 0 || region->mapping != NULL) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    if ((physical_address % sizeof(uint32_t)) != 0u) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    const long page_size_result = sysconf(_SC_PAGESIZE);
    if (page_size_result <= 0) {
        return ADAS_PL_MMIO_SYSTEM_ERROR;
    }

    const size_t page_size = (size_t)page_size_result;
    const size_t page_offset = (size_t)(physical_address % page_size);
    const uintptr_t aligned_physical_address =
        physical_address - page_offset;

    if (page_offset > SIZE_MAX - span) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    const size_t mapping_length = page_offset + span;

    const int memory_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memory_fd < 0) {
        return ADAS_PL_MMIO_SYSTEM_ERROR;
    }

    void* mapping = mmap(
        NULL,
        mapping_length,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        memory_fd,
        (off_t)aligned_physical_address
    );

    if (mapping == MAP_FAILED) {
        close(memory_fd);
        return ADAS_PL_MMIO_SYSTEM_ERROR;
    }

    region->memory_fd = memory_fd;
    region->mapping = mapping;
    region->mapping_length = mapping_length;
    region->base =
        (volatile uint8_t*)mapping + page_offset;
    region->span = span;

    return ADAS_PL_MMIO_OK;
}

adas_pl_mmio_status_t adas_pl_mmio_read32(
    const adas_pl_mmio_region_t* region,
    size_t offset,
    uint32_t* value
) {
    if (region == NULL
        || value == NULL
        || region->base == NULL
        || region->memory_fd < 0) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    if ((offset % sizeof(uint32_t)) != 0u) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    if (region->span < sizeof(uint32_t)
        || offset > region->span - sizeof(uint32_t)) {
        return ADAS_PL_MMIO_OUT_OF_RANGE;
    }

    volatile const uint32_t* register_address =
        (volatile const uint32_t*)(region->base + offset);

    *value = *register_address;

    __sync_synchronize();

    return ADAS_PL_MMIO_OK;
}

adas_pl_mmio_status_t adas_pl_mmio_write32(
    adas_pl_mmio_region_t* region,
    size_t offset,
    uint32_t value
) {
    if (region == NULL
        || region->base == NULL
        || region->memory_fd < 0) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    if ((offset % sizeof(uint32_t)) != 0u) {
        return ADAS_PL_MMIO_INVALID_ARGUMENT;
    }

    if (region->span < sizeof(uint32_t)
        || offset > region->span - sizeof(uint32_t)) {
        return ADAS_PL_MMIO_OUT_OF_RANGE;
    }

    volatile uint32_t* register_address =
        (volatile uint32_t*)(region->base + offset);

    __sync_synchronize();

    *register_address = value;

    __sync_synchronize();

    return ADAS_PL_MMIO_OK;
}

void adas_pl_mmio_close(adas_pl_mmio_region_t* region) {
    if (region == NULL) {
        return;
    }

    if (region->mapping != NULL) {
        munmap(region->mapping, region->mapping_length);
    }

    if (region->memory_fd >= 0) {
        close(region->memory_fd);
    }

    adas_pl_mmio_init(region);
}
