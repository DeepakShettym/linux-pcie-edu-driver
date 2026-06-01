#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/atomic.h>

#define EDU_VENDOR_ID      0x1234
#define EDU_DEVICE_ID      0x11e8

#define EDU_REG_ID         0x00
#define EDU_REG_FACT       0x08
#define EDU_REG_STATUS     0x20
#define EDU_REG_IRQ_STATUS 0x24
#define EDU_REG_IRQ_RAISE  0x60
#define EDU_REG_IRQ_ACK    0x64
#define EDU_DMA_SRC        0x80
#define EDU_DMA_DST        0x88
#define EDU_DMA_CNT        0x90
#define EDU_DMA_CMD        0x98

#define EDU_DMA_SIZE       4096

/* DMA command bits */
#define DMA_START          0x1
#define DMA_FROM_DEV       0x2
#define DMA_IRQ            0x4

struct edu_dev {
    struct pci_dev   *pdev;
    void __iomem     *regs;
    void             *dma_buf;
    dma_addr_t        dma_addr;
    struct completion irq_done;
    atomic_t          irq_count;
};

static struct edu_dev *edu;

static irqreturn_t edu_irq(int irq, void *dev_id)
{
    struct edu_dev *dev = dev_id;
    u32 status;

    status = ioread32(dev->regs + EDU_REG_IRQ_STATUS);
    pr_info("edu: IRQ fired! status=0x%x\n", status);

    if (!status)
        return IRQ_NONE;

    /* acknowledge */
    iowrite32(status, dev->regs + EDU_REG_IRQ_ACK);

    atomic_inc(&dev->irq_count);
    complete(&dev->irq_done);

    return IRQ_HANDLED;
}

static void edu_test_factorial(struct edu_dev *dev)
{
    u32 result;
    iowrite32(5, dev->regs + EDU_REG_FACT);
    msleep(10);
    result = ioread32(dev->regs + EDU_REG_FACT);
    pr_info("edu: factorial(5) = %u (expected 120)\n", result);
}

static void edu_test_dma(struct edu_dev *dev)
{
    memset(dev->dma_buf, 0xAB, EDU_DMA_SIZE);
    init_completion(&dev->irq_done);

    pr_info("edu: dma_addr=0x%llx size=%d\n",
            (unsigned long long)dev->dma_addr, EDU_DMA_SIZE);

    iowrite32(dev->dma_addr,       dev->regs + EDU_DMA_SRC);
    iowrite32(dev->dma_addr >> 32, dev->regs + EDU_DMA_SRC + 4);
    iowrite32(0x40000,             dev->regs + EDU_DMA_DST);
    iowrite32(0,                   dev->regs + EDU_DMA_DST + 4);
    iowrite32(EDU_DMA_SIZE,        dev->regs + EDU_DMA_CNT);
    iowrite32(0,                   dev->regs + EDU_DMA_CNT + 4);

    /* 0x1=start, 0x4=raise IRQ when done */
    iowrite32(DMA_START | DMA_IRQ, dev->regs + EDU_DMA_CMD);

    if (wait_for_completion_timeout(&dev->irq_done, HZ) == 0)
        pr_err("edu: DMA timeout!\n");
    else
        pr_info("edu: DMA complete!\n");
}

static int edu_probe(struct pci_dev *pdev,
                     const struct pci_device_id *id)
{
    int ret;

    pr_info("edu: probe called\n");

    edu = kzalloc(sizeof(*edu), GFP_KERNEL);
    if (!edu)
        return -ENOMEM;

    edu->pdev = pdev;
    atomic_set(&edu->irq_count, 0);
    init_completion(&edu->irq_done);

    ret = pci_enable_device(pdev);
    if (ret) {
        pr_err("edu: pci_enable_device failed\n");
        goto err_free;
    }

    pci_set_master(pdev);

    /* edu device supports only 28-bit DMA addresses */
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
    if (ret) {
        pr_err("edu: dma_set_mask failed\n");
        goto err_disable;
    }

    ret = pci_request_regions(pdev, "edu_driver");
    if (ret) {
        pr_err("edu: pci_request_regions failed\n");
        goto err_disable;
    }

    edu->regs = pci_ioremap_bar(pdev, 0);
    if (!edu->regs) {
        pr_err("edu: pci_ioremap_bar failed\n");
        ret = -ENOMEM;
        goto err_regions;
    }

    edu->dma_buf = dma_alloc_coherent(&pdev->dev,
                                      EDU_DMA_SIZE,
                                      &edu->dma_addr,
                                      GFP_KERNEL);
    if (!edu->dma_buf) {
        pr_err("edu: dma_alloc_coherent failed\n");
        ret = -ENOMEM;
        goto err_iounmap;
    }

    ret = pci_enable_msi(pdev);
    if (ret) {
        pr_err("edu: pci_enable_msi failed\n");
        goto err_dma;
    }

    ret = request_irq(pdev->irq, edu_irq, 0, "edu_driver", edu);
    if (ret) {
        pr_err("edu: request_irq failed\n");
        goto err_msi;
    }

    pr_info("edu: probe complete. BAR0=%p IRQ=%d\n",
            edu->regs, pdev->irq);

    edu_test_factorial(edu);
    edu_test_dma(edu);


    return 0;

err_msi:
    pci_disable_msi(pdev);
err_dma:
    dma_free_coherent(&pdev->dev, EDU_DMA_SIZE,
                      edu->dma_buf, edu->dma_addr);
err_iounmap:
    iounmap(edu->regs);
err_regions:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
err_free:
    kfree(edu);
    return ret;
}

static void edu_remove(struct pci_dev *pdev)
{
    pr_info("edu: removing\n");
    free_irq(pdev->irq, edu);
    pci_disable_msi(pdev);
    dma_free_coherent(&pdev->dev, EDU_DMA_SIZE,
                      edu->dma_buf, edu->dma_addr);
    iounmap(edu->regs);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    kfree(edu);
    pr_info("edu: removed\n");
}

static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
    .name     = "edu_driver",
    .id_table = edu_ids,
    .probe    = edu_probe,
    .remove   = edu_remove,
};

module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Deepak");
MODULE_DESCRIPTION("EDU PCIe device driver");
