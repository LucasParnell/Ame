// kernel/init/boot_caps.cc — assemble the initial capability set for init.
//
// Spec ref: §8 V0 Boot Flow (step 9), §7.1 Init

#include "anko/cap.h"
#include "anko/hw_info.h"
#include "anko/pci.h"
#include "anko/sched.h"
#include "anko/smp.h"
#include "anko/mm.h"
#include "anko/platform.h"
#include "anko/smmuv3.h"
#include "anko/init.h"
#include "anko/vm.h"
#include "anko/ipc.h"
#include "anko/log.h"
#include "anko/panic.h"
#include "midix/abi/boot.h"
#include "libk/printf.h"
#include "libk/string.h"

namespace drivers::fwcfg {
    bool write_ramfb(PhysAddr pixel_phys, u32 width, u32 height);
} // namespace drivers::fwcfg

namespace drivers::pci_probe {
    struct XhciInfo {
        PhysAddr bar0_phys;
        u64      bar0_size;
        u32      irq_line;
        u32      requester_id; // PCI requester id == SMMU stream id
        bool     found;
    };
    XhciInfo find_xhci();
    MmioRange* alloc_xhci_mmio(const XhciInfo& info);
    IrqLine* alloc_xhci_irq(const XhciInfo& info);
} // namespace drivers::pci_probe

