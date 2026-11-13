@echo off
rem Build the ReXGlue SDK v0.10.1 from source, with AMD FidelityFX (FSR and CAS filters),
rem Skullburglar's performance patches and our own fixes in tools\sdk-patches. See docs/ROADMAP.md.
rem
rem Run from an "x64 Native Tools Command Prompt for VS" (it provides clang, cmake and ninja).
rem Needs git and an internet connection. Takes a while: the SDK is a large build.
rem
rem   tools\build_sdk.bat [work folder]      (default C:\rexglue-src)
rem
rem With TBC_NATIVE=1 set (tools\build_native.bat does it), the SDK is also built with Vulkan
rem and with Skullburglar's native renderer as a second GPU plugin (native-plugin\).
rem
rem Afterwards, point this project at the new SDK by reconfiguring with
rem   -DCMAKE_PREFIX_PATH=<work folder>/rexglue-sdk/out/install/win-amd64
setlocal EnableDelayedExpansion

set "SDK_TAG=v0.10.1"
rem Skullburglar commit the patch list below was checked against.
set "SKULL_COMMIT=67aacb28670077e839f84931dee9b162526bbdb2"
rem 0003 is a Vulkan change, but it also carries header declarations that 0006 and 0008 need.
rem 0024 (VK_EXT_external_memory_host) and 0027 (prefer a discrete GPU) are for the native
rem renderer plugin (tools\build_native.bat); they change nothing in a D3D12-only build.
set "PATCHES=0002 0003 0004 0006 0008 0009 0024 0026 0027 0029 0030"
rem Our own SDK fixes (tools\sdk-patches\*.patch), applied after Skullburglar's. Change this tag
rem when that folder changes, so an existing SDK checkout is patched again.
set "OWN_PATCHES=own7"
set "PATCH_SET=%PATCHES% %OWN_PATCHES%"

set "WORK=%~1"
if "%WORK%"=="" set "WORK=C:\rexglue-src"
set "SDK=%WORK%\rexglue-sdk"
set "SKULL=%WORK%\skullburglar"

where clang >nul 2>&1 || (echo clang not found. Run this from the x64 Native Tools Command Prompt. & exit /b 1)
where cmake >nul 2>&1 || (echo cmake not found. & exit /b 1)
where git >nul 2>&1 || (echo git not found. & exit /b 1)

if not exist "%WORK%" mkdir "%WORK%"

rem The patches have Unix (LF) line endings. Git for Windows normally converts checkouts to
rem CRLF (core.autocrlf), which makes "git apply" fail, so both repos are checked out as-is.
echo === 1/4 Getting the SDK source (%SDK_TAG%)
if not exist "%SDK%\.git" (
  git -c core.autocrlf=false clone --recursive --branch %SDK_TAG% https://github.com/rexglue/rexglue-sdk "%SDK%" || exit /b 1
) else (
  echo Already there: %SDK%
)
rem Patches are applied once to a clean checkout; a marker file records which set. If the
rem set changes, the SDK checkout is reset and patched again.
set "MARKER=%SDK%\.tbc_patches"
set "APPLIED="
if exist "%MARKER%" set /p APPLIED=<"%MARKER%"
set "NEED_PATCH=1"
if "%APPLIED%"=="%PATCH_SET%" set "NEED_PATCH=0"
if "%NEED_PATCH%"=="1" (
  call :lf_checkout "%SDK%" || exit /b 1
)

echo === 2/4 Getting Skullburglar's patches
if not exist "%SKULL%\.git" (
  git -c core.autocrlf=false clone --filter=blob:none https://github.com/nlwstein/skullburglar "%SKULL%" || exit /b 1
)
git -C "%SKULL%" -c advice.detachedHead=false checkout --quiet %SKULL_COMMIT% || exit /b 1
call :lf_checkout "%SKULL%" || exit /b 1
rem The native renderer plugin's sources get our changes for Bourne (native-plugin\patches).
if "%TBC_NATIVE%"=="1" (
  for %%F in ("%~dp0..\native-plugin\patches\*.patch") do (
    git -C "%SKULL%" apply "%%~fF" || (echo Patch %%~nxF failed to apply & exit /b 1)
    echo %%~nxF applied to the native renderer
  )
)

echo === 3/4 Applying patches
if "%NEED_PATCH%"=="0" (
  echo Already applied: %PATCH_SET%
) else (
  for %%P in (%PATCHES%) do (
    set "FILE="
    for %%F in ("%SKULL%\patches\rexglue-sdk\%%P-*.patch") do set "FILE=%%~fF"
    if "!FILE!"=="" (echo Patch %%P not found & exit /b 1)
    git -C "%SDK%" apply "!FILE!" || (echo Patch %%P failed to apply & exit /b 1)
    echo %%P applied
  )
  for %%F in ("%~dp0sdk-patches\*.patch") do (
    git -C "%SDK%" apply "%%~fF" || (echo Patch %%~nxF failed to apply & exit /b 1)
    echo %%~nxF applied
  )
  >"%MARKER%" echo %PATCH_SET%
)

rem Symbolic links in the SDK's submodules (libmspack) come out as text stubs on Windows.
set "FIX=%~dp0fix_symlinks.sh"
set "FIX=%FIX:\=/%"
git -C "%SDK%" submodule foreach --quiet --recursive "sh '%FIX%'" || exit /b 1

echo === 4/4 Building and installing (RelWithDebInfo and Release)
rem Both settings are always passed: CMake keeps cached values, so a build without TBC_NATIVE
rem turns the native plugin (and Vulkan) off again.
set "NATIVE_DIR=%~dp0..\native-plugin"
for %%I in ("%NATIVE_DIR%") do set "NATIVE_DIR=%%~fI"
set "NATIVE_DIR=%NATIVE_DIR:\=/%"
set "SKULL_FWD=%SKULL:\=/%"
if "%TBC_NATIVE%"=="1" (
  echo Including the native renderer plugin from %NATIVE_DIR%
  set "EXTRA=-DREXGLUE_USE_VULKAN=ON -DREXGLUE_EXTRA_GPU_PLUGINS=%NATIVE_DIR% -DTBC_SKULL_DIR=%SKULL_FWD%"
) else (
  set "EXTRA=-DREXGLUE_USE_VULKAN=OFF -DREXGLUE_EXTRA_GPU_PLUGINS="
)
pushd "%SDK%"
cmake --preset win-amd64 -DREXGLUE_ENABLE_FIDELITYFX=ON !EXTRA! || (popd & exit /b 1)
cmake --build out\build\win-amd64 --config RelWithDebInfo --target install --parallel || (popd & exit /b 1)
cmake --build out\build\win-amd64 --config Release --target install --parallel || (popd & exit /b 1)
popd

echo.
echo Done. SDK installed to:
echo   %SDK%\out\install\win-amd64
echo Reconfigure this project against it, from the project folder:
echo   cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=%SDK:\=/%/out/install/win-amd64
echo   cmake --build out\build\win-amd64-relwithdebinfo
endlocal
exit /b 0

rem Set a repo to keep LF line endings and rewrite its working files that way. This also
rem discards any earlier patches (their changes to tracked files and the files they added).
:lf_checkout
git -C "%~1" config core.autocrlf false
git -C "%~1" config core.eol lf
git -C "%~1" rm --cached -r -q . >nul 2>&1
git -C "%~1" reset --hard --quiet || exit /b 1
rem Files an earlier patch set added are untracked, so reset leaves them; remove them too
rem (only in the source folders: the build output in out\ stays).
git -C "%~1" clean -fdq -- include src cmake || exit /b 1
exit /b 0
