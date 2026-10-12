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
option(ANKO_PANIC_DEMO_VECTOR
    "Dev only: with the panic demo, raise this exception-vector slot instead of a plain panic (0 = plain panic)"
    0)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Board selection — sets ANKO_LOAD_BASE and ANKO_CONSOLE_KIND.
# ---------------------------------------------------------------------------
set(ANKO_BOARD "qemu-virt" CACHE STRING "Target board (qemu-virt | switch-t210 | huawei-anne)")
set_property(CACHE ANKO_BOARD PROPERTY STRINGS qemu-virt switch-t210 huawei-anne)
option(ANKO_ANNE_DIAG_QEMU "Exercise Huawei boot diagnostics on synthetic QEMU peripherals" OFF)
set(ANKO_ANNE_DIAG_QEMU_FAULT_STAGE 0 CACHE STRING "Validation only: inject an exception at this boot checkpoint")
set(ANKO_ANNE_DIAG_CAPTURE_SECONDS 0 CACHE STRING "Native diagnostic run: retain trace by resetting at readiness or this timeout; zero disables")
option(ANKO_ANNE_DIAG_RESET_AT_READY "Capture: reset at services ready instead of observing until the runtime deadline" ON)
option(ANKO_ANNE_DISPLAY_CAPTURE "Capture Huawei DSS/DSI state and paint only a verified inherited framebuffer" OFF)
set(ANKO_ANNE_FRAMEBUFFER_DEFAULT OFF)
if(ANKO_BOARD STREQUAL "huawei-anne")
    set(ANKO_ANNE_FRAMEBUFFER_DEFAULT ON)
endif()
option(ANKO_ANNE_FRAMEBUFFER "Mirror native Huawei boot and io.console output to verified physical scanout" ${ANKO_ANNE_FRAMEBUFFER_DEFAULT})
option(ANKO_ANNE_WATCHDOG "Adopt and keep alive an active native Huawei SP805 watchdog" ${ANKO_ANNE_FRAMEBUFFER_DEFAULT})
option(ANKO_ANNE_DIAG_WATCHDOG_RETENTION "Capture only: deliberately stop keepalive after observation so firmware retains the trace" OFF)
set(ANKO_ANNE_DIAG_READY_HOLD_SECONDS 0 CACHE STRING "Capture only: extra console viewing time at service readiness")
set(ANKO_ANNE_DIAG_RUN_TAG "untagged" CACHE STRING "Identity for matching retained native traces to a capture package")
string(LENGTH "${ANKO_ANNE_DIAG_RUN_TAG}" ANKO_ANNE_DIAG_RUN_TAG_LENGTH)
if(NOT ANKO_ANNE_DIAG_RUN_TAG MATCHES "^[A-Za-z0-9_.-]+$" OR ANKO_ANNE_DIAG_RUN_TAG_LENGTH GREATER 48)
    message(FATAL_ERROR "ANKO_ANNE_DIAG_RUN_TAG must contain 1-48 letters, digits, dots, underscores or hyphens")
endif()
if(NOT ANKO_ANNE_DIAG_READY_HOLD_SECONDS MATCHES "^[0-9]+$" OR ANKO_ANNE_DIAG_READY_HOLD_SECONDS GREATER 15)
    message(FATAL_ERROR "ANKO_ANNE_DIAG_READY_HOLD_SECONDS must be an integer from 0 to 15")
endif()
if(NOT ANKO_ANNE_DIAG_CAPTURE_SECONDS MATCHES "^[0-9]+$" OR ANKO_ANNE_DIAG_CAPTURE_SECONDS GREATER 60)
    message(FATAL_ERROR "ANKO_ANNE_DIAG_CAPTURE_SECONDS must be an integer from 0 to 60")
endif()
if(ANKO_ANNE_DIAG_QEMU AND NOT ANKO_BOARD STREQUAL "qemu-virt")
    message(FATAL_ERROR "ANKO_ANNE_DIAG_QEMU is for qemu-virt validation only")
endif()
if(ANKO_ANNE_DISPLAY_CAPTURE AND NOT (ANKO_BOARD STREQUAL "huawei-anne" OR ANKO_ANNE_DIAG_QEMU))
    message(FATAL_ERROR "ANKO_ANNE_DISPLAY_CAPTURE requires the Huawei board or synthetic diagnostic target")
endif()
if(ANKO_ANNE_FRAMEBUFFER AND NOT (ANKO_BOARD STREQUAL "huawei-anne" OR ANKO_ANNE_DIAG_QEMU))
    message(FATAL_ERROR "ANKO_ANNE_FRAMEBUFFER requires the Huawei board or synthetic diagnostic target")
endif()
if(ANKO_ANNE_WATCHDOG AND NOT (ANKO_BOARD STREQUAL "huawei-anne" OR ANKO_ANNE_DIAG_QEMU))
    message(FATAL_ERROR "ANKO_ANNE_WATCHDOG requires the Huawei board or synthetic diagnostic target")
endif()
if(ANKO_ANNE_DIAG_WATCHDOG_RETENTION AND (NOT ANKO_ANNE_WATCHDOG OR ANKO_ANNE_DIAG_RESET_AT_READY OR ANKO_ANNE_DIAG_CAPTURE_SECONDS EQUAL 0))
    message(FATAL_ERROR "Watchdog retention requires keepalive, a positive capture deadline, and readiness reset disabled")
