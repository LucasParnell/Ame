cmake_minimum_required(VERSION 3.25)

# Midix system — V0
#   anko      = microkernel
#   mochi     = userland (init, nameservice, io.console, demo.ping, demo.pong)
project(Midix
    VERSION 0.0.1
    DESCRIPTION "Midix V0: Anko microkernel + Mochi userland, AArch64"
    LANGUAGES CXX ASM)

# Prefer the project environment, including when launched outside an activated shell.
if(NOT DEFINED Python3_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.venv/bin/python3")
    set(Python3_EXECUTABLE "${CMAKE_SOURCE_DIR}/.venv/bin/python3")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR
        "Ame must be cross-compiled for AArch64. Reconfigure with: "
        "cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ANKO_BUILD_DEV "Allow the development bundle signing key" ON)
option(ANKO_PANIC_SCREEN_DEMO
    "Dev only: raise a panic right after the framebuffer is bound so the panic screen can be captured"
    OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Board selection — sets ANKO_LOAD_BASE and ANKO_CONSOLE_KIND.
# ---------------------------------------------------------------------------
set(ANKO_BOARD "qemu-virt" CACHE STRING "Target board (qemu-virt | switch-t210)")
set_property(CACHE ANKO_BOARD PROPERTY STRINGS qemu-virt switch-t210)
include(${CMAKE_SOURCE_DIR}/boards/${ANKO_BOARD}/board.cmake)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/board.json CONTENT
    "{\"board\":\"${ANKO_BOARD}\",\"load_base\":\"${ANKO_LOAD_BASE}\",\"text_offset\":\"${ANKO_IMAGE_TEXT_OFFSET}\"}\n")
if(ANKO_BOARD STREQUAL "switch-t210")
    set(ANKO_BOOT_MANIFEST ${CMAKE_SOURCE_DIR}/userland/manifest.switch.v0)
    add_compile_definitions(ANKO_BOARD_SWITCH_T210=1)
else()
    set(ANKO_BOOT_MANIFEST ${CMAKE_SOURCE_DIR}/userland/manifest.v0)
endif()

# ---------------------------------------------------------------------------
# Global flags — freestanding, no exceptions/RTTI
# ---------------------------------------------------------------------------
add_compile_options(
    -ffreestanding
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
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
set(AME_SIGNING_KEY "" CACHE FILEPATH "Ed25519 PKCS8 private signing key")
set(AME_TRUSTED_KEY_FILES "" CACHE STRING "Semicolon-separated Ed25519 public key files")
set(AME_TRUST_ARGS --output ${CMAKE_BINARY_DIR}/generated/manifest_trust.h)
if(ANKO_BUILD_DEV)
    list(APPEND AME_TRUST_ARGS --development)
endif()
if(AME_SIGNING_KEY)
    list(APPEND AME_TRUST_ARGS --signing-key ${AME_SIGNING_KEY})
endif()
list(APPEND AME_TRUST_ARGS --trusted ${AME_TRUSTED_KEY_FILES})
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mx/signing.py ${AME_TRUST_ARGS}
    RESULT_VARIABLE AME_TRUST_RESULT ERROR_VARIABLE AME_TRUST_ERROR)
if(NOT AME_TRUST_RESULT EQUAL 0)
    message(FATAL_ERROR "Signing configuration failed: ${AME_TRUST_ERROR}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${AME_TRUSTED_KEY_FILES} ${AME_SIGNING_KEY} ${CMAKE_SOURCE_DIR}/scripts/mx/signing.py)
add_subdirectory(lib)       # libk — used by anko only
add_subdirectory(userland)  # Mochi userland services + initrd artifact
add_subdirectory(kernel)    # anko microkernel

enable_testing()
add_test(NAME host-regressions COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/tests -p "test_*.py" -v)
set_tests_properties(host-regressions PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_custom_target(ame_vfs
    COMMAND ${CMAKE_COMMAND} -E env
            AME_BUILD_DIR=${CMAKE_BINARY_DIR}
            ${CMAKE_SOURCE_DIR}/scripts/install-ame-vfs.sh
    DEPENDS anko mochi_system
    COMMENT "Installing Ame into the QEMU virtual filesystem image"
    VERBATIM
)

add_custom_target(ame_disk
    COMMAND ${CMAKE_COMMAND} -E env
            AME_BUILD_DIR=${CMAKE_BINARY_DIR}
            ${CMAKE_SOURCE_DIR}/scripts/build-ame-disk.sh
    DEPENDS anko mochi_system
    COMMENT "Building the U-Boot/QEMU raw-boot + AmeFS disk image"
    VERBATIM
)

# anko-boot is the U-Boot / real-hw shim. QEMU `-kernel` boots anko directly,
# so we only build the boot shim once it is needed (post Phase 1).
# add_subdirectory(boot)

# Mochi services are added before the kernel so anko can embed their blobs.

# ---------------------------------------------------------------------------
# Build provenance — records the source revision, the build configuration, the
# signing identity, and the sha256 of the produced system.mxb.
#
# This is intentionally best-effort: scripts/provenance.py degrades to null
# fields when git (or a key) is unavailable and never fails the build. An
# ordinary `cmake --build` therefore always lands generated/provenance.json
# and generated/provenance.h next to the other generated headers.
# ---------------------------------------------------------------------------
set(AME_PROVENANCE_JSON ${CMAKE_BINARY_DIR}/generated/provenance.json)
set(AME_PROVENANCE_HEADER ${CMAKE_BINARY_DIR}/generated/provenance.h)
set(AME_TRUST_MODE development)
if(NOT ANKO_BUILD_DEV)
    set(AME_TRUST_MODE release)
endif()
# Refresh provenance when the checkout moves HEAD; harmless for non-git trees.
set(AME_PROVENANCE_GIT_DEPS)
if(EXISTS ${CMAKE_SOURCE_DIR}/.git/HEAD)
    list(APPEND AME_PROVENANCE_GIT_DEPS ${CMAKE_SOURCE_DIR}/.git/HEAD)
endif()
# Optional arguments are appended only when configured, so an empty value never
# collapses into a stray flag in the command line.
set(AME_PROVENANCE_OPT_ARGS)
if(AME_SIGNING_KEY)
    list(APPEND AME_PROVENANCE_OPT_ARGS --signing-key ${AME_SIGNING_KEY})
    list(APPEND AME_PROVENANCE_OPT_ARGS --trusted ${AME_TRUSTED_KEY_FILES})
endif()
add_custom_command(
    OUTPUT ${AME_PROVENANCE_JSON} ${AME_PROVENANCE_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/provenance.py
            --source-dir ${CMAKE_SOURCE_DIR}
            --build-dir ${CMAKE_BINARY_DIR}
            --output-json ${AME_PROVENANCE_JSON}
            --output-header ${AME_PROVENANCE_HEADER}
            --build-type ${CMAKE_BUILD_TYPE}
            --board ${ANKO_BOARD}
            --build-dev $<BOOL:${ANKO_BUILD_DEV}>
            --trust-mode ${AME_TRUST_MODE}
            --system-mxb ${CMAKE_BINARY_DIR}/system.mxb
            ${AME_PROVENANCE_OPT_ARGS}
    DEPENDS ${CMAKE_BINARY_DIR}/system.mxb
            ${CMAKE_SOURCE_DIR}/scripts/provenance.py
            ${AME_SIGNING_KEY}
            ${AME_PROVENANCE_GIT_DEPS}
    COMMENT "Recording build provenance (generated/provenance.json)"
    VERBATIM)
add_custom_target(provenance ALL
    DEPENDS ${AME_PROVENANCE_JSON} ${AME_PROVENANCE_HEADER})
