set pagination off
set confirm off
set breakpoint pending on
set architecture aarch64
set print pretty on
set disassemble-next-line on

file build/kernel/anko

add-symbol-file build/userland/init             0x10000000
add-symbol-file build/userland/nameservice      0x10100000
add-symbol-file build/userland/io.console       0x10200000
add-symbol-file build/userland/demo.pong        0x10300000
add-symbol-file build/userland/demo.ping        0x10400000
add-symbol-file build/userland/dango.manager    0x10500000
add-symbol-file build/userland/dango.auth_probe 0x10600000

# -----------------------------------------------------------------------------
# Generic fault helpers
# -----------------------------------------------------------------------------
define dump_fault
  set $tf = (unsigned long *)$x2
  set $ec = ($x0 >> 26) & 0x3f
  set $iss = $x0 & 0x01ffffff
  set $fsc = $x0 & 0x3f

  printf "\n--- fault dump ---\n"
  printf "EC  = 0x%lx\n", $ec
  printf "ISS = 0x%lx\n", $iss
  printf "FSC = 0x%lx\n", $fsc
  printf "ESR = 0x%lx\n", $x0
  printf "FAR = 0x%lx\n", $x1
  printf "TF  = 0x%lx\n", $x2

  printf "saved ELR  = 0x%lx\n", $tf[31]
  printf "saved SPSR = 0x%lx\n", $tf[32]
  printf "saved SP   = 0x%lx\n", $tf[33]

  printf "\nSymbol at saved ELR:\n"
  p/a $tf[31]
  info symbol $tf[31]

  printf "\nSaved registers:\n"
  printf "x0  = 0x%lx\n", $tf[0]
  printf "x1  = 0x%lx\n", $tf[1]
  printf "x2  = 0x%lx\n", $tf[2]
  printf "x3  = 0x%lx\n", $tf[3]
  printf "x4  = 0x%lx\n", $tf[4]
  printf "x5  = 0x%lx\n", $tf[5]
  printf "x6  = 0x%lx\n", $tf[6]
  printf "x7  = 0x%lx\n", $tf[7]
  printf "x8  = 0x%lx\n", $tf[8]
  printf "x29 = 0x%lx\n", $tf[29]
  printf "x30 = 0x%lx\n", $tf[30]

  printf "\nKernel backtrace:\n"
  bt
end

# El2RunState layout from kernel/include/anko/vm.h.
# regs[0..30]       offset 0
# pc                offset 256
# pstate            offset 264
# vttbr             offset 272
# vtcr              offset 280
# exit_esr          offset 288
# exit_far          offset 296
# exit_hpfar        offset 304
# guest_irq_pending offset 480
# timer_hw_mask     offset 488
# exit_kind         offset 496
# guest_vmpidr      offset 504
# guest_vpidr       offset 512
# guest_sp_el0      offset 592

define dump_el2_state
  set $s = (unsigned long *)$arg0
  printf "\n--- El2RunState %p ---\n", $s
  printf "guest GPRs: x0=%#lx x1=%#lx x2=%#lx x3=%#lx x4=%#lx x5=%#lx x6=%#lx x7=%#lx\n", $s[0], $s[1], $s[2], $s[3], $s[4], $s[5], $s[6], $s[7]
  printf "guest LR/x30=%#lx\n", $s[30]
  set $guest_pc = *(unsigned long *)((char *)$s + 256)
  set $guest_pstate = *(unsigned long *)((char *)$s + 264)
  set $guest_sp_el1 = *(unsigned long *)((char *)$s + 424)
  set $guest_sp_el0 = *(unsigned long *)((char *)$s + 592)
  printf "guest pc=%#lx pstate=%#lx sp_el1=%#lx sp_el0=%#lx\n", $guest_pc, $guest_pstate, $guest_sp_el1, $guest_sp_el0
  set $vttbr = *(unsigned long *)((char *)$s + 272)
  set $vtcr = *(unsigned long *)((char *)$s + 280)
  set $guest_vmpidr = *(unsigned long *)((char *)$s + 504)
  set $guest_vpidr = *(unsigned long *)((char *)$s + 512)
  printf "vttbr=%#lx vtcr=%#lx vmpidr=%#lx vpidr=%#lx\n", $vttbr, $vtcr, $guest_vmpidr, $guest_vpidr
  set $exit_kind = *(unsigned long *)((char *)$s + 496)
  set $exit_esr = *(unsigned long *)((char *)$s + 288)
  set $exit_far = *(unsigned long *)((char *)$s + 296)
  set $exit_hpfar = *(unsigned long *)((char *)$s + 304)
  set $irq_pending = *(unsigned long *)((char *)$s + 480)
  set $timer_mask = *(unsigned long *)((char *)$s + 488)
  printf "exit_kind=%#lx exit_esr=%#lx exit_far=%#lx exit_hpfar=%#lx irq_pending=%#lx timer_mask=%#lx\n", $exit_kind, $exit_esr, $exit_far, $exit_hpfar, $irq_pending, $timer_mask
end
define vm_threads
  thread apply all info registers pc sp cpsr x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 x10 x11 x12 x29 x30
end

