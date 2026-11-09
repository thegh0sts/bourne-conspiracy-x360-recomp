@echo off
rem Builds BourneConspiracy.exe (launcher + installer) from tbc_launcher.py and the project files.
rem Run from this folder (installer\) in any terminal with Python on PATH.
setlocal
cd /d "%~dp0"

echo Collecting project files into payload\project ...
if exist payload rmdir /s /q payload
mkdir payload\project\src payload\project\generated
for %%F in (tbc_manifest.toml default_functions.toml default_functions_scan.toml CMakeLists.txt CMakePresets.json) do copy /y "..\%%F" payload\project\ >nul || goto :missing
copy /y ..\src\*.* payload\project\src\ >nul || goto :missing
copy /y ..\generated\rexglue.cmake payload\project\generated\ >nul || goto :missing
copy /y ..\tools\tbc_config.py tbc_config.py >nul || goto :missing

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
