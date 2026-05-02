add_executable(anko-boot
    entry/start.S
    entry/boot_main.cc
    dtb/fdt.cc
    memory/phys_map.cc
    mmu/page_table.cc
    mmu/mmu_enable.S
)

target_include_directories(anko-boot PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/lib/include
)

target_link_libraries(anko-boot PRIVATE libk)

target_link_options(anko-boot PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/linker/boot.ld
    -Wl,--build-id=none
)

add_custom_command(TARGET anko-boot POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:anko-boot>
            $<TARGET_FILE_DIR:anko-boot>/anko-boot.bin
    COMMENT "Stripping to raw binary"
)
