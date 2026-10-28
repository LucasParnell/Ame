// SPDX-License-Identifier: GPL-2.0
/* dango_shm.c — Dango shared regions: guest exposure (Phase L) + dma-buf export (Phase M).
 *
 * The host publishes a read-only descriptor page (DANGO_SHM_GUEST window) that
 * names every region a native owner has attached to this VM. This built-in
 * platform driver ("ame,dango-shm-v1") reads it and gives appliance userspace
 * one device node:
 *
 *   /dev/dango-shm0
 *     DANGO_SHM_QUERY          enumerate live regions (ids + geometry)
 *     mmap(mmap_offset)        map one region into userspace
 *     DANGO_SHM_EXPORT_DMABUF  export a *registered* region as a dma-buf fd
 *
 * The device performs no DMA itself and does not know about virtio-gpu; it only
 * publishes what the host attached and enforces the DMA ownership rule, which is
 * the point of the file:
 *
 *   * a region is VmDmaClass::None until the host registers it SharedGpu in Anko,
 *     and only that registration sets DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU on the
 *     region record here. The descriptor page is read-only at stage 2, so the
 *     guest cannot set the bit itself;
 *   * DANGO_SHM_EXPORT_DMABUF fails closed (-EPERM) for a region without the
 *     flag, so a bug that opens a DMA path before registration cannot succeed,
 *     and map_dma_buf() re-checks the bit so a stale export cannot be mapped;
 *   * the number of live exports is bounded by the region count and every export
 *     releases exactly one slot, so a region cannot be retired (descriptor entry
 *     dropped by the host) while the guest still has an unmapped reference that
 *     the host has not accounted for. Anko owns the actual page-release ordering
 *     (kernel/vm/vm_shared.cc + vm_device.cc device_revoke_impl(): detach the
 *     requester into the faulting domain before any page is reused).
 */
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#ifdef CONFIG_ARM64
#include <asm/cacheflush.h>
#endif

#include "midix/abi/dango_shm_guest.h"

/* One live export of a region. A dma-buf's `priv` points here, so the buffer
 * never references the (host-owned, mutable) descriptor page. */
struct dango_shm_export_slot {
    bool used;
    unsigned int flags;
    u64 id;
    u64 base;
    u64 size;
    void* vmap_va;
};

struct dango_shm {
    struct miscdevice misc;
    void* window; /* the read-only descriptor page */
    resource_size_t window_size;
    spinlock_t lock;
    struct dango_shm_export_slot exports[DANGO_SHM_MAX_REGIONS];
};

/* Read the descriptor header `count`, bounded by the wire capacity. The page is
 * host-written and guest-read-only, so a torn value only ever truncates the
 * region list; it can never invent one. */
static unsigned int dango_shm_count(const struct dango_shm* shm) {
    const struct dango_shm_desc* desc = shm->window;
    unsigned int count = READ_ONCE(desc->count);
    return count < DANGO_SHM_MAX_REGIONS ? count : DANGO_SHM_MAX_REGIONS;
}

static bool dango_shm_region_at(const struct dango_shm* shm, unsigned int index,
                                struct dango_shm_region* out) {
    const struct dango_shm_desc* desc = shm->window;
    if (index >= dango_shm_count(shm)) {
        return false;
    }
    memcpy(out, &desc->regions[index], sizeof(*out));
    if (out->id == 0 || out->size == 0 || out->reserved0 != 0 ||
        (out->flags & ~(unsigned int)DANGO_SHM_REGION_FLAG_MASK) != 0 ||
        !IS_ALIGNED(out->guest_pa, PAGE_SIZE) || !IS_ALIGNED(out->size, PAGE_SIZE)) {
        return false;
    }
    return true;
}

