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
)

if(WIN32)
    add_executable(tbc WIN32 ${TBC_SOURCES})
else()
    add_executable(tbc ${TBC_SOURCES})
endif()

rexglue_setup_target(tbc GPU_PLUGINS xenos)
