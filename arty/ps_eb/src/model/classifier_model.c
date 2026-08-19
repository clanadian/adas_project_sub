#include "model/classifier_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADAS_MODEL_PATH_CAPACITY 4096u

/* directory와 filename을 안전하게 결합합니다. */
static adas_classifier_model_status_t make_path(
    char path[ADAS_MODEL_PATH_CAPACITY],
    const char* directory,
    const char* filename
) {
    const size_t directory_length = strlen(directory);
    const char* separator =
        directory_length > 0u && directory[directory_length - 1u] == '/'
        ? ""
        : "/";

    const int length = snprintf(
        path,
        ADAS_MODEL_PATH_CAPACITY,
        "%s%s%s",
        directory,
        separator,
        filename
    );
    if (length < 0 || (size_t)length >= ADAS_MODEL_PATH_CAPACITY) {
        return ADAS_CLASSIFIER_MODEL_INVALID_ARGUMENT;
    }
    return ADAS_CLASSIFIER_MODEL_OK;
}

/* 파일이 기대 크기와 정확히 일치할 때만 destination에 읽습니다. */
static adas_classifier_model_status_t read_exact_file(
    const char* directory,
    const char* filename,
    void* destination,
    size_t expected_size
) {
    char path[ADAS_MODEL_PATH_CAPACITY];
    adas_classifier_model_status_t status =
        make_path(path, directory, filename);
    if (status != ADAS_CLASSIFIER_MODEL_OK) {
        return status;
    }

    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return ADAS_CLASSIFIER_MODEL_IO_ERROR;
    }

    /* expected_size 바이트를 먼저 읽습니다. */
    const size_t read_size = fread(destination, 1u, expected_size, file);
    if (read_size != expected_size) {
        const int had_error = ferror(file);
        fclose(file);
        return had_error
            ? ADAS_CLASSIFIER_MODEL_IO_ERROR
            : ADAS_CLASSIFIER_MODEL_SIZE_MISMATCH;
    }

    /* 한 바이트라도 더 있으면 잘못된 모델 파일로 판정합니다. */
    const int trailing_byte = fgetc(file);
    if (trailing_byte != EOF) {
        fclose(file);
        return ADAS_CLASSIFIER_MODEL_SIZE_MISMATCH;
    }
    if (ferror(file)) {
        fclose(file);
        return ADAS_CLASSIFIER_MODEL_IO_ERROR;
    }

    if (fclose(file) != 0) {
        return ADAS_CLASSIFIER_MODEL_IO_ERROR;
    }
    return ADAS_CLASSIFIER_MODEL_OK;
}

void adas_classifier_model_init(adas_classifier_model_t* model) {
    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
}

adas_classifier_model_status_t adas_classifier_model_load(
    adas_classifier_model_t* model,
    const char* directory,
    const adas_classifier_model_metadata_t* metadata
) {
    if (model == NULL
        || directory == NULL
        || directory[0] == '\0'
        || metadata == NULL
        || metadata->class_count == 0u
        || metadata->gap_divisor == 0u
        || model->fc_weights != NULL
        || model->fc_biases != NULL) {
        return ADAS_CLASSIFIER_MODEL_INVALID_ARGUMENT;
    }

    /* class_count*64 및 class_count*sizeof(int32_t)의 크기 넘침을 막습니다. */
    if (metadata->class_count > SIZE_MAX / ADAS_CLASSIFIER_OUTPUT_CHANNELS
        || metadata->class_count > SIZE_MAX / sizeof(int32_t)) {
        return ADAS_CLASSIFIER_MODEL_INVALID_ARGUMENT;
    }

    const size_t fc_weight_size =
        metadata->class_count * ADAS_CLASSIFIER_OUTPUT_CHANNELS;
    const size_t fc_bias_size = metadata->class_count * sizeof(int32_t);

    model->fc_weights = (int8_t*)malloc(fc_weight_size);
    model->fc_biases = (int32_t*)malloc(fc_bias_size);
    if (model->fc_weights == NULL || model->fc_biases == NULL) {
        adas_classifier_model_unload(model);
        return ADAS_CLASSIFIER_MODEL_OUT_OF_MEMORY;
    }

    /* PL용 Conv weight 세 개를 파일 크기까지 검증하며 읽습니다. */
    adas_classifier_model_status_t status = read_exact_file(
        directory, "w_conv0.bin", model->w_conv0, sizeof(model->w_conv0));
    if (status == ADAS_CLASSIFIER_MODEL_OK) {
        status = read_exact_file(
            directory, "w_conv1.bin", model->w_conv1, sizeof(model->w_conv1));
    }
    if (status == ADAS_CLASSIFIER_MODEL_OK) {
        status = read_exact_file(
            directory, "w_conv2.bin", model->w_conv2, sizeof(model->w_conv2));
    }

    /* PL 레지스터로 전달할 Conv bias 세 개를 읽습니다. */
    if (status == ADAS_CLASSIFIER_MODEL_OK) {
        status = read_exact_file(
            directory,
            "b_conv0.bin",
            model->b_conv0,
            sizeof(model->b_conv0)
        );
    }
    if (status == ADAS_CLASSIFIER_MODEL_OK) {
        status = read_exact_file(
            directory,
            "b_conv1.bin",
            model->b_conv1,
            sizeof(model->b_conv1)
        );
    }
    if (status == ADAS_CLASSIFIER_MODEL_OK) {
        status = read_exact_file(
            directory,
            "b_conv2.bin",
            model->b_conv2,
            sizeof(model->b_conv2)
        );
    }

    /* PS에서 GAP 뒤에 사용할 FC weight와 bias를 읽습니다. */
    if (status == ADAS_CLASSIFIER_MODEL_OK) {
        status = read_exact_file(
            directory, "fc_weight.bin", model->fc_weights, fc_weight_size);
    }
    if (status == ADAS_CLASSIFIER_MODEL_OK) {
        status = read_exact_file(
            directory, "fc_bias.bin", model->fc_biases, fc_bias_size);
    }

    /* 하나라도 실패하면 부분적으로 읽힌 모델을 사용할 수 없게 정리합니다. */
    if (status != ADAS_CLASSIFIER_MODEL_OK) {
        adas_classifier_model_unload(model);
        return status;
    }

    /* manifest 대신 전달받은 메타데이터를 런타임 모델에 복사합니다. */
    model->pl_parameters.rq_conv0 = metadata->rq_conv0;
    model->pl_parameters.rq_conv1 = metadata->rq_conv1;
    model->pl_parameters.rq_conv2 = metadata->rq_conv2;
    model->class_count = metadata->class_count;
    model->gap_divisor = metadata->gap_divisor;

    return ADAS_CLASSIFIER_MODEL_OK;
}

void adas_classifier_model_unload(adas_classifier_model_t* model) {
    if (model == NULL) {
        return;
    }

    free(model->fc_weights);
    free(model->fc_biases);
    /* 포인터와 메타데이터를 포함한 전체 구조체를 재사용 가능한 빈 상태로 만듭니다. */
    memset(model, 0, sizeof(*model));
}
