# Mochi userland — each service is a separate freestanding ELF64.
# All services use the mochi userland library (syscall wrappers, ring library).
# They are linked with a minimal linker script (no libc, no crt0 — _start is in each main.cc).

# Common include paths for all Mochi services.
set(MOCHI_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/include   # mochi/syscall.h, mochi/ring.h
    ${CMAKE_SOURCE_DIR}/shared/include        # midix/abi/*.h
    ${CMAKE_SOURCE_DIR}/lib/include           # freestanding cstdint/cstddef/cstdarg
)

# Midix userland C++ runtime: the compiler-emitted hooks that would normally
# come from compiler-rt/libsupc++ (absent under -nostdlib). Currently the Itanium
# __cxa_guard_* for function-local statics; see lib/runtime/cxx_guard.cc.
add_library(mochi_runtime STATIC lib/runtime/cxx_guard.cc)
target_include_directories(mochi_runtime PRIVATE ${MOCHI_INCLUDES})

function(mochi_service name src load_base)
    set(USER_LOAD_BASE ${load_base})
    set(LINKER_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/${name}.ld)
    configure_file(
        ${CMAKE_SOURCE_DIR}/scripts/linker/user.ld.in
        ${LINKER_SCRIPT}
        @ONLY
    )

    add_executable(${name} ${src} ${ARGN})
    target_include_directories(${name} PRIVATE ${MOCHI_INCLUDES})
    # Services must not touch the raw memory floor (libk/string.h); the header
    # rejects AME_MOCHI_SERVICE builds that try.
    target_compile_definitions(${name} PRIVATE AME_MOCHI_SERVICE=1)
    target_link_libraries(${name} PRIVATE libk freestanding_memory ame_format mochi_runtime)
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

set(MOCHI_SYSTEM_DESCRIPTOR "${CMAKE_CURRENT_SOURCE_DIR}/system.json")
# A board keeps its own profile by default. An explicit override permits local
# bring-up of a new board's service policy on QEMU without claiming its hardware.
set(MOCHI_SYSTEM_PROFILE "" CACHE STRING "Mochi service profile override (empty uses ANKO_BOARD)")
set_property(CACHE MOCHI_SYSTEM_PROFILE PROPERTY STRINGS "" qemu-virt switch-t210 huawei-anne)
set(MOCHI_SELECTED_PROFILE "${ANKO_BOARD}")
if(MOCHI_SYSTEM_PROFILE)
    set(MOCHI_SELECTED_PROFILE "${MOCHI_SYSTEM_PROFILE}")
