# tbc - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tbc LANGUAGES CXX)

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
)

if(WIN32)
    add_executable(tbc WIN32 ${TBC_SOURCES})
else()
    add_executable(tbc ${TBC_SOURCES})
endif()

rexglue_setup_target(tbc GPU_PLUGINS xenos)

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
