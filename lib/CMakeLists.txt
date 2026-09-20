add_library(freestanding_memory STATIC libk/string.cc)
target_include_directories(freestanding_memory PUBLIC include)
# These routines also run before the MMU establishes Normal memory attributes.
target_compile_options(freestanding_memory PRIVATE -mstrict-align)
add_library(libk STATIC libk/printf.cc libk/panic.cc)
target_include_directories(libk PUBLIC include)
target_compile_options(libk PRIVATE -mstrict-align)
target_link_libraries(libk PUBLIC freestanding_memory)

set(AME_CRYPTO_DIR ${CMAKE_SOURCE_DIR}/third_party/monocypher)
set_source_files_properties(${AME_CRYPTO_DIR}/monocypher.c ${AME_CRYPTO_DIR}/monocypher-ed25519.c PROPERTIES LANGUAGE CXX)
add_library(ame_format STATIC
    ${CMAKE_SOURCE_DIR}/shared/format/integrity.cc
    ${CMAKE_SOURCE_DIR}/shared/format/bundle.cc
    ${CMAKE_SOURCE_DIR}/shared/format/signature.cc
    ${CMAKE_SOURCE_DIR}/shared/format/image.cc
    ${AME_CRYPTO_DIR}/monocypher.c ${AME_CRYPTO_DIR}/monocypher-ed25519.c)
target_include_directories(ame_format PUBLIC ${CMAKE_SOURCE_DIR}/shared/include PRIVATE ${AME_CRYPTO_DIR})
target_compile_options(ame_format PRIVATE -mstrict-align -ffunction-sections -fdata-sections)
target_link_libraries(ame_format PUBLIC freestanding_memory)
