define dump_fault
  set $tf = (unsigned long *)$x2

  printf "\n--- fault dump ---\n"
  printf "ESR = 0x%lx\n", $x0
  printf "FAR = 0x%lx\n", $x1
  printf "TF  = 0x%lx\n", $x2

  printf "saved ELR = 0x%lx\n", $tf[31]
  printf "saved SPSR = 0x%lx\n", $tf[32]
  printf "saved SP   = 0x%lx\n", $tf[33]

  printf "\nSymbol at saved ELR:\n"
  p/a $tf[31]

  printf "\nInstructions at saved ELR:\n"
  x/8i $tf[31]

  printf "\nSaved registers:\n"
  printf "x0  = 0x%lx\n", $tf[0]
  printf "x1  = 0x%lx\n", $tf[1]
  printf "x2  = 0x%lx\n", $tf[2]
  printf "x8  = 0x%lx\n", $tf[8]
  printf "x30 = 0x%lx\n", $tf[30]

  printf "\nKernel backtrace:\n"
  bt
end

document dump_fault
Dump ESR/FAR/TrapFrame from syscall_fault or kernel_sync_fault.
Call this while stopped in one of those handlers.
end
