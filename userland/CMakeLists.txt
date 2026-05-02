# Mochi userland — each service is a separate freestanding ELF64.
# All services use the mochi userland library (syscall wrappers, ring library).
# They are linked with a minimal linker script (no libc, no crt0 — _start is in each main.cc).

# Common include paths for all Mochi services.
set(MOCHI_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/include   # mochi/syscall.h, mochi/ring.h
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
    target_link_options(${name} PRIVATE
        -nostdlib -static -fno-pie
        -T ${LINKER_SCRIPT}
    )
endfunction()

mochi_service(init          init/main.cc       0x47000000)
mochi_service(nameservice   nameservice/main.cc 0x47100000)
mochi_service(io.console    console/main.cc    0x47200000)
mochi_service(demo.pong     demo/pong/main.cc  0x47300000)
mochi_service(demo.ping     demo/ping/main.cc  0x47400000)
mochi_service(dango.manager dango/manager/main.cc 0x47500000)
mochi_service(dango.auth_probe dango/auth_probe/main.cc 0x47600000)

set(DANGO_LINUX_ARTIFACT_DIR "" CACHE PATH "Optional prebuilt Dango Linux artifact directory")
option(DANGO_BUILD_LINUX_ARTIFACTS "Build the minimal Dango Linux Image/initramfs/DTB and package it into initrd.tar" OFF)
set(DANGO_LINUX_ARCHIVE_COMMANDS)
set(DANGO_LINUX_ARCHIVE_FILES)
set(DANGO_LINUX_ARCHIVE_ENTRIES)
set(DANGO_LINUX_TARGET_DEPS)

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
    OUTPUT ${CMAKE_BINARY_DIR}/initrd.tar
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest.v0
            ${CMAKE_CURRENT_BINARY_DIR}/manifest.v0
    ${DANGO_LINUX_ARCHIVE_COMMANDS}
    COMMAND tar -cf ${CMAKE_BINARY_DIR}/initrd.tar
            manifest.v0 init nameservice io.console demo.ping demo.pong
            dango.manager dango.auth_probe
            ${DANGO_LINUX_ARCHIVE_ENTRIES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS init nameservice io.console demo.ping demo.pong dango.manager dango.auth_probe
            ${DANGO_LINUX_ARCHIVE_FILES}
    COMMENT "Packing Mochi initrd (ustar)"
)

add_custom_target(mochi_initrd ALL DEPENDS ${CMAKE_BINARY_DIR}/initrd.tar)