endif()
set(MOCHI_CHANNEL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/system_channels.h")
add_custom_command(
    OUTPUT ${MOCHI_CHANNEL_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mx/system_descriptor.py
            --descriptor ${MOCHI_SYSTEM_DESCRIPTOR}
            --channel-header ${MOCHI_CHANNEL_HEADER}
    DEPENDS ${MOCHI_SYSTEM_DESCRIPTOR} ${CMAKE_SOURCE_DIR}/scripts/mx/system_descriptor.py
    VERBATIM
)
mochi_service(init "init/main.cc;${MOCHI_CHANNEL_HEADER}" 0x10010000)
target_include_directories(init PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
mochi_service(nameservice   nameservice/main.cc 0x10100000)
mochi_service(io.console    console/main.cc    0x10200000)
mochi_service(demo.pong     demo/pong/main.cc  0x10300000)
mochi_service(demo.ping     demo/ping/main.cc  0x10400000)
mochi_service(dango.manager dango/manager/main.cc 0x10500000 drivers/gpu/linux/main.cc)
# drv.gpu.linux has one implementation (userland/drivers/gpu/linux/main.cc) linked
# into both of its halves: the client (io.framebuffer) and the server host
# (dango.manager, which owns the Dango channel). See gpu_linux.h for why there is
# deliberately no separate drv.gpu.linux process.
target_include_directories(dango.manager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/drivers/gpu/linux)
mochi_service(dango.auth_probe dango/auth_probe/main.cc 0x10600000)
mochi_service(io.framebuffer  drivers/framebuffer/main.cc 0x10700000 drivers/gpu/linux/main.cc)
target_include_directories(io.framebuffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/drivers/gpu/linux)
if(DEFINED ANKO_FB_WIDTH AND DEFINED ANKO_FB_HEIGHT)
    target_compile_definitions(io.framebuffer PRIVATE
        ANKO_FB_WIDTH=${ANKO_FB_WIDTH}
        ANKO_FB_HEIGHT=${ANKO_FB_HEIGHT})
endif()
# dango/vulkan.h — the native Vulkan session client and the `ame_remote` command
# marshalling. It is an *object* library: the client is not linked into a service
# yet (no service drives a remote Vulkan session in V0), but compiling it under
# the freestanding userland flags proves it carries no Linux/host dependency and
# no hot-path allocation. The host transport regression links these same sources.
add_library(dango_vulkan OBJECT
    lib/vulkan/session_client.cc
    lib/vulkan/mochi_transport.cc)
target_include_directories(dango_vulkan PRIVATE ${MOCHI_INCLUDES})

mochi_service(tty.fb0         drivers/tty/main.cc         0x10800000)
mochi_service(drv.pci         drivers/pci/main.cc         0x10900000)
mochi_service(drv.usb.xhci    drivers/usb/xhci/main.cc   0x10A00000)
if(DEFINED ANKO_XHCI_NORESET AND ANKO_XHCI_NORESET)
    target_compile_definitions(drv.usb.xhci PRIVATE ANKO_XHCI_NORESET=1)
endif()
mochi_service(io.usb          usb/main.cc                0x11800000)
mochi_service(drv.i2c.dw drivers/i2c/dw/main.cc 0x12000000)
mochi_service(drv.typec.fusb301 drivers/typec/fusb301/main.cc 0x12100000)
mochi_service(drv.power.hi6523 drivers/power/hi6523/main.cc 0x12200000)
mochi_service(drv.usb.anne-phy drivers/usb/anne-phy/main.cc 0x12300000)
mochi_service(drv.usb.dwc2 drivers/usb/dwc2/main.cc 0x12400000)
mochi_service(io.usb.role usb/role/main.cc 0x12500000)
mochi_service(io.usb.gadget usb/gadget/main.cc 0x12600000)
mochi_service(usb.fn.acm usb/functions/acm/main.cc 0x12700000)
mochi_service(usb.fn.bulk usb/functions/bulk/main.cc 0x12800000)
mochi_service(dev.usb-input debug/usb/main.cc 0x12900000)
mochi_service(dev.serial   usb/serial/main.cc   0x12A00000)
mochi_service(drv.usb.hid     drivers/usb/hid/main.cc    0x10B00000)
option(MOCHI_USB_TRACE_INPUT "Development-only USB input regression diagnostics" OFF)
if(MOCHI_USB_TRACE_INPUT)
    if(NOT ANKO_BUILD_DEV)
        message(FATAL_ERROR "MOCHI_USB_TRACE_INPUT is development-only")
    endif()
    target_compile_definitions(drv.usb.hid PRIVATE MOCHI_USB_TRACE_INPUT=1)
endif()
mochi_service(ui.input        drivers/input/main.cc      0x10C00000)
mochi_service(drv.blk.virtio  drivers/blk/virtio/main.cc 0x10D00000)
# io.fs is split into cohesive modules behind fs/fs_internal.h.
set(FS_SOURCES
    fs/fs_ring.cc
    fs/fs_core.cc
    fs/fs_handles.cc
    fs/fs_snapshot.cc
    fs/fs_disk.cc
    fs/fs_durable.cc)
mochi_service(io.fs        "${FS_SOURCES}"               0x10E00000)
# msh is split into cohesive modules behind userland/msh/msh.h.
set(MSH_SOURCES
    msh/main.cc
    msh/msh_io.cc
    msh/msh_line.cc
    msh/msh_parse.cc
    msh/msh_builtins.cc
    msh/msh_fs.cc
    msh/msh_svc.cc
)
mochi_service(msh "${MSH_SOURCES}" 0x10F00000)
mochi_service(app.hello   app/hello/main.cc       0x11400000)
mochi_service(test.anne_smp ${CMAKE_SOURCE_DIR}/tests/kernel/anne_smp.cc 0x11500000)

set(DANGO_LINUX_ARTIFACT_DIR "" CACHE PATH "Optional prebuilt Dango Linux artifact directory")
set(DANGO_LINUX_PROFILE "hello" CACHE STRING "Dango Linux profile: hello or gpu")
set_property(CACHE DANGO_LINUX_PROFILE PROPERTY STRINGS hello gpu)
# Which guest userspace the appliance boots. `minimal` is the freestanding
# appliance initramfs the tree has always built; `lavapipe` is the libc guest
# carrying a real Vulkan ICD (Mesa lavapipe), which is what the Phase N/O
# acceptance needs and why the guest memory layout has to grow.
set(DANGO_LINUX_GUEST "minimal" CACHE STRING "Dango Linux guest variant: minimal or lavapipe")
set_property(CACHE DANGO_LINUX_GUEST PROPERTY STRINGS minimal lavapipe)
if(NOT DANGO_LINUX_GUEST MATCHES "^(minimal|lavapipe)$")
    message(FATAL_ERROR "DANGO_LINUX_GUEST must be minimal or lavapipe")
endif()
# The guest variant and the guest memory layout must agree: the artifacts and the
# kernel bounds come from the same choice, and a mismatch is a guest that faults
# on a window the kernel put somewhere else (or a 512 MiB rootfs in 128 MiB).
if(NOT "${DANGO_LINUX_GUEST}" STREQUAL "${ANKO_GUEST_LAYOUT}")
    message(FATAL_ERROR
        "DANGO_LINUX_GUEST (${DANGO_LINUX_GUEST}) must match ANKO_GUEST_LAYOUT "
        "(${ANKO_GUEST_LAYOUT}); both select the same guest layout")
endif()
if(NOT DANGO_LINUX_PROFILE MATCHES "^(hello|gpu)$")
    message(FATAL_ERROR "DANGO_LINUX_PROFILE must be hello or gpu")
endif()
set(DANGO_LINUX_SOURCE_DIR "$ENV{HOME}/Code/linux" CACHE PATH "Linux source tree for guest builds")
option(DANGO_BUILD_LINUX_ARTIFACTS "Build and package Dango Linux artifacts" OFF)
set(DANGO_LINUX_ARCHIVE_COMMANDS)
set(DANGO_LINUX_ARCHIVE_FILES)
set(DANGO_LINUX_ARCHIVE_ENTRIES)
set(DANGO_LINUX_TARGET_DEPS)
set(DANGO_LINUX_BUNDLE_OBJECT_ARGS)

if(DANGO_BUILD_LINUX_ARTIFACTS AND NOT DANGO_LINUX_ARTIFACT_DIR)
    set(DANGO_LINUX_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/dango-linux-${DANGO_LINUX_PROFILE}")
endif()
if(DANGO_LINUX_ARTIFACT_DIR)
    foreach(name Image initramfs.cpio dango-linux.dtb manifest.toml)
        list(APPEND DANGO_LINUX_ARCHIVE_FILES "${DANGO_LINUX_ARTIFACT_DIR}/${name}")
        if(NOT EXISTS "${DANGO_LINUX_ARTIFACT_DIR}/${name}")
            list(APPEND _ame_missing_artifacts "${DANGO_LINUX_ARTIFACT_DIR}/${name}")
        endif()
    endforeach()
    # When the artifacts are prebuilt (not built here) every one must be
    # present; report the missing file(s) once, clearly, instead of failing deep
    # inside the content-hash stamp below.
    if(_ame_missing_artifacts AND NOT DANGO_BUILD_LINUX_ARTIFACTS)
        message(FATAL_ERROR "Missing prebuilt Linux artifact(s): ${_ame_missing_artifacts}")
    endif()
endif()

# ninja decides freshness by mtime, so an artifact directory refreshed with older
# timestamps (rsync -a, restored backup) would not re-trigger the staging and
# packing edge, and system.mxb would keep its previous payload. Stamp the
# artifact *contents* instead: the stamp file name carries a content hash, so it
# changes exactly when the artifacts change, and the bundle depends on it.
#
# The hash is computed at configure time, so a reconfigure is what invalidates
# the stamp; a plain `cmake --build` does not re-run this file and does not
# re-stamp. Only hash once every artifact is on disk -- a partially populated
# directory (e.g. mid-build) is left unstamped rather than erroring here.
set(DANGO_LINUX_STAMP_DEPS)
if(DANGO_LINUX_ARTIFACT_DIR)
    set(_ame_artifact_hash "")
    set(_ame_artifacts_present TRUE)
    foreach(name Image initramfs.cpio dango-linux.dtb manifest.toml)
        if(EXISTS "${DANGO_LINUX_ARTIFACT_DIR}/${name}")
            file(SHA256 "${DANGO_LINUX_ARTIFACT_DIR}/${name}" _ame_part)
            string(APPEND _ame_artifact_hash "${_ame_part}")
        else()
            set(_ame_artifacts_present FALSE)
        endif()
    endforeach()
    if(_ame_artifacts_present)
        string(SHA256 _ame_artifact_hash "${_ame_artifact_hash}")
        set(_ame_stamp "${CMAKE_BINARY_DIR}/dango-artifacts-${_ame_artifact_hash}.stamp")
        if(NOT EXISTS "${_ame_stamp}")
            file(WRITE "${_ame_stamp}" "${_ame_artifact_hash}\n")
        endif()
        file(GLOB _ame_old_stamps "${CMAKE_BINARY_DIR}/dango-artifacts-*.stamp")
        foreach(_ame_old ${_ame_old_stamps})
            if(NOT _ame_old STREQUAL _ame_stamp)
                file(REMOVE "${_ame_old}")
            endif()
        endforeach()
        set(DANGO_LINUX_STAMP_DEPS "${_ame_stamp}")
    endif()
endif()

if(DANGO_BUILD_LINUX_ARTIFACTS)
    set(DANGO_LINUX_BUILDER "${CMAKE_SOURCE_DIR}/scripts/linux/build-min-linux.sh")
    if(DANGO_LINUX_PROFILE STREQUAL "gpu")
        set(DANGO_LINUX_BUILDER "${CMAKE_SOURCE_DIR}/scripts/linux/build-gpu-linux.sh")
        if(CMAKE_HOST_APPLE)
            set(DANGO_LINUX_BUILDER "${CMAKE_SOURCE_DIR}/scripts/linux/build-gpu-linux-docker.sh")
        endif()
    endif()
    add_custom_command(
        OUTPUT ${DANGO_LINUX_ARCHIVE_FILES}
        COMMAND ${CMAKE_COMMAND} -E env
                DANGO_LINUX_OUT=${DANGO_LINUX_ARTIFACT_DIR}
                DANGO_LINUX_SRC=${DANGO_LINUX_SOURCE_DIR}
                ${DANGO_LINUX_BUILDER}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS
            ${DANGO_LINUX_BUILDER}
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-gpu-linux.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-agent.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/stage-gpu-source.py
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-manifest.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/check-profile.py
            ${CMAKE_SOURCE_DIR}/scripts/linux/check-minimal-config.py
            ${CMAKE_SOURCE_DIR}/shared/include/midix/abi/dango_channel_guest.h
            ${CMAKE_SOURCE_DIR}/linux-artifacts/guest-driver/dango_channel.c
            ${CMAKE_SOURCE_DIR}/linux-artifacts/channel_test/channel_test.c
            ${CMAKE_SOURCE_DIR}/linux-artifacts/kms_test/kms_test.c
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-initramfs.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/build-dtb.sh
            ${CMAKE_SOURCE_DIR}/scripts/linux/mkinitramfs.py
            ${CMAKE_SOURCE_DIR}/scripts/linux/check-artifacts.sh
            ${CMAKE_SOURCE_DIR}/linux-artifacts/agent/dango-agent.c
            ${CMAKE_SOURCE_DIR}/linux-artifacts/config/dango_hello_arm64.fragment
            ${CMAKE_SOURCE_DIR}/linux-artifacts/config/dango_gpu_arm64.fragment
            ${CMAKE_SOURCE_DIR}/linux-artifacts/dts/dango-linux-minimal.dts.in
            ${CMAKE_SOURCE_DIR}/linux-artifacts/dts/dango-linux-gpu.dts.in
        COMMENT "Building Dango Linux ${DANGO_LINUX_PROFILE} artifacts"
        VERBATIM
    )
    add_custom_target(dango_linux_artifacts DEPENDS ${DANGO_LINUX_ARCHIVE_FILES})
    list(APPEND DANGO_LINUX_TARGET_DEPS dango_linux_artifacts)
endif()

if(DANGO_LINUX_ARTIFACT_DIR)
    list(APPEND DANGO_LINUX_ARCHIVE_COMMANDS
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/linux/check-profile.py
                ${DANGO_LINUX_ARTIFACT_DIR} ${DANGO_LINUX_PROFILE})
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
           ${CMAKE_CURRENT_BINARY_DIR}/manifest.v0
    ${DANGO_LINUX_ARCHIVE_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E env
            AME_SIGNING_KEY=${AME_SIGNING_KEY}
            AME_BUILD_DEV=$<BOOL:${ANKO_BUILD_DEV}>
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mx/mksystem.py
            --build-dir ${CMAKE_BINARY_DIR}
            --descriptor ${MOCHI_SYSTEM_DESCRIPTOR}
            --profile ${MOCHI_SELECTED_PROFILE}
            --manifest-output ${CMAKE_BINARY_DIR}/manifest.json
            --output ${CMAKE_BINARY_DIR}/system.mxb
            ${DANGO_LINUX_BUNDLE_OBJECT_ARGS}
    DEPENDS ${MOCHI_MXI_FILES}
            ${MOCHI_SYSTEM_DESCRIPTOR}
            ${CMAKE_SOURCE_DIR}/scripts/mx/system_descriptor.py
            ${CMAKE_SOURCE_DIR}/scripts/mx/mksystem.py
            ${CMAKE_SOURCE_DIR}/scripts/mx/mxpack.py
            ${CMAKE_SOURCE_DIR}/scripts/mx/signing.py
            ${AME_SIGNING_KEY}
            ${DANGO_LINUX_ARCHIVE_FILES}
            ${DANGO_LINUX_STAMP_DEPS}
    COMMENT "Packing Ame signed system bundle (MXB1 + manifest.json)"
    VERBATIM
)

add_custom_target(mochi_system ALL
    DEPENDS
        ${CMAKE_BINARY_DIR}/system.mxb
        ${CMAKE_BINARY_DIR}/manifest.json
        ${CMAKE_CURRENT_BINARY_DIR}/manifest.v0)

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

    # Durability persistence fixture: a normal system bundle plus a probe service
    # that writes /data, syncs, and reads the file back after a reboot. Built
    # as its own bundle so the default system.mxb never runs it.
    mochi_service(app.durable ${CMAKE_SOURCE_DIR}/tests/durable/durable.cc 0x11500000)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/durable.mxb
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test/package-durable.py ${CMAKE_BINARY_DIR}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/app.durable.mxi
                ${CMAKE_BINARY_DIR}/manifest.json
                ${MOCHI_MXI_FILES}
                ${CMAKE_SOURCE_DIR}/scripts/test/package-durable.py
        VERBATIM)
    add_custom_target(durable_bundle DEPENDS ${CMAKE_BINARY_DIR}/durable.mxb)

    add_custom_target(kernel_regressions DEPENDS ${CMAKE_BINARY_DIR}/lifecycle.mxb
                                              ${CMAKE_BINARY_DIR}/restart.mxb
                                              ${CMAKE_BINARY_DIR}/durable.mxb)
endif()
