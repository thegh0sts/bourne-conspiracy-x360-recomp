# Skullburglar's pre-1.0 native renderer as a ReXGlue GPU plugin: rexgpu-native[rd].dll,
# selected with --gpu_plugin=native.
#
# Skullburglar (https://github.com/nlwstein/skullburglar, BSD 3-Clause) is a ReXGlue port of
# another Xbox 360 Unreal Engine 3 game. This renderer keeps the SDK's command processor (the
# PM4 ring, interrupts, the CPU/GPU handshakes) and its Xenos -> SPIR-V shader translator, but
# draws with Vulkan into real images instead of emulating EDRAM: each EDRAM surface is its own
# image and a resolve is a copy. Its n1 renderer (Rust, reads that game's scene from memory) is
# left out; without it, and without that game's shared state (BOTS_HYBRID_STATE), the plugin
# draws everything itself.
#
# Built inside the SDK's own build (tools\build_native.bat), through SDK patch
# 0007-extra-gpu-plugins: it needs the SDK's source tree and its glslang, VMA and Vulkan
# targets, as Skullburglar's own build does. The sources come from the Skullburglar checkout
# tools\build_sdk.bat already makes (TBC_SKULL_DIR), at the commit it pins. This file follows
# that commit's CMakeLists.txt (BOTS_NATIVE_GFX_SOURCES and the rexgpu-native target).
#
# Render scale: BOTS_GFX_RENDER_SCALE (environment, 0.25-3.0) or a "render_scale" setting;
# default 1.

if(NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "rexgpu-native needs the SDK built with -DREXGLUE_USE_VULKAN=ON")
endif()
set(TBC_SKULL_DIR "" CACHE PATH "Skullburglar checkout with the native renderer's sources")
if(NOT EXISTS "${TBC_SKULL_DIR}/src/gfx/native.cpp")
    message(FATAL_ERROR "TBC_SKULL_DIR (${TBC_SKULL_DIR}) has no src/gfx/native.cpp")
endif()

set(K "${TBC_SKULL_DIR}")
set(G "${REXGLUE_ROOT}/src/graphics")

# Keyboard/mouse and PlayStation prompt glyphs (Kenney Input Prompts, CC0) compiled in as byte
# arrays; the renderer's prompt atlas needs them to link (Bourne doesn't use them).
set(_glyphs_src "#include <cstddef>\n")
foreach(_sheet keyboard-mouse:PromptGlyphs playstation:PlayStationGlyphs)
    string(REPLACE ":" ";" _sheet "${_sheet}")
    list(GET _sheet 0 _name)
    list(GET _sheet 1 _symbol)
    foreach(_kind Png Xml)
        string(TOLOWER ${_kind} _ext)
        set(_file ${K}/assets/prompts/${_name}.${_ext})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_file})
        file(READ ${_file} _hex HEX)
        string(LENGTH "${_hex}" _len)
        math(EXPR _len "${_len} / 2")
        string(REGEX REPLACE "(..)" "0x\\1," _hex "${_hex}")
        string(APPEND _glyphs_src "extern const unsigned char k${_symbol}${_kind}[] = {${_hex}};\n"
            "extern const size_t k${_symbol}${_kind}Size = ${_len};\n")
    endforeach()
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp.in "${_glyphs_src}")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp
    COPYONLY)
set_source_files_properties(${K}/src/gfx/prompt_atlas.cpp PROPERTIES INCLUDE_DIRECTORIES ${REXGLUE_ROOT}/thirdparty/stb)

add_library(rexgpu-native SHARED
    ${K}/src/gfx/native.cpp
    ${K}/src/gfx/host_stats.cpp
    ${K}/src/gfx/vk/memory_sync.cpp
    ${K}/src/upk.cpp
    ${K}/src/gfx/prompt_atlas.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/prompt_glyphs.cpp
    ${K}/src/gfx/pm4.cpp
    ${K}/src/gfx/vk/draw.cpp
    ${K}/src/gfx/vk/manifest.cpp
    ${K}/src/gfx/vk/occlusion.cpp
    ${K}/src/gfx/vk/present.cpp
    ${K}/src/gfx/vk/renderer.cpp
    ${K}/src/gfx/vk/shaders.cpp
    ${K}/src/gfx/vk/texture.cpp
    ${K}/src/gfx/vk/transfer.cpp
    ${K}/src/gfx/vk/hybrid_fill.cpp
    ${G}/register_file.cpp
    ${G}/registers.cpp
    # Xenos microcode -> SPIR-V: the SDK's translator, compiled in again (the xenos plugin
    # exports nothing).
    ${G}/flags.cpp
    ${G}/format/ucode.cpp
    ${G}/sampler_info.cpp
    ${G}/pipeline/texture/conversion.cpp
    ${G}/pipeline/texture/extent.cpp
    ${G}/pipeline/texture/info.cpp
    ${G}/pipeline/texture/info_formats.cpp
    ${G}/pipeline/texture/util.cpp
    ${G}/util/draw.cpp
    ${G}/pipeline/shader/shader.cpp
    ${G}/pipeline/shader/translator.cpp
    ${G}/pipeline/shader/translator_disasm.cpp
    ${G}/pipeline/shader/spirv.cpp
    ${G}/pipeline/shader/spirv_builder.cpp
    ${G}/pipeline/shader/spirv_translator.cpp
    ${G}/pipeline/shader/spirv_translator_alu.cpp
    ${G}/pipeline/shader/spirv_translator_fetch.cpp
    ${G}/pipeline/shader/spirv_translator_memexport.cpp
    ${G}/pipeline/shader/spirv_translator_rb.cpp
    ${G}/vulkan/shader.cpp
    # The command processor and graphics system the plugin builds on.
    ${G}/shared_memory.cpp
    ${G}/graphics_system.cpp
    ${G}/command_processor.cpp)

# These two define GPU settings that belong to whichever GPU plugin is loaded: here they keep
# their storage and defaults but aren't registered (Skullburglar's src/gfx/sdk_cvars.h).
set_source_files_properties(${G}/flags.cpp ${G}/util/draw.cpp
    PROPERTIES COMPILE_OPTIONS "-include;${K}/src/gfx/sdk_cvars.h")

set_target_properties(rexgpu-native PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    DEBUG_POSTFIX d
    RELWITHDEBINFO_POSTFIX rd)
target_include_directories(rexgpu-native PRIVATE ${K}/src ${REXGLUE_ROOT}/thirdparty/renderdoc)
target_compile_options(rexgpu-native PRIVATE -Wno-unused-variable)
target_link_libraries(rexgpu-native PRIVATE
    rexruntime
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    xxHash::xxhash
    glslang::SPIRV
    glslang
    MachineIndependent
    GenericCodeGen
    OSDependent
    OGLCompiler)
if(WIN32)
    target_link_libraries(rexgpu-native PRIVATE powrprof)  # host_stats.cpp: CPU clocks, power plan
endif()

install(TARGETS rexgpu-native
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib)
