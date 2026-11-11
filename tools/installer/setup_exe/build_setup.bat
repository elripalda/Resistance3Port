@echo off
rem Builds Setup.exe (the one-click setup) into the port folder. Needs the Visual Studio C++ build tools;
rem uses clang-cl when LLVM is installed, else cl. One small file: no game code is compiled here.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
rem vcvars looks for vswhere on PATH
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not defined VSDIR (echo Visual Studio C++ build tools not found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CC=cl"
if exist "C:\Program Files\LLVM\bin\clang-cl.exe" set "CC=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist obj mkdir obj
rc /nologo /fo obj\setup.res setup.rc || exit /b 1
"%CC%" /nologo /O1 /MT /EHsc /std:c++17 /W3 /DNDEBUG setup.cpp /Fo:obj\setup.obj /Fe:..\..\..\Setup.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO obj\setup.res || exit /b 1
echo Built %~dp0..\..\..\Setup.exe