namespace kernel::init {

// DTB physical address — provided by arch/arm64/entry.S.
extern "C" PhysAddr g_dtb_phys;

namespace {

constexpr u32 kKernelLogRingOrder = 6;

constexpr usize align_up(usize value, usize align) {
    return (value + (align - 1)) & ~(align - 1);
}

} // namespace

void issue_boot_caps(Process& init_proc,
                     PhysAddr manifest_addr,
                     usize    manifest_size,
                     PhysAddr initrd_phys,
                     usize    initrd_size) {
    if (!init_proc.cap_table) panic("init has no cap table");
    const PhysAddr boot_page = mm::alloc_page_for(init_proc.pid);
    auto* boot_arena = static_cast<Arena*>(mm::slab_alloc(sizeof(Arena), alignof(Arena)));
    if (!boot_page || !boot_arena) panic("init BootInfo allocation failed");
    libk::memset(reinterpret_cast<void*>(boot_page), 0, PAGE_SIZE);
    libk::memset(boot_arena, 0, sizeof(*boot_arena));
    boot_arena->phys_base = boot_page; boot_arena->size = PAGE_SIZE;
    boot_arena->owner_pid = init_proc.pid; boot_arena->rights = RIGHT_READ;
    const CapHandle boot_cap = cap::alloc(*init_proc.cap_table, CAP_ARENA, boot_arena, RIGHT_READ | RIGHT_MAP);
    if (cap_index(boot_cap) != 0 || !boot_cap) panic("init BootInfo must be slot zero");
    auto* boot = reinterpret_cast<MxBootInfo*>(boot_page);
    boot->version = MX_BOOT_VERSION;
    auto add = [&](const char* name, CapHandle handle, u64 size = 0) {
        if (!handle || boot->entry_count >= MX_BOOT_MAX_ENTRIES) panic("boot capability allocation failed");
        auto& e = boot->entries[boot->entry_count++];
        e.type = MX_BOOT_ENTRY_HANDLE; e.handle = handle; e.aux0 = size;
        usize i = 0; for (; name[i] && i + 1 < sizeof(e.name); ++i) e.name[i] = name[i];
        e.name[i] = 0;
    };
    auto grant = [&](const char* name, CapType type, void* target, u32 rights) {
        const CapHandle h = cap::alloc(*init_proc.cap_table, type, target, rights);
        const u64 size = type == CAP_PAGE ? static_cast<MemoryObject*>(target)->size : 0;
        add(name, h, size); return h;
    };

    auto* manifest = static_cast<MemoryObject*>(mm::heap_alloc(sizeof(MemoryObject), alignof(MemoryObject)));
    auto* initrd = static_cast<MemoryObject*>(mm::heap_alloc(sizeof(MemoryObject), alignof(MemoryObject)));
    if (!manifest || !initrd) return;

    const usize manifest_alloc_size = align_up(manifest_size ? manifest_size : PAGE_SIZE, PAGE_SIZE);
    const usize manifest_pages = manifest_alloc_size / PAGE_SIZE;
    const PhysAddr manifest_page = mm::alloc_pages_contiguous(manifest_pages);
    if (!manifest_page) return;
    libk::memset(reinterpret_cast<void*>(manifest_page), 0, manifest_alloc_size);
    if (manifest_addr) {
        libk::memcpy(reinterpret_cast<void*>(manifest_page),
                     reinterpret_cast<const void*>(manifest_addr),
                     manifest_size);
    }
    manifest->phys_base = manifest_page;
    manifest->size = manifest_alloc_size;

    initrd->phys_base = initrd_phys;
    initrd->size = initrd_size;

    auto& cpus = boot->entries[boot->entry_count++];
    cpus.type = MX_BOOT_ENTRY_VALUE; cpus.aux0 = smp::cpu_count();
    libk::memcpy(cpus.name, "cpu.count", 10);

    // 1) manifest cap
    grant("manifest", CAP_PAGE, manifest, RIGHT_READ | RIGHT_MAP);

    boot->entries[boot->entry_count - 1].aux0 = manifest_size;

    // 2) initrd cap (read-only in V0)
    grant("boot.initrd", CAP_PAGE, initrd, RIGHT_READ | RIGHT_MAP | RIGHT_DUP);

    grant("self", CAP_PROCESS, &init_proc, RIGHT_READ | RIGHT_SPAWN | RIGHT_DUP);

    // 3) root supervisor cap
    grant("supervisor", CAP_SUPERVISOR, &init_proc,
               RIGHT_READ | RIGHT_KILL | RIGHT_SPAWN | RIGHT_REGISTER | RIGHT_DUP);

    // 4) nameservice placeholder cap
    grant("nameservice", CAP_NAMESERVICE, &init_proc,
               RIGHT_REGISTER | RIGHT_READ | RIGHT_DUP);

    // 5) UART cap
    grant("uart.mmio", CAP_UART,
               reinterpret_cast<void*>(g_hw.console_base),
               RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);

    // 6) VM root authority. Init policy grants reduced VM authority to Dango.
    grant("vm.root", CAP_VM_ROOT, vm::root_object(),
               RIGHT_VM_CREATE | RIGHT_VM_INSPECT | RIGHT_DUP);

    // 7) Global log doorbell followed by one kernel log ring per supported CPU.
    Doorbell* log_db = ipc::doorbell_create_object(init_proc.pid);
    if (!log_db) panic("log doorbell allocation failed");
    add("log.doorbell", ipc::doorbell_cap_alloc(init_proc.pid, log_db, RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP));

    Ring* kernel_log_rings[kMaxCpus]{};
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
        Ring* ring = ipc::ring_create_object(init_proc.pid, kKernelLogRingOrder, 0);
        kernel_log_rings[cpu] = ring;
        if (!ring) panic("kernel log ring allocation failed");
        char name[] = "kernel.log.0"; name[11] = static_cast<char>('0' + cpu);
        add(name, ipc::ring_cap_alloc(init_proc.pid, ring, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP));
    }
    anko::log::attach_console(kernel_log_rings, log_db, kMaxCpus);

