// SPDX-License-Identifier: GPL-2.0
/*
 * ADAS ROI classifier — EB PL 드라이버.
 *
 * DB 판과 하드웨어가 다르다. DB 는 IP 하나가 ap_start 한 번으로 전체를 돌고
 * bias·requant 를 AXI-Lite 레지스터로 받았다. EB 는 엔진 3개
 * (conv_engine / conv0_engine / maxpool_engine)를 **6번 나눠 기동**하고,
 * bias 와 가중치는 DDR 주소로 넘긴다.
 *
 * 6-op 시퀀스를 커널에 두는 이유: 사용자 공간이 op 마다 ioctl 을 돌면
 * ROI 한 건에 왕복이 6번 생기고, 그 사이에 다른 프로세스가 같은 엔진을
 * 건드릴 여지가 열린다. RUN 하나가 mutex 아래에서 6개를 끝낸다.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include "adas_classifier_uapi.h"

struct adas_classifier_dev {
    struct device *dev;
    /* 엔진별 제어 창. reg-names 순서는 dtsi 가 정한다. */
    void __iomem *conv;
    void __iomem *conv0;
    void __iomem *maxpool;
    void *dma_cpu;
    dma_addr_t dma_handle;
    struct miscdevice miscdev;
    struct mutex lock;
    bool parameters_set;
    struct adas_classifier_requant_uapi requant[ADAS_EB_NUM_CONVS];
    struct adas_classifier_status_uapi status;
};

static void write_address(void __iomem *base, u32 lo, u32 hi, dma_addr_t address)
{
    writel(lower_32_bits(address), base + lo);
    writel(upper_32_bits(address), base + hi);
}

static void __iomem *region_for_kind(struct adas_classifier_dev *cls, unsigned kind)
{
    switch (kind) {
    case ADAS_EB_OP_CONV0:   return cls->conv0;
    case ADAS_EB_OP_CONV:    return cls->conv;
    case ADAS_EB_OP_MAXPOOL: return cls->maxpool;
    default:                 return NULL;
    }
}

/*
 * op 하나의 입력/출력.
 *   op0        : roi   -> act_a
 *   홀수 op    : act_a -> act_b
 *   짝수 op(>0): act_b -> act_a
 * op 이 6개(짝수)이므로 최종 출력은 항상 act_b 다.
 */
static void buffers_for_op(struct adas_classifier_dev *cls, unsigned index,
                           dma_addr_t *src, dma_addr_t *dst)
{
    dma_addr_t base = cls->dma_handle;

    if (index == 0) {
        *src = base + ADAS_CLASSIFIER_ROI_OFFSET;
        *dst = base + ADAS_CLASSIFIER_ACT_A_OFFSET;
        return;
    }
    if (index % 2 == 1) {
        *src = base + ADAS_CLASSIFIER_ACT_A_OFFSET;
        *dst = base + ADAS_CLASSIFIER_ACT_B_OFFSET;
    } else {
        *src = base + ADAS_CLASSIFIER_ACT_B_OFFSET;
        *dst = base + ADAS_CLASSIFIER_ACT_A_OFFSET;
    }
}

static void weights_for_conv(struct adas_classifier_dev *cls, unsigned conv_index,
                             dma_addr_t *weights, dma_addr_t *bias)
{
    dma_addr_t base = cls->dma_handle;

    switch (conv_index) {
    case 0:
        *weights = base + ADAS_CLASSIFIER_W_CONV0_OFFSET;
        *bias = base + ADAS_CLASSIFIER_B_CONV0_OFFSET;
        break;
    case 1:
        *weights = base + ADAS_CLASSIFIER_W_CONV1_OFFSET;
        *bias = base + ADAS_CLASSIFIER_B_CONV1_OFFSET;
        break;
    default:
        *weights = base + ADAS_CLASSIFIER_W_CONV2_OFFSET;
        *bias = base + ADAS_CLASSIFIER_B_CONV2_OFFSET;
        break;
    }
}