define vm_regs
  info registers pc sp cpsr x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 x10 x11 x12 x29 x30 esr_el2 elr_el2 far_el2 hpfar_el2 hcr_el2 vttbr_el2 vtcr_el2 sctlr_el1 ttbr0_el1 tcr_el1 vbar_el1
end

# -----------------------------------------------------------------------------
# Connect and install stop/trace points
# -----------------------------------------------------------------------------
target remote :1234

break syscall_fault
commands
  silent
  set $ec = ($x0 >> 26) & 0x3f

  if $ec == 0x3c
    set $tf = (unsigned long *)$x2
    printf "\n--- ignoring EL0 BRK/trap at "
    p/a $tf[31]
    continue
  end

  printf "\n--- EL0 real fault ---\n"
  dump_fault
  printf "\nStopped. Useful commands: vm_threads, vm_regs, bt\n"
end

break kernel_sync_fault
commands
  silent
  printf "\n--- EL1 kernel fault ---\n"
  dump_fault
  printf "\nStopped. Useful commands: vm_threads, vm_regs, bt\n"
end

break panic
commands
  silent
  printf "\n--- panic ---\n"
  bt
  printf "\nStopped. Useful commands: vm_threads, vm_regs\n"
end

# Count-limited tracing so the log is useful but does not flood forever.
set $run_vcpu_hits = 0
set $el2_entry_hits = 0
set $guest_start_hits = 0
set $guest_next_hits = 0
set $finish_hits = 0

# New ABI entry: vm::run_vcpu(Process&, CapHandle, VcpuRunBudget const&, VcpuRunResult*)
break vm::run_vcpu
commands
  silent
  set $run_vcpu_hits = $run_vcpu_hits + 1
  if $run_vcpu_hits <= 16
    printf "\n--- run_vcpu #%ld ---\n", $run_vcpu_hits
    printf "caller=%p vcpu_cap=%#lx budget=%p out=%p\n", $x0, $x1, $x2, $x3
    if $x2 != 0
      set $budget_ns = *(unsigned long *)$x2
      set $deadline_ns = *(unsigned long *)($x2 + 8)
      set $max_exits = *(unsigned int *)($x2 + 16)
      set $budget_flags = *(unsigned int *)($x2 + 20)
      printf "budget_ns=%lu deadline_ns=%lu max_exits=%u flags=%#x\n", $budget_ns, $deadline_ns, $max_exits, $budget_flags
    end
  end
  continue
end

break vm_el2_run
commands
  silent
  set $el2_entry_hits = $el2_entry_hits + 1
  if $el2_entry_hits <= 16
    printf "\n--- vm_el2_run entry #%ld ---\n", $el2_entry_hits
    dump_el2_state $x0
    if *(unsigned long *)$x0 == 0
      printf "WARNING: guest x0/DTB argument is zero before EL2 entry. Check VCPU_SET_ENTRY / boot arg path.\n"
    end
  end
  continue
end

# Guest first instructions. These tell us whether we execute past Linux _start.
hbreak *0x40080000
commands
  silent
  set $guest_start_hits = $guest_start_hits + 1
  printf "\n--- guest _start hit #%ld on thread ---\n", $guest_start_hits
  info registers pc sp cpsr x0 x1 x2 x3 x4 x5 x6 x7 x8 x29 x30
  if $guest_start_hits >= 8
    printf "Disabling _start breakpoint after 8 hits.\n"
    disable $_hit_bpnum
  end
  continue
end

hbreak *0x40080004
commands
  silent
  set $guest_next_hits = $guest_next_hits + 1
  printf "\n--- guest executed past first instruction #%ld ---\n", $guest_next_hits
  info registers pc sp cpsr x0 x1 x2 x3 x4 x5 x6 x7 x8 x29 x30
  if $guest_next_hits >= 8
    printf "Disabling _start+4 breakpoint after 8 hits.\n"
    disable $_hit_bpnum
  end
  continue
end

# vCPU exit completion. Args:
# x0=&Vcpu, x1=&Vm, w2=VcpuState, w3=VcpuExitReason, x4=last_esr, x5=used_ns, x6=out
break vm::finish_vcpu_run
commands
  silent
  set $finish_hits = $finish_hits + 1
  printf "\n--- finish_vcpu_run #%ld ---\n", $finish_hits
  printf "vcpu=%p vm=%p state=%u reason=%u last_esr=%#lx used_ns=%lu out=%p\n", $x0, $x1, $w2, $w3, $x4, $x5, $x6
  if $x0 != 0
    # Vcpu::el2 offset in current patched tree is not assumed here; print the public saved fields by conservative offsets only if useful manually.
    printf "Note: reason map 0=QUANTUM 1=WFI_BLOCKED 2=VIRQ 3=MMIO_BATCH 4=UNSUPPORTED 5=SHUTDOWN 6=HOST_PREEMPTED\n"
  end
  if $finish_hits >= 64
    printf "Disabling finish_vcpu_run breakpoint after 64 hits.\n"
    disable $_hit_bpnum
  end
  continue
end

printf "\nLoaded Anko debug script with vCPU tracing.\n"
printf "Useful manual commands: vm_threads, vm_regs, dump_el2_state <ptr>\n"
printf "Continuing.\n"

continue
