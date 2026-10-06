@echo off
rem Builds testbot.exe next to this script (needs the Visual Studio Build Tools, like the mod).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++20 /W3 /DUNICODE /D_UNICODE /I"%~dp0..\..\src" testbot.cpp "%~dp0..\..\src\net.cpp" /Fe:testbot.exe advapi32.lib
del /q *.obj 2>nul