static void program_conv0(struct adas_classifier_dev *cls,
                          const struct adas_eb_op *op,
                          dma_addr_t src, dma_addr_t dst,
                          dma_addr_t weights, dma_addr_t bias)
{
    void __iomem *base = cls->conv0;
    const struct adas_classifier_requant_uapi *rq = &cls->requant[op->conv_index];

    write_address(base, ADAS_EB_CONV0_IFMAP_LO, ADAS_EB_CONV0_IFMAP_HI, src);
    write_address(base, ADAS_EB_CONV0_WEIGHTS_LO, ADAS_EB_CONV0_WEIGHTS_HI, weights);
    write_address(base, ADAS_EB_CONV0_BIAS_LO, ADAS_EB_CONV0_BIAS_HI, bias);
    write_address(base, ADAS_EB_CONV0_OFMAP_LO, ADAS_EB_CONV0_OFMAP_HI, dst);
    /* img_h/img_w 는 **이미 패딩된** 98x98 이다. pad 포트 자체가 없다. */
    writel(op->img_h, base + ADAS_EB_CONV0_IMG_H);
    writel(op->img_w, base + ADAS_EB_CONV0_IMG_W);
    writel((u32)rq->multiplier, base + ADAS_EB_CONV0_REQUANT_MUL);
    writel(rq->shift, base + ADAS_EB_CONV0_REQUANT_SHIFT);
    writel(rq->leaky, base + ADAS_EB_CONV0_LEAKY_ENABLE);
}

static void program_conv(struct adas_classifier_dev *cls,
                         const struct adas_eb_op *op,
                         dma_addr_t src, dma_addr_t dst,
                         dma_addr_t weights, dma_addr_t bias)
{
    void __iomem *base = cls->conv;
    const struct adas_classifier_requant_uapi *rq = &cls->requant[op->conv_index];

    write_address(base, ADAS_EB_CONV_IFMAP_LO, ADAS_EB_CONV_IFMAP_HI, src);
    write_address(base, ADAS_EB_CONV_WEIGHTS_LO, ADAS_EB_CONV_WEIGHTS_HI, weights);
    /*
     * weights_hi 는 같은 버퍼를 읽는 두 번째 AXI 포트다. 다른 주소를 주면
     * 에러 없이 틀린 결과가 나오므로 항상 weights 와 같은 값을 쓴다.
     */
    write_address(base, ADAS_EB_CONV_WEIGHTS2_LO, ADAS_EB_CONV_WEIGHTS2_HI, weights);
    write_address(base, ADAS_EB_CONV_BIAS_LO, ADAS_EB_CONV_BIAS_HI, bias);
    write_address(base, ADAS_EB_CONV_OFMAP_LO, ADAS_EB_CONV_OFMAP_HI, dst);
    writel(op->img_h, base + ADAS_EB_CONV_IMG_H);
    writel(op->img_w, base + ADAS_EB_CONV_IMG_W);
    writel(op->in_ch, base + ADAS_EB_CONV_IN_CH);
    writel(op->out_ch, base + ADAS_EB_CONV_OUT_CH);
    writel(op->k, base + ADAS_EB_CONV_K);
    /* stride 는 1 만 동작한다 (RTL 에 분기 없음). 표에도 1 만 들어 있다. */
    writel(op->stride, base + ADAS_EB_CONV_STRIDE);
    writel(op->pad, base + ADAS_EB_CONV_PAD);
    writel((u32)rq->multiplier, base + ADAS_EB_CONV_REQUANT_MUL);
    writel(rq->shift, base + ADAS_EB_CONV_REQUANT_SHIFT);
    writel(rq->leaky, base + ADAS_EB_CONV_LEAKY_ENABLE);
}

