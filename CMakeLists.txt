cmake_minimum_required(VERSION 3.25)

# Midix system — V0
#   anko      = microkernel
#   mochi     = userland (init, nameservice, io.console, demo.ping, demo.pong)
project(Midix
    VERSION 0.0.1
    DESCRIPTION "Midix V0: Anko microkernel + Mochi userland, AArch64"
    LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Global flags — freestanding, no exceptions/RTTI
# ---------------------------------------------------------------------------
add_compile_options(
    -O3
    -ffreestanding
    -mgeneral-regs-only
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
    -fno-pie
    -Wall
    -Wextra
    -Werror
)

add_link_options(
    -nostdlib
    -static
)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(lib)       # libk — used by anko only
add_subdirectory(userland)  # Mochi userland services + initrd artifact
add_subdirectory(kernel)    # anko microkernel

# anko-boot is the U-Boot / real-hw shim. QEMU `-kernel` boots anko directly,
# so we only build the boot shim once it is needed (post Phase 1).
# add_subdirectory(boot)

# Mochi services are added before the kernel so anko can embed their blobs.
