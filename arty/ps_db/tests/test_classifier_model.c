#define _POSIX_C_SOURCE 200809L

#include "model/classifier_model.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_pattern_file(
    const char* directory,
    const char* filename,
    size_t size,
    uint8_t pattern
) {
    char path[512];
    assert(snprintf(path, sizeof(path), "%s/%s", directory, filename) > 0);

    FILE* file = fopen(path, "wb");
    assert(file != NULL);
    for (size_t index = 0u; index < size; ++index) {
        assert(fputc(pattern, file) != EOF);
    }
    assert(fclose(file) == 0);
}

static void create_model_files(const char* directory, size_t class_count) {
    write_pattern_file(directory, "w_conv0.bin", ADAS_CLASSIFIER_W_CONV0_SIZE, 1u);
    write_pattern_file(directory, "w_conv1.bin", ADAS_CLASSIFIER_W_CONV1_SIZE, 2u);
    write_pattern_file(directory, "w_conv2.bin", ADAS_CLASSIFIER_W_CONV2_SIZE, 3u);
    write_pattern_file(
        directory,
        "b_conv0.bin",
        ADAS_CLASSIFIER_B_CONV0_COUNT * sizeof(int32_t),
        4u
    );
    write_pattern_file(
        directory,
        "b_conv1.bin",
        ADAS_CLASSIFIER_B_CONV1_COUNT * sizeof(int32_t),
        5u
    );
    write_pattern_file(
        directory,
        "b_conv2.bin",
        ADAS_CLASSIFIER_B_CONV2_COUNT * sizeof(int32_t),
        6u
    );
    write_pattern_file(
        directory,
        "fc_weight.bin",
        class_count * ADAS_CLASSIFIER_OUTPUT_CHANNELS,
        7u
    );
    write_pattern_file(
        directory,
        "fc_bias.bin",
        class_count * sizeof(int32_t),
        8u
    );
}

static void remove_model_files(const char* directory) {
    const char* filenames[] = {
        "w_conv0.bin", "w_conv1.bin", "w_conv2.bin",
        "b_conv0.bin", "b_conv1.bin", "b_conv2.bin",
        "fc_weight.bin", "fc_bias.bin"
    };
    char path[512];
    for (size_t index = 0u;
         index < sizeof(filenames) / sizeof(filenames[0]);
         ++index) {
        assert(snprintf(
            path,
            sizeof(path),
            "%s/%s",
            directory,
            filenames[index]
        ) > 0);
        assert(unlink(path) == 0);
    }
}

static adas_classifier_model_metadata_t test_metadata(size_t class_count) {
    adas_classifier_model_metadata_t metadata = {0};
    metadata.class_count = class_count;
    metadata.gap_divisor = ADAS_CLASSIFIER_GAP_AREA;
    metadata.rq_conv0.multiplier = 101;
    metadata.rq_conv0.shift = 7u;
    metadata.rq_conv1.multiplier = 202;
    metadata.rq_conv1.shift = 8u;
    metadata.rq_conv2.multiplier = 303;
    metadata.rq_conv2.shift = 9u;
    return metadata;
}

static void test_load_and_unload(void) {
    char directory[] = "/tmp/adas-model-test-XXXXXX";
    assert(mkdtemp(directory) != NULL);

    const size_t class_count = 6u;
    create_model_files(directory, class_count);
    const adas_classifier_model_metadata_t metadata =
        test_metadata(class_count);

    adas_classifier_model_t model;
    adas_classifier_model_init(&model);
    assert(adas_classifier_model_load(&model, directory, &metadata)
        == ADAS_CLASSIFIER_MODEL_OK);

    assert(model.class_count == class_count);
    assert(model.gap_divisor == ADAS_CLASSIFIER_GAP_AREA);
    assert(model.w_conv0[0] == 1);
    assert(model.w_conv1[0] == 2);
    assert(model.w_conv2[0] == 3);
    assert(model.fc_weights[0] == 7);
    assert(model.pl_parameters.rq_conv2.multiplier == 303);

    adas_classifier_model_unload(&model);
    assert(model.fc_weights == NULL);
    assert(model.fc_biases == NULL);
    assert(model.class_count == 0u);

    remove_model_files(directory);
    assert(rmdir(directory) == 0);
}

static void test_size_mismatch(void) {
    char directory[] = "/tmp/adas-model-size-test-XXXXXX";
    assert(mkdtemp(directory) != NULL);

    const size_t class_count = 2u;
    create_model_files(directory, class_count);
    /* 정상 파일을 한 바이트 더 긴 파일로 덮어써서 크기 검사를 유도합니다. */
    write_pattern_file(
        directory,
        "w_conv0.bin",
        ADAS_CLASSIFIER_W_CONV0_SIZE + 1u,
        1u
    );

    const adas_classifier_model_metadata_t metadata =
        test_metadata(class_count);
    adas_classifier_model_t model;
    adas_classifier_model_init(&model);
    assert(adas_classifier_model_load(&model, directory, &metadata)
        == ADAS_CLASSIFIER_MODEL_SIZE_MISMATCH);
    assert(model.fc_weights == NULL);
    assert(model.fc_biases == NULL);

    remove_model_files(directory);
    assert(rmdir(directory) == 0);
}

int main(void) {
    test_load_and_unload();
    test_size_mismatch();

    puts("Classifier model loader tests passed");
    return EXIT_SUCCESS;
}
