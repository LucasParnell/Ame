# Mochi userland — each service is a separate freestanding ELF64.
# All services use the mochi userland library (syscall wrappers, ring library).
# They are linked with a minimal linker script (no libc, no crt0 — _start is in each main.cc).

# Common include paths for all Mochi services.
set(MOCHI_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/include   # mochi/syscall.h, mochi/ring.h
    ${CMAKE_SOURCE_DIR}/shared/include        # midix/abi/*.h
    ${CMAKE_SOURCE_DIR}/lib/include           # freestanding cstdint/cstddef/cstdarg
)

function(mochi_service name src load_base)
    set(USER_LOAD_BASE ${load_base})
    set(LINKER_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/${name}.ld)
    configure_file(
        ${CMAKE_SOURCE_DIR}/scripts/linker/user.ld.in
        ${LINKER_SCRIPT}
        @ONLY
    )

    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${MOCHI_INCLUDES})
    target_link_libraries(${name} PRIVATE freestanding_memory ame_format)
    target_link_options(${name} PRIVATE
        -nostdlib -static -fno-pie
        -T ${LINKER_SCRIPT}
    )

    set(MXI_FILE ${CMAKE_CURRENT_BINARY_DIR}/${name}.mxi)
    add_custom_command(
        OUTPUT ${MXI_FILE}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/mx/mxld.py
                $<TARGET_FILE:${name}>
                ${MXI_FILE}
                --kind exec
        DEPENDS ${name}
                ${CMAKE_SOURCE_DIR}/scripts/mx/mxld.py
        COMMENT "Converting ${name} to MXI1"
        VERBATIM
    )
    set(MOCHI_MXI_FILES ${MOCHI_MXI_FILES} ${MXI_FILE} PARENT_SCOPE)
    set(MOCHI_BUNDLE_OBJECT_ARGS
        ${MOCHI_BUNDLE_OBJECT_ARGS}
        --object exec:${name}:${MXI_FILE}
        PARENT_SCOPE
    )
endfunction()

mochi_service(init          init/main.cc       0x10010000)
mochi_service(nameservice   nameservice/main.cc 0x10100000)
mochi_service(io.console    console/main.cc    0x10200000)
mochi_service(demo.pong     demo/pong/main.cc  0x10300000)
mochi_service(demo.ping     demo/ping/main.cc  0x10400000)
mochi_service(dango.manager dango/manager/main.cc 0x10500000)
mochi_service(dango.auth_probe dango/auth_probe/main.cc 0x10600000)
mochi_service(io.framebuffer  drivers/framebuffer/main.cc 0x10700000)
if(DEFINED ANKO_FB_WIDTH AND DEFINED ANKO_FB_HEIGHT)
    target_compile_definitions(io.framebuffer PRIVATE
        ANKO_FB_WIDTH=${ANKO_FB_WIDTH}
        ANKO_FB_HEIGHT=${ANKO_FB_HEIGHT})
endif()
mochi_service(tty.fb0         drivers/tty/main.cc         0x10800000)
mochi_service(drv.pci         drivers/pci/main.cc         0x10900000)
mochi_service(drv.usb.xhci    drivers/usb/xhci/main.cc   0x10A00000)
if(DEFINED ANKO_XHCI_NORESET AND ANKO_XHCI_NORESET)
    target_compile_definitions(drv.usb.xhci PRIVATE ANKO_XHCI_NORESET=1)
endif()
mochi_service(drv.usb.hid     drivers/usb/hid/main.cc    0x10B00000)
mochi_service(ui.input        drivers/input/main.cc      0x10C00000)
mochi_service(drv.blk.virtio  drivers/blk/virtio/main.cc 0x10D00000)
# io.fs is split into cohesive modules behind fs/fs_internal.h.
set(FS_SOURCES
    fs/fs_ring.cc
    fs/fs_core.cc
    fs/fs_handles.cc
    fs/fs_snapshot.cc
    fs/fs_disk.cc)
