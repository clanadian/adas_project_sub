#include "accelerator/classifier_accelerator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t register_value(const uint32_t* registers, size_t offset) {
    return registers[offset / sizeof(uint32_t)];
}

static void attach_fake_region(
    adas_pl_mmio_region_t* region,
    uint32_t* registers,
    size_t byte_size
) {
    adas_pl_mmio_init(region);
    region->memory_fd = 0;
    region->base = (volatile uint8_t*)registers;
    region->span = byte_size;
}

static void test_buffer_configuration(void) {
    uint32_t args_registers[64] = {0u};
    uint32_t exec_registers[128] = {0u};
    adas_classifier_accelerator_t accelerator;
    adas_classifier_accelerator_init(&accelerator);
    attach_fake_region(
        &accelerator.args_region,
        args_registers,
        sizeof(args_registers)
    );
    attach_fake_region(
        &accelerator.exec_region,
        exec_registers,
        sizeof(exec_registers)
    );

    const adas_classifier_buffer_addresses_t addresses = {
        .ifmap = (uintptr_t)0x10001000u,
        .w_conv0 = (uintptr_t)0x10009000u,
        .w_conv1 = (uintptr_t)0x1000a000u,
        .w_conv2 = (uintptr_t)0x1000c000u,
        .output = (uintptr_t)0x10011000u
    };

    assert(adas_classifier_accelerator_configure_buffers(
        &accelerator,
        &addresses
    ) == ADAS_CLASSIFIER_OK);

    assert(register_value(args_registers, ADAS_CLASSIFIER_ARGS_IFMAP_LOW)
        == (uint32_t)addresses.ifmap);
    assert(register_value(args_registers, ADAS_CLASSIFIER_ARGS_IFMAP_HIGH) == 0u);
    assert(register_value(args_registers, ADAS_CLASSIFIER_ARGS_W_CONV0_LOW)
        == (uint32_t)addresses.w_conv0);
    assert(register_value(args_registers, ADAS_CLASSIFIER_ARGS_W_CONV1_LOW)
        == (uint32_t)addresses.w_conv1);
    assert(register_value(args_registers, ADAS_CLASSIFIER_ARGS_W_CONV2_LOW)
        == (uint32_t)addresses.w_conv2);
    assert(register_value(args_registers, ADAS_CLASSIFIER_ARGS_OUTPUT_LOW)
        == (uint32_t)addresses.output);

    adas_classifier_buffer_addresses_t invalid = addresses;
    invalid.output += 1u;
    assert(adas_classifier_accelerator_configure_buffers(
        &accelerator,
        &invalid
    ) == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    adas_classifier_accelerator_init(&accelerator);
}

static void test_parameter_programming(void) {
    uint32_t args_registers[64] = {0u};
    uint32_t exec_registers[128] = {0u};
    adas_classifier_accelerator_t accelerator;
    adas_classifier_accelerator_init(&accelerator);
    attach_fake_region(
        &accelerator.args_region,
        args_registers,
        sizeof(args_registers)
    );
    attach_fake_region(
        &accelerator.exec_region,
        exec_registers,
        sizeof(exec_registers)
    );

    adas_classifier_parameters_t parameters = {0};
    for (size_t i = 0u; i < ADAS_CLASSIFIER_B_CONV0_COUNT; ++i) {
        parameters.b_conv0[i] = (int32_t)i - 8;
    }
    for (size_t i = 0u; i < ADAS_CLASSIFIER_B_CONV1_COUNT; ++i) {
        parameters.b_conv1[i] = (int32_t)i - 16;
    }
    for (size_t i = 0u; i < ADAS_CLASSIFIER_B_CONV2_COUNT; ++i) {
        parameters.b_conv2[i] = (int32_t)i - 32;
    }
    parameters.rq_conv0.multiplier = -1234567;
    parameters.rq_conv0.shift = 7u;
    parameters.rq_conv1.multiplier = 2345678;
    parameters.rq_conv1.shift = 11u;
    parameters.rq_conv2.multiplier = -3456789;
    parameters.rq_conv2.shift = 13u;

    assert(adas_classifier_accelerator_load_parameters(
        &accelerator,
        &parameters
    ) == ADAS_CLASSIFIER_OK);

    assert(register_value(exec_registers, ADAS_CLASSIFIER_EXEC_RQ_CONV0_LOW)
        == (uint32_t)parameters.rq_conv0.multiplier);
    assert(register_value(exec_registers, ADAS_CLASSIFIER_EXEC_RQ_CONV0_HIGH)
        == parameters.rq_conv0.shift);
    assert(register_value(exec_registers, ADAS_CLASSIFIER_EXEC_RQ_CONV1_LOW)
        == (uint32_t)parameters.rq_conv1.multiplier);
    assert(register_value(exec_registers, ADAS_CLASSIFIER_EXEC_RQ_CONV2_HIGH)
        == parameters.rq_conv2.shift);

    for (size_t i = 0u; i < ADAS_CLASSIFIER_B_CONV0_COUNT; ++i) {
        assert(register_value(
            exec_registers,
            ADAS_CLASSIFIER_EXEC_B_CONV0_BASE + i * sizeof(uint32_t)
        ) == (uint32_t)parameters.b_conv0[i]);
    }
    for (size_t i = 0u; i < ADAS_CLASSIFIER_B_CONV1_COUNT; ++i) {
        assert(register_value(
            exec_registers,
            ADAS_CLASSIFIER_EXEC_B_CONV1_BASE + i * sizeof(uint32_t)
        ) == (uint32_t)parameters.b_conv1[i]);
    }
    for (size_t i = 0u; i < ADAS_CLASSIFIER_B_CONV2_COUNT; ++i) {
        assert(register_value(
            exec_registers,
            ADAS_CLASSIFIER_EXEC_B_CONV2_BASE + i * sizeof(uint32_t)
        ) == (uint32_t)parameters.b_conv2[i]);
    }

    adas_classifier_accelerator_init(&accelerator);
}

static void test_start_done_and_timeout(void) {
    uint32_t args_registers[64] = {0u};
    uint32_t exec_registers[128] = {0u};
    adas_classifier_accelerator_t accelerator;
    adas_classifier_accelerator_init(&accelerator);
    attach_fake_region(
        &accelerator.args_region,
        args_registers,
        sizeof(args_registers)
    );
    attach_fake_region(
        &accelerator.exec_region,
        exec_registers,
        sizeof(exec_registers)
    );

    assert(adas_classifier_accelerator_start(&accelerator)
        == ADAS_CLASSIFIER_OK);
    assert(register_value(exec_registers, ADAS_CLASSIFIER_EXEC_AP_CTRL)
        == ADAS_CLASSIFIER_AP_START_MASK);

    exec_registers[ADAS_CLASSIFIER_EXEC_AP_CTRL / sizeof(uint32_t)] =
        ADAS_CLASSIFIER_AP_DONE_MASK;
    assert(adas_classifier_accelerator_wait_done(&accelerator, 10u)
        == ADAS_CLASSIFIER_OK);

    exec_registers[ADAS_CLASSIFIER_EXEC_AP_CTRL / sizeof(uint32_t)] = 0u;
    assert(adas_classifier_accelerator_wait_done(&accelerator, 1u)
        == ADAS_CLASSIFIER_TIMEOUT);
    assert(adas_classifier_accelerator_wait_done(&accelerator, 0u)
        == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    adas_classifier_accelerator_init(&accelerator);
}

static void test_unopened_accelerator(void) {
    adas_classifier_accelerator_t accelerator;
    adas_classifier_accelerator_init(&accelerator);

    const adas_classifier_buffer_addresses_t addresses = {0};
    const adas_classifier_parameters_t parameters = {0};
    assert(adas_classifier_accelerator_configure_buffers(
        &accelerator,
        &addresses
    ) == ADAS_CLASSIFIER_INVALID_ARGUMENT);
    assert(adas_classifier_accelerator_load_parameters(
        &accelerator,
        &parameters
    ) == ADAS_CLASSIFIER_INVALID_ARGUMENT);
    assert(adas_classifier_accelerator_start(&accelerator)
        == ADAS_CLASSIFIER_INVALID_ARGUMENT);

    adas_classifier_accelerator_close(&accelerator);
}

int main(void) {
    test_buffer_configuration();
    test_parameter_programming();
    test_start_done_and_timeout();
    test_unopened_accelerator();

    puts("Classifier accelerator tests passed");
    return EXIT_SUCCESS;
}
