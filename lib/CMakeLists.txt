# Separate archives keep compiler-generated NEON out of the kernel and early
# boot while allowing the complete userland runtime to use FP/Advanced SIMD.
foreach(variant IN ITEMS "" "_user")
add_library(freestanding_memory${variant} STATIC libk/string.cc)
target_include_directories(freestanding_memory${variant} PUBLIC include)
# These routines also run before the MMU establishes Normal memory attributes.
target_compile_options(freestanding_memory${variant} PRIVATE -mstrict-align)
add_library(libk${variant} STATIC libk/printf.cc libk/panic.cc)
target_include_directories(libk${variant} PUBLIC include)
target_compile_options(libk${variant} PRIVATE -mstrict-align)
target_link_libraries(libk${variant} PUBLIC freestanding_memory${variant})

set(AME_CRYPTO_DIR ${CMAKE_SOURCE_DIR}/third_party/monocypher)
set_source_files_properties(${AME_CRYPTO_DIR}/monocypher.c ${AME_CRYPTO_DIR}/monocypher-ed25519.c PROPERTIES LANGUAGE CXX)
add_library(ame_format${variant} STATIC
    ${CMAKE_SOURCE_DIR}/shared/format/integrity.cc
    ${CMAKE_SOURCE_DIR}/shared/format/bundle.cc
    ${CMAKE_SOURCE_DIR}/shared/format/archive.cc
    ${CMAKE_SOURCE_DIR}/shared/format/signature.cc
    ${CMAKE_SOURCE_DIR}/shared/format/image.cc
    ${AME_CRYPTO_DIR}/monocypher.c ${AME_CRYPTO_DIR}/monocypher-ed25519.c)
target_include_directories(ame_format${variant} PUBLIC ${CMAKE_SOURCE_DIR}/shared/include PRIVATE ${AME_CRYPTO_DIR})
target_compile_options(ame_format${variant} PRIVATE -mstrict-align -ffunction-sections -fdata-sections)
target_link_libraries(ame_format${variant} PUBLIC freestanding_memory${variant})

if(variant STREQUAL "_user")
    set_target_properties(freestanding_memory${variant} libk${variant} ame_format${variant}
        PROPERTIES AME_USERLAND_SIMD TRUE)
endif()
endforeach()
