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
    ${CMAKE_SOURCE_DIR}/boards/${ANKO_BOARD}/board_fallback.cc
    log/log.cc
    lzma.cc
    panic.cc
    panic_report.cc
    panic_screen.cc
    qr.cc
    smp.cc
    platform/cpu_topology.cc
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

    # ---- Phase L: dynamic shared arenas (spec §12.3) ----------------------
    vm/vm_shared.cc

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
if(ANKO_BOARD STREQUAL "huawei-anne" OR ANKO_ANNE_DIAG_QEMU)
    set(ANNE_MINIMAL_DIR ${CMAKE_SOURCE_DIR}/boards/huawei-anne/minimal)
    add_executable(anko-minimal EXCLUDE_FROM_ALL
        ${ANNE_MINIMAL_DIR}/entry.S ${ANNE_MINIMAL_DIR}/main.cc
        ${CMAKE_SOURCE_DIR}/boards/huawei-anne/native_log.cc init/fdt.cc)
    target_include_directories(anko-minimal PRIVATE include ${CMAKE_SOURCE_DIR}/lib/include
        ${CMAKE_SOURCE_DIR}/boards/huawei-anne)
    target_link_libraries(anko-minimal PRIVATE libk)
    target_compile_options(anko-minimal PRIVATE -mstrict-align -O2)
    target_link_options(anko-minimal PRIVATE -T ${ANNE_MINIMAL_DIR}/minimal.ld -Wl,--build-id=none)
    set_property(TARGET anko-minimal APPEND PROPERTY LINK_DEPENDS ${ANNE_MINIMAL_DIR}/minimal.ld)
    add_custom_command(TARGET anko-minimal POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:anko-minimal>
            $<TARGET_FILE_DIR:anko-minimal>/anko-minimal.bin)
    set(ANNE_PROBE_DIR ${CMAKE_SOURCE_DIR}/boards/huawei-anne/entry-probe)
    add_executable(anne-entry-probe EXCLUDE_FROM_ALL
        ${ANNE_PROBE_DIR}/entry.S ${ANNE_PROBE_DIR}/probe.cc)
    target_include_directories(anne-entry-probe PRIVATE ${CMAKE_SOURCE_DIR}/lib/include)
    target_compile_options(anne-entry-probe PRIVATE -mstrict-align -O2)
    target_link_options(anne-entry-probe PRIVATE -T ${ANNE_PROBE_DIR}/probe.ld -Wl,--build-id=none)
    set_property(TARGET anne-entry-probe APPEND PROPERTY LINK_DEPENDS ${ANNE_PROBE_DIR}/probe.ld)
    add_custom_command(TARGET anne-entry-probe POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:anne-entry-probe>
                $<TARGET_FILE_DIR:anne-entry-probe>/anne-entry-probe.bin)
    configure_file(${CMAKE_SOURCE_DIR}/boards/huawei-anne/native_bundle.S.in
        ${CMAKE_BINARY_DIR}/generated/anne_bundle.S @ONLY)
    set_source_files_properties(${CMAKE_BINARY_DIR}/generated/anne_bundle.S
        PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/system.mxb)
    target_sources(anko PRIVATE ${CMAKE_BINARY_DIR}/generated/anne_bundle.S
        ${CMAKE_SOURCE_DIR}/boards/huawei-anne/native_log.cc
        ${CMAKE_SOURCE_DIR}/boards/huawei-anne/boot_diagnostics.cc
        ${CMAKE_SOURCE_DIR}/boards/huawei-anne/boot_diagnostics.S)
    target_include_directories(anko PRIVATE ${CMAKE_SOURCE_DIR}/boards/huawei-anne)
    if(ANKO_ANNE_WATCHDOG)
        target_sources(anko PRIVATE ${CMAKE_SOURCE_DIR}/boards/huawei-anne/watchdog.cc)
        target_compile_definitions(anko PRIVATE ANKO_ANNE_WATCHDOG=1)
    endif()
    if(ANKO_ANNE_DISPLAY_CAPTURE OR ANKO_ANNE_FRAMEBUFFER)
        target_sources(anko PRIVATE ${CMAKE_SOURCE_DIR}/boards/huawei-anne/display_diagnostics.cc)
    endif()
    if(ANKO_ANNE_DISPLAY_CAPTURE)
        target_compile_definitions(anko PRIVATE ANKO_ANNE_DISPLAY_CAPTURE=1)
    endif()
    if(ANKO_ANNE_FRAMEBUFFER)
        target_sources(anko PRIVATE drivers/framebuffer_console.cc)
        target_compile_definitions(anko PRIVATE ANKO_ANNE_FRAMEBUFFER=1)
    endif()
    target_compile_definitions(anko PRIVATE ANKO_ANNE_DIAG_READY_HOLD_SECONDS=${ANKO_ANNE_DIAG_READY_HOLD_SECONDS})
    target_compile_definitions(anko PRIVATE ANKO_ANNE_DIAG_RESET_AT_READY=$<BOOL:${ANKO_ANNE_DIAG_RESET_AT_READY}>)
    target_compile_definitions(anko PRIVATE ANKO_ANNE_DIAG_WATCHDOG_RETENTION=$<BOOL:${ANKO_ANNE_DIAG_WATCHDOG_RETENTION}>)
    target_compile_definitions(anko PRIVATE ANKO_ANNE_DIAG_RUN_TAG="${ANKO_ANNE_DIAG_RUN_TAG}")
    if(ANKO_ANNE_DIAG_QEMU_FAULT_STAGE)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/boards/huawei-anne/boot_diagnostics.cc
            PROPERTIES COMPILE_DEFINITIONS "ANKO_ANNE_DIAG_TEST_FAULT_STAGE=${ANKO_ANNE_DIAG_QEMU_FAULT_STAGE}")
    endif()
    set_property(SOURCE ${CMAKE_SOURCE_DIR}/boards/huawei-anne/boot_diagnostics.cc APPEND
        PROPERTY COMPILE_DEFINITIONS "ANKO_ANNE_DIAG_CAPTURE_SECONDS=${ANKO_ANNE_DIAG_CAPTURE_SECONDS}")
    add_dependencies(anko mochi_system)
