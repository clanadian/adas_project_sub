#include "accelerator/pl_mmio.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void test_init_and_empty_close(void) {
    adas_pl_mmio_region_t region;
    adas_pl_mmio_init(&region);

    assert(region.memory_fd == -1);
    assert(region.mapping == NULL);
    assert(region.mapping_length == 0u);
    assert(region.base == NULL);
    assert(region.span == 0u);

    /* 열리지 않은 초기 상태를 닫아도 안전해야 한다. */
    adas_pl_mmio_close(&region);
    assert(region.memory_fd == -1);
    assert(region.mapping == NULL);
}

static void test_fake_register_read_and_write(void) {
    uint32_t registers[16] = {0u};

    adas_pl_mmio_region_t region;
    adas_pl_mmio_init(&region);

    /*
     * /dev/mem 대신 일반 배열을 가짜 MMIO 영역으로 연결한다.
     * memory_fd=0은 read32/write32에 열린 영역임을 표시하기 위한 테스트 값이다.
     * 이 가짜 영역에는 adas_pl_mmio_close()를 호출하지 않는다.
     */
    region.memory_fd = 0;
    region.base = (volatile uint8_t*)registers;
    region.span = sizeof(registers);

    assert(adas_pl_mmio_write32(&region, 0u, 0x12345678u)
        == ADAS_PL_MMIO_OK);
    assert(registers[0] == 0x12345678u);

    assert(adas_pl_mmio_write32(&region, 12u, 0xa5a55a5au)
        == ADAS_PL_MMIO_OK);
    assert(registers[3] == 0xa5a55a5au);

    uint32_t value = 0u;
    assert(adas_pl_mmio_read32(&region, 12u, &value)
        == ADAS_PL_MMIO_OK);
    assert(value == 0xa5a55a5au);

    adas_pl_mmio_init(&region);
}

static void test_invalid_access(void) {
    uint32_t registers[4] = {0u};

    adas_pl_mmio_region_t region;
    adas_pl_mmio_init(&region);

    uint32_t value = 0u;
    assert(adas_pl_mmio_read32(&region, 0u, &value)
        == ADAS_PL_MMIO_INVALID_ARGUMENT);
    assert(adas_pl_mmio_write32(&region, 0u, 1u)
        == ADAS_PL_MMIO_INVALID_ARGUMENT);

    region.memory_fd = 0;
    region.base = (volatile uint8_t*)registers;
    region.span = sizeof(registers);

    assert(adas_pl_mmio_read32(&region, 2u, &value)
        == ADAS_PL_MMIO_INVALID_ARGUMENT);
    assert(adas_pl_mmio_write32(&region, 2u, 1u)
        == ADAS_PL_MMIO_INVALID_ARGUMENT);

    assert(adas_pl_mmio_read32(&region, sizeof(registers), &value)
        == ADAS_PL_MMIO_OUT_OF_RANGE);
    assert(adas_pl_mmio_write32(&region, sizeof(registers), 1u)
        == ADAS_PL_MMIO_OUT_OF_RANGE);

    assert(adas_pl_mmio_read32(NULL, 0u, &value)
        == ADAS_PL_MMIO_INVALID_ARGUMENT);
    assert(adas_pl_mmio_read32(&region, 0u, NULL)
        == ADAS_PL_MMIO_INVALID_ARGUMENT);
}

int main(void) {
    test_init_and_empty_close();
    test_fake_register_read_and_write();
    test_invalid_access();

    puts("PL MMIO tests passed");
    return EXIT_SUCCESS;
}
