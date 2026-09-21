# anko microkernel — sources are added phase-by-phase as V0 progresses.
# Spec ref: docs/v0-phases.md.

set(ANKO_SOURCES
    # ---- Phase 0: build and run --------------------------------------------
    arch/arm64/entry.S
    arch/arm64/el2.S
    arch/arm64/vectors.S
    arch/arm64/syscall.S
    drivers/uart/pl011.cc
    drivers/uart/tegra_uart.cc
    drivers/console.cc
    drivers/gic/gicv2.cc
    drivers/gic/irq_table.cc
    drivers/timer/generic_timer.cc
    platform/hw_info.cc
    platform/probe.cc
    ${CMAKE_SOURCE_DIR}/boards/${ANKO_BOARD}/early_console.cc
    ${CMAKE_SOURCE_DIR}/boards/${ANKO_BOARD}/board_fallback.cc
    log/log.cc
    lzma.cc
    panic.cc
    panic_report.cc
    panic_screen.cc
    qr.cc
    smp.cc
    kernel_main.cc

    # ---- Phase 1/2: minimal kernel + userspace bring-up --------------------
    mm/phys.cc
    mm/vm_space.cc
    mm/arena.cc
    syscall/dispatch.cc
    init/image_loader.cc
    init/fdt.cc
    init/boot_disk.cc
    init/boot_archive.cc
    init/boot_caps.cc
    init/fat_log.cc
    drivers/sdmmc/sdmmc.cc

    # ---- Phase 3/4: scheduler + capabilities -------------------------------
    arch/arm64/context.S
    arch/arm64/psci.S
    sched/thread.cc
    sched/sched_class.cc
    sched/fair_queue.cc
    sched/sched_context.cc
    sched/accounting.cc
    sched/handoff.cc
    sched/steal.cc
    sched/metrics.cc
    sched/runqueue.cc
    cap/cap_table.cc
    cap/object.cc

    # ---- Phase 6/7: IPC ----------------------------------------------------
    ipc/doorbell.cc
    ipc/ring.cc

    # ---- Phase V3: Dango VM scaffolding -----------------------------------
    vm/vm.cc
    vm/vm_memory.cc
    vm/stage2.cc
    vm/vcpu.cc
    vm/vm_channel.cc
    vm/vm_channel_notify.cc

    # ---- Phase D: PCI device assignment + interrupt virtualization --------
    vm/vm_device.cc
    vm/vm_device_irq.cc
    vm/vm_device_selftest.cc

    # ---- Phase B: backend-independent IOMMU core --------------------------
    iommu/core.cc

    # ---- Phase C: ARM SMMUv3 backend, fault containment, native arenas ----
    iommu/smmuv3.cc
    iommu/faults.cc
    iommu/native.cc
    iommu/selftest.cc

    # ---- Framebuffer + PCI driver support (M2 / M3.5) --------------------
    drivers/fwcfg.cc
    drivers/pci_probe.cc

    # ---- Phase A: PCI / IOMMU inventory -----------------------------------
    pci/enumerate.cc

    # ---- Phase D: PCI assignment, mediation, MSI virtualization -----------
    pci/assignment.cc
    pci/ecam.cc
    pci/msi.cc
)

add_executable(anko ${ANKO_SOURCES})

# The panic-screen logo is a data asset, never hand-copied into a source file:
# scripts/mk-ascii-header.py turns kernel/assets/panic_art.txt into a header.
set(PANIC_ART_SRC ${CMAKE_SOURCE_DIR}/kernel/assets/panic_art.txt)
set(PANIC_ART_HDR ${CMAKE_BINARY_DIR}/generated/panic_art.h)
add_custom_command(OUTPUT ${PANIC_ART_HDR}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mk-ascii-header.py
            ${PANIC_ART_SRC} ${PANIC_ART_HDR} anko::art kPanicArt
    DEPENDS ${PANIC_ART_SRC} ${CMAKE_SOURCE_DIR}/scripts/mk-ascii-header.py
    COMMENT "Generating panic_art.h from kernel/assets/panic_art.txt"
    VERBATIM)
add_custom_target(panic_art_header DEPENDS ${PANIC_ART_HDR})
add_dependencies(anko panic_art_header)
target_sources(anko PRIVATE ${PANIC_ART_HDR})

# QR spec tables are a data asset too: scripts/gen-qr-tables.py turns
# kernel/assets/qr_tables.json into a constexpr header at build time.
set(QR_TABLES_JSON ${CMAKE_SOURCE_DIR}/kernel/assets/qr_tables.json)
set(QR_TABLES_HDR ${CMAKE_BINARY_DIR}/generated/qr_tables.h)
add_custom_command(OUTPUT ${QR_TABLES_HDR}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-qr-tables.py
            ${QR_TABLES_JSON} ${QR_TABLES_HDR}
    DEPENDS ${QR_TABLES_JSON} ${CMAKE_SOURCE_DIR}/scripts/gen-qr-tables.py
    COMMENT "Generating qr_tables.h from kernel/assets/qr_tables.json"
    VERBATIM)
add_custom_target(qr_tables_header DEPENDS ${QR_TABLES_HDR})
add_dependencies(anko qr_tables_header)
target_sources(anko PRIVATE ${QR_TABLES_HDR})

# Build identifier shown on the panic screen (git describe, or "unknown").
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} describe --tags --always --dirty
    OUTPUT_VARIABLE ANKO_BUILD_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT ANKO_BUILD_DESCRIBE)
    set(ANKO_BUILD_DESCRIBE "unknown")
endif()

target_include_directories(anko PRIVATE
    include                            # anko/*.h
    ${CMAKE_SOURCE_DIR}/shared/include  # midix/abi/*.h
    ${CMAKE_SOURCE_DIR}/lib/include    # libk/*.h
    ${CMAKE_SOURCE_DIR}/boot/include   # boot/hw_info.h (used from Phase 1)
    ${CMAKE_SOURCE_DIR}/boards/${ANKO_BOARD}  # board-local overrides (unused for now)
)

target_link_libraries(anko PRIVATE libk ame_format)
target_include_directories(anko PRIVATE ${CMAKE_BINARY_DIR}/generated)

# Boot reaches C++ before the MMU has established Normal memory attributes.
# At -O2, Clang may widen byte-buffer operations into unaligned word/dword
# accesses. Keep kernel C++ safe across the early no-MMU window.
target_compile_options(anko PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-mstrict-align>
)

target_compile_definitions(anko PRIVATE
    $<$<BOOL:${ANKO_BUILD_DEV}>:ANKO_BUILD_DEV=1>
    $<$<BOOL:${ANKO_PANIC_SCREEN_DEMO}>:ANKO_PANIC_SCREEN_DEMO=1>
    ANKO_PANIC_DEMO_VECTOR=${ANKO_PANIC_DEMO_VECTOR}
    ANKO_BUILD_DESCRIBE="${ANKO_BUILD_DESCRIBE}"
    ANKO_IMAGE_TEXT_OFFSET=${ANKO_IMAGE_TEXT_OFFSET}
)

target_link_options(anko PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/linker/kernel.ld
    -Wl,--build-id=none
    -Wl,--defsym,ANKO_LOAD_BASE=${ANKO_LOAD_BASE}
)

# Strip ELF -> raw binary suitable for QEMU `-kernel`.
add_custom_command(TARGET anko POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:anko>
            $<TARGET_FILE_DIR:anko>/anko.bin
    COMMENT "Producing anko.bin"
)
