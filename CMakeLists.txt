cmake_minimum_required(VERSION 3.25)

# Midix system — V0
#   anko      = microkernel
#   mochi     = userland (init, nameservice, io.console, demo.ping, demo.pong)
project(Midix
    VERSION 0.0.1
    DESCRIPTION "Midix V0: Anko microkernel + Mochi userland, AArch64"
    LANGUAGES CXX ASM)

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
