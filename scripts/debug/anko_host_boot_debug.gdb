set pagination off
set confirm off
set breakpoint pending on
set architecture aarch64
set print pretty on
set print asm-demangle on
set disassemble-next-line on

file build/kernel/anko

# Native userland symbols. These are optional conveniences for later userspace faults.
add-symbol-file build/userland/init             0x10000000
add-symbol-file build/userland/nameservice      0x10100000
add-symbol-file build/userland/io.console       0x10200000
add-symbol-file build/userland/demo.pong        0x10300000
add-symbol-file build/userland/demo.ping        0x10400000
add-symbol-file build/userland/dango.manager    0x10500000
add-symbol-file build/userland/dango.auth_probe 0x10600000

# Connect before target-dependent commands.
target remote :1234
set scheduler-locking off

# -----------------------------------------------------------------------------
# Helpers: only use core registers already shown valid by your target.
# Do not reference CurrentEL/SCTLR_EL1/etc. QEMU's AArch64 gdbstub register set
# varies by GDB/QEMU version, and an unavailable sysreg aborts the script.
# -----------------------------------------------------------------------------

define core_regs
  printf "\n--- core regs ---\n"
  info registers pc sp cpsr x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 x10 x11 x12 x19 x20 x29 x30
  printf "symbol pc: "
  info symbol $pc
  x/8i $pc
end

define threads_regs
  info threads
  thread apply all info registers pc sp cpsr x0 x1 x2 x3 x19 x20 x29 x30
  thread apply all p/a $pc
end

define where_all
  info threads
  thread apply all bt
  thread apply all p/a $pc
end

define boot_step
  # In your last dump CPU#1 was the one sitting at 0x40080000 <_start>.
  # If info threads shows a different boot CPU, switch to that thread manually.
  thread 2
  stepi
  core_regs
end

define boot_go
  continue
end

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

  if $x2 != 0
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
  end

  printf "\nKernel backtrace:\n"
  bt
end

# -----------------------------------------------------------------------------
# Boot trace breakpoints.
# Avoid `break _start`: after add-symbol-file, _start is ambiguous across kernel
# and userland. Your kernel entry in the last session was exactly 0x40080000.
# A breakpoint at the current PC will not trigger until PC changes, so use +4.
# -----------------------------------------------------------------------------

break *0x40080004
commands
  silent
  printf "\n--- Anko kernel executed past _start first instruction (0x40080004) ---\n"
  core_regs
  printf "\nStopped here intentionally. Type `c` to keep tracing, or `boot_step` to single-step CPU#1.\n"
end

break kernel_main
commands
  silent
  printf "\n--- kernel_main hit ---\n"
  core_regs
  continue
end

break drivers::uart::early_init
commands
  silent
  printf "\n--- drivers::uart::early_init arg0=%#lx ---\n", $x0
  continue
end

break anko::log::early_init
commands
  silent
  printf "\n--- anko::log::early_init hit ---\n"
  continue
end

break anko::log::write
commands
  silent
  printf "\n--- anko::log::write args: x0=%#lx x1=%#lx x2=%#lx x3=%#lx ---\n", $x0, $x1, $x2, $x3
  # Do not x/s these pointers in auto mode; bad pointers abort command files.
  continue
end

break sched::enter
commands
  silent
  printf "\n--- sched::enter hit ---\n"
  core_regs
  continue
end

break secondary_start
commands
  silent
  printf "\n--- secondary_start hit ---\n"
  core_regs
  continue
end

break secondary_kernel_main
commands
  silent
  printf "\n--- secondary_kernel_main arg0=%#lx ---\n", $x0
  continue
end

# Fault and panic breakpoints.
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
  printf "\nStopped. Useful commands: core_regs, threads_regs, where_all, dump_fault.\n"
end

break kernel_sync_fault
commands
  silent
  printf "\n--- EL1 kernel fault ---\n"
  dump_fault
  printf "\nStopped. Useful commands: core_regs, threads_regs, where_all, dump_fault.\n"
end

break panic
commands
  silent
  printf "\n--- panic hit ---\n"
  core_regs
  bt
  printf "\nStopped.\n"
end

break panicf
commands
  silent
  printf "\n--- panicf hit ---\n"
  core_regs
  bt
  printf "\nStopped.\n"
end

printf "\nLoaded Anko host boot debug script v5.\n"
printf "This version avoids QEMU/GDB system-register names entirely.\n"
printf "Commands: core_regs, threads_regs, where_all, boot_step, boot_go, dump_fault.\n"
printf "Type `c` to continue. First intended stop: 0x40080004.\n"
