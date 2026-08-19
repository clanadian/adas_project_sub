// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include "adas_classifier_uapi.h"

#define REG_IFMAP_LO    0x10
#define REG_IFMAP_HI    0x14
#define REG_W0_LO       0x1c
#define REG_W0_HI       0x20
#define REG_W1_LO       0x28
#define REG_W1_HI       0x2c
#define REG_W2_LO       0x34
#define REG_W2_HI       0x38
#define REG_OUTPUT_LO   0x40
#define REG_OUTPUT_HI   0x44

#define REG_AP_CTRL     0x000
#define REG_RQ0_MUL     0x010
#define REG_RQ0_SHIFT   0x014
#define REG_RQ1_MUL     0x01c
#define REG_RQ1_SHIFT   0x020
#define REG_RQ2_MUL     0x028
#define REG_RQ2_SHIFT   0x02c
#define REG_B0          0x040
#define REG_B1          0x080
#define REG_B2          0x100
#define AP_START         BIT(0)
#define AP_DONE          BIT(1)

struct adas_classifier_dev {
    struct device *dev;
    void __iomem *args;
    void __iomem *exec;
    void *dma_cpu;
    dma_addr_t dma_handle;
    struct miscdevice miscdev;
    struct mutex lock;
    bool parameters_set;
};

static void write_dma_address(void __iomem *base, u32 lo, u32 hi,
                              dma_addr_t address)
{
    writel(lower_32_bits(address), base + lo);
    writel(upper_32_bits(address), base + hi);
}

static void configure_dma_addresses(struct adas_classifier_dev *classifier)
{
    dma_addr_t base = classifier->dma_handle;

    write_dma_address(classifier->args, REG_IFMAP_LO, REG_IFMAP_HI,
                      base + ADAS_CLASSIFIER_IFMAP_OFFSET);
    write_dma_address(classifier->args, REG_W0_LO, REG_W0_HI,
                      base + ADAS_CLASSIFIER_W_CONV0_OFFSET);
    write_dma_address(classifier->args, REG_W1_LO, REG_W1_HI,
                      base + ADAS_CLASSIFIER_W_CONV1_OFFSET);
    write_dma_address(classifier->args, REG_W2_LO, REG_W2_HI,
                      base + ADAS_CLASSIFIER_W_CONV2_OFFSET);
    write_dma_address(classifier->args, REG_OUTPUT_LO, REG_OUTPUT_HI,
                      base + ADAS_CLASSIFIER_OUTPUT_OFFSET);
}

static void write_bias(void __iomem *base, u32 offset,
                       const __s32 *bias, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i)
        writel((u32)bias[i], base + offset + i * sizeof(u32));
}

static long set_parameters(struct adas_classifier_dev *classifier,
                           void __user *argument)
{
    struct adas_classifier_parameters_uapi parameters;

    if (copy_from_user(&parameters, argument, sizeof(parameters)))
        return -EFAULT;
    if (parameters.requant[0].shift > 63 ||
        parameters.requant[1].shift > 63 ||
        parameters.requant[2].shift > 63)
        return -EINVAL;

    writel((u32)parameters.requant[0].multiplier,
            classifier->exec + REG_RQ0_MUL);
    writel(parameters.requant[0].shift, classifier->exec + REG_RQ0_SHIFT);
    writel((u32)parameters.requant[1].multiplier,
            classifier->exec + REG_RQ1_MUL);
    writel(parameters.requant[1].shift, classifier->exec + REG_RQ1_SHIFT);
    writel((u32)parameters.requant[2].multiplier,
            classifier->exec + REG_RQ2_MUL);
    writel(parameters.requant[2].shift, classifier->exec + REG_RQ2_SHIFT);
    write_bias(classifier->exec, REG_B0, parameters.bias_conv0, 16);
    write_bias(classifier->exec, REG_B1, parameters.bias_conv1, 32);
    write_bias(classifier->exec, REG_B2, parameters.bias_conv2, 64);
    classifier->parameters_set = true;
    return 0;
}

static long run_classifier(struct adas_classifier_dev *classifier,
                           void __user *argument)
{
    struct adas_classifier_run_uapi run;
    unsigned long deadline;

    if (copy_from_user(&run, argument, sizeof(run)))
        return -EFAULT;
    if (!classifier->parameters_set || run.timeout_ms == 0)
        return -EINVAL;

    configure_dma_addresses(classifier);
    writel(AP_START, classifier->exec + REG_AP_CTRL);
    deadline = jiffies + msecs_to_jiffies(run.timeout_ms);

    do {
        if (readl(classifier->exec + REG_AP_CTRL) & AP_DONE)
            return 0;
        usleep_range(50, 100);
    } while (time_before(jiffies, deadline));

    return -ETIMEDOUT;
}

