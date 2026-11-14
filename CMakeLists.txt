# tbc - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# The release number. Shown in the log's first line (version_log.cpp); keep it the same as VERSION in
# installer/tbc_launcher.py.
project(tbc VERSION 0.2.4 LANGUAGES CXX)
# The release name, which can carry a suffix CMake's VERSION can't (0.2.4-1).
set(TBC_RELEASE "0.2.4-1")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TBC_SOURCES
    src/main.cpp
    src/gpu_preload.cpp
    src/fps_unlock.cpp
    src/frame_pacing.cpp
    src/fps_overlay.cpp
    src/crash_win.cpp
    src/gpu_wait_hook.cpp
    src/display_refresh.cpp
    src/debug_tools.cpp
    src/prompt_trace.cpp
    src/keyboard_prompts.cpp
    src/input_trace.cpp
    src/version_log.cpp
    src/script_trace.cpp
    src/msaa.cpp
    src/ultrawide.cpp
    src/qte.cpp
    src/glyph_probe.cpp
)

if(WIN32)
    add_executable(tbc WIN32 ${TBC_SOURCES})
else()
    add_executable(tbc ${TBC_SOURCES})
endif()

rexglue_setup_target(tbc GPU_PLUGINS xenos)
target_compile_definitions(tbc PRIVATE TBC_VERSION="${TBC_RELEASE}")

# AMD FidelityFX runtime (FSR / CAS output filters). Only present when the SDK was built with
# REXGLUE_ENABLE_FIDELITYFX (tools/build_sdk.bat); the runtime DLL needs it next to tbc.exe.
# The stock SDK download doesn't have it, and then there's nothing to copy.
get_filename_component(TBC_SDK_BIN "${rexglue_DIR}/../../../bin" ABSOLUTE)
file(GLOB TBC_FFX_DLLS "${TBC_SDK_BIN}/amd_fidelityfx_dx12*.dll")
if(TBC_FFX_DLLS)
    add_custom_command(TARGET tbc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TBC_FFX_DLLS} $<TARGET_FILE_DIR:tbc>
        COMMENT "Copying FidelityFX runtime")
endif()

# Skullburglar's native renderer plugin (--gpu_plugin=native), when the SDK was built with it
# (tools/build_native.bat). Checked when the build runs, so building the plugin later needs no
# reconfigure. The copy script is written into the build folder, so the installer's copy of the
# project (src and CMakeLists only, no native-plugin/) builds too.
file(WRITE ${CMAKE_BINARY_DIR}/copy_if_exists.cmake
    "if(EXISTS \"\${SRC}\")\n  file(COPY \"\${SRC}\" DESTINATION \"\${DST}\")\nendif()\n")
add_custom_command(TARGET tbc POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DSRC=${TBC_SDK_BIN}/rexgpu-native$<$<CONFIG:RelWithDebInfo>:rd>$<$<CONFIG:Debug>:d>.dll"
        "-DDST=$<TARGET_FILE_DIR:tbc>"
        -P ${CMAKE_BINARY_DIR}/copy_if_exists.cmake
    VERBATIM)
