// Exercise the production DOP planner/emitter against dirty allocator pages.
// Only privileged CPU instructions and page/object ownership are substituted.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>
#include "../kernel/mm/vm_space.cc"

HwInfo g_hw{};
extern "C" {
alignas(4096) char __text_start[4096]{};
char __kernel_end[1]{};
}
static std::unordered_set<PhysAddr> pages;
static int allocation_budget = -1;
static Process process;
static u32 tlbi_count;
static u32 owned_frees;

namespace mm {
PhysAddr alloc_page() {
    if (allocation_budget == 0) return 0;
    if (allocation_budget > 0) --allocation_budget;
    auto* p = std::aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    assert(p);
    std::memset(p, 0xa5, PAGE_SIZE); // detect uninitialized descriptors/holes
    const auto pa = reinterpret_cast<PhysAddr>(p);
    assert(pages.insert(pa).second);
    return pa;
}
void free_page(PhysAddr pa) {
    assert(pages.erase(pa) == 1);
    std::free(reinterpret_cast<void*>(pa));
}
void free_page_for(u32, PhysAddr pa) { ++owned_frees; free_page(pa); }
}
namespace object {
Record* acquire(void* p) { return reinterpret_cast<Record*>(p); }
void release(Record*) {}
}
namespace sched {
object::Ref<Process> find_process(u32 pid) {
    return pid == process.pid ? object::Ref<Process>(&process) : object::Ref<Process>{};
}
}
[[noreturn]] void panic(const char*) { std::abort(); }
#ifdef ANKO_BOARD_HUAWEI_ANNE
namespace anne { void boot_checkpoint(BootStage) {} }
#endif

// Decode stage-1 tables independently; all identity leaves must remain L3.
static u64 descriptor(PhysAddr root, u64 va) {
    constexpr u64 mask = 0x0000fffffffff000ull;
    for (unsigned shift : {39, 30, 21}) {
        const auto* table = reinterpret_cast<const u64*>(root);
        const u64 link = table[(va >> shift) & 511];
        if (!(link & 1)) return 0;
        assert((link & 3) == 3);
        root = link & mask;
    }
    return reinterpret_cast<const u64*>(root)[(va >> 12) & 511];
}
struct Observed { PhysAddr root; u64 va, original; bool broken; };
static std::vector<Observed> observed;
extern "C" void anko_vm_test_tlbi() {
    ++tlbi_count;
    // A changed valid attribute must have had an invalid descriptor at an
    // earlier shootdown. This catches make-before-break in the NC update path.
    for (auto& entry : observed) {
        const u64 value = descriptor(entry.root, entry.va);
        if (!value) entry.broken = true;
        else if (value != entry.original) assert(entry.broken);
    }
}

static void verify_plan(const mm::IdentityPlan& plan, bool device) {
    for (u32 i = 0; i < plan.count; ++i) {
        const auto& run = plan.runs[i];
        assert(run.begin < run.end && !((run.begin | run.end) & 4095));
        for (u64 pa = run.begin; pa < run.end; pa += 4096) {
            // The old per-page policy is the reference. The test expands every
            // run, including non-page-aligned framebuffer endpoints.
            assert(run.attributes == mm::kernel_page_attrs(device, pa));
        }
    }
}

static void policy_boundaries() {
    g_hw.ram_base = 0x40000000;
    g_hw.ram_size = 128 * 1024 * 1024;
    g_hw.has_t210_dc = true;
    g_hw.t210_dc_fb_phys = 0x40200104;
    g_hw.t210_dc_fb_width = 17;
    g_hw.t210_dc_fb_height = 257;
    mm::g_noncached[0] = {0x401ff000, 3 * 4096};
    mm::g_noncached[1] = {0x40200000, 0x400000}; // overlapping NC policies
    mm::g_noncached_count = 2;
    for (bool device : {false, true}) {
        mm::IdentityPlan plan;
        assert(mm::append_identity_range(plan, 0x40000001, 128 * 1024 * 1024 - 1, device));
        assert(plan.runs[0].begin == 0x40000000);
        assert(plan.runs[plan.count - 1].end == 0x48000000);
        assert(plan.count < 20);
        verify_plan(plan, device);
    }
    mm::IdentityPlan invalid;
    assert(!mm::append_identity_range(invalid, ~u64{0} - 4095, 8192, false));
    assert(invalid.count == 0);
    g_hw.has_t210_dc = false;
    mm::g_noncached_count = 0;
}

static void partial_leaf_and_duplicate_batch() {
    VmSpace space{mm::alloc_page(), 0};
    mm::clear_table(space.pgd_phys);
    assert(mm::map_identity_range(space, 0x10003001, 8192, false));
    for (u64 va = 0x10000000; va < 0x10200000; va += 4096) {
        const u64 pte = descriptor(space.pgd_phys, va);
        if (va >= 0x10003000 && va < 0x10006000) {
            assert((pte & 0x0000fffffffff000ull) == va);
            assert((pte & 3) == 3 && !(pte & (1ull << 6))); // privileged only
        } else assert(pte == 0);
    }
    const auto count = pages.size();
    assert(!mm::map_identity_range(space, 0x10002000, 8192, true));
    assert(!descriptor(space.pgd_phys, 0x10002000));
    assert(pages.size() == count);
    mm::vm_space_destroy(space);
    assert(pages.empty());
}

