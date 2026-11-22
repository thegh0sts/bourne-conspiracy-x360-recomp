@echo off
rem Build the SDK (tools\build_sdk.bat) with Vulkan and Skullburglar's pre-1.0 native renderer
rem as a second GPU plugin, rexgpu-native (native-plugin\CMakeLists.txt). Then rebuild tbc
rem and start the game with --gpu_plugin=native.
rem
rem Run from an "x64 Native Tools Command Prompt for VS", like build_sdk.bat.
rem
rem   tools\build_native.bat [work folder]      (default C:\rexglue-src)
rem
rem tools\build_sdk.bat on its own builds the SDK without the plugin again.
rem
rem The SDK's install bin folder is shared with other projects (007 installs its own
rem rexgpu-nativerd.dll there), and cmake only copies the plugin next to tbc.exe when
rem tbc.exe relinks. So copy the plugin we just built straight into tbc's build folder.
setlocal
set "TBC_NATIVE=1"
set "WORK=%~1"
if "%WORK%"=="" set "WORK=C:\rexglue-src"
call "%~dp0build_sdk.bat" %*
if errorlevel 1 exit /b %ERRORLEVEL%
set "PLUGIN=%WORK%\rexglue-sdk\out\install\win-amd64\bin\rexgpu-nativerd.dll"
set "DEST=%~dp0..\out\build\win-amd64-relwithdebinfo"
if not exist "%PLUGIN%" (echo plugin not found: %PLUGIN% & exit /b 1)
if not exist "%DEST%\" (echo tbc build folder not found: %DEST% - plugin not copied & exit /b 0)
copy /y "%PLUGIN%" "%DEST%\" >nul || (echo plugin copy failed & exit /b 1)
for %%F in ("%DEST%\rexgpu-nativerd.dll") do echo copied rexgpu-nativerd.dll to tbc build folder (%%~zF bytes)
exit /b 0
