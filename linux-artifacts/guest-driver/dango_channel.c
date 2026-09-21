// SPDX-License-Identifier: GPL-2.0
/* Generic Dango SPSC transport. No GPU or DMA API is used here. */
#include <linux/arm-smccc.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include "midix/abi/dango_channel_guest.h"

static_assert(DANGO_CH_QUERY == _IOR('D', 0, struct dango_channel_layout));
static_assert(DANGO_CH_NOTIFY == _IO('D', 1));

struct dango_channel {
    struct miscdevice misc;
    struct dango_channel_layout layout;
    void *rx;
    void *tx;
    wait_queue_head_t wait;
    atomic_t opened;
    atomic64_t irqs;
};

static u64 head(void *ring) { return smp_load_acquire((u64 *)ring); }
static u64 tail(void *ring) { return smp_load_acquire((u64 *)((char *)ring + 64)); }

static long channel_call(struct dango_channel *ch, u32 fn, u64 arg, u64 counter)
{
    struct arm_smccc_res res;
    arm_smccc_1_1_hvc(fn, ch->layout.channel_id, arg, counter, &res);
    return (long)res.a0;
}

static int arm_channel(struct dango_channel *ch)
{
    /* ARM compares against head in Anko. If a publisher raced this snapshot,
     * it injects an IRQ immediately; otherwise its next notify wakes us.
     * wait_event/poll register the waiter before their final readiness check.
     */
    return channel_call(ch, DANGO_CH_HVC_ARM, head(ch->rx), 0) ? -EIO : 0;
}

static irqreturn_t channel_irq(int irq, void *data)
{
    struct dango_channel *ch = data;
    atomic64_inc(&ch->irqs);
    wake_up_interruptible_poll(&ch->wait, EPOLLIN | EPOLLRDNORM);
    return IRQ_HANDLED;
}

static int channel_open(struct inode *inode, struct file *file)
{
    struct dango_channel *ch = container_of(file->private_data, struct dango_channel, misc);
    if (atomic_cmpxchg(&ch->opened, 0, 1)) return -EBUSY; /* one SPSC endpoint */
    file->private_data = ch;
    return nonseekable_open(inode, file);
}

static int channel_release(struct inode *inode, struct file *file)
{
    struct dango_channel *ch = file->private_data;
    atomic_set(&ch->opened, 0);
    return 0;
}

/* read is a blocking doorbell wait, not a second ring consumer. The mapped
 * consumer owns tail; an eight-byte result is the number of received vIRQs.
 */
static ssize_t channel_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
    struct dango_channel *ch = file->private_data;
    u64 count;
    int rc;
    if (len != sizeof(count)) return -EINVAL;
    if (head(ch->rx) == tail(ch->rx)) {
        if (file->f_flags & O_NONBLOCK) return -EAGAIN;
        rc = arm_channel(ch);
        if (rc) return rc;
        rc = wait_event_interruptible(ch->wait, head(ch->rx) != tail(ch->rx));
        if (rc) return rc;
    }
    count = atomic64_read(&ch->irqs);
    return copy_to_user(buf, &count, sizeof(count)) ? -EFAULT : sizeof(count);
}

static __poll_t channel_poll(struct file *file, poll_table *wait)
{
    struct dango_channel *ch = file->private_data;
    poll_wait(file, &ch->wait, wait);
    if (arm_channel(ch)) return EPOLLERR;
    return head(ch->rx) != tail(ch->rx) ? EPOLLIN | EPOLLRDNORM : 0;
}

static long channel_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct dango_channel *ch = file->private_data;
    switch (cmd) {
    case DANGO_CH_QUERY:
        return copy_to_user((void __user *)arg, &ch->layout, sizeof(ch->layout)) ? -EFAULT : 0;
    case DANGO_CH_NOTIFY:
        if (arg) return -EINVAL;
        return channel_call(ch, DANGO_CH_HVC_NOTIFY, 2, head(ch->tx)) ? -EIO : 0;
    default: return -ENOTTY;
    }
}

