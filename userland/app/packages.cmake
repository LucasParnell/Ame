# Explicit package builds; applications are provisioned on AmeFS, never
# installed by init. The same packer accepts compiler manifests and shaders.
mochi_service(app.package-demo app/package-demo/main.cc 0x12E00000)
set_target_properties(app.package-demo PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/package-demo.amp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mx/mkapppackage.py
        --executable ${CMAKE_CURRENT_BINARY_DIR}/app.package-demo.mxi
        --resource ui/label.txt=${CMAKE_CURRENT_SOURCE_DIR}/app/package-demo/label.txt
        --output ${CMAKE_CURRENT_BINARY_DIR}/package-demo.amp
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/app.package-demo.mxi
        ${CMAKE_CURRENT_SOURCE_DIR}/app/package-demo/label.txt
        ${CMAKE_SOURCE_DIR}/scripts/mx/mkapppackage.py
    VERBATIM)
add_custom_target(app_package_demo DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/package-demo.amp)

if(ANKO_BUILD_DEV AND ANKO_BOARD STREQUAL "qemu-virt")
    mochi_service(test.app_package_system ${CMAKE_SOURCE_DIR}/tests/kernel/app_system.cc
        0x10F00000 msh/msh_fs.cc msh/msh_io.cc)
    target_compile_definitions(test.app_package_system PRIVATE APP_PACKAGE_ONLY=1)
    target_compile_options(test.app_package_system PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test.app_package_system PRIVATE -Wl,--gc-sections)
    set_target_properties(test.app_package_system PROPERTIES EXCLUDE_FROM_ALL TRUE)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/app-package-system.mxb
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test/package-app-system.py
            ${CMAKE_BINARY_DIR} --packages
        DEPENDS mochi_system ${CMAKE_BINARY_DIR}/system.mxb
            ${CMAKE_CURRENT_BINARY_DIR}/test.app_package_system.mxi
            ${CMAKE_SOURCE_DIR}/scripts/test/package-app-system.py
        VERBATIM)
    add_custom_target(app_package_regressions DEPENDS
        ${CMAKE_BINARY_DIR}/app-package-system.mxb)
    add_dependencies(app_package_regressions app_package_demo)
    add_dependencies(app_package_regressions app_ui_demo)
endif()