static long adas_classifier_ioctl(struct file *file, unsigned int command,
                                  unsigned long argument)
{
    struct miscdevice *misc = file->private_data;
    struct adas_classifier_dev *classifier =
        container_of(misc, struct adas_classifier_dev, miscdev);
    void __user *user_argument = (void __user *)argument;
    long result;

    if (_IOC_TYPE(command) != ADAS_CLASSIFIER_IOC_MAGIC)
        return -ENOTTY;

    mutex_lock(&classifier->lock);
    switch (command) {
    case ADAS_CLASSIFIER_IOC_GET_INFO: {
        const struct adas_classifier_info_uapi info = {
            .abi_version = ADAS_CLASSIFIER_ABI_VERSION,
            .dma_span = ADAS_CLASSIFIER_DMA_SPAN,
            .ifmap_offset = ADAS_CLASSIFIER_IFMAP_OFFSET,
            .output_offset = ADAS_CLASSIFIER_OUTPUT_OFFSET,
        };
        result = copy_to_user(user_argument, &info, sizeof(info))
            ? -EFAULT : 0;
        break;
    }
    case ADAS_CLASSIFIER_IOC_SET_PARAMETERS:
        result = set_parameters(classifier, user_argument);
        break;
    case ADAS_CLASSIFIER_IOC_RUN:
        result = run_classifier(classifier, user_argument);
        break;
    default:
        result = -ENOTTY;
        break;
    }
    mutex_unlock(&classifier->lock);
    return result;
}

static int adas_classifier_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct miscdevice *misc = file->private_data;
    struct adas_classifier_dev *classifier =
        container_of(misc, struct adas_classifier_dev, miscdev);
    size_t requested = vma->vm_end - vma->vm_start;

    if (vma->vm_pgoff != 0 || requested > ADAS_CLASSIFIER_DMA_SPAN)
        return -EINVAL;
    return dma_mmap_coherent(classifier->dev, vma, classifier->dma_cpu,
                             classifier->dma_handle,
                             ADAS_CLASSIFIER_DMA_SPAN);
}

static const struct file_operations adas_classifier_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = adas_classifier_ioctl,
    .mmap = adas_classifier_mmap,
    .llseek = no_llseek,
};

static int adas_classifier_probe(struct platform_device *pdev)
{
    struct adas_classifier_dev *classifier;
    int result;

    classifier = devm_kzalloc(&pdev->dev, sizeof(*classifier), GFP_KERNEL);
    if (!classifier)
        return -ENOMEM;
    classifier->dev = &pdev->dev;
    mutex_init(&classifier->lock);

    result = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
    if (result)
        return dev_err_probe(&pdev->dev, result, "32-bit DMA unavailable\n");

    classifier->args = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(classifier->args))
        return PTR_ERR(classifier->args);
    classifier->exec = devm_platform_ioremap_resource(pdev, 1);
    if (IS_ERR(classifier->exec))
        return PTR_ERR(classifier->exec);

    classifier->dma_cpu = dma_alloc_coherent(
        &pdev->dev, ADAS_CLASSIFIER_DMA_SPAN,
        &classifier->dma_handle, GFP_KERNEL);
    if (!classifier->dma_cpu)
        return -ENOMEM;
    memset(classifier->dma_cpu, 0, ADAS_CLASSIFIER_DMA_SPAN);

    classifier->miscdev.minor = MISC_DYNAMIC_MINOR;
    classifier->miscdev.name = ADAS_CLASSIFIER_DEVICE_NAME;
    classifier->miscdev.fops = &adas_classifier_fops;
    classifier->miscdev.parent = &pdev->dev;
    result = misc_register(&classifier->miscdev);
    if (result) {
        dma_free_coherent(&pdev->dev, ADAS_CLASSIFIER_DMA_SPAN,
                          classifier->dma_cpu, classifier->dma_handle);
        return result;
    }

    platform_set_drvdata(pdev, classifier);
    dev_info(&pdev->dev, "DMA buffer at %pad, size %#x\n",
             &classifier->dma_handle, ADAS_CLASSIFIER_DMA_SPAN);
    return 0;
}

static int adas_classifier_remove(struct platform_device *pdev)
{
    struct adas_classifier_dev *classifier = platform_get_drvdata(pdev);

    misc_deregister(&classifier->miscdev);
    dma_free_coherent(&pdev->dev, ADAS_CLASSIFIER_DMA_SPAN,
                      classifier->dma_cpu, classifier->dma_handle);
    return 0;
}

static const struct of_device_id adas_classifier_of_match[] = {
    { .compatible = "adas,classifier-1.0" },
    { }
};
MODULE_DEVICE_TABLE(of, adas_classifier_of_match);

static struct platform_driver adas_classifier_driver = {
    .probe = adas_classifier_probe,
    .remove = adas_classifier_remove,
    .driver = {
        .name = "adas_classifier",
        .of_match_table = adas_classifier_of_match,
    },
};
module_platform_driver(adas_classifier_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ADAS ROI classifier coherent-DMA driver");
MODULE_AUTHOR("ADAS project team");
