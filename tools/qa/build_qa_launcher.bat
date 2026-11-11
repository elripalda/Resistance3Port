@echo off
rem Builds build_qa\Launcher_qa.exe from project\launcher.cpp (with launcher_crash.inc). Launcher.exe is not touched.
rem To try it: copy build_qa\Launcher_qa.exe next to Launcher.exe (it also finds the game folder from build_qa\).
setlocal
cd /d "%~dp0\..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not exist build_qa mkdir build_qa
if not exist logs\qa mkdir logs\qa
set "LRES="
rc /nologo /i project /fo build_qa\launcher.res project\launcher.rc > logs\qa\build_launcher_rc.log 2>&1 && set "LRES=build_qa\launcher.res"
clang-cl /nologo /O2 /EHsc /W0 project\launcher.cpp /Fo:build_qa\launcher.obj /Fe:build_qa\Launcher_qa.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED %LRES% user32.lib gdi32.lib shell32.lib gdiplus.lib dwmapi.lib > logs\qa\build_launcher.log 2>&1
if errorlevel 1 (echo Launcher_qa build failed - see logs\qa\build_launcher.log & exit /b 1)
echo built build_qa\Launcher_qa.exe