mochi_service(io.fs        "${FS_SOURCES}"               0x10E00000)
mochi_service(msh             msh/main.cc                 0x10F00000)

set(MOCHI_BOOT_MANIFEST "${ANKO_BOOT_MANIFEST}")
if(NOT MOCHI_BOOT_MANIFEST)
    set(MOCHI_BOOT_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/manifest.v0")
endif()

set(DANGO_LINUX_ARTIFACT_DIR "" CACHE PATH "Optional prebuilt Dango Linux artifact directory")
option(DANGO_BUILD_LINUX_ARTIFACTS "Build the minimal Dango Linux Image/initramfs/DTB and package it into initrd.tar" OFF)
set(DANGO_LINUX_ARCHIVE_COMMANDS)
set(DANGO_LINUX_ARCHIVE_FILES)
set(DANGO_LINUX_ARCHIVE_ENTRIES)
set(DANGO_LINUX_TARGET_DEPS)
set(DANGO_LINUX_BUNDLE_OBJECT_ARGS)

if(DANGO_BUILD_LINUX_ARTIFACTS)
    set(DANGO_LINUX_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/dango-linux" CACHE PATH "Dango Linux artifact directory" FORCE)
    set(DANGO_LINUX_ARCHIVE_FILES
        "${DANGO_LINUX_ARTIFACT_DIR}/Image"
        "${DANGO_LINUX_ARTIFACT_DIR}/initramfs.cpio"
        "${DANGO_LINUX_ARTIFACT_DIR}/dango-linux.dtb"
        "${DANGO_LINUX_ARTIFACT_DIR}/manifest.toml"
    )
    add_custom_command(
        OUTPUT ${DANGO_LINUX_ARCHIVE_FILES}
        COMMAND ${CMAKE_COMMAND} -E env
                DANGO_LINUX_OUT=${DANGO_LINUX_ARTIFACT_DIR}
                ${CMAKE_SOURCE_DIR}/scripts/linux/build-min-linux.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-min-linux.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-agent.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-initramfs.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-dtb.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/check-artifacts.sh
            ${CMAKE_SOURCE_DIR}/linux-artifacts/agent/dango-agent.c
            ${CMAKE_SOURCE_DIR}/linux-artifacts/config/dango_hello_arm64.fragment
            ${CMAKE_SOURCE_DIR}/linux-artifacts/dts/dango-linux-minimal.dts.in
        COMMENT "Building minimal Dango Linux artifacts"
        VERBATIM
    )
    add_custom_target(dango_linux_artifacts DEPENDS ${DANGO_LINUX_ARCHIVE_FILES})
    list(APPEND DANGO_LINUX_TARGET_DEPS dango_linux_artifacts)
endif()

if(DANGO_LINUX_ARTIFACT_DIR)
    if(NOT DANGO_BUILD_LINUX_ARTIFACTS)
        foreach(name Image initramfs.cpio dango-linux.dtb manifest.toml)
            if(NOT EXISTS "${DANGO_LINUX_ARTIFACT_DIR}/${name}")
                message(FATAL_ERROR "linux artifact directory is missing ${name}; run scripts/linux/build-min-linux.sh or configure with -DDANGO_BUILD_LINUX_ARTIFACTS=ON")
            endif()
        endforeach()
    endif()
    foreach(name Image initramfs.cpio dango-linux.dtb manifest.toml)
        list(APPEND DANGO_LINUX_ARCHIVE_ENTRIES "linux/${name}")
    endforeach()
    list(APPEND DANGO_LINUX_BUNDLE_OBJECT_ARGS
        --object resource:linux/Image:${CMAKE_CURRENT_BINARY_DIR}/linux/Image
        --object resource:linux/initramfs.cpio:${CMAKE_CURRENT_BINARY_DIR}/linux/initramfs.cpio
        --object resource:linux/dango-linux.dtb:${CMAKE_CURRENT_BINARY_DIR}/linux/dango-linux.dtb
        --object resource:linux/manifest.toml:${CMAKE_CURRENT_BINARY_DIR}/linux/manifest.toml
    )
    list(APPEND DANGO_LINUX_ARCHIVE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/linux
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DANGO_LINUX_ARTIFACT_DIR}/Image
                ${CMAKE_CURRENT_BINARY_DIR}/linux/Image
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DANGO_LINUX_ARTIFACT_DIR}/initramfs.cpio
                ${CMAKE_CURRENT_BINARY_DIR}/linux/initramfs.cpio
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DANGO_LINUX_ARTIFACT_DIR}/dango-linux.dtb
                ${CMAKE_CURRENT_BINARY_DIR}/linux/dango-linux.dtb
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DANGO_LINUX_ARTIFACT_DIR}/manifest.toml
                ${CMAKE_CURRENT_BINARY_DIR}/linux/manifest.toml
    )
