mochi_service(app.ui-demo app/ui-demo/main.cc 0x12F00000)
set_target_properties(app.ui-demo PROPERTIES EXCLUDE_FROM_ALL TRUE)
ame_qml_component(app.ui-demo app/ui-demo/Counter.qml)
ame_ui_shaders(ui_shaders solid glyph)
get_target_property(counter_metadata app.ui-demo AME_QML_METADATA)
set(ui_resource_flags)
foreach(module IN LISTS ui_shaders)
    get_filename_component(module_name ${module} NAME)
    list(APPEND ui_resource_flags --resource ui/${module_name}=${module})
endforeach()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ui-demo.amp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mx/mkapppackage.py
        --executable ${CMAKE_CURRENT_BINARY_DIR}/app.ui-demo.mxi
        --resource ui/Counter.json=${counter_metadata}
        ${ui_resource_flags}
        --output ${CMAKE_CURRENT_BINARY_DIR}/ui-demo.amp
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/app.ui-demo.mxi ${counter_metadata} ${ui_shaders}
        ${CMAKE_SOURCE_DIR}/scripts/mx/mkapppackage.py
    VERBATIM)
add_custom_target(app_ui_demo DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ui-demo.amp)