static bool dango_shm_region_by_id(const struct dango_shm* shm, u64 id,
                                   struct dango_shm_region* out) {
    unsigned int i;
    for (i = 0; i < dango_shm_count(shm); ++i) {
        if (dango_shm_region_at(shm, i, out) && out->id == id) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* dma-buf export (Phase M)                                                   */
/* ------------------------------------------------------------------------- */

/* Importers receive guest physical DMA addresses. On Anne, the host-owned
 * Mali shadow MMU translates and validates them against registered SharedGpu
 * backing; the external SMMU is not enabled. The export gate rejects CPU-only
 * regions before an importer can reach this path. */
static struct sg_table* dango_shm_map_dma_buf(struct dma_buf_attachment* attach,
                                              enum dma_data_direction dir) {
    struct dango_shm_export_slot* slot = attach->dmabuf->priv;
    struct sg_table* sgt;
    struct scatterlist* sg;
    unsigned int nents = (unsigned int)(slot->size >> PAGE_SHIFT);
    unsigned int i;
    int ret;

    if (!(slot->flags & DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU)) {
        return ERR_PTR(-EOPNOTSUPP);
    }
    if (nents == 0) {
        return ERR_PTR(-EINVAL);
    }
    sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
    if (!sgt) {
        return ERR_PTR(-ENOMEM);
    }
    ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
    if (ret) {
        kfree(sgt);
        return ERR_PTR(ret);
    }
    for_each_sg(sgt->sgl, sg, sgt->nents, i) {
        sg_dma_address(sg) = slot->base + ((u64)i << PAGE_SHIFT);
        sg_dma_len(sg) = PAGE_SIZE;
        sg->length = PAGE_SIZE;
    }
    return sgt;
}

static void dango_shm_unmap_dma_buf(struct dma_buf_attachment* attach, struct sg_table* sgt,
                                    enum dma_data_direction dir) {
    sg_free_table(sgt);
    kfree(sgt);
}

static void dango_shm_dma_buf_release(struct dma_buf* dmabuf) {
    struct dango_shm_export_slot* slot = dmabuf->priv;
    if (slot->vmap_va) {
        memunmap(slot->vmap_va);
        slot->vmap_va = NULL;
    }
    memset(slot, 0, sizeof(*slot)); /* frees the slot: used = false */
}

static int dango_shm_dma_buf_mmap(struct dma_buf* dmabuf, struct vm_area_struct* vma) {
    struct dango_shm_export_slot* slot = dmabuf->priv;
    unsigned long len = vma->vm_end - vma->vm_start;

    if (len != slot->size) {
        return -EINVAL;
    }
    return remap_pfn_range(vma, vma->vm_start, slot->base >> PAGE_SHIFT, len, vma->vm_page_prot);
}

static int dango_shm_dma_buf_vmap(struct dma_buf* dmabuf, struct iosys_map* map) {
    struct dango_shm_export_slot* slot = dmabuf->priv;

    if (!slot->vmap_va) {
        slot->vmap_va = memremap(slot->base, slot->size, MEMREMAP_WB);
        if (!slot->vmap_va) {
            return -ENOMEM;
        }
    }
    iosys_map_set_vaddr(map, slot->vmap_va);
    return 0;
}

static void dango_shm_dma_buf_vunmap(struct dma_buf* dmabuf, struct iosys_map* map) {
    struct dango_shm_export_slot* slot = dmabuf->priv;

    if (slot->vmap_va) {
        memunmap(slot->vmap_va);
        slot->vmap_va = NULL;
    }
    iosys_map_clear(map);
}

/* These external pages are not Linux-managed RAM. Cache maintenance uses the
 * memremap alias, never pfn_to_page()/phys_to_virt() beyond guest RAM. */
static int dango_shm_begin_cpu_access(struct dma_buf* dmabuf, enum dma_data_direction dir) {
    struct dango_shm_export_slot* slot = dmabuf->priv;
    bool write = dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL;
    long waited;
    /* Linux's generic dma_buf_begin_cpu_access waits AFTER this callback.
     * Cache invalidation must follow completion of the tracked device writer,
     * otherwise a CPU can refill stale lines before the generic wait finishes.
     * Fail closed on timeout: never expose a frame whose writer is still live.
     */
    waited = dma_resv_wait_timeout(dmabuf->resv, dma_resv_usage_rw(write), true,
                                   msecs_to_jiffies(10000));
    if (waited <= 0) {
        return waited < 0 ? (int)waited : -ETIMEDOUT;
    }
    if (!slot->vmap_va) {
        slot->vmap_va = memremap(slot->base, slot->size, MEMREMAP_WB);
        if (!slot->vmap_va) {
            return -ENOMEM;
        }
    }
#ifdef CONFIG_ARM64
    if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) {
        dcache_inval_poc((unsigned long)slot->vmap_va, (unsigned long)slot->vmap_va + slot->size);
    }
#endif
    return 0;
}
static int dango_shm_end_cpu_access(struct dma_buf* dmabuf, enum dma_data_direction dir) {
    struct dango_shm_export_slot* slot = dmabuf->priv;
#ifdef CONFIG_ARM64
    if (slot->vmap_va && (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL)) {
        dcache_clean_poc((unsigned long)slot->vmap_va, (unsigned long)slot->vmap_va + slot->size);
    }
#endif
    return 0;
}

static const struct dma_buf_ops dango_shm_dma_buf_ops = {
    .begin_cpu_access = dango_shm_begin_cpu_access,
    .end_cpu_access = dango_shm_end_cpu_access,
    .map_dma_buf = dango_shm_map_dma_buf,
    .unmap_dma_buf = dango_shm_unmap_dma_buf,
    .release = dango_shm_dma_buf_release,
    .mmap = dango_shm_dma_buf_mmap,
    .vmap = dango_shm_dma_buf_vmap,
    .vunmap = dango_shm_dma_buf_vunmap,
};

static long dango_shm_export(struct dango_shm* shm, struct dango_shm_export* in) {
    struct dango_shm_region region;
    struct dango_shm_export_slot* slot = NULL;
    struct dma_buf* dmabuf;
    DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
    unsigned long flags;
    unsigned int i;
    int fd;

    if (in->flags != DANGO_SHM_EXPORT_FLAG_NONE) {
        return -EINVAL;
    }
    if (!dango_shm_region_by_id(shm, in->id, &region)) {
        return -ENOENT;
    }
    /* Non-DMA until registered SharedGpu: refuse before any DMA-facing object
     * exists. The descriptor page is guest read-only, so this bit can only have
     * been set by the host after Anko's explicit registration. */
    if (!(region.flags & DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU)) {
        return -EPERM;
    }

    spin_lock_irqsave(&shm->lock, flags);
    for (i = 0; i < DANGO_SHM_MAX_REGIONS; ++i) {
        if (!shm->exports[i].used) {
            slot = &shm->exports[i];
            break;
        }
    }
    if (slot) {
        slot->used = true;
        slot->flags = region.flags;
        slot->id = region.id;
        slot->base = region.guest_pa;
        slot->size = region.size;
        slot->vmap_va = NULL;
    }
    spin_unlock_irqrestore(&shm->lock, flags);
    if (!slot) {
        return -ENOSPC;
    }

    exp_info.ops = &dango_shm_dma_buf_ops;
    exp_info.size = slot->size;
    exp_info.flags = O_RDWR | O_CLOEXEC;
    exp_info.priv = slot;
    dmabuf = dma_buf_export(&exp_info);
    if (IS_ERR(dmabuf)) {
        long ret = PTR_ERR(dmabuf);
        memset(slot, 0, sizeof(*slot)); /* release the slot on the way out */
        return ret;
    }
    fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        dma_buf_put(dmabuf); /* -> release() frees the slot */
        return fd;
    }
    in->fd = fd;
    fd_install(fd, dmabuf->file);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* File operations                                                            */
/* ------------------------------------------------------------------------- */

static int dango_shm_open(struct inode* inode, struct file* file) {
    struct dango_shm* shm = container_of(file->private_data, struct dango_shm, misc);
    file->private_data = shm;
    return nonseekable_open(inode, file);
}

static long dango_shm_query(struct dango_shm* shm, struct dango_shm_query __user* uarg) {
    struct dango_shm_query out;
    struct dango_shm_region region;
    unsigned int index;

    if (copy_from_user(&index, &uarg->index, sizeof(index))) {
        return -EFAULT;
    }
    if (!dango_shm_region_at(shm, index, &region)) {
        return -ENOENT;
    }
    memset(&out, 0, sizeof(out));
    out.region = region;
    out.index = index;
    out.count = dango_shm_count(shm);
    out.boot_generation = ((const struct dango_shm_desc*)shm->window)->boot_generation;
    /* A per-slot page, never 0, so a plain mmap() cannot alias a region. */
    out.mmap_offset = (unsigned long long)(index + 1) << PAGE_SHIFT;
    return copy_to_user(uarg, &out, sizeof(out)) ? -EFAULT : 0;
}

static long dango_shm_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
    struct dango_shm* shm = file->private_data;

    if (!shm) {
        return -ENODEV;
    }
    switch (cmd) {
    case DANGO_SHM_QUERY:
        return dango_shm_query(shm, (struct dango_shm_query __user*)arg);
    case DANGO_SHM_EXPORT_DMABUF: {
        struct dango_shm_export in;
        long ret;
        if (copy_from_user(&in, (void __user*)arg, sizeof(in))) {
            return -EFAULT;
        }
        ret = dango_shm_export(shm, &in);
        if (ret) {
            return ret;
        }
        return copy_to_user((void __user*)arg, &in, sizeof(in)) ? -EFAULT : 0;
    }
    default:
        return -ENOTTY;
    }
}

