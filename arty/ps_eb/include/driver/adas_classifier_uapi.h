#ifndef ADAS_CLASSIFIER_UAPI_H
#define ADAS_CLASSIFIER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ADAS_CLASSIFIER_DEVICE_NAME "adas_classifier"

#define ADAS_CLASSIFIER_DMA_SPAN       0x13000u
#define ADAS_CLASSIFIER_IFMAP_OFFSET   0x00000u
#define ADAS_CLASSIFIER_W_CONV0_OFFSET 0x08000u
#define ADAS_CLASSIFIER_W_CONV1_OFFSET 0x09000u
#define ADAS_CLASSIFIER_W_CONV2_OFFSET 0x0b000u
#define ADAS_CLASSIFIER_OUTPUT_OFFSET  0x10000u

#define ADAS_CLASSIFIER_IFMAP_SIZE  (98u * 98u * 3u)
#define ADAS_CLASSIFIER_OUTPUT_SIZE (12u * 12u * 64u)

struct adas_classifier_requant_uapi {
    __s32 multiplier;
    __u32 shift;
};

struct adas_classifier_parameters_uapi {
    struct adas_classifier_requant_uapi requant[3];
    __s32 bias_conv0[16];
    __s32 bias_conv1[32];
    __s32 bias_conv2[64];
};

struct adas_classifier_run_uapi {
    __u32 timeout_ms;
    __u32 reserved;
};

struct adas_classifier_info_uapi {
    __u32 abi_version;
    __u32 dma_span;
    __u32 ifmap_offset;
    __u32 output_offset;
};

#define ADAS_CLASSIFIER_ABI_VERSION 1u
#define ADAS_CLASSIFIER_IOC_MAGIC 'A'
#define ADAS_CLASSIFIER_IOC_GET_INFO \
    _IOR(ADAS_CLASSIFIER_IOC_MAGIC, 0x00, struct adas_classifier_info_uapi)
#define ADAS_CLASSIFIER_IOC_SET_PARAMETERS \
    _IOW(ADAS_CLASSIFIER_IOC_MAGIC, 0x01, struct adas_classifier_parameters_uapi)
#define ADAS_CLASSIFIER_IOC_RUN \
    _IOW(ADAS_CLASSIFIER_IOC_MAGIC, 0x02, struct adas_classifier_run_uapi)

#endif