int main() {
    policy_boundaries();
    partial_leaf_and_duplicate_batch();
    g_hw.console_base = 0x09000000;
    g_hw.gic_dist_base = 0x08000000;
    g_hw.gic_cpu_base = 0x08010000;
    mm::vm_init();
    const u32 plan_count = mm::g_identity_plan.count;
    assert(mm::g_identity_plan_valid && plan_count < 32);
    VmSpace first = mm::vm_space_create(1);
    assert(first.pgd_phys && mm::g_identity_plan.count == plan_count);
    process.pid = 1;
    process.ttbr0 = first.pgd_phys;
    for (u64 pa = 0x40000000; pa < 0x48000000; pa += 4096) {
        const u64 expected = pa | 3 | mm::kernel_page_attrs(false, pa);
        assert(descriptor(first.pgd_phys, pa) == expected);
        assert(descriptor(mm::g_kernel_root, pa) == expected);
    }
    PhysAddr translated = 0;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    const u64 emmc = 0xff390000 | 3 | mm::kernel_page_attrs(true, 0xff390000);
    assert(descriptor(first.pgd_phys, 0xff390000) == emmc);
    assert(descriptor(mm::g_kernel_root, 0xff390000) == emmc);
    assert(!mm::user_va_to_pa(first, 0xff390000, RIGHT_WRITE, &translated));
#else
    assert(!descriptor(first.pgd_phys, 0xff390000));
#endif
    assert(!mm::user_va_to_pa(first, 0x40000000, RIGHT_READ, &translated));
    assert(!mm::user_range_mapped(first, 0x40000000, 4096, RIGHT_READ));
    assert(!mm::map_kernel_device_mmio(0x40000000, 4096));

    // Conversion spans two leaves and updates both existing identity maps.
    for (PhysAddr root : {mm::g_kernel_root, first.pgd_phys}) {
        for (u64 va = 0x401ff000; va < 0x40202000; va += 4096) {
            observed.push_back({root, va, descriptor(root, va), false});
        }
    }
    assert(mm::register_noncached_range(0x401ff000, 3 * 4096));
    assert(!mm::g_identity_plan_valid);
    for (const auto& entry : observed) {
        assert(entry.broken);
        assert(((descriptor(entry.root, entry.va) >> 2) & 7) == 2);
        assert(((descriptor(entry.root, entry.va - 3 * 4096) >> 2) & 7) == 0);
    }
    observed.clear();
    VmSpace second = mm::vm_space_create(2);
    assert(second.pgd_phys && mm::g_identity_plan_valid);
    assert(((descriptor(second.pgd_phys, 0x401ff000) >> 2) & 7) == 2);
    assert(((descriptor(second.pgd_phys, 0x40202000) >> 2) & 7) == 0);

    // Real Anne logo pixels begin 0xb00 bytes into a page. The whole span
    // must use NC attributes, including the extra last page, in every alias.
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    constexpr u64 scanout = 0x5d964b00;
    constexpr u64 pixel_bytes = 1080ull * 2280 * 4;
    constexpr u64 pixel_offset = scanout & 4095;
    constexpr u64 fb_base = scanout - pixel_offset;
    constexpr u64 fb_size = (pixel_offset + pixel_bytes + 4095) & ~4095ull;
    assert(!mm::register_noncached_range(scanout, fb_size));
    assert(mm::register_noncached_range(fb_base, fb_size));
    assert(mm::range_is_noncached(fb_base, fb_size));
    assert(((descriptor(first.pgd_phys, fb_base) >> 2) & 7) == 2);
    assert(((descriptor(first.pgd_phys, fb_base + fb_size - 4096) >> 2) & 7) == 2);
    constexpr u64 fb_va = 0x30000000;
    assert(mm::vm_map_range(first, fb_va, fb_base, fb_size, RIGHT_READ | RIGHT_WRITE));
    assert(mm::user_va_to_pa(first, fb_va + pixel_offset, RIGHT_WRITE, &translated));
    assert(translated == scanout);
    assert(mm::user_va_to_pa(first, fb_va + pixel_offset + pixel_bytes - 1, RIGHT_WRITE, &translated));
    assert(translated == scanout + pixel_bytes - 1);
#endif

    const auto allocated = pages.size();
    for (int budget = 0; budget < 8; ++budget) {
        allocation_budget = budget;
        VmSpace failed = mm::vm_space_create(3);
        assert(!failed.pgd_phys && pages.size() == allocated);
    }
    allocation_budget = -1;
    constexpr u64 user = 0x20000000;
    const PhysAddr backing = mm::alloc_page();
    assert(!mm::vm_map_owned(first, user, backing, RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC));
    assert(mm::vm_map_owned(first, user, backing, RIGHT_READ | RIGHT_WRITE));
    assert(mm::user_va_to_pa(first, user + 17, RIGHT_WRITE, &translated));
    assert(translated == backing + 17);
    mm::vm_unmap(first, user);
    assert(owned_frees == 1 && !mm::user_va_to_pa(first, user, RIGHT_READ, &translated));

    process.pid = 0;
    mm::vm_space_destroy(first);
    mm::vm_space_destroy(second);
    VmSpace kernel{mm::g_kernel_root, 0};
    mm::vm_space_destroy(kernel);
    assert(pages.empty() && tlbi_count);
}
