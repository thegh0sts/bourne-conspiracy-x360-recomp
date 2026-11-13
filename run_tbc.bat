@echo off
rem Launches the recompiled game with graphics enabled and logging to tbc_log.txt.
rem Edit GAME_DIR if you move the extracted game files.
set "GAME_DIR=%~dp0..\BOURNE-CONSPIRACY-X360-PORT\tbc-game"
"%~dp0out\build\win-amd64-relwithdebinfo\tbc.exe" --game_data_root "%GAME_DIR%" --gpu_plugin native --log_level debug --log_file "%~dp0tbc_log.txt"
