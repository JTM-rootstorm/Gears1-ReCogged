# gears1_recomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(gears1_recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(GEARS1RECOMP_SOURCES
    src/main.cpp
    src/hooks_gen.cpp
    src/mouse_look.cpp
    src/native_buffers.cpp
    src/native_input.cpp
    src/native_renderer.cpp
    src/native_targets.cpp
    src/native_textures.cpp
    src/probes.cpp
    src/shader_dump.cpp
    third_party/imgui/imgui_impl_sdlgpu3.cpp
)

# The native renderer's own shaders, compiled to SPIR-V arrays
# (<name>.<stage>.h in the build tree, variable <name>_<stage>).
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
foreach(shader mesh_preview.vert mesh_preview.frag rect.vert clear.frag
        clear_depth.frag copy.frag)
    string(REPLACE "." "_" variable ${shader})
    set(header ${CMAKE_CURRENT_BINARY_DIR}/${shader}.h)
    add_custom_command(OUTPUT ${header}
        COMMAND ${GLSLANG_VALIDATOR} -V --vn ${variable} -o ${header}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${shader}
        DEPENDS src/shaders/${shader}
        VERBATIM)
    list(APPEND GEARS1RECOMP_SOURCES ${header})
endforeach()

if(WIN32)
    add_executable(gears1_recomp WIN32 ${GEARS1RECOMP_SOURCES})
else()
    add_executable(gears1_recomp ${GEARS1RECOMP_SOURCES})
endif()

rexglue_setup_target(gears1_recomp GPU_PLUGINS xenos)
target_include_directories(gears1_recomp PRIVATE third_party/imgui ${CMAKE_CURRENT_BINARY_DIR})

# Optional patched xenos plugin (e.g. built from the SDK source tree). Copied
# after the stock plugin that rexglue_setup_target stages, so it wins.
set(GEARS1_GPU_PLUGIN_OVERRIDE "" CACHE FILEPATH
    "Patched librexgpu-xenos*.so to stage next to the executable instead of the SDK's")
if(GEARS1_GPU_PLUGIN_OVERRIDE)
    # A separate target (not POST_BUILD) so a rebuilt plugin is re-staged even
    # when the executable itself doesn't relink.
    add_custom_target(stage_gpu_plugin_override ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GEARS1_GPU_PLUGIN_OVERRIDE}
            $<TARGET_FILE_DIR:gears1_recomp>/$<TARGET_FILE_NAME:rex::gpu-xenos>
        VERBATIM)
    add_dependencies(stage_gpu_plugin_override gears1_recomp)
    # rexglue_setup_target copies the stock plugin after every relink of the
    # executable; copy the override after it too, so building just
    # gears1_recomp can't leave the stock plugin staged.
    add_custom_command(TARGET gears1_recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${GEARS1_GPU_PLUGIN_OVERRIDE}
            $<TARGET_FILE_DIR:gears1_recomp>/$<TARGET_FILE_NAME:rex::gpu-xenos>
        VERBATIM)
endif()

# Runtime config (cvars) read from <exe dir>/gears1_recomp.toml.
add_custom_command(TARGET gears1_recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/config/gears1_recomp.toml
        $<TARGET_FILE_DIR:gears1_recomp>/gears1_recomp.toml
    VERBATIM)
