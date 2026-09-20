set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TRIPLE aarch64-none-elf)

# Prefer a GCC bare-metal toolchain when available.
find_program(AARCH64_GCC   ${TRIPLE}-gcc)
find_program(AARCH64_GXX   ${TRIPLE}-g++)
find_program(AARCH64_OBJCOPY ${TRIPLE}-objcopy)
find_program(AARCH64_OBJDUMP ${TRIPLE}-objdump)

if (AARCH64_GCC AND AARCH64_GXX)
    set(CMAKE_C_COMPILER   "${AARCH64_GCC}")
    set(CMAKE_CXX_COMPILER "${AARCH64_GXX}")
    set(CMAKE_ASM_COMPILER "${AARCH64_GCC}")
    set(CMAKE_OBJCOPY      "${AARCH64_OBJCOPY}")
    set(CMAKE_OBJDUMP      "${AARCH64_OBJDUMP}")
else()
    # Fallback for dev machines without aarch64-none-elf-gcc:
    # clang/clang++ with an explicit freestanding target triple.
    find_program(CLANG_BIN   clang REQUIRED)
    find_program(CLANGXX_BIN clang++ REQUIRED)
    find_program(LLVM_OBJCOPY llvm-objcopy REQUIRED)
    find_program(LLVM_OBJDUMP llvm-objdump)

    set(CMAKE_C_COMPILER   "${CLANG_BIN}")
    set(CMAKE_CXX_COMPILER "${CLANGXX_BIN}")
    set(CMAKE_ASM_COMPILER "${CLANG_BIN}")
    set(CMAKE_C_COMPILER_TARGET   "${TRIPLE}")
    set(CMAKE_CXX_COMPILER_TARGET "${TRIPLE}")
    set(CMAKE_ASM_COMPILER_TARGET "${TRIPLE}")
    set(CMAKE_OBJCOPY "${LLVM_OBJCOPY}")
    set(CMAKE_OBJDUMP "${LLVM_OBJDUMP}")

    add_compile_options(--target=${TRIPLE})
    add_link_options(--target=${TRIPLE} -fuse-ld=lld)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Prevent CMake from probing the compiler with host-style tests
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