static void program_maxpool(struct adas_classifier_dev *cls,
                            const struct adas_eb_op *op,
                            dma_addr_t src, dma_addr_t dst)
{
    void __iomem *base = cls->maxpool;

    write_address(base, ADAS_EB_MAXPOOL_IFMAP_LO, ADAS_EB_MAXPOOL_IFMAP_HI, src);
    write_address(base, ADAS_EB_MAXPOOL_OFMAP_LO, ADAS_EB_MAXPOOL_OFMAP_HI, dst);
    writel(op->img_h, base + ADAS_EB_MAXPOOL_IMG_H);
    writel(op->img_w, base + ADAS_EB_MAXPOOL_IMG_W);
    writel(op->in_ch, base + ADAS_EB_MAXPOOL_CH);
    writel(op->stride, base + ADAS_EB_MAXPOOL_STRIDE);
    writel(0, base + ADAS_EB_MAXPOOL_PAD_RIGHT);
    writel(0, base + ADAS_EB_MAXPOOL_PAD_BOTTOM);
}

static int wait_for_bit(void __iomem *base, u32 mask, unsigned long deadline)
{
    do {
        if (readl(base + ADAS_EB_REG_CTRL) & mask)
            return 0;
        usleep_range(20, 50);
    } while (time_before(jiffies, deadline));

    return -ETIMEDOUT;
}

static long set_parameters(struct adas_classifier_dev *cls, void __user *argument)
{
    struct adas_classifier_parameters_uapi parameters;
    unsigned i;

    if (copy_from_user(&parameters, argument, sizeof(parameters)))
        return -EFAULT;

    for (i = 0; i < ADAS_EB_NUM_CONVS; i++) {
        if (parameters.requant[i].shift > 63)
            return -EINVAL;
        /*
         * multiplier 0 은 "manifest 값을 아직 안 넣었다"는 뜻이다. 그대로
         * 돌면 출력이 전부 0 이 되고, 그건 배선 오류처럼 보인다.
         */
        if (parameters.requant[i].multiplier == 0)
            return -EINVAL;
        if (parameters.requant[i].leaky > 1)
            return -EINVAL;
    }

    memcpy(cls->requant, parameters.requant, sizeof(cls->requant));
    cls->parameters_set = true;
    return 0;
}

/* op 하나를 프로그램하고 기동해 ap_done 까지 기다린다. */
static int run_one_op(struct adas_classifier_dev *cls, unsigned index,
                      unsigned long deadline)
{
    static const struct adas_eb_op ops[] = ADAS_EB_OP_TABLE_INITIALIZER;
    const struct adas_eb_op *op = &ops[index];
    void __iomem *base = region_for_kind(cls, op->kind);
    dma_addr_t src, dst, weights, bias;
    ktime_t started_op;
    int result;

    if (!base)
        return -EINVAL;

    cls->status.last_op = index;
    buffers_for_op(cls, index, &src, &dst);

    if (op->kind == ADAS_EB_OP_MAXPOOL) {
        program_maxpool(cls, op, src, dst);
    } else {
        weights_for_conv(cls, op->conv_index, &weights, &bias);
        if (op->kind == ADAS_EB_OP_CONV0)
            program_conv0(cls, op, src, dst, weights, bias);
        else
            program_conv(cls, op, src, dst, weights, bias);
    }

    /*
     * 시작 전에 idle 을 본다. 앞 op 이 아직 도는 중이면 ap_start 가
     * 무시되고, 그러면 이번 op 의 출력 버퍼에 옛 내용이 남는다 -
     * 실패가 아니라 조용히 틀린 결과가 된다.
     */
    result = wait_for_bit(base, ADAS_EB_AP_IDLE_MASK, deadline);
    if (result)
        return result;

    started_op = ktime_get();
    writel(ADAS_EB_AP_START_MASK, base + ADAS_EB_REG_CTRL);

    result = wait_for_bit(base, ADAS_EB_AP_DONE_MASK, deadline);
    cls->status.last_op_us =
        (u32)ktime_to_us(ktime_sub(ktime_get(), started_op));
    if (result) {
        dev_err_ratelimited(cls->dev, "op %u (%s) timeout\n", index, op->name);
        return result;
    }
    return 0;
}