endif()
if(NOT ANKO_ANNE_DIAG_QEMU AND ANKO_ANNE_DIAG_QEMU_FAULT_STAGE)
    message(FATAL_ERROR "Fault injection is only supported in the QEMU diagnostic validation target")
endif()
# Guest memory layout (shared/include/midix/abi/dango_guest_layout.h): the RAM
# bounds and every fixed window derive from it, in the kernel, the guest DTS and
# the artifact manifest. `minimal` is the freestanding appliance guest the tree
# boots today; `lavapipe` is the 512 MiB libc guest, which moves all windows.
set(ANKO_GUEST_LAYOUT "minimal" CACHE STRING "Guest memory layout (minimal | lavapipe)")
set_property(CACHE ANKO_GUEST_LAYOUT PROPERTY STRINGS minimal lavapipe)
if(ANKO_GUEST_LAYOUT STREQUAL "lavapipe")
    add_compile_definitions(DANGO_GUEST_LAYOUT_LAVAPIPE=1)
endif()
include(${CMAKE_SOURCE_DIR}/boards/${ANKO_BOARD}/board.cmake)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/board.json CONTENT
    "{\"board\":\"${ANKO_BOARD}\",\"load_base\":\"${ANKO_LOAD_BASE}\",\"text_offset\":\"${ANKO_IMAGE_TEXT_OFFSET}\"}\n")
if(ANKO_BOARD STREQUAL "switch-t210")
    add_compile_definitions(ANKO_BOARD_SWITCH_T210=1)
endif()
if(ANKO_BOARD STREQUAL "huawei-anne" OR ANKO_ANNE_DIAG_QEMU)
    add_compile_definitions(ANKO_BOARD_HUAWEI_ANNE=1)
endif()
if(ANKO_ANNE_DIAG_QEMU)
    add_compile_definitions(ANKO_ANNE_DIAG_TEST=1)
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
set(AME_SIGNING_KEY "" CACHE FILEPATH "Ed25519 PKCS8 private signing key (release packaging only)")
set(AME_TRUSTED_KEY_FILES "" CACHE STRING "Semicolon-separated Ed25519 public key files")

# The private key and the trust store are a pair: the signer refuses a key that the
# trust store does not contain. Build the signer arguments from the current values,
# run the signer, and report failures without a Python traceback.
function(ame_signing_args out)
    set(args --output ${CMAKE_BINARY_DIR}/generated/manifest_trust.h)
    if(ANKO_BUILD_DEV)
        list(APPEND args --development)
    endif()
    if(AME_SIGNING_KEY)
        list(APPEND args --signing-key ${AME_SIGNING_KEY})
    endif()
    list(APPEND args --trusted ${AME_TRUSTED_KEY_FILES})
    set(${out} ${args} PARENT_SCOPE)
endfunction()

function(ame_run_signer result_var error_var)
    ame_signing_args(args)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mx/signing.py ${args}
        RESULT_VARIABLE result ERROR_VARIABLE error)
    set(${result_var} ${result} PARENT_SCOPE)
    set(${error_var} ${error} PARENT_SCOPE)
endfunction()

if(NOT ANKO_BUILD_DEV)
    # Release packaging is explicit: both halves of the pair must be given and must
    # exist, so a typo or a stale cache entry is named before the signer runs.
    if(NOT AME_SIGNING_KEY)
        message(FATAL_ERROR
            "Release packaging requires a signing key:\n"
            "  -DAME_SIGNING_KEY=<Ed25519 PKCS8 .pem>   (scripts/make-release-key.sh writes one)")
    endif()
    if(NOT AME_TRUSTED_KEY_FILES)
        message(FATAL_ERROR
            "Release packaging requires a trust store:\n"
            "  -DAME_TRUSTED_KEY_FILES=<public key .pem>")
    endif()
    foreach(path ${AME_SIGNING_KEY} ${AME_TRUSTED_KEY_FILES})
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "Signing key file does not exist: '${path}'")
        endif()
    endforeach()
endif()

ame_run_signer(AME_TRUST_RESULT AME_TRUST_ERROR)
if(NOT AME_TRUST_RESULT EQUAL 0 AND ANKO_BUILD_DEV AND (AME_SIGNING_KEY OR AME_TRUSTED_KEY_FILES))
    # A development build signs with the development key, so key material cached
    # from another mode (a release key, a release trust store, a path that has
    # since moved) cannot be honoured. Clear the pair so the tree converges on
    # development signing instead of failing on every configure from here on.
    message(STATUS "signing: clearing stale AME_SIGNING_KEY / AME_TRUSTED_KEY_FILES; "
                   "a dev build signs with the development key")
    unset(AME_SIGNING_KEY CACHE)
    unset(AME_TRUSTED_KEY_FILES CACHE)
    set(AME_SIGNING_KEY "")
    set(AME_TRUSTED_KEY_FILES "")
    ame_run_signer(AME_TRUST_RESULT AME_TRUST_ERROR)
endif()
if(NOT AME_TRUST_RESULT EQUAL 0)
    string(STRIP "${AME_TRUST_ERROR}" AME_TRUST_ERROR)
    # Lead with the signer's own reason: a traceback is noise in a configure log.
    if(AME_TRUST_ERROR MATCHES "ValueError: ([^\n]*)")
        set(AME_TRUST_ERROR "${CMAKE_MATCH_1}")
    endif()
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
