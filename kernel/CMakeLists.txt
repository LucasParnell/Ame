# anko microkernel — sources are added phase-by-phase as V0 progresses.
# Spec ref: docs/v0-phases.md.

set(ANKO_SOURCES
    # ---- Phase 0: build and run --------------------------------------------
    arch/arm64/entry.S
    arch/arm64/el2.S
    arch/arm64/vectors.S
    arch/arm64/syscall.S
    drivers/uart/pl011.cc
    drivers/gic/gicv2.cc
    drivers/timer/generic_timer.cc
    log/log.cc
    smp.cc
    kernel_main.cc

    # ---- Phase 1/2: minimal kernel + userspace bring-up --------------------
    mm/phys.cc
    mm/vm_space.cc
    syscall/dispatch.cc
    init/elf_loader.cc
    init/boot_archive.cc
    init/boot_caps.cc

    # ---- Phase 3/4: scheduler + capabilities -------------------------------
    arch/arm64/context.S
    sched/thread.cc
    sched/runqueue.cc
    cap/cap_table.cc

    # ---- Phase 6/7: IPC ----------------------------------------------------
    ipc/doorbell.cc
    ipc/ring.cc

    # ---- Phase V3: Dango VM scaffolding -----------------------------------
    vm/vm.cc
    vm/vm_memory.cc
    vm/stage2.cc
    vm/vcpu.cc
    vm/vm_channel.cc
)

add_executable(anko ${ANKO_SOURCES})

set(ANKO_EMBED_DIR ${CMAKE_CURRENT_BINARY_DIR}/embedded)
file(MAKE_DIRECTORY ${ANKO_EMBED_DIR})

function(anko_embed_target target_name blob_name)
    set(BIN_FILE ${ANKO_EMBED_DIR}/${blob_name}.bin)
    set(OBJ_FILE ${ANKO_EMBED_DIR}/${blob_name}.bin.o)
    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target_name}> ${BIN_FILE}
        COMMAND ${CMAKE_OBJCOPY} -I binary -O elf64-littleaarch64 -B aarch64
                ${blob_name}.bin ${blob_name}.bin.o
        WORKING_DIRECTORY ${ANKO_EMBED_DIR}
        DEPENDS ${target_name}
        COMMENT "Embedding ${blob_name}"
    )
    set(ANKO_EMBED_OBJECTS ${ANKO_EMBED_OBJECTS} ${OBJ_FILE} PARENT_SCOPE)
endfunction()

function(anko_embed_file src_file blob_name)
    set(BIN_FILE ${ANKO_EMBED_DIR}/${blob_name}.bin)
    set(OBJ_FILE ${ANKO_EMBED_DIR}/${blob_name}.bin.o)
    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${CMAKE_COMMAND} -E copy ${src_file} ${BIN_FILE}
        COMMAND ${CMAKE_OBJCOPY} -I binary -O elf64-littleaarch64 -B aarch64
                ${blob_name}.bin ${blob_name}.bin.o
        WORKING_DIRECTORY ${ANKO_EMBED_DIR}
        DEPENDS ${src_file}
        COMMENT "Embedding ${blob_name}"
    )
    set(ANKO_EMBED_OBJECTS ${ANKO_EMBED_OBJECTS} ${OBJ_FILE} PARENT_SCOPE)
endfunction()

anko_embed_target(init init)
anko_embed_target(nameservice nameservice)
anko_embed_target(io.console io_console)
anko_embed_target(demo.ping demo_ping)
anko_embed_target(demo.pong demo_pong)
anko_embed_target(dango.manager dango_manager)
anko_embed_target(dango.auth_probe dango_auth_probe)
anko_embed_file(${CMAKE_SOURCE_DIR}/userland/manifest.v0 manifest_v0)
# Embed the complete Mochi/Dango initrd as a fallback for direct QEMU/ELF boots
# where the DTB initrd pointer is not available in x0. This keeps the external
# -initrd path working when DTB discovery succeeds, while making the embedded
# archive contain the same linux/* artifacts that dango.manager expects.
anko_embed_file(${CMAKE_BINARY_DIR}/initrd.tar initrd_tar)

target_sources(anko PRIVATE ${ANKO_EMBED_OBJECTS})

# initrd.tar is produced by the userland mochi_initrd target. anko embeds it as
# a fallback archive, so make the dependency explicit for Ninja/Make.
add_dependencies(anko mochi_initrd)

target_include_directories(anko PRIVATE
    include                            # anko/*.h
    ${CMAKE_SOURCE_DIR}/lib/include    # libk/*.h
    ${CMAKE_SOURCE_DIR}/boot/include   # boot/hw_info.h (used from Phase 1)
)

target_link_libraries(anko PRIVATE libk)

target_link_options(anko PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/linker/kernel.ld
    -Wl,--build-id=none
)

# Strip ELF -> raw binary suitable for QEMU `-kernel`.
add_custom_command(TARGET anko POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:anko>
            $<TARGET_FILE_DIR:anko>/anko.bin
    COMMENT "Producing anko.bin"
)