static long run_classifier(struct adas_classifier_dev *cls, void __user *argument)
{
    struct adas_classifier_run_uapi run;
    unsigned long deadline;
    ktime_t started_all;
    unsigned i;

    if (copy_from_user(&run, argument, sizeof(run)))
        return -EFAULT;
    if (!cls->parameters_set || run.timeout_ms == 0)
        return -EINVAL;

    memset(&cls->status, 0, sizeof(cls->status));
    deadline = jiffies + msecs_to_jiffies(run.timeout_ms);
    started_all = ktime_get();

    for (i = 0; i < ADAS_EB_NUM_OPS; i++) {
        int result = run_one_op(cls, i, deadline);

        if (result)
            return result;
        cls->status.completed_ops = i + 1;
    }

    cls->status.total_us = (u32)ktime_to_us(ktime_sub(ktime_get(), started_all));
    return 0;
}

static long run_single_op(struct adas_classifier_dev *cls, void __user *argument)
{
    struct adas_classifier_run_op_uapi request;
    unsigned long deadline;
    int result;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    if (!cls->parameters_set || request.timeout_ms == 0 ||
        request.op_index >= ADAS_EB_NUM_OPS)
        return -EINVAL;

    memset(&cls->status, 0, sizeof(cls->status));
    deadline = jiffies + msecs_to_jiffies(request.timeout_ms);
    result = run_one_op(cls, request.op_index, deadline);
    if (result)
        return result;

    cls->status.completed_ops = 1;
    cls->status.total_us = cls->status.last_op_us;
    return 0;
}

static long adas_classifier_ioctl(struct file *file, unsigned int command,
                                  unsigned long argument)
{
    struct miscdevice *misc = file->private_data;
    struct adas_classifier_dev *cls =
        container_of(misc, struct adas_classifier_dev, miscdev);
    void __user *user_argument = (void __user *)argument;
    long result;

    if (_IOC_TYPE(command) != ADAS_CLASSIFIER_IOC_MAGIC)
        return -ENOTTY;

    mutex_lock(&cls->lock);
    switch (command) {
    case ADAS_CLASSIFIER_IOC_GET_INFO: {
        const struct adas_classifier_info_uapi info = {
            .abi_version = ADAS_CLASSIFIER_ABI_VERSION,
            .dma_span = ADAS_CLASSIFIER_DMA_SPAN,
            .ifmap_offset = ADAS_CLASSIFIER_IFMAP_OFFSET,
            .output_offset = ADAS_CLASSIFIER_OUTPUT_OFFSET,
            .act_a_offset = ADAS_CLASSIFIER_ACT_A_OFFSET,
            .act_b_offset = ADAS_CLASSIFIER_ACT_B_OFFSET,
            .num_ops = ADAS_EB_NUM_OPS,
            .reserved = 0,
        };
        result = copy_to_user(user_argument, &info, sizeof(info))
            ? -EFAULT : 0;
        break;
    }
    case ADAS_CLASSIFIER_IOC_SET_PARAMETERS:
        result = set_parameters(cls, user_argument);
        break;
    case ADAS_CLASSIFIER_IOC_RUN:
        result = run_classifier(cls, user_argument);
        break;
    case ADAS_CLASSIFIER_IOC_RUN_OP:
        result = run_single_op(cls, user_argument);
        break;
    case ADAS_CLASSIFIER_IOC_GET_STATUS:
        result = copy_to_user(user_argument, &cls->status, sizeof(cls->status))
            ? -EFAULT : 0;
        break;
    default:
        result = -ENOTTY;
        break;
    }
    mutex_unlock(&cls->lock);
    return result;
}