    // 11) Framebuffer.  Two paths:
    //   T210: Hekate pre-configured TegraDC0; g_hw.t210_dc_fb_phys is the
    //         physical address of the active buffer. Map it as NORMAL_NC.
    //   QEMU: Allocate RAM, register it with ramfb via fw-cfg.
    if (g_hw.has_t210_dc) {
        // T210 path — re-use Hekate's framebuffer directly.
        const usize fb_size = static_cast<usize>(g_hw.t210_dc_fb_width) *
                              g_hw.t210_dc_fb_height * 4u;
        const usize fb_size_pages = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        auto* fb_range = static_cast<MmioRange*>(
            mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
        if (fb_range) {
            fb_range->phys_base    = g_hw.t210_dc_fb_phys;
            fb_range->size         = fb_size_pages;
            fb_range->attr         = MX_MMIO_NORMAL_NC;
            fb_range->flags        = 0;
            fb_range->device_hint  = 0;
            fb_range->region_index = 0;
            grant("fb.memory", CAP_MMIO, fb_range,
                       RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
        } else {
            ANKO_LOG_WARN("kernel/init", "T210 fb_range alloc failed");
        }
    } else if (g_hw.has_fwcfg) {
        // QEMU ramfb path.
        constexpr u32 kFbWidth  = 1024;
        constexpr u32 kFbHeight = 768;
        constexpr usize kFbPages = (kFbWidth * kFbHeight * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
        const PhysAddr fb_phys = mm::alloc_pages_contiguous(kFbPages);
        if (fb_phys) {
            libk::memset(reinterpret_cast<void*>(fb_phys), 0, kFbPages * PAGE_SIZE);
            if (!mm::register_noncached_range(fb_phys, kFbPages * PAGE_SIZE)) panic("framebuffer memory attributes failed");
            const bool ramfb_ok = drivers::fwcfg::write_ramfb(fb_phys, kFbWidth, kFbHeight);
            if (!ramfb_ok) {
                ANKO_LOG_WARN("kernel/init", "ramfb init failed — framebuffer will be blank");
            }
            auto* fb_range = static_cast<MmioRange*>(
                mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
            if (fb_range) {
                fb_range->phys_base    = fb_phys;
                fb_range->size         = kFbPages * PAGE_SIZE;
                fb_range->attr         = MX_MMIO_NORMAL_NC;
                fb_range->flags        = 0;
                fb_range->device_hint  = 0;
                fb_range->region_index = 0;
                grant("fb.memory", CAP_MMIO, fb_range,
                           RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
            }
        } else {
            ANKO_LOG_WARN("kernel/init", "framebuffer RAM alloc failed");
        }
    }

    // 12..14) xHCI host controller.  Two paths:
    //   T210: T210 XUSB at 0x70090000 (xHCI-compatible, FALCON pre-loaded).
    //         No IRQ cap — polling mode, SMMU required for IOMMU setup.
    //   QEMU: PCI xHCI discovered via ECAM scan.
    if (g_hw.console_kind == ConsoleKind::Tegra16550) {
        // T210 XUSB xHCI registers.
        constexpr PhysAddr kXusbBase = 0x7009'0000ull;
        constexpr usize    kXusbSize = 0x8000u;   // 32 KiB xHCI register window
        constexpr usize kXhciDmaPages = 16;
        auto* xhci_mmio = static_cast<MmioRange*>(
            mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
        const PhysAddr xhci_dma_phys = mm::alloc_pages_contiguous(kXhciDmaPages);
        auto* xhci_dma = static_cast<Arena*>(
            mm::heap_alloc(sizeof(Arena), alignof(Arena)));
        if (xhci_mmio && xhci_dma_phys && xhci_dma) {
            xhci_mmio->phys_base    = kXusbBase;
            xhci_mmio->size         = kXusbSize;
            xhci_mmio->attr         = MX_MMIO_DEVICE_nGnRnE;
            xhci_mmio->flags        = 0;
            xhci_mmio->device_hint  = 0;
            xhci_mmio->region_index = 0;

            libk::memset(reinterpret_cast<void*>(xhci_dma_phys), 0,
                         kXhciDmaPages * PAGE_SIZE);
            xhci_dma->base_va   = 0;
            xhci_dma->phys_base = xhci_dma_phys;
            xhci_dma->size      = kXhciDmaPages * PAGE_SIZE;
            xhci_dma->owner_pid = init_proc.pid;
            xhci_dma->rights    = RIGHT_READ | RIGHT_WRITE;
            xhci_dma->flags     = kArenaFlagExposePhys;
            libk::memset(xhci_dma->pad, 0, sizeof(xhci_dma->pad));

            // Slot 12: xhci.mmio
            grant("xhci.mmio", CAP_MMIO, xhci_mmio,
                       RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
            // Slot 13: placeholder IRQ cap — keeps slot numbering consistent
            // with the QEMU path.  irq_id=0xFFFFFFFF signals no real IRQ;
            // the xhci driver uses polling mode via ANKO_XHCI_NORESET.
            auto* t210_irq = static_cast<IrqLine*>(
                mm::heap_alloc(sizeof(IrqLine), alignof(IrqLine)));
            if (t210_irq) {
                t210_irq->irq_id      = 0xFFFF'FFFFu; // sentinel: invalid
                t210_irq->trigger     = 0;
                t210_irq->flags       = 0;
                t210_irq->device_hint = 0;
                t210_irq->doorbell_cap = 0;
                grant("xhci.irq", CAP_IRQ, t210_irq,
                           RIGHT_READ | RIGHT_DUP);
            }
            // Slot 14: xhci.dma arena
            grant("xhci.dma", CAP_ARENA, xhci_dma,
                       RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_REGISTER | RIGHT_DUP);
        } else {
            ANKO_LOG_WARN("kernel/init", "T210 xhci cap object allocation failed");
        }
    } else if (g_hw.has_pcie) {
        // QEMU path: PCI xHCI scan.
        constexpr usize kXhciDmaPages = 16;
        const drivers::pci_probe::XhciInfo xhci = drivers::pci_probe::find_xhci();
        // Phase A: inventory the ECAM hierarchy.  The PCI command register is
        // read (bus-master observation) but never written here, and BAR probing
        // restores every BAR it touches, so find_xhci()'s enablement survives.
        pci::log_inventory(g_hw.ecam_base);
        {
            const u32 contained = pci::contain_assignable(g_hw.ecam_base);
            ANKO_LOGF_INFO("kernel/init", "pci: contained %u assignable function(s)", contained);
        }
        if (xhci.found) {
            MmioRange* xhci_mmio = drivers::pci_probe::alloc_xhci_mmio(xhci);
            IrqLine* xhci_irq    = drivers::pci_probe::alloc_xhci_irq(xhci);
            const PhysAddr xhci_dma_phys = mm::alloc_pages_contiguous(kXhciDmaPages);
            Arena* xhci_dma = static_cast<Arena*>(
                mm::heap_alloc(sizeof(Arena), alignof(Arena)));
            if (xhci_mmio && xhci_irq && xhci_dma_phys && xhci_dma) {
                libk::memset(reinterpret_cast<void*>(xhci_dma_phys), 0,
                             kXhciDmaPages * PAGE_SIZE);
                xhci_dma->base_va   = 0;
                xhci_dma->phys_base = xhci_dma_phys;
                xhci_dma->size      = kXhciDmaPages * PAGE_SIZE;
                xhci_dma->owner_pid = init_proc.pid;
                xhci_dma->rights    = RIGHT_READ | RIGHT_WRITE;
                xhci_dma->flags     = kArenaFlagExposePhys;
                libk::memset(xhci_dma->pad, 0, sizeof(xhci_dma->pad));

                grant("xhci.mmio", CAP_MMIO, xhci_mmio,
                           RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
                grant("xhci.irq", CAP_IRQ, xhci_irq,
                           RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
                grant("xhci.dma", CAP_ARENA, xhci_dma,
                           RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_REGISTER | RIGHT_DUP);
                // Phase C: once the SMMU enforces translation the native xHCI
                // needs a context too; register its wired DMA arena (IOVA == PA)
                // at its stream id so the native USB path keeps working.
                if (iommu::smmuv3_ready()) {
                    (void)iommu::register_native_dma_arena(
                        iommu::IommuDeviceId{pci::kSegment, xhci.requester_id}, xhci_dma_phys,
                        static_cast<u64>(kXhciDmaPages) * PAGE_SIZE);
                }
            } else {
                ANKO_LOG_WARN("kernel/init", "xhci cap object allocation failed");
            }
        }
    }

    // 14b) Appliance GPU function authority (§6.1). Publish the assignable
    // virtio-gpu-pci function (and its INTx line) to init so it can hand the
    // authority to dango.manager; the VM never names raw physical addresses.
    if (g_hw.has_pcie) {
        constexpr u16 kVirtioVendor = 0x1af4;
        constexpr u16 kVirtioGpuDevice = 0x1050;
        // QEMU virt swizzles PCI INTx onto GIC SPIs 3..6 (rotating per slot).
        // The DTB interrupt specifier is <type number flags> and for arm,gic the
        // INTID is 32 + number; the fixture (tests/fixtures/phasea-iommu-smmu-node.dts)
        // shows pin A of slot 0 -> number 3.
        //   intid = 32 + (3 + pin_index) % 4,  pin A = 1 -> (3 + device) % 4
        // Physical boards parse their own map in Phase P.
        constexpr u32 kQemuVirtPciIntxSpiBase = 32 + 3;
        pci::PciFunctionInfo gpu_info{};
        if (pci::find_by_id(g_hw.ecam_base, kVirtioVendor, kVirtioGpuDevice, &gpu_info)) {
            static pci::HostFunction g_gpu_function;
            g_gpu_function.info = gpu_info;
            g_gpu_function.group_id =
                pci::isolation_group_id(gpu_info.segment, gpu_info.bus, gpu_info.device);
            g_gpu_function.flags = 0;

            static IrqLine g_gpu_irq;
            g_gpu_irq.irq_id = kQemuVirtPciIntxSpiBase + (gpu_info.device % 4u);
            g_gpu_irq.trigger = 0; // INTx is level-triggered
            g_gpu_irq.flags = 0;
            g_gpu_irq.device_hint = 0;
            g_gpu_irq.doorbell_cap = 0;

            grant("gpu.function", CAP_PCI_FUNCTION, &g_gpu_function,
                  RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
            grant("gpu.irq", CAP_IRQ, &g_gpu_irq, RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
            ANKO_LOGF_INFO("kernel/init", "gpu: function bus=%u dev=%u fn=%u vendor=%x device=%x group=%u intx=%u",
                           gpu_info.bus, gpu_info.device, gpu_info.function,
                           gpu_info.vendor_id, gpu_info.device_id,
                           g_gpu_function.group_id, g_gpu_irq.irq_id);
        }
    }

    // 15..16) Virtio-blk block device (QEMU only — T210 uses AmeFS via SATA/SD).
    if (g_hw.has_virtio_blk) {
        auto* blk_mmio = static_cast<MmioRange*>(
            mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
        constexpr usize kBlkDmaPages = 64;
        const PhysAddr blk_dma_phys = mm::alloc_pages_contiguous(kBlkDmaPages);
        Arena* blk_dma = static_cast<Arena*>(
            mm::heap_alloc(sizeof(Arena), alignof(Arena)));
        if (blk_mmio && blk_dma_phys && blk_dma) {
            blk_mmio->phys_base    = g_hw.virtio_base;
            blk_mmio->size         = platform::kVirtioMmioSize;
            blk_mmio->attr         = MX_MMIO_DEVICE_nGnRnE;
            blk_mmio->flags        = 0;
            blk_mmio->device_hint  = 0;
            blk_mmio->region_index = 0;

            libk::memset(reinterpret_cast<void*>(blk_dma_phys), 0,
                         kBlkDmaPages * PAGE_SIZE);
            blk_dma->base_va   = 0;
            blk_dma->phys_base = blk_dma_phys;
            blk_dma->size      = kBlkDmaPages * PAGE_SIZE;
            blk_dma->owner_pid = init_proc.pid;
            blk_dma->rights    = RIGHT_READ | RIGHT_WRITE;
            blk_dma->flags     = kArenaFlagExposePhys;
            libk::memset(blk_dma->pad, 0, sizeof(blk_dma->pad));

            grant("virtio.blk.mmio", CAP_MMIO, blk_mmio,
                       RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
            grant("virtio.blk.dma", CAP_ARENA, blk_dma,
                       RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_REGISTER | RIGHT_DUP);
        } else {
            ANKO_LOG_WARN("kernel/init", "virtio-blk cap object allocation failed");
        }
    }
    __atomic_store_n(&boot->magic, MX_BOOT_MAGIC, __ATOMIC_RELEASE);
}

Process* spawn_image(const void* elf_data, usize elf_size, PrincipalClass principal,
                     u32 supervisor_pid, u32 flags, Thread** out_main_thread) {
    if (!elf_data || elf_size == 0) return nullptr;

    Process* p = sched::process_create(principal, supervisor_pid);
    if (!p) { ANKO_LOG_ERROR("kernel/loader", "process allocation failed"); return nullptr; }

    VmSpace space{};
    space.pgd_phys = p->ttbr0;
    space.pid = p->pid;

    const VirtAddr entry = load_image(space, elf_data, elf_size);
    if (!entry) {
        ANKO_LOG_ERROR("kernel/loader", "image validation or mapping failed");
        sched::process_destroy(p);
        return nullptr;
    }

    const PhysAddr stack = mm::alloc_page_for(p->pid);
    if (!stack) {
        ANKO_LOG_ERROR("kernel/loader", "stack allocation failed");
        sched::process_destroy(p);
        return nullptr;
    }
    libk::memset(reinterpret_cast<void*>(stack), 0, PAGE_SIZE);
    const VirtAddr stack_base = USER_STACK_TOP - PAGE_SIZE;
    if (!mm::vm_map_owned(space, stack_base, stack, RIGHT_READ | RIGHT_WRITE)) {
        ANKO_LOG_ERROR("kernel/loader", "stack mapping failed");
        mm::free_page_for(p->pid, stack);
        sched::process_destroy(p);
        return nullptr;
    }
    const VirtAddr sp_top = USER_STACK_TOP - 16;
    p->next_stack_slot = 1;

    const u32 thread_flags = (flags & SPAWN_SUSPENDED) ? THREAD_SUSPENDED : 0;
    Thread* t = sched::thread_create(p->pid, entry, sp_top, PRIO_SERVICE, 0, thread_flags);
    if (!t) {
        sched::process_destroy(p);
        return nullptr;
    }
    if (out_main_thread) *out_main_thread = t;
    if (!(flags & SPAWN_SUSPENDED)) sched::enqueue(t);
    return p;
}

Process* spawn_service(const char* service_name, PrincipalClass principal, u32 supervisor_pid,
                       u32 flags, Thread** out_main_thread) {
    if (!service_name) return nullptr;

    const void* blob = nullptr;
    usize blob_size = 0;
    if (!archive_get(service_name, &blob, &blob_size)) return nullptr;
    Process* p = spawn_image(blob, blob_size, principal, supervisor_pid, flags, out_main_thread);
    if (p) sched::process_set_name(p, service_name);
    return p;
}

void create_init_process() {
    PhysAddr initrd_base = 0;
    usize initrd_size = 0;

    if (!discover_initrd(g_dtb_phys, &initrd_base, &initrd_size) &&
        !load_system_bundle_from_disk(&initrd_base, &initrd_size)) {
        panic("no external system bundle");
    }
    if (!init_archive(initrd_base, initrd_size)) panic("system bundle authentication failed");

    const void* manifest_blob = nullptr;
    usize manifest_size = 0;
    if (!archive_get("manifest.json", &manifest_blob, &manifest_size)) panic("missing authenticated manifest");

    Process* init_proc = spawn_service("init", PRINCIPAL_NATIVE_TRUSTED, /*supervisor=*/0);
    if (!init_proc) {
        ANKO_LOG_ERROR("kernel/init", "spawn failed");
        return;
    }
    issue_boot_caps(*init_proc,
                    static_cast<PhysAddr>(reinterpret_cast<u64>(manifest_blob)),
                    manifest_size,
                    initrd_base,
                    initrd_size);
    ANKO_LOG_INFO("kernel/init", "init queued");
}

} // namespace kernel::init