static int channel_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct dango_channel *ch = file->private_data;
    if (vma->vm_pgoff || vma->vm_end - vma->vm_start != ch->layout.mapping_bytes ||
        !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC)) return -EINVAL;
    vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
    vm_flags_clear(vma, VM_MAYEXEC);
    /* Normal WB on both sides: release/acquire atomics require normal memory.
     * Descriptor and adjacent guest RAM are never exposed through this mmap.
     */
    return remap_pfn_range(vma, vma->vm_start,
                          ch->layout.desc.host_to_guest_ring_pa >> PAGE_SHIFT,
                          ch->layout.mapping_bytes, vma->vm_page_prot);
}

static const struct file_operations channel_fops = {
    .open = channel_open, .release = channel_release, .read = channel_read,
    .poll = channel_poll, .unlocked_ioctl = channel_ioctl,
    .mmap = channel_mmap,
};

static int channel_probe(struct platform_device *pdev)
{
    struct dango_channel *ch;
    struct resource *r;
    void *desc;
    u64 bytes;
    int irq, rc;
    if (PAGE_SIZE != 4096) return -EINVAL;
    r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!r || resource_size(r) < PAGE_SIZE || !IS_ALIGNED(r->start, PAGE_SIZE)) return -EINVAL;
    ch = devm_kzalloc(&pdev->dev, sizeof(*ch), GFP_KERNEL);
    if (!ch) return -ENOMEM;
    desc = devm_memremap(&pdev->dev, r->start, PAGE_SIZE, MEMREMAP_WB);
    if (IS_ERR(desc)) return PTR_ERR(desc);
    memcpy(&ch->layout, desc, sizeof(ch->layout));
    if (ch->layout.desc.magic != DANGO_CH_MAGIC ||
        ch->layout.desc.version_major != DANGO_CH_VERSION ||
        ch->layout.desc.version_minor || ch->layout.desc.reserved0 || ch->layout.reserved0 ||
        ch->layout.desc.flags != DANGO_CH_ARM_SUPPORTED ||
        ch->layout.desc.ring_order < 4 || ch->layout.desc.ring_order > 10 ||
        ch->layout.desc.shared_staging_pa || ch->layout.desc.shared_staging_size)
        return -EINVAL;
    bytes = ALIGN(256 + (64ULL << ch->layout.desc.ring_order), PAGE_SIZE);
    if (ch->layout.ring_bytes != bytes || ch->layout.mapping_bytes != 2 * bytes ||
        resource_size(r) < PAGE_SIZE + 2 * bytes ||
        ch->layout.desc.host_to_guest_ring_pa != r->start + PAGE_SIZE ||
        ch->layout.desc.guest_to_host_ring_pa != r->start + PAGE_SIZE + bytes)
        return -EINVAL;
    ch->rx = devm_memremap(&pdev->dev, r->start + PAGE_SIZE, 2 * bytes, MEMREMAP_WB);
    if (IS_ERR(ch->rx)) return PTR_ERR(ch->rx);
    ch->tx = (char *)ch->rx + bytes;
    init_waitqueue_head(&ch->wait);
    atomic_set(&ch->opened, 0);
    atomic64_set(&ch->irqs, 0);
    irq = platform_get_irq(pdev, 0);
    if (irq < 0) return irq;
    rc = devm_request_irq(&pdev->dev, irq, channel_irq, 0, "dango-channel", ch);
    if (rc) return rc;
    ch->misc = (struct miscdevice) {
        .minor = MISC_DYNAMIC_MINOR, .name = "dango-channel0", .fops = &channel_fops,
        .parent = &pdev->dev, .mode = 0600,
    };
    return misc_register(&ch->misc);
}

static const struct of_device_id channel_ids[] = { { .compatible = "ame,dango-channel-v1" }, {} };
/* Fixed boot-time device; no unbind while user VMAs refer to its pinned rings. */
static struct platform_driver channel_driver = {
    .probe = channel_probe,
    .driver = { .name = "dango-channel", .of_match_table = channel_ids, .suppress_bind_attrs = true },
};
builtin_platform_driver(channel_driver);
