// kernel/init/boot_caps.cc — assemble the initial capability set for init.
//
// Spec ref: §8 V0 Boot Flow (step 9), §7.1 Init

#include "anko/cap.h"
#include "anko/platform_control.h"
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
#include "../../boards/huawei-anne/display_diagnostics.h"
#endif
#include "anko/boot_trace.h"
#include "anko/hw_info.h"
#include "anko/irq.h"
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
#include "anko/panic_screen.h"
#include "midix/abi/boot.h"
#include "libk/integer.h"
#include "libk/printf.h"
#include "libk/string.h"
#include "fido2_seed.h"

namespace drivers::fwcfg {
bool write_ramfb(PhysAddr pixel_phys, u32 width, u32 height);
} // namespace drivers::fwcfg

namespace drivers::pci_probe {
struct XhciInfo {
    PhysAddr bar0_phys;
    u64 bar0_size;
    u32 irq_line;
    u32 requester_id; // PCI requester id == SMMU stream id
    bool found;
};
XhciInfo find_xhci();
bool enable_xhci_bus_master(const XhciInfo& info);
MmioRange* alloc_xhci_mmio(const XhciInfo& info);
IrqLine* alloc_xhci_irq(const XhciInfo& info);
} // namespace drivers::pci_probe

namespace kernel::init {

// DTB physical address — provided by arch/arm64/entry.S.
extern "C" PhysAddr g_dtb_phys;

namespace {

constexpr u32 kKernelLogRingOrder = 6;

} // namespace

void issue_boot_caps(Process& init_proc, PhysAddr manifest_addr, usize manifest_size,
                     PhysAddr initrd_phys, usize initrd_size) {
    ANKO_BOOT_CHECKPOINT(CapsEnter);
    if (!init_proc.cap_table) {
        panic("init has no cap table");
    }
    const PhysAddr boot_page = mm::alloc_page_for(init_proc.pid);
    ANKO_BOOT_CHECKPOINT(CapsBootPage);
    auto* boot_arena = static_cast<Arena*>(mm::slab_alloc(sizeof(Arena), alignof(Arena)));
    if (!boot_page || !boot_arena) {
        panic("init BootInfo allocation failed");
    }
    ANKO_BOOT_CHECKPOINT(CapsSlab);
    libk::memset(reinterpret_cast<void*>(boot_page), 0, PAGE_SIZE);
    libk::memset(boot_arena, 0, sizeof(*boot_arena));
    boot_arena->phys_base = boot_page;
    boot_arena->size = PAGE_SIZE;
    boot_arena->owner_pid = init_proc.pid;
    boot_arena->rights = RIGHT_READ;
    const CapHandle boot_cap =
        cap::alloc(*init_proc.cap_table, CAP_ARENA, boot_arena, RIGHT_READ | RIGHT_MAP);
    if (cap_index(boot_cap) != 0 || !boot_cap) {
        panic("init BootInfo must be slot zero");
    }
    ANKO_BOOT_CHECKPOINT(CapsSlot0);
    auto* boot = reinterpret_cast<MxBootInfo*>(boot_page);
    boot->version = MX_BOOT_VERSION;
    auto add = [&](const char* name, CapHandle handle, u64 size = 0) {
        if (!handle || boot->entry_count >= MX_BOOT_MAX_ENTRIES) {
            panic("boot capability allocation failed");
        }
        auto& e = boot->entries[boot->entry_count++];
        e.type = MX_BOOT_ENTRY_HANDLE;
        e.handle = handle;
        e.aux0 = size;
        usize i = 0;
        for (; name[i] && i + 1 < sizeof(e.name); ++i) {
            e.name[i] = name[i];
        }
        e.name[i] = 0;
    };
    auto grant = [&](const char* name, CapType type, void* target, u32 rights) {
        const CapHandle h = cap::alloc(*init_proc.cap_table, type, target, rights);
        const u64 size = type == CAP_PAGE ? static_cast<MemoryObject*>(target)->size : 0;
        add(name, h, size);
        return h;
    };

    ANKO_BOOT_CHECKPOINT(CapsArena);
    auto* manifest =
        static_cast<MemoryObject*>(mm::heap_alloc(sizeof(MemoryObject), alignof(MemoryObject)));
    auto* initrd =
        static_cast<MemoryObject*>(mm::heap_alloc(sizeof(MemoryObject), alignof(MemoryObject)));
    if (!manifest || !initrd) {
        return;
    }

    const usize manifest_alloc_size =
        libk::align_up(manifest_size ? manifest_size : PAGE_SIZE, PAGE_SIZE);
    const usize manifest_pages = manifest_alloc_size / PAGE_SIZE;
    const PhysAddr manifest_page = mm::alloc_pages_contiguous(manifest_pages);
    if (!manifest_page) {
        return;
    }
    libk::memset(reinterpret_cast<void*>(manifest_page), 0, manifest_alloc_size);
    if (manifest_addr) {
        libk::memcpy(reinterpret_cast<void*>(manifest_page),
                     reinterpret_cast<const void*>(manifest_addr), manifest_size);
    }
    ANKO_BOOT_CHECKPOINT(CapsManifest);
    manifest->phys_base = manifest_page;
    manifest->size = manifest_alloc_size;

    ANKO_BOOT_CHECKPOINT(CapsCopy);
    initrd->phys_base = initrd_phys;
    initrd->size = initrd_size;

    auto& cpus = boot->entries[boot->entry_count++];
    cpus.type = MX_BOOT_ENTRY_VALUE;
    cpus.aux0 = smp::cpu_count();
    libk::memcpy(cpus.name, "cpu.count", 10);
    auto& cpu_mask = boot->entries[boot->entry_count++];
    cpu_mask.type = MX_BOOT_ENTRY_VALUE;
    cpu_mask.aux0 = sched::online_cpu_mask();
    libk::memcpy(cpu_mask.name, "cpu.mask", 9);

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
    grant("nameservice", CAP_NAMESERVICE, &init_proc, RIGHT_REGISTER | RIGHT_READ | RIGHT_DUP);

    // 5) UART cap
    grant("uart.mmio", CAP_UART, reinterpret_cast<void*>(g_hw.console_base),
          RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);

    // 6) VM root authority. Init policy grants reduced VM authority to Dango.
    grant("vm.root", CAP_VM_ROOT, vm::root_object(),
          RIGHT_VM_CREATE | RIGHT_VM_INSPECT | RIGHT_DUP);

    // 6b) System power authority: kernel-mediated shutdown/reset (PSCI).
    grant("system.control", CAP_PLATFORM, platform::board_control(platform::Domain::System),
          RIGHT_WRITE | RIGHT_DUP);

    // 7) Global log doorbell followed by one kernel log ring per supported CPU.
    Doorbell* log_db = ipc::doorbell_create_object(init_proc.pid);
    if (!log_db) {
        panic("log doorbell allocation failed");
    }
    add("log.doorbell",
        ipc::doorbell_cap_alloc(init_proc.pid, log_db, RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP));

    Ring* kernel_log_rings[kMaxCpus]{};
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
        Ring* ring = ipc::ring_create_object(init_proc.pid, kKernelLogRingOrder, 0);
        kernel_log_rings[cpu] = ring;
        if (!ring) {
            panic("kernel log ring allocation failed");
        }
        char name[] = "kernel.log.0";
        name[11] = static_cast<char>('0' + cpu);
        add(name, ipc::ring_cap_alloc(init_proc.pid, ring,
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP));
    }
    anko::log::attach_console(kernel_log_rings, log_db, kMaxCpus);

#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    // Dedicated platform authority; no raw GPU/MMU or firmware-mailbox mapping
    // is granted to userland. Assignment validates VM and vCPU ownership.
    grant("gpu.platform", CAP_PLATFORM, platform::board_control(platform::Domain::Gpu),
          RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
    if (g_hw.has_anne_usb) {
        struct Resource {
            const char* name;
            PhysAddr base;
            u64 bytes;
        };
        constexpr Resource resources[] = {
            {"dwc2.mmio", 0xff100000, 0x40000},
            {"usb.phy.mmio", 0xff200000, 4096},
            {"i2c.mmio", 0xfdf0d000, 4096},
            {"emmc.mmio", 0xff390000, 4096},
            // Captured Anne DTB: gpio25, volume-up bit 1, active low.
            {"fido2.presence.mmio", 0xfff0e000, 4096},
        };
        for (const auto& r : resources) {
            auto* range =
                static_cast<MmioRange*>(mm::slab_alloc(sizeof(MmioRange), alignof(MmioRange)));
            if (!range) {
                panic("Anne USB resource allocation failed");
            }
            *range = {r.base, r.bytes, MX_MMIO_DEVICE_nGnRnE, 0, 0, 0};
            grant(r.name, CAP_MMIO, range, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
        }
        const auto seed_page = mm::alloc_page_for(init_proc.pid);
        auto* seed_arena = static_cast<Arena*>(mm::slab_alloc(sizeof(Arena), alignof(Arena)));
        if (!seed_page || !seed_arena) {
            panic("FIDO2 seed allocation failed");
        }
        libk::memset(reinterpret_cast<void*>(seed_page), 0, PAGE_SIZE);
        libk::memcpy(reinterpret_cast<void*>(seed_page), &kFido2Seed, sizeof(kFido2Seed));
        *seed_arena = {};
        seed_arena->phys_base = seed_page;
        seed_arena->size = PAGE_SIZE;
        seed_arena->owner_pid = init_proc.pid;
        seed_arena->rights = RIGHT_READ;
        grant("fido2.seed", CAP_ARENA, seed_arena, RIGHT_READ | RIGHT_MAP | RIGHT_DUP);
        auto* line = static_cast<IrqLine*>(mm::slab_alloc(sizeof(IrqLine), alignof(IrqLine)));
        if (!line) {
            panic("Anne USB IRQ allocation failed");
        }
        *line = {191, 0, 0, 0, 0};
        grant("dwc2.irq", CAP_IRQ, line, RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
        grant("usb.phy.control", CAP_PLATFORM, platform::board_control(platform::Domain::UsbPhy),
              RIGHT_WRITE | RIGHT_DUP);
        grant("usb.fence", CAP_PLATFORM, platform::board_control(platform::Domain::UsbController),
              RIGHT_WRITE | RIGHT_DUP);
        grant("i2c.control", CAP_PLATFORM, platform::board_control(platform::Domain::I2c),
              RIGHT_WRITE | RIGHT_DUP);
        grant("emmc.control", CAP_PLATFORM, platform::board_control(platform::Domain::Emmc),
              RIGHT_WRITE | RIGHT_DUP);
        // Dedicated ADMA descriptor/data memory, pinned for the entire boot.
        // Register before publishing so identity and user aliases share NC
        // attributes. Ordinary filesystem/client arenas stay CPU-cacheable.
        constexpr usize kEmmcDmaPages = 17;
        const PhysAddr emmc_dma_phys = mm::alloc_pages_contiguous(kEmmcDmaPages);
        auto* emmc_dma = static_cast<Arena*>(mm::heap_alloc(sizeof(Arena), alignof(Arena)));
        if (!emmc_dma || !emmc_dma_phys ||
            emmc_dma_phys + kEmmcDmaPages * PAGE_SIZE > (1ull << 32)) {
            panic("eMMC DMA allocation failed");
        }
        if (!mm::register_noncached_range(emmc_dma_phys, kEmmcDmaPages * PAGE_SIZE)) {
            panic("eMMC DMA memory attributes failed");
        }
        libk::memset(reinterpret_cast<void*>(emmc_dma_phys), 0, kEmmcDmaPages * PAGE_SIZE);
        *emmc_dma = {};
        emmc_dma->phys_base = emmc_dma_phys;
        emmc_dma->size = kEmmcDmaPages * PAGE_SIZE;
        emmc_dma->owner_pid = init_proc.pid;
        emmc_dma->rights = RIGHT_READ | RIGHT_WRITE;
        emmc_dma->flags = kArenaFlagExposePhys; // no OwnsPages: never recycled
        grant("emmc.dma", CAP_ARENA, emmc_dma, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
#ifdef ANKO_ANNE_FRAMEBUFFER
        u64 address = 0;
        u32 width = 0, height = 0;
        if (anne::native_display::framebuffer(address, width, height)) {
            // LK's logo scanout is pixel aligned, but not necessarily page aligned.
            const u64 base = address & ~(u64{PAGE_SIZE} - 1);
            const u64 offset = address - base;
            const usize size = libk::align_up(offset + u64{width} * height * 4, PAGE_SIZE);
            if (!mm::register_noncached_range(base, size)) {
                panic("Anne framebuffer attributes failed");
            }
            auto* range =
                static_cast<MmioRange*>(mm::slab_alloc(sizeof(MmioRange), alignof(MmioRange)));
            if (!range) {
                panic("Anne framebuffer allocation failed");
            }
            *range = {base, size, MX_MMIO_NORMAL_NC, 0, 0, 0};
            if (boot->entry_count >= MX_BOOT_MAX_ENTRIES) {
                panic("boot capability allocation failed");
            }
            auto& pixel_offset = boot->entries[boot->entry_count++];
            pixel_offset.type = MX_BOOT_ENTRY_VALUE;
            pixel_offset.aux0 = offset;
            libk::memcpy(pixel_offset.name, "fb.offset", 10);
            grant("fb.memory", CAP_MMIO, range, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
            grant("fb.control", CAP_PLATFORM, platform::board_control(platform::Domain::Display),
                  RIGHT_WRITE | RIGHT_DUP);
        }
#endif
    }
#endif

    // 11) Framebuffer.  Two paths:
    //   T210: Hekate pre-configured TegraDC0; g_hw.t210_dc_fb_phys is the
    //         physical address of the active buffer. Map it as NORMAL_NC.
    //   QEMU: Allocate RAM, register it with ramfb via fw-cfg.
    if (g_hw.has_t210_dc) {
        // T210 path — re-use Hekate's framebuffer directly.
        const usize fb_size =
            static_cast<usize>(g_hw.t210_dc_fb_width) * g_hw.t210_dc_fb_height * 4u;
        const usize fb_size_pages = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        auto* fb_range =
            static_cast<MmioRange*>(mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
        if (fb_range) {
            fb_range->phys_base = g_hw.t210_dc_fb_phys;
            fb_range->size = fb_size_pages;
            fb_range->attr = MX_MMIO_NORMAL_NC;
            fb_range->flags = 0;
            fb_range->device_hint = 0;
            fb_range->region_index = 0;
            grant("fb.memory", CAP_MMIO, fb_range,
                  RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
        } else {
            ANKO_LOG_WARN("kernel/init", "T210 fb_range alloc failed");
        }
        // Give the panic path a screen to paint before init owns the framebuffer.
        anko::panic_screen::bind(reinterpret_cast<volatile u32*>(g_hw.t210_dc_fb_phys),
                                 g_hw.t210_dc_fb_width, g_hw.t210_dc_fb_height);
    } else if (g_hw.has_fwcfg) {
        // QEMU ramfb path.
        constexpr u32 kFbWidth = 1024;
        constexpr u32 kFbHeight = 768;
        constexpr usize kFbPages = (kFbWidth * kFbHeight * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
        const PhysAddr fb_phys = mm::alloc_pages_contiguous(kFbPages);
        if (fb_phys) {
            libk::memset(reinterpret_cast<void*>(fb_phys), 0, kFbPages * PAGE_SIZE);
            if (!mm::register_noncached_range(fb_phys, kFbPages * PAGE_SIZE)) {
                panic("framebuffer memory attributes failed");
            }
            const bool ramfb_ok = drivers::fwcfg::write_ramfb(fb_phys, kFbWidth, kFbHeight);
            if (!ramfb_ok) {
                ANKO_LOG_WARN("kernel/init", "ramfb init failed — framebuffer will be blank");
            }
            auto* fb_range =
                static_cast<MmioRange*>(mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
            if (fb_range) {
                fb_range->phys_base = fb_phys;
                fb_range->size = kFbPages * PAGE_SIZE;
                fb_range->attr = MX_MMIO_NORMAL_NC;
                fb_range->flags = 0;
                fb_range->device_hint = 0;
                fb_range->region_index = 0;
                grant("fb.memory", CAP_MMIO, fb_range,
                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
            }
            // Give the panic path a screen to paint before init owns the framebuffer.
            anko::panic_screen::bind(reinterpret_cast<volatile u32*>(fb_phys), kFbWidth, kFbHeight);
        } else {
            ANKO_LOG_WARN("kernel/init", "framebuffer RAM alloc failed");
        }
    }

#ifdef ANKO_PANIC_SCREEN_DEMO
    // Dev-only capture hook: raise a real panic once the framebuffer exists so
    // scripts/test/panic-screen.py can assert the whole panic-screen path end to
    // end (no userland has run yet, so nothing repaints over it).
    //
    // ANKO_PANIC_DEMO_VECTOR selects an exception vector slot instead of the
    // plain panic: it exercises kernel_unexpected_exception (the SError/FIQ path)
    // with a synthetic frame, which the capture harness checks for the reason
    // string. Zero keeps the plain panic.
#if ANKO_PANIC_DEMO_VECTOR
    {
        TrapFrame frame{};
        kernel_unexpected_exception(ANKO_PANIC_DEMO_VECTOR, 0, 0, &frame);
    }
#else
    panic("panic screen demo (ANKO_PANIC_SCREEN_DEMO)");
#endif
#endif

    // 12..14) xHCI host controller.  Two paths:
    //   T210: T210 XUSB at 0x70090000 (xHCI-compatible, FALCON pre-loaded).
    //         No IRQ cap — polling mode, SMMU required for IOMMU setup.
    //   QEMU: PCI xHCI discovered via ECAM scan.
    if (g_hw.console_kind == ConsoleKind::Tegra16550) {
        // T210 XUSB xHCI registers.
        constexpr PhysAddr kXusbBase = 0x7009'0000ull;
        constexpr usize kXusbSize = 0x8000u; // 32 KiB xHCI register window
        constexpr usize kXhciDmaPages = 64;
        auto* xhci_mmio =
            static_cast<MmioRange*>(mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
        const PhysAddr xhci_dma_phys = mm::alloc_pages_contiguous_aligned(kXhciDmaPages, 16);
        auto* xhci_dma = static_cast<Arena*>(mm::heap_alloc(sizeof(Arena), alignof(Arena)));
        if (xhci_mmio && xhci_dma_phys && xhci_dma) {
            if (!mm::register_noncached_range(xhci_dma_phys, kXhciDmaPages * PAGE_SIZE)) {
                panic("xhci DMA memory attributes failed");
            }
            xhci_mmio->phys_base = kXusbBase;
            xhci_mmio->size = kXusbSize;
            xhci_mmio->attr = MX_MMIO_DEVICE_nGnRnE;
            xhci_mmio->flags = 0;
            xhci_mmio->device_hint = 0;
            xhci_mmio->region_index = 0;

            libk::memset(reinterpret_cast<void*>(xhci_dma_phys), 0, kXhciDmaPages * PAGE_SIZE);
            xhci_dma->base_va = 0;
            xhci_dma->phys_base = xhci_dma_phys;
            xhci_dma->size = kXhciDmaPages * PAGE_SIZE;
            xhci_dma->owner_pid = init_proc.pid;
            xhci_dma->rights = RIGHT_READ | RIGHT_WRITE;
            xhci_dma->flags = kArenaFlagExposePhys;
            libk::memset(xhci_dma->pad, 0, sizeof(xhci_dma->pad));

            // Slot 12: xhci.mmio
            grant("xhci.mmio", CAP_MMIO, xhci_mmio,
                  RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
            // Slot 13: placeholder IRQ cap — keeps slot numbering consistent
            // with the QEMU path.  irq_id=0xFFFFFFFF signals no real IRQ;
            // the xhci driver uses polling mode via ANKO_XHCI_NORESET.
            auto* t210_irq =
                static_cast<IrqLine*>(mm::heap_alloc(sizeof(IrqLine), alignof(IrqLine)));
            if (t210_irq) {
                t210_irq->irq_id = 0xFFFF'FFFFu; // sentinel: invalid
                t210_irq->trigger = 0;
                t210_irq->flags = 0;
                t210_irq->device_hint = 0;
                t210_irq->doorbell_cap = 0;
                grant("xhci.irq", CAP_IRQ, t210_irq, RIGHT_READ | RIGHT_DUP);
            }
            // Slot 14: xhci.dma arena
            grant("xhci.dma", CAP_ARENA, xhci_dma,
                  RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_REGISTER | RIGHT_DUP);
        } else {
            ANKO_LOG_WARN("kernel/init", "T210 xhci cap object allocation failed");
        }
    } else if (g_hw.has_pcie) {
        // QEMU path: PCI xHCI scan.
        constexpr usize kXhciDmaPages = 64;
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
            IrqLine* xhci_irq = drivers::pci_probe::alloc_xhci_irq(xhci);
            const PhysAddr xhci_dma_phys = mm::alloc_pages_contiguous_aligned(kXhciDmaPages, 16);
            Arena* xhci_dma = static_cast<Arena*>(mm::heap_alloc(sizeof(Arena), alignof(Arena)));
            if (xhci_mmio && xhci_irq && xhci_dma_phys && xhci_dma) {
                if (!mm::register_noncached_range(xhci_dma_phys, kXhciDmaPages * PAGE_SIZE)) {
                    panic("xhci DMA memory attributes failed");
                }
                libk::memset(reinterpret_cast<void*>(xhci_dma_phys), 0, kXhciDmaPages * PAGE_SIZE);
                xhci_dma->base_va = 0;
                xhci_dma->phys_base = xhci_dma_phys;
                xhci_dma->size = kXhciDmaPages * PAGE_SIZE;
                xhci_dma->owner_pid = init_proc.pid;
                xhci_dma->rights = RIGHT_READ | RIGHT_WRITE;
                xhci_dma->flags = kArenaFlagExposePhys;
                libk::memset(xhci_dma->pad, 0, sizeof(xhci_dma->pad));

                bool dma_ready = true;
                if (iommu::backend_for({pci::kSegment, xhci.requester_id})) {
                    dma_ready =
                        iommu::register_native_dma_arena(
                            iommu::IommuDeviceId{pci::kSegment, xhci.requester_id}, xhci_dma_phys,
                            static_cast<u64>(kXhciDmaPages) * PAGE_SIZE) == E_OK;
                }
#ifndef ANKO_BUILD_DEV
                else
                    dma_ready = false;
#endif
                if (!dma_ready || !drivers::pci_probe::enable_xhci_bus_master(xhci)) {
                    ANKO_LOG_ERROR("kernel/init",
                                   "xhci DMA activation denied; controller remains offline");
                } else {

                    grant("xhci.mmio", CAP_MMIO, xhci_mmio,
                          RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
                    grant("xhci.irq", CAP_IRQ, xhci_irq, RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
                    grant("xhci.dma", CAP_ARENA, xhci_dma,
                          RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_REGISTER | RIGHT_DUP);
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
        // QEMU virt swizzles PCI INTx onto GIC SPIs 3..6 (rotating per slot).
        // The DTB interrupt specifier is <type number flags> and for arm,gic the
        // INTID is 32 + number; the fixture (tests/fixtures/phasea-iommu-smmu-node.dts)
        // shows pin A of slot 0 -> number 3.
        //   intid = 32 + (3 + pin_index) % 4,  pin A = 1 -> (3 + device) % 4
        // Physical boards parse their own map in Phase P.
        pci::PciFunctionInfo gpu_info{};
        if (pci::find_by_id(g_hw.ecam_base, pci::kVirtioVendorId, pci::kVirtioGpuDeviceId,
                            &gpu_info)) {
            static pci::HostFunction g_gpu_function;
            g_gpu_function.info = gpu_info;
            g_gpu_function.group_id =
                pci::isolation_group_id(gpu_info.segment, gpu_info.bus, gpu_info.device);
            g_gpu_function.flags = 0;

            static IrqLine g_gpu_irq;
            g_gpu_irq.irq_id =
                irq::spi_intid(pci::kQemuVirtIntxPinA + (gpu_info.device % pci::kPciIntxPinCount));
            g_gpu_irq.trigger = 0; // INTx is level-triggered
            g_gpu_irq.flags = 0;
            g_gpu_irq.device_hint = 0;
            g_gpu_irq.doorbell_cap = 0;

            grant("gpu.function", CAP_PCI_FUNCTION, &g_gpu_function,
                  RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
            grant("gpu.irq", CAP_IRQ, &g_gpu_irq, RIGHT_READ | RIGHT_WRITE | RIGHT_DUP);
            ANKO_LOGF_INFO("kernel/init",
                           "gpu: function bus=%u dev=%u fn=%u vendor=%x device=%x group=%u intx=%u",
                           gpu_info.bus, gpu_info.device, gpu_info.function, gpu_info.vendor_id,
                           gpu_info.device_id, g_gpu_function.group_id, g_gpu_irq.irq_id);
        }
    }

    // 15..16) Virtio-blk block device (QEMU only — T210 uses AmeFS via SATA/SD).
    if (g_hw.has_virtio_blk) {
        auto* blk_mmio =
            static_cast<MmioRange*>(mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
        constexpr usize kBlkDmaPages = 64;
        const PhysAddr blk_dma_phys = mm::alloc_pages_contiguous(kBlkDmaPages);
        Arena* blk_dma = static_cast<Arena*>(mm::heap_alloc(sizeof(Arena), alignof(Arena)));
        if (blk_mmio && blk_dma_phys && blk_dma) {
            // Virtio and io.fs share this physical arena with the device.
            // Register before the first write so kernel and EL0 aliases agree;
            // ordering barriers cannot repair cached/noncached aliasing.
            if (!mm::register_noncached_range(blk_dma_phys, kBlkDmaPages * PAGE_SIZE)) {
                panic("virtio-blk DMA noncached registration failed");
            }
            blk_mmio->phys_base = g_hw.virtio_base;
            blk_mmio->size = platform::kVirtioMmioSize;
            blk_mmio->attr = MX_MMIO_DEVICE_nGnRnE;
            blk_mmio->flags = 0;
            blk_mmio->device_hint = 0;
            blk_mmio->region_index = 0;

            libk::memset(reinterpret_cast<void*>(blk_dma_phys), 0, kBlkDmaPages * PAGE_SIZE);
            blk_dma->base_va = 0;
            blk_dma->phys_base = blk_dma_phys;
            blk_dma->size = kBlkDmaPages * PAGE_SIZE;
            blk_dma->owner_pid = init_proc.pid;
            blk_dma->rights = RIGHT_READ | RIGHT_WRITE;
            blk_dma->flags = kArenaFlagExposePhys;
            libk::memset(blk_dma->pad, 0, sizeof(blk_dma->pad));

            grant("virtio.blk.mmio", CAP_MMIO, blk_mmio,
                  RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
            grant("virtio.blk.dma", CAP_ARENA, blk_dma,
                  RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_REGISTER | RIGHT_DUP);
        } else {
            ANKO_LOG_WARN("kernel/init", "virtio-blk cap object allocation failed");
        }
    }
    ANKO_BOOT_CHECKPOINT(CapsGrants);
    __atomic_store_n(&boot->magic, MX_BOOT_MAGIC, __ATOMIC_RELEASE);
}

Process* spawn_image(const void* elf_data, usize elf_size, PrincipalClass principal,
                     u32 supervisor_pid, u32 flags, Thread** out_main_thread) {
    if (!elf_data || elf_size == 0) {
        return nullptr;
    }

    Process* p = sched::process_create(principal, supervisor_pid);
    if (!p) {
        ANKO_LOG_ERROR("kernel/loader", "process allocation failed");
        return nullptr;
    }
    p->kill_with_supervisor = (flags & MX_SPAWN_KILL_WITH_SUPERVISOR) != 0;

    VmSpace space{};
    space.pgd_phys = p->ttbr0;
    space.pid = p->pid;

    const VirtAddr entry = load_image(space, elf_data, elf_size);
    if (!entry) {
        ANKO_LOG_ERROR("kernel/loader", "image validation or mapping failed");
        sched::process_destroy(p);
        return nullptr;
    }

    // The initial user thread needs more than a single 4 KiB page: both msh and
    // the appliance manager blew a one-page stack (see the note in
    // userland/msh/main.cc). Give the entry thread a few pages and start later
    // per-thread stacks below them, so a service cannot fault on stack depth.
    constexpr u32 kInitialStackPages = 4;
    const VirtAddr stack_base = USER_STACK_TOP - kInitialStackPages * PAGE_SIZE;
    u32 mapped = 0;
    for (; mapped < kInitialStackPages; ++mapped) {
        const PhysAddr stack = mm::alloc_page_for(p->pid);
        if (!stack) {
            break;
        }
        libk::memset(reinterpret_cast<void*>(stack), 0, PAGE_SIZE);
        if (!mm::vm_map_owned(space, stack_base + static_cast<VirtAddr>(mapped) * PAGE_SIZE, stack,
                              RIGHT_READ | RIGHT_WRITE)) {
            mm::free_page_for(p->pid, stack);
            break;
        }
    }
    if (mapped != kInitialStackPages) {
        for (u32 i = 0; i < mapped; ++i) {
            mm::vm_unmap_owned_range(space, stack_base + static_cast<VirtAddr>(i) * PAGE_SIZE,
                                     PAGE_SIZE);
        }
        ANKO_LOG_ERROR("kernel/loader", "stack allocation failed");
        sched::process_destroy(p);
        return nullptr;
    }
    const VirtAddr sp_top = USER_STACK_TOP - 16;
    p->next_stack_slot = kInitialStackPages;

    const u32 thread_flags = (flags & SPAWN_SUSPENDED) ? THREAD_SUSPENDED : 0;
    Thread* t = sched::thread_create(p->pid, entry, sp_top, PRIO_SERVICE, 0, thread_flags);
    if (!t) {
        sched::process_destroy(p);
        return nullptr;
    }
    if (out_main_thread) {
        *out_main_thread = t;
    }
    if (!(flags & SPAWN_SUSPENDED)) {
        sched::enqueue(t);
    }
    return p;
}

Process* spawn_service(const char* service_name, PrincipalClass principal, u32 supervisor_pid,
                       u32 flags, Thread** out_main_thread) {
    if (!service_name) {
        return nullptr;
    }

    const void* blob = nullptr;
    usize blob_size = 0;
    if (!archive_get(service_name, &blob, &blob_size)) {
        return nullptr;
    }
    Process* p = spawn_image(blob, blob_size, principal, supervisor_pid, flags, out_main_thread);
    if (p) {
        sched::process_set_name(p, service_name);
    }
    return p;
}

void create_init_process() {
    ANKO_BOOT_CHECKPOINT(BundleBegin);
    PhysAddr initrd_base = 0;
    usize initrd_size = 0;

    if (!discover_initrd(g_dtb_phys, &initrd_base, &initrd_size) &&
        !load_system_bundle_from_disk(&initrd_base, &initrd_size)) {
        panic("no external system bundle");
    }
    ANKO_BOOT_CHECKPOINT(BundleLoaded);
    if (!init_archive(initrd_base, initrd_size)) {
        panic("system bundle authentication failed");
    }
    ANKO_BOOT_CHECKPOINT(BundleAuthenticated);

    const void* manifest_blob = nullptr;
    usize manifest_size = 0;
    if (!archive_get("manifest.json", &manifest_blob, &manifest_size)) {
        panic("missing authenticated manifest");
    }

    ANKO_BOOT_CHECKPOINT(InitSpawnBegin);
    Process* init_proc = spawn_service("init", PRINCIPAL_NATIVE_TRUSTED, /*supervisor=*/0);
    if (!init_proc) {
        ANKO_LOG_ERROR("kernel/init", "spawn failed");
        return;
    }
    ANKO_BOOT_CHECKPOINT(InitSpawnDone);
    issue_boot_caps(*init_proc, static_cast<PhysAddr>(reinterpret_cast<u64>(manifest_blob)),
                    manifest_size, initrd_base, initrd_size);
    ANKO_BOOT_CHECKPOINT(InitCapsDone);
    ANKO_LOG_INFO("kernel/init", "init queued");
}

} // namespace kernel::init