endif()

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/system.mxb
           ${CMAKE_BINARY_DIR}/manifest.json
    ${DANGO_LINUX_ARCHIVE_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E env
            AME_SIGNING_KEY=${AME_SIGNING_KEY}
            AME_BUILD_DEV=$<BOOL:${ANKO_BUILD_DEV}>
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mx/mksystem.py
            --build-dir ${CMAKE_BINARY_DIR}
            --legacy-manifest ${MOCHI_BOOT_MANIFEST}
            --manifest-output ${CMAKE_BINARY_DIR}/manifest.json
            --output ${CMAKE_BINARY_DIR}/system.mxb
            ${DANGO_LINUX_BUNDLE_OBJECT_ARGS}
    DEPENDS ${MOCHI_MXI_FILES}
            ${MOCHI_BOOT_MANIFEST}
            ${CMAKE_SOURCE_DIR}/scripts/mx/mksystem.py
            ${CMAKE_SOURCE_DIR}/scripts/mx/mxpack.py
            ${CMAKE_SOURCE_DIR}/scripts/mx/signing.py
            ${AME_SIGNING_KEY}
            ${DANGO_LINUX_ARCHIVE_FILES}
    COMMENT "Packing Ame signed system bundle (MXB1 + manifest.json)"
    VERBATIM
)

add_custom_target(mochi_system ALL
    DEPENDS
        ${CMAKE_BINARY_DIR}/system.mxb
        ${CMAKE_BINARY_DIR}/manifest.json)

# Regression processes are built explicitly and never included in system.mxb.
if(ANKO_BUILD_DEV)
    mochi_service(test.lifecycle ${CMAKE_SOURCE_DIR}/tests/kernel/lifecycle.cc 0x11000000)
    mochi_service(test.busy ${CMAKE_SOURCE_DIR}/tests/kernel/busy.cc 0x11100000)
    mochi_service(test.share ${CMAKE_SOURCE_DIR}/tests/kernel/share.cc 0x11200000)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/lifecycle.mxb
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test/package-lifecycle.py ${CMAKE_BINARY_DIR}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test.lifecycle.mxi
                ${CMAKE_CURRENT_BINARY_DIR}/test.busy.mxi
                ${CMAKE_CURRENT_BINARY_DIR}/test.share.mxi
                ${CMAKE_SOURCE_DIR}/scripts/test/package-lifecycle.py
        VERBATIM)
    # Supervision restart fixture — shares userland/init/supervision.h policy.
    mochi_service(test.restart ${CMAKE_SOURCE_DIR}/tests/kernel/restart.cc 0x11300000)
    target_include_directories(test.restart PRIVATE ${CMAKE_SOURCE_DIR}/userland/init)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/restart.mxb
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test/package-restart.py ${CMAKE_BINARY_DIR}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test.restart.mxi
                ${CMAKE_CURRENT_BINARY_DIR}/test.busy.mxi
                ${CMAKE_SOURCE_DIR}/scripts/test/package-restart.py
        VERBATIM)
    add_custom_target(kernel_regressions DEPENDS ${CMAKE_BINARY_DIR}/lifecycle.mxb
                                              ${CMAKE_BINARY_DIR}/restart.mxb)
endif()
