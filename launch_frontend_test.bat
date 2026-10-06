@echo off
setlocal
rem Experiment: boot into the game's own main menu (frontend) offline instead of straight into the PU,
rem to see whether it loads and whether its Character Customization button can work.
rem Any SC_OFFLINE_BOOT_MAP other than PU / PU_All leaves the frontend request alone.
set "SC_OFFLINE_BOOT_MAP=Frontend"
call "%~dp0launch_offline.bat"