static int dango_shm_mmap(struct file* file, struct vm_area_struct* vma) {
    struct dango_shm* shm = file->private_data;
    struct dango_shm_region region;
    unsigned long len = vma->vm_end - vma->vm_start;
    unsigned long index;

    if (!shm || vma->vm_pgoff == 0) {
        return -EINVAL;
    }
    if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC)) {
        return -EINVAL;
    }
    index = vma->vm_pgoff - 1; /* QUERY's mmap_offset is (index + 1) pages */
    if (index >= DANGO_SHM_MAX_REGIONS || !dango_shm_region_at(shm, (unsigned int)index, &region) ||
        len != region.size) {
        return -EINVAL;
    }
    vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
    vm_flags_clear(vma, VM_MAYEXEC);
    return remap_pfn_range(vma, vma->vm_start, region.guest_pa >> PAGE_SHIFT, len,
                           vma->vm_page_prot);
}

static const struct file_operations dango_shm_fops = {
    .owner = THIS_MODULE,
    .open = dango_shm_open,
    .unlocked_ioctl = dango_shm_ioctl,
    .mmap = dango_shm_mmap,
};

static int dango_shm_probe(struct platform_device* pdev) {
    const struct dango_shm_desc* desc;
    struct dango_shm* shm;
    struct resource* r;
    int rc;

    if (PAGE_SIZE != 4096) {
        return -EINVAL;
    }
    r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!r || resource_size(r) < PAGE_SIZE || !IS_ALIGNED(r->start, PAGE_SIZE)) {
        return -EINVAL;
    }
    shm = devm_kzalloc(&pdev->dev, sizeof(*shm), GFP_KERNEL);
    if (!shm) {
        return -ENOMEM;
    }
    spin_lock_init(&shm->lock);
    shm->window = devm_memremap(&pdev->dev, r->start, resource_size(r), MEMREMAP_WB);
    if (IS_ERR(shm->window)) {
        return PTR_ERR(shm->window);
    }
    shm->window_size = resource_size(r);
    desc = shm->window;
    if (desc->magic != DANGO_SHM_DESC_MAGIC || desc->version_major != DANGO_SHM_VER_MAJOR ||
        desc->version_minor != DANGO_SHM_VER_MINOR || desc->reserved0 != 0 ||
        desc->count > DANGO_SHM_MAX_REGIONS) {
        return -EINVAL;
    }
    shm->misc = (struct miscdevice){
        .minor = MISC_DYNAMIC_MINOR,
        .name = "dango-shm0",
        .fops = &dango_shm_fops,
        .parent = &pdev->dev,
        .mode = 0600,
    };
    rc = misc_register(&shm->misc);
    if (rc) {
        return rc;
    }
    dev_set_drvdata(&pdev->dev, shm);
    return 0;
}

static const struct of_device_id dango_shm_ids[] = {{.compatible = "ame,dango-shm-v1"}, {}};
/* Fixed boot-time device; no unbind while user VMAs refer to its regions. */
static struct platform_driver dango_shm_driver = {
    .probe = dango_shm_probe,
    .driver = {.name = "dango-shm", .of_match_table = dango_shm_ids, .suppress_bind_attrs = true},
};
builtin_platform_driver(dango_shm_driver);
