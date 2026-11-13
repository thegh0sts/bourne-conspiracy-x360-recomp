@echo off
rem Build the SDK (tools\build_sdk.bat) with Vulkan and Skullburglar's pre-1.0 native renderer
rem as a second GPU plugin, rexgpu-native (native-plugin\CMakeLists.txt). Then rebuild tbc,
rem which copies the plugin next to tbc.exe, and start the game with --gpu_plugin=native.
rem
rem Run from an "x64 Native Tools Command Prompt for VS", like build_sdk.bat.
rem
rem   tools\build_native.bat [work folder]      (default C:\rexglue-src)
rem
rem tools\build_sdk.bat on its own builds the SDK without the plugin again.
setlocal
set "TBC_NATIVE=1"
call "%~dp0build_sdk.bat" %*
exit /b %ERRORLEVEL%