endif()

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
# The kernel is runtime-side and may use the memory floor (libk/string.h).
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

# Opt-in diagnostic at the ATF -> BL33 boundary; uses the same validated
# framebuffer reader as the kernel, without loading U-Boot or Ame.
if(ANKO_BOARD STREQUAL "switch-t210")
    set(HANDOFF_DIR ${CMAKE_SOURCE_DIR}/boards/switch-t210/handoff)
    add_executable(switch-handoff EXCLUDE_FROM_ALL
        ${HANDOFF_DIR}/entry.S ${HANDOFF_DIR}/backlight.S ${HANDOFF_DIR}/probe.cc
        ${CMAKE_SOURCE_DIR}/boards/switch-t210/board_fallback.cc)
    target_include_directories(switch-handoff PRIVATE include
        ${CMAKE_SOURCE_DIR}/shared/include ${CMAKE_SOURCE_DIR}/lib/include)
    target_compile_options(switch-handoff PRIVATE -mstrict-align)
    target_link_options(switch-handoff PRIVATE -T ${HANDOFF_DIR}/probe.ld -Wl,--build-id=none
        -Wl,--defsym,PROBE_LOAD_BASE=0xaa000000)
    set_property(TARGET switch-handoff APPEND PROPERTY LINK_DEPENDS ${HANDOFF_DIR}/probe.ld)
    add_custom_command(TARGET switch-handoff POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:switch-handoff>
                $<TARGET_FILE_DIR:switch-handoff>/switch-handoff.bin)

    file(READ ${CMAKE_SOURCE_DIR}/boards/switch-t210/firmware.json FIRMWARE_PROFILE)
    string(JSON ATF_BASE GET "${FIRMWARE_PROFILE}" tzdram_base)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/boards/switch-t210/firmware.json)
    add_executable(switch-cpu-probe EXCLUDE_FROM_ALL
        ${HANDOFF_DIR}/entry.S ${HANDOFF_DIR}/backlight.S)
    target_include_directories(switch-cpu-probe PRIVATE ${CMAKE_SOURCE_DIR}/shared/include)
    target_compile_definitions(switch-cpu-probe PRIVATE ANKO_CPU_ENTRY_PROBE=1)
    target_link_options(switch-cpu-probe PRIVATE -T ${HANDOFF_DIR}/probe.ld -Wl,--build-id=none
        -Wl,--defsym,PROBE_LOAD_BASE=${ATF_BASE})
    set_property(TARGET switch-cpu-probe APPEND PROPERTY LINK_DEPENDS ${HANDOFF_DIR}/probe.ld)
    add_custom_command(TARGET switch-cpu-probe POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:switch-cpu-probe>
                $<TARGET_FILE_DIR:switch-cpu-probe>/switch-cpu-probe.bin)
endif()
