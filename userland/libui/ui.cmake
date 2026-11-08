# Device-agnostic GLSL stays the source of truth; the build emits a validated
# Vulkan 1.0 module that ships as an application resource. The driver compiles
# the module once and its own cache retains the result.
find_program(AME_GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(AME_SPIRV_VAL spirv-val REQUIRED)
add_library(ame_ui INTERFACE)
target_include_directories(ame_ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libui/include)
add_library(ame_ui_vulkan STATIC libui/vulkan.cc)
set_property(TARGET ame_ui_vulkan PROPERTY AME_USERLAND_SIMD TRUE)
target_link_libraries(ame_ui_vulkan PUBLIC ame_ui libVulkan)

# Generate native code before compilation and expose versioned metadata for
# the existing application packer. No QML source parser ships in the app.
function(ame_qml_component target source)
    get_filename_component(qml_source "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(component "${source}" NAME_WE)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ui/${target}")
    add_custom_command(OUTPUT "${generated}/${component}.h" "${generated}/${component}.json"
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/ui/qmlc.py
            "${qml_source}" --header "${generated}/${component}.h"
            --metadata "${generated}/${component}.json"
        DEPENDS "${qml_source}" ${CMAKE_SOURCE_DIR}/scripts/ui/qmlc.py
        VERBATIM)
    target_sources(${target} PRIVATE "${generated}/${component}.h")
    target_include_directories(${target} PRIVATE "${generated}")
    target_link_libraries(${target} PRIVATE ame_ui)
    set_property(TARGET ${target} APPEND PROPERTY AME_QML_METADATA "${generated}/${component}.json")
endfunction()

# ame_ui_shaders(<out-var> <name>...) appends `<name>.<stage>.spv` build outputs.
function(ame_ui_shaders output)
    set(modules)
    foreach(name IN LISTS ARGN)
        foreach(stage IN ITEMS vert frag)
            set(source ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders/${name}.${stage})
            set(module ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.${stage}.spv)
            add_custom_command(OUTPUT ${module}
                COMMAND ${AME_GLSLANG_VALIDATOR} -V --target-env vulkan1.0 ${source} -o ${module}
                COMMAND ${AME_SPIRV_VAL} --target-env vulkan1.0 ${module}
                DEPENDS ${source} VERBATIM)
            list(APPEND modules ${module})
        endforeach()
    endforeach()
    set(${output} ${modules} PARENT_SCOPE)
endfunction()
