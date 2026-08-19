#define _POSIX_C_SOURCE 200809L

#include "driver/classifier_device.h"
#include "model/classifier_model.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum {
    GOLDEN_INPUT_BYTES = ADAS_CLASSIFIER_IFMAP_SIZE,
    GOLDEN_OUTPUT_BYTES = ADAS_CLASSIFIER_OUTPUT_SIZE,
    MODEL_CLASS_COUNT = 6
};

static int join_path(char* output, size_t capacity,
                     const char* directory, const char* name) {
    const int length = snprintf(output, capacity, "%s/%s", directory, name);
    return length >= 0 && (size_t)length < capacity ? 0 : -1;
}

static int write_exact_file(const char* path, const void* data, size_t size) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return -1;
    const int result = fwrite(data, 1u, size, file) == size ? 0 : -1;
    if (fclose(file) != 0) return -1;
    return result;
}

/* NumPy v1/v2, C-contiguous signed INT8 배열의 raw payload만 읽는다. */
static int read_npy_int8(const char* path, int8_t* output, size_t size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return -1;

    uint8_t prefix[12] = {0};
    if (fread(prefix, 1u, sizeof(prefix), file) != sizeof(prefix)
        || memcmp(prefix, "\x93NUMPY", 6u) != 0) {
        fclose(file);
        return -1;
    }

    uint32_t header_size = 0u;
    size_t prefix_size = 0u;
    if (prefix[6] == 1u) {
        header_size = (uint32_t)prefix[8] | ((uint32_t)prefix[9] << 8u);
        prefix_size = 10u;
    } else if (prefix[6] == 2u || prefix[6] == 3u) {
        header_size = (uint32_t)prefix[8]
            | ((uint32_t)prefix[9] << 8u)
            | ((uint32_t)prefix[10] << 16u)
            | ((uint32_t)prefix[11] << 24u);
        prefix_size = 12u;
    } else {
        fclose(file);
        return -1;
    }

    if (fseek(file, (long)prefix_size, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    char* header = (char*)malloc((size_t)header_size + 1u);
    if (header == NULL) {
        fclose(file);
        return -1;
    }
    const int header_ok = fread(header, 1u, header_size, file) == header_size;
    header[header_size] = '\0';
    const int format_ok = header_ok
        && (strstr(header, "'descr': '|i1'") != NULL
            || strstr(header, "'descr': '<i1'") != NULL)
        && strstr(header, "'fortran_order': False") != NULL;
    free(header);
    if (!format_ok) {
        fclose(file);
        return -1;
    }

    const size_t count = fread(output, 1u, size, file);
    const int extra = fgetc(file);
    const int result = count == size && extra == EOF && !ferror(file) ? 0 : -1;
    fclose(file);
    return result;
}

static char* read_text_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    const long length = ftell(file);
    if (length < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char* text = (char*)malloc((size_t)length + 1u);
    if (text == NULL || fread(text, 1u, (size_t)length, file) != (size_t)length) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[length] = '\0';
    fclose(file);
    return text;
}

static int parse_next_integer(const char** cursor, const char* key,
                              int64_t* value) {
    const char* found = strstr(*cursor, key);
    if (found == NULL) return -1;
    found = strchr(found + strlen(key), ':');
    if (found == NULL) return -1;
    ++found;
    errno = 0;
    char* end = NULL;
    const long long parsed = strtoll(found, &end, 10);
    if (errno != 0 || end == found) return -1;
    *cursor = end;
    *value = (int64_t)parsed;
    return 0;
}

static int read_requant_manifest(const char* model_dir,
                                 adas_classifier_model_metadata_t* metadata) {
    char path[4096];
    if (join_path(path, sizeof(path), model_dir, "manifest.json") != 0)
        return -1;
    char* text = read_text_file(path);
    if (text == NULL) return -1;

    const char* cursor = text;
    int64_t multiplier[3] = {0};
    int64_t shift[3] = {0};
    int result = 0;
    for (size_t i = 0u; i < 3u; ++i) {
        if (parse_next_integer(&cursor, "\"requant_multiplier\"",
                               &multiplier[i]) != 0
            || parse_next_integer(&cursor, "\"requant_shift\"",
                                  &shift[i]) != 0
            || multiplier[i] < INT32_MIN || multiplier[i] > INT32_MAX
            || shift[i] < 0 || shift[i] > UINT8_MAX) {
            result = -1;
            break;
        }
    }
    free(text);
    if (result != 0) return -1;

    metadata->class_count = MODEL_CLASS_COUNT;
    metadata->gap_divisor = 1u;
    metadata->rq_conv0.multiplier = (int32_t)multiplier[0];
    metadata->rq_conv0.shift = (uint8_t)shift[0];
    metadata->rq_conv1.multiplier = (int32_t)multiplier[1];
    metadata->rq_conv1.shift = (uint8_t)shift[1];
    metadata->rq_conv2.multiplier = (int32_t)multiplier[2];
    metadata->rq_conv2.shift = (uint8_t)shift[2];
    return 0;
}

static uint64_t elapsed_us(const struct timespec* start,
                           const struct timespec* end) {
    const int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    const int64_t nanoseconds =
        (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    return (uint64_t)(seconds * 1000000 + nanoseconds / 1000);
}

static int ensure_directory(const char* path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void print_usage(const char* program) {
    fprintf(stderr,
        "usage: %s <model-dir> [device=/dev/adas_classifier] "
        "[report-dir=golden_report] [timeout-ms=2000]\n", program);
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char* model_dir = argv[1];
    const char* device_path = argc >= 3 ? argv[2] : "/dev/adas_classifier";
    const char* report_dir = argc >= 4 ? argv[3] : "golden_report";
    char* timeout_end = NULL;
    errno = 0;
    const unsigned long timeout = argc >= 5 ? strtoul(argv[4], &timeout_end, 10)
                                             : 2000ul;
    if (timeout == 0ul || timeout > UINT32_MAX || errno != 0
        || (argc >= 5 && (timeout_end == argv[4] || *timeout_end != '\0'))) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    char input_path[4096];
    char expected_path[4096];
    if (join_path(input_path, sizeof(input_path), model_dir,
                  "golden_input_98x98x3_int8.npy") != 0
        || join_path(expected_path, sizeof(expected_path), model_dir,
                     "golden_conv2_pool.npy") != 0) {
        fprintf(stderr, "model path is too long\n");
        return EXIT_FAILURE;
    }
    int8_t golden_input[GOLDEN_INPUT_BYTES];
    int8_t expected_output[GOLDEN_OUTPUT_BYTES];
    if (read_npy_int8(input_path, golden_input, sizeof(golden_input)) != 0
        || read_npy_int8(expected_path, expected_output,
                         sizeof(expected_output)) != 0) {
        fprintf(stderr, "failed to read INT8 golden npy files\n");
        return EXIT_FAILURE;
    }

    adas_classifier_model_metadata_t metadata;
    if (read_requant_manifest(model_dir, &metadata) != 0) {
        fprintf(stderr, "failed to parse requant values from manifest.json\n");
        return EXIT_FAILURE;
    }
    adas_classifier_model_t model;
    adas_classifier_model_init(&model);
    if (adas_classifier_model_load(&model, model_dir, &metadata)
        != ADAS_CLASSIFIER_MODEL_OK) {
        fprintf(stderr, "failed to load model binaries\n");
        return EXIT_FAILURE;
    }

    adas_classifier_device_t device;
    adas_classifier_device_init(&device);
    if (adas_classifier_device_open(&device, device_path)
        != ADAS_CLASSIFIER_DEVICE_OK) {
        fprintf(stderr, "failed to open %s: %s\n", device_path,
                strerror(errno));
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }
    memcpy(adas_classifier_device_ifmap(&device), golden_input,
           sizeof(golden_input));
    memcpy(adas_classifier_device_w_conv0(&device), model.w_conv0,
           sizeof(model.w_conv0));
    memcpy(adas_classifier_device_w_conv1(&device), model.w_conv1,
           sizeof(model.w_conv1));
    memcpy(adas_classifier_device_w_conv2(&device), model.w_conv2,
           sizeof(model.w_conv2));
    memset(adas_classifier_device_output(&device), 0xa5, GOLDEN_OUTPUT_BYTES);

    if (adas_classifier_device_load_parameters(&device, &model.pl_parameters)
        != ADAS_CLASSIFIER_DEVICE_OK) {
        fprintf(stderr, "failed to load PL parameters: %s\n", strerror(errno));
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }

    struct timespec start;
    struct timespec end;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    const adas_classifier_device_status_t run_status =
        adas_classifier_device_run(&device, (uint32_t)timeout);
    const int run_errno = errno;
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    const uint64_t duration_us = elapsed_us(&start, &end);

    if (ensure_directory(report_dir) != 0) {
        fprintf(stderr, "failed to create report directory: %s\n", strerror(errno));
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }
    char actual_path[4096];
    char comparison_path[4096];
    if (join_path(actual_path, sizeof(actual_path), report_dir,
                  "actual_output.bin") != 0
        || join_path(comparison_path, sizeof(comparison_path), report_dir,
                     "comparison.txt") != 0) {
        fprintf(stderr, "report path is too long\n");
        adas_classifier_device_close(&device);
        adas_classifier_model_unload(&model);
        return EXIT_FAILURE;
    }
    const int8_t* actual = adas_classifier_device_output(&device);
    (void)write_exact_file(actual_path, actual, GOLDEN_OUTPUT_BYTES);

    size_t mismatch_count = 0u;
    size_t first_mismatch = SIZE_MAX;
    for (size_t i = 0u; i < GOLDEN_OUTPUT_BYTES; ++i) {
        if (actual[i] != expected_output[i]) {
            if (first_mismatch == SIZE_MAX) first_mismatch = i;
            ++mismatch_count;
        }
    }

    FILE* report = fopen(comparison_path, "w");
    if (report != NULL) {
        fprintf(report, "run_status=%d\nerrno=%d\nduration_us=%" PRIu64 "\n",
                (int)run_status, run_errno, duration_us);
        fprintf(report, "output_bytes=%u\nmismatch_count=%zu\n",
                GOLDEN_OUTPUT_BYTES, mismatch_count);
        if (first_mismatch != SIZE_MAX) {
            fprintf(report,
                    "first_mismatch=%zu\nexpected=%d\nactual=%d\n",
                    first_mismatch, (int)expected_output[first_mismatch],
                    (int)actual[first_mismatch]);
        }
        fclose(report);
    }

    if (run_status != ADAS_CLASSIFIER_DEVICE_OK) {
        fprintf(stderr, "FAIL: accelerator run failed after %" PRIu64
                        " us: %s\n", duration_us, strerror(run_errno));
    } else if (mismatch_count != 0u) {
        fprintf(stderr,
                "FAIL: %zu/%u bytes differ; first[%zu] expected=%d actual=%d\n",
                mismatch_count, GOLDEN_OUTPUT_BYTES, first_mismatch,
                (int)expected_output[first_mismatch], (int)actual[first_mismatch]);
    } else {
        printf("PASS: %u bytes bit-exact, accelerator time=%" PRIu64 " us\n",
               GOLDEN_OUTPUT_BYTES, duration_us);
    }
    printf("report: %s\n", report_dir);

    adas_classifier_device_close(&device);
    adas_classifier_model_unload(&model);
    return run_status == ADAS_CLASSIFIER_DEVICE_OK && mismatch_count == 0u
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
