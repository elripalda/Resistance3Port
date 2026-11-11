@echo off
rem Relinks build_qa\Resistance3.exe from the live build\ objects + build_qa_rpcs3\rpcs3_rsx.lib
rem (run build_qa_rpcs3.bat first). build\ is not written.
setlocal
cd /d "%~dp0\..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_qa_exe.ps1" %*
exit /b %errorlevel%