static int adas_classifier_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct miscdevice *misc = file->private_data;
    struct adas_classifier_dev *cls =
        container_of(misc, struct adas_classifier_dev, miscdev);
    size_t requested = vma->vm_end - vma->vm_start;

    if (vma->vm_pgoff != 0 || requested > ADAS_CLASSIFIER_DMA_SPAN)
        return -EINVAL;
    return dma_mmap_coherent(cls->dev, vma, cls->dma_cpu,
                             cls->dma_handle, ADAS_CLASSIFIER_DMA_SPAN);
}

static const struct file_operations adas_classifier_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = adas_classifier_ioctl,
    .mmap = adas_classifier_mmap,
    .llseek = no_llseek,
};

static int adas_classifier_probe(struct platform_device *pdev)
{
    struct adas_classifier_dev *cls;
    int result;

    cls = devm_kzalloc(&pdev->dev, sizeof(*cls), GFP_KERNEL);
    if (!cls)
        return -ENOMEM;
    cls->dev = &pdev->dev;
    mutex_init(&cls->lock);

    result = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
    if (result)
        return dev_err_probe(&pdev->dev, result, "32-bit DMA unavailable\n");

    /* dtsi 의 reg 순서: conv, conv0, maxpool. 순서가 바뀌면 전부 어긋난다. */
    cls->conv = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(cls->conv))
        return PTR_ERR(cls->conv);
    cls->conv0 = devm_platform_ioremap_resource(pdev, 1);
    if (IS_ERR(cls->conv0))
        return PTR_ERR(cls->conv0);
    cls->maxpool = devm_platform_ioremap_resource(pdev, 2);
    if (IS_ERR(cls->maxpool))
        return PTR_ERR(cls->maxpool);

    cls->dma_cpu = dma_alloc_coherent(&pdev->dev, ADAS_CLASSIFIER_DMA_SPAN,
                                      &cls->dma_handle, GFP_KERNEL);
    if (!cls->dma_cpu)
        return -ENOMEM;
    memset(cls->dma_cpu, 0, ADAS_CLASSIFIER_DMA_SPAN);

    cls->miscdev.minor = MISC_DYNAMIC_MINOR;
    cls->miscdev.name = ADAS_CLASSIFIER_DEVICE_NAME;
    cls->miscdev.fops = &adas_classifier_fops;
    cls->miscdev.parent = &pdev->dev;
    result = misc_register(&cls->miscdev);
    if (result) {
        dma_free_coherent(&pdev->dev, ADAS_CLASSIFIER_DMA_SPAN,
                          cls->dma_cpu, cls->dma_handle);
        return result;
    }

    platform_set_drvdata(pdev, cls);
    dev_info(&pdev->dev,
             "EB classifier: DMA buffer at %pad, size %#x, %u ops\n",
             &cls->dma_handle, ADAS_CLASSIFIER_DMA_SPAN, ADAS_EB_NUM_OPS);
    return 0;
}

static int adas_classifier_remove(struct platform_device *pdev)
{
    struct adas_classifier_dev *cls = platform_get_drvdata(pdev);

    misc_deregister(&cls->miscdev);
    dma_free_coherent(&pdev->dev, ADAS_CLASSIFIER_DMA_SPAN,
                      cls->dma_cpu, cls->dma_handle);
    return 0;
}

static const struct of_device_id adas_classifier_of_match[] = {
    { .compatible = "adas,classifier-eb-1.0" },
    { }
};
MODULE_DEVICE_TABLE(of, adas_classifier_of_match);

static struct platform_driver adas_classifier_driver = {
    .probe = adas_classifier_probe,
    .remove = adas_classifier_remove,
    .driver = {
        .name = "adas_classifier_eb",
        .of_match_table = adas_classifier_of_match,
    },
};
module_platform_driver(adas_classifier_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ADAS ROI classifier coherent-DMA driver (EB 3-engine PL)");
MODULE_AUTHOR("ADAS project team");
