# rexgpu-regpu for The Bourne Conspiracy: ReGPU as a ReXGlue GPU plugin (--gpu_plugin=regpu).
# Experimental. Built by tools\build_regpu.bat, which fetches the pinned sources and sets:
#   CMAKE_PREFIX_PATH   the installed ReXGlue SDK tbc.exe is built against
#   TBC_REGPU_SRC       ReGPU checkout (rexglue-driver branch)
#   TBC_REXGLUE_SRC     ReXGlue SDK source (include/rex/ppc/context.h, thirdparty/simde)
#   REGPU_FFX           FidelityFX SDK's Kits/FidelityFX folder (FSR1 shader headers)
# Build with the SDK's compiler (clang) and in the same configuration as tbc.exe
# (RelWithDebInfo), so the plugin shares tbc's rexruntime DLL.
cmake_minimum_required(VERSION 3.25)
project(tbc_regpu CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TBC_REGPU_SRC "" CACHE PATH "ReGPU checkout (rexglue-driver branch)")
set(TBC_REXGLUE_SRC "" CACHE PATH "ReXGlue SDK source tree")
if(NOT EXISTS "${TBC_REGPU_SRC}/include/regpu/regpu.h")
    message(FATAL_ERROR "Set TBC_REGPU_SRC to a ReGPU checkout")
endif()

find_package(rexglue CONFIG REQUIRED)

# ReGPU without its Vd* exports, against ReXGlue's PPCContext (regpu::driver).
set(REGPU_GUEST_ABI rexglue CACHE STRING "" FORCE)
set(REGPU_REXGLUE_DIR "${TBC_REXGLUE_SRC}" CACHE PATH "" FORCE)
add_subdirectory("${TBC_REGPU_SRC}" regpu)

add_library(rexgpu-regpu SHARED
    src/plugin_main.cpp
    src/regpu_graphics_system.cpp
    src/stack_dump.cpp)
target_link_libraries(rexgpu-regpu PRIVATE regpu rex::runtime dbghelp)
# The runtime loads rexgpu-<name><postfix>.dll, with the SDK's per-configuration postfix.
set_target_properties(rexgpu-regpu PROPERTIES
    DEBUG_POSTFIX d
    RELWITHDEBINFO_POSTFIX rd)
