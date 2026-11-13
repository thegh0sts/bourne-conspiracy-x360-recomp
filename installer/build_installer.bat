@echo off
rem Builds BourneConspiracy.exe (launcher + installer) from tbc_launcher.py and the project files.
rem Run from this folder (installer\) in any terminal with Python on PATH.
rem
rem   build_installer.bat [SDK folder]
rem
rem The SDK folder is a ReXGlue SDK install made by ..\tools\build_native.bat (FSR/CAS,
rem performance patches and Skullburglar's native Vulkan renderer, the default renderer;
rem ..\tools\build_sdk.bat alone leaves the native renderer out and players get Xenos).
rem It's zipped into the exe, so the installer needs no SDK download.
rem Default: C:\rexglue-src\rexglue-sdk\out\install\win-amd64 if it exists. Without one, the
rem installer downloads the stock SDK from GitHub instead.
setlocal
cd /d "%~dp0"

set "SDK_FOLDER=%~1"
if "%SDK_FOLDER%"=="" if exist "C:\rexglue-src\rexglue-sdk\out\install\win-amd64\lib\cmake\rexglue" set "SDK_FOLDER=C:\rexglue-src\rexglue-sdk\out\install\win-amd64"
rem Name of the bundled SDK: bump the suffix whenever the SDK build changes, so existing
rem installs replace their SDK on the next install or repair.
set "SDK_BUNDLE=rexglue-sdk-0.10.1-tbc3-win-amd64.zip"

echo Collecting project files into payload\project ...
if exist payload rmdir /s /q payload
mkdir payload\project\src payload\project\generated
for %%F in (tbc_manifest.toml default_functions.toml default_functions_scan.toml hooks.toml CMakeLists.txt CMakePresets.json) do copy /y "..\%%F" payload\project\ >nul || goto :missing
copy /y ..\src\*.* payload\project\src\ >nul || goto :missing
copy /y ..\generated\rexglue.cmake payload\project\generated\ >nul || goto :missing
copy /y ..\tools\tbc_config.py tbc_config.py >nul || goto :missing
mkdir payload\licenses
copy /y ..\licenses\*.txt payload\licenses\ >nul || goto :missing
copy /y ..\LICENSE payload\licenses\bourne-conspiracy-x360-recomp-LICENSE.txt >nul 2>&1

if "%SDK_FOLDER%"=="" (
  echo No SDK folder: the installer will download the stock ReXGlue SDK.
) else (
  if not exist "%SDK_FOLDER%\lib\cmake\rexglue" (
    echo "%SDK_FOLDER%" doesn't look like a ReXGlue SDK install.
    exit /b 1
  )
  echo Packing the SDK from %SDK_FOLDER% ...
  mkdir payload\sdk
  python -c "import os,sys,zipfile; src=sys.argv[1]; z=zipfile.ZipFile(sys.argv[2],'w',zipfile.ZIP_DEFLATED); [z.write(os.path.join(d,f), os.path.relpath(os.path.join(d,f),src)) for d,_,fs in os.walk(src) for f in fs]; z.close()" "%SDK_FOLDER%" "payload\sdk\%SDK_BUNDLE%" || goto :fail
)

echo Installing PyInstaller (first time only) ...
python -m pip install --upgrade pyinstaller || goto :fail

echo Building the installer ...
python -m PyInstaller --noconfirm --onefile --windowed --name BourneConspiracy --icon icon.ico --add-data "payload;payload" --add-data "icon.ico;." tbc_launcher.py || goto :fail

echo.
echo Done: %~dp0dist\BourneConspiracy.exe
exit /b 0

:missing
echo A project file is missing. Run this from the installer folder inside the project.
exit /b 1
:fail
echo Build failed, see the messages above.
exit /b 1
