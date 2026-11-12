@echo off
rem Build the experimental ReGPU renderer plugin (rexgpu-regpurd.dll) for tbc.exe.
rem ReGPU is a Direct3D 12 Xbox 360 GPU by zer0ducksgiven, used here through Glue2GPU's
rem ReXGlue plugin, adapted for this game in regpu-plugin\. See docs\ROADMAP.md.
rem
rem Run from an "x64 Native Tools Command Prompt for VS" (clang, cmake, ninja and fxc), after
rem tools\build_sdk.bat and after tbc.exe has been built once. Needs git, the Python launcher
rem (py) and an internet connection.
rem
rem ReGPU is checked out at a pinned commit and our patches in tools\regpu-patches are applied.
rem
rem   tools\build_regpu.bat [work folder]      (default C:\rexglue-src, as build_sdk.bat)
rem
rem Then start the game with the new renderer:
rem   out\build\win-amd64-relwithdebinfo\tbc.exe --gpu_plugin=regpu ...
setlocal EnableDelayedExpansion

rem Pinned sources.
set "REGPU_COMMIT=7320e6a56eae64f42bdd7e4565b35317249ec050"
set "FFX_TAG=v2.1.0"

set "WORK=%~1"
if "%WORK%"=="" set "WORK=C:\rexglue-src"
set "SDK_SRC=%WORK%\rexglue-sdk"
set "SDK_INSTALL=%SDK_SRC%\out\install\win-amd64"
set "REGPU=%WORK%\regpu"
set "FFX=%WORK%\fidelityfx-sdk"
for %%I in ("%~dp0..") do set "REPO=%%~fI"
set "BUILD=%REPO%\out\build\regpu"
set "GAME_BIN=%REPO%\out\build\win-amd64-relwithdebinfo"

where clang >nul 2>&1 || (echo clang not found. Run this from the x64 Native Tools Command Prompt. & exit /b 1)
where cmake >nul 2>&1 || (echo cmake not found. & exit /b 1)
where ninja >nul 2>&1 || (echo ninja not found. & exit /b 1)
where git >nul 2>&1 || (echo git not found. & exit /b 1)
where fxc >nul 2>&1 || (echo fxc not found. Run this from the x64 Native Tools Command Prompt ^(it puts the Windows SDK on PATH^). & exit /b 1)
where py >nul 2>&1 || (echo The Python launcher ^(py^) was not found. Install Python from python.org. & exit /b 1)
if not exist "%SDK_SRC%\include\rex\ppc\context.h" (echo ReXGlue SDK source not found at %SDK_SRC%. Run tools\build_sdk.bat first. & exit /b 1)
if not exist "%SDK_INSTALL%\lib\cmake\rexglue" (echo ReXGlue SDK install not found at %SDK_INSTALL%. Run tools\build_sdk.bat first. & exit /b 1)

echo === 1/4 Getting ReGPU (%REGPU_COMMIT:~0,10%)
if not exist "%REGPU%\.git" (
  git -c core.autocrlf=false clone --filter=blob:none --no-checkout https://github.com/zer0ducksgiven/ReGPU "%REGPU%" || exit /b 1
)
rem Checked out as-is (LF), so the patches apply: Git for Windows would otherwise convert to CRLF.
git -C "%REGPU%" config core.autocrlf false
git -C "%REGPU%" fetch --quiet origin %REGPU_COMMIT% 2>nul
git -C "%REGPU%" -c advice.detachedHead=false checkout --quiet --force %REGPU_COMMIT% || exit /b 1
rem Every file written again from the commit (undoes the last run's patches and any CRLF copies).
git -C "%REGPU%" rm --cached -r --quiet . >nul
git -C "%REGPU%" reset --quiet --hard %REGPU_COMMIT% || exit /b 1
rem Our fixes to ReGPU (tools\regpu-patches), on a clean checkout each time.
for %%P in ("%REPO%\tools\regpu-patches\*.patch") do (
  echo Applying %%~nxP
  git -C "%REGPU%" apply --whitespace=nowarn "%%~fP" || (echo Patch %%~nxP didn't apply. & exit /b 1)
)

echo === 2/4 Getting the FidelityFX SDK's FSR 1 shader headers (%FFX_TAG%)
if not exist "%FFX%\.git" (
  set "GIT_LFS_SKIP_SMUDGE=1"
  git -c core.autocrlf=false clone --depth 1 --branch %FFX_TAG% --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK "%FFX%" || exit /b 1
)
git -C "%FFX%" sparse-checkout set Kits/FidelityFX/api/internal/gpu Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr1 || exit /b 1
if not exist "%FFX%\Kits\FidelityFX\upscalers\fsr3\include\gpu\fsr1\ffx_fsr1.h" (echo FidelityFX headers missing from %FFX%. & exit /b 1)

echo === 3/4 Building rexgpu-regpu (RelWithDebInfo, to match tbc.exe)
cmake -S "%REPO%\regpu-plugin" -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
  -DCMAKE_CXX_FLAGS=-march=x86-64-v2 ^
  -DCMAKE_PREFIX_PATH="%SDK_INSTALL%" ^
  -Drexglue_DIR="%SDK_INSTALL%\lib\cmake\rexglue" ^
  -DTBC_REGPU_SRC="%REGPU%" ^
  -DTBC_REXGLUE_SRC="%SDK_SRC%" ^
  -DREGPU_FFX="%FFX%\Kits\FidelityFX" || exit /b 1
cmake --build "%BUILD%" --target rexgpu-regpu || exit /b 1

echo === 4/4 Copying beside tbc.exe
if not exist "%GAME_BIN%" (
  echo %GAME_BIN% not found: build tbc.exe first, then copy these from %BUILD% yourself:
  echo   rexgpu-regpurd.dll  rexgpu-regpurd.pdb
  exit /b 0
)
copy /y "%BUILD%\rexgpu-regpurd.dll" "%GAME_BIN%\" >nul || exit /b 1
if exist "%BUILD%\rexgpu-regpurd.pdb" copy /y "%BUILD%\rexgpu-regpurd.pdb" "%GAME_BIN%\" >nul
rem ReGPU compiles the game's shaders with d3dcompiler_47 at run time. Windows has one, but
rem the redistributable copy from the Windows SDK is the version ReGPU is tested with.
set "D3DC=%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll"
if exist "%D3DC%" copy /y "%D3DC%" "%GAME_BIN%\" >nul

echo.
echo Done: %GAME_BIN%\rexgpu-regpurd.dll
echo Test with:  tbc.exe --gpu_plugin=regpu   (plus your usual --game_data_root and settings)
echo The first start compiles shaders for a while; later starts reuse the cache.
exit /b 0
