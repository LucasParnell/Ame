#!/usr/bin/env bash
set -euo pipefail

SCHED="kernel/sched/runqueue.cc"

cp "$SCHED" "$SCHED.bak.runqueue-underflow.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path
import re

p = Path("kernel/sched/runqueue.cc")
s = p.read_text()

# 1. Do not leave IRQs enabled after scheduler WFI.
old = '''static inline void send_event()          { asm volatile("sev" ::: "memory"); }
static inline void irq_enable()          { asm volatile("msr DAIFClr, #2" ::: "memory"); }
static inline void wait_for_interrupt()  { irq_enable(); asm volatile("wfi" ::: "memory"); }'''

new = '''static inline void send_event()          { asm volatile("sev" ::: "memory"); }
static inline void irq_enable()          { asm volatile("msr DAIFClr, #2" ::: "memory"); }
static inline void irq_disable()         { asm volatile("msr DAIFSet, #2" ::: "memory"); }

// Only enable IRQs for the actual WFI window.  Scheduler queue mutation must
// resume with IRQs masked, otherwise timer/IPI paths can reenter queue code.
static inline void wait_for_interrupt()  {
    irq_enable();
    asm volatile("wfi" ::: "memory");
    irq_disable();
}'''

if old in s:
    s = s.replace(old, new, 1)
elif "static inline void irq_disable()" not in s:
    raise SystemExit("could not patch irq_enable/wait_for_interrupt block")

# 2. Also force IRQs masked at post_syscall entry.  This makes the invariant
# explicit even if a future path reaches post_syscall with IRQs enabled.
needle = 'extern "C" void post_syscall(void* frame_ptr) {\n'
if needle in s and 'extern "C" void post_syscall(void* frame_ptr) {\n    irq_disable();' not in s:
    s = s.replace(needle, needle + "    irq_disable();\n", 1)

# 3. Keep the earlier address-space fix if it has not already been applied.
old_branch = '''    if (!requeue && cur->state == TS_RUNNING) {
        // No switch needed: thread continues
        cur->last_start_ns = sched_clock_ns();
        return;
    }
'''

new_branch = '''    if (!requeue && cur->state == TS_RUNNING) {
        // No switch needed: thread continues.  Still restore this thread's
        // address space before returning to EL0.
        cur->last_start_ns = sched_clock_ns();
        activate_thread_space(cur);
        return;
    }
'''

if old_branch in s:
    s = s.replace(old_branch, new_branch, 1)

# 4. Harden fpq_pop_min so a bad size cannot become q.data[0xffffffff].
pattern = re.compile(
    r"static PrincipalEntity\* fpq_pop_min\(FairPrincipalQueue& q\) \{.*?\n\}\n\n"
    r"static PrincipalEntity\* fpq_peek_min",
    re.S,
)

replacement = '''static void fpq_rebuild(FairPrincipalQueue& q) {
    u32 n = q.size;
    if (n > kMaxPrincipals) n = kMaxPrincipals;

    u32 w = 0;
    for (u32 r = 0; r < n; ++r) {
        if (q.data[r]) q.data[w++] = q.data[r];
    }
    for (u32 r = w; r < kMaxPrincipals; ++r) q.data[r] = nullptr;

    q.size = w;
    if (w == 0) return;

    for (u32 i = w / 2; i > 0; --i) {
        fpq_sift_down(q, i - 1);
    }
    for (u32 i = 0; i < w; ++i) {
        if (q.data[i]) q.data[i]->heap_idx = static_cast<u8>(i);
    }
}

static PrincipalEntity* fpq_pop_min(FairPrincipalQueue& q) {
    if (q.size == 0) return nullptr;

    // Corruption guard.  The observed crash was q.size underflowing and the
    // generated load indexing q.data[0xffffffff].
    if (q.size > kMaxPrincipals || q.data[0] == nullptr) {
        fpq_rebuild(q);
        if (q.size == 0) return nullptr;
    }

    const u32 old_size = q.size;
    PrincipalEntity* min = q.data[0];

    const u32 new_size = old_size - 1;
    q.size = new_size;

    if (new_size > 0) {
        PrincipalEntity* moved = q.data[new_size];
        q.data[new_size] = nullptr;
        q.data[0] = moved;

        if (moved) {
            moved->heap_idx = 0;
            fpq_sift_down(q, 0);
        } else {
            fpq_rebuild(q);
        }
    } else {
        q.data[0] = nullptr;
    }

    if (min) min->heap_idx = 0xff;
    return min;
}

static PrincipalEntity* fpq_peek_min'''

s2, n = pattern.subn(replacement, s, count=1)
if n != 1:
    raise SystemExit("could not replace fpq_pop_min block")

p.write_text(s2)
PY

cmake --build build

echo
echo "patched runqueue IRQ masking + fpq_pop_min guard"
echo "now run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"
