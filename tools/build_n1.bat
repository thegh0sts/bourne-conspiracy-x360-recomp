@echo off
rem Build Skullburglar's n1 renderer (n1/, Rust) for Bourne: tools\n1\target\release\n1ffi.lib, which
rem tools\build_native.bat then links into the native renderer plugin (hybrid fill 4, --hybrid_fill=4).
rem
rem The sources are n1/ from the Skullburglar checkout tools\build_sdk.bat makes, at the commit it pins, with
rem Bourne's asset-reader changes (tools\n1probe\ue3: UE3 v511 packages, no texture file caches) laid over them.
rem tools\n1 is made again on every run (its target\ folder, cargo's build output, is kept).
rem
rem   tools\build_n1.bat [work folder]      (default C:\rexglue-src; build_native.bat must have run there once)
setlocal
set "WORK=%~1"
if "%WORK%"=="" set "WORK=C:\rexglue-src"
set "SKULL=%WORK%\skullburglar"
set "SKULL_COMMIT=67aacb28670077e839f84931dee9b162526bbdb2"
set "TOOLS=%~dp0"
set "N1=%~dp0n1"

where cargo >nul 2>&1 || (echo cargo not found: install Rust from https://rustup.rs & exit /b 1)
where tar >nul 2>&1 || (echo tar not found & exit /b 1)
if not exist "%SKULL%\.git" (echo No Skullburglar checkout at %SKULL%: run tools\build_native.bat first & exit /b 1)

echo === 1/3 n1 sources from Skullburglar %SKULL_COMMIT:~0,8%
if not exist "%N1%" mkdir "%N1%"
for /d %%D in ("%N1%\*") do if /i not "%%~nxD"=="target" rmdir /s /q "%%D"
del /q "%N1%\*" >nul 2>&1
git -C "%SKULL%" archive --format=tar %SKULL_COMMIT% n1 | tar -xf - -C "%TOOLS%." || (echo git archive failed & exit /b 1)
if not exist "%N1%\n1ffi\Cargo.toml" (echo n1 sources missing after the extract & exit /b 1)

echo === 2/3 Bourne's asset readers (tools\n1probe\ue3)
xcopy /y /q "%TOOLS%n1probe\ue3\src\*" "%N1%\ue3\src\" >nul || (echo copy failed & exit /b 1)

echo === 3/3 cargo build (release)
pushd "%N1%"
cargo build --release --locked -p n1ffi --lib || (popd & exit /b 1)
popd
if not exist "%N1%\target\release\n1ffi.lib" (echo n1ffi.lib wasn't made & exit /b 1)
echo.
echo Done: %N1%\target\release\n1ffi.lib
echo Next: tools\build_native.bat (it links n1 into the native renderer plugin), then rebuild tbc.
endlocal
exit /b 0
