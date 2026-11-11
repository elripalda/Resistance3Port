@echo off
rem QA copy of the RPCS3 RSX core (R2_QA_CAPTURE frame capture) -> build_qa_rpcs3\rpcs3_rsx.lib. Does not touch build_rpcs3.
cd /d "%~dp0\..\.."
if not exist logs\qa mkdir logs\qa
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
if not exist build_qa_rpcs3\build.ninja cmake -S ps3recomp\third_party\rpcs3 -B build_qa_rpcs3 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRPCS3_RSX_BUILD_LINKTEST=OFF -DENABLE_HLSL=ON > logs\qa\configure_rpcs3.log 2>&1
if errorlevel 1 (echo CONFIGURE FAILED & exit /b 1)
cmake --build build_qa_rpcs3 -j 6 --target rpcs3_rsx > logs\qa\build_rpcs3.log 2>&1
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo RPCS3 QA BUILD OK

