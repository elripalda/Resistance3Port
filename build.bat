@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist recompiled\ppu_recomp.h (echo The game code has not been generated yet - run Setup.bat first. & goto fail)
rem A game window left open locks the .exe and the link fails -- close any that are still running.
taskkill /IM Resistance3.exe /F >nul 2>&1 && echo Closed a Resistance3.exe that was still running.
echo ============================================================
echo  Resistance 3 - native PC build
echo  Builds one program (EBOOT.BIN; game.self is the same file).
echo  The first build takes a long time. Leave it running.
echo ============================================================
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (echo Visual Studio C++ build tools not found - run Setup.bat. & goto fail)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo Could not set up the Visual Studio build tools. & goto fail)
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
set "FFOPT=-DPS3RECOMP_FFMPEG_RUNTIME=ON -DPS3RECOMP_FFMPEG_INCLUDE_DIR=%~dp0ffmpeg\include"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not defined PORT_JOBS set "PORT_JOBS=4"

echo Building the RPCS3 RSX core...
cmake -S ps3recomp\third_party\rpcs3 -B build_rpcs3 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRPCS3_RSX_BUILD_LINKTEST=OFF -DENABLE_HLSL=ON > logs\configure_rpcs3.log 2>&1
if errorlevel 1 (echo RPCS3 RSX configure FAILED - see logs\configure_rpcs3.log & goto fail)
cmake --build build_rpcs3 -j 8 --target rpcs3_rsx > logs\build_rpcs3.log 2>&1
if errorlevel 1 (echo RPCS3 RSX build FAILED - see logs\build_rpcs3.log & goto fail)

(
  echo Configuring...
  cmake -S project -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=Resistance3 -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled" -DPORT_SPU_AVX2=ON %FFOPT% > logs\configure.log 2>&1
  if errorlevel 1 (echo Configure FAILED - see logs\configure.log & type logs\configure.log & goto fail)
)
echo Building the game...
"%PS%" -NoProfile -Command "cmake --build build -j %PORT_JOBS% 2>&1 | Tee-Object -FilePath logs\build.log; $c = $LASTEXITCODE; if (Select-String -Path logs\build.log -Pattern '^FAILED: |^ninja: build stopped' -Quiet) { $c = 1 }; exit $c"
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build.log & goto fail)


echo Building the launcher...
set "LRES="
rc /nologo /i project /fo build\launcher.res project\launcher.rc > logs\build_launcher_rc.log 2>&1 && set "LRES=build\launcher.res"
clang-cl /nologo /O2 /EHsc /W0 project\launcher.cpp /Fo:build\launcher.obj /Fe:Launcher.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED %LRES% user32.lib gdi32.lib shell32.lib gdiplus.lib dwmapi.lib > logs\build_launcher.log 2>&1
if errorlevel 1 (echo Launcher build failed - see logs\build_launcher.log) else (echo Launcher.exe built)

rem FFmpeg (video decode) is loaded at run time: Setup.bat puts the DLLs in tools\ffmpeg_bin
for %%D in (build) do for %%F in (avcodec-61.dll avutil-59.dll swresample-5.dll) do if exist "tools\ffmpeg_bin\%%F" copy /Y "tools\ffmpeg_bin\%%F" %%D\ >nul
rem NVIDIA DLSS (the "NVIDIA DLSS" option): r2_dlss.dll is the only binary linking the NGX SDK, which setup.ps1
rem downloads into tools\dlss (NVIDIA RTX SDK licence in tools\dlss\LICENSE.txt); nvngx_dlss.dll is NVIDIA's redistributable.
if exist "tools\dlss\lib\nvsdk_ngx_s.lib" (
  clang-cl /nologo /O2 /EHsc /MT /LD /W0 /Itools\dlss\include /Ips3recomp\third_party\rpcs3\3rdparty\Vulkan-Headers\include project\r2_dlss.cpp /Fo:build\r2_dlss.obj /Fe:build\r2_dlss.dll /link tools\dlss\lib\nvsdk_ngx_s.lib advapi32.lib user32.lib > logs\build_r2_dlss.log 2>&1
  if errorlevel 1 (echo Vulkan DLSS bridge build failed - see logs\build_r2_dlss.log) else (
    echo r2_dlss.dll built
    for %%D in (build) do copy /Y "tools\dlss\nvngx_dlss.dll" %%D\ >nul
  )
)
rem FSR 3.1 for the Vulkan renderer (AMD FidelityFX SDK 1.1.x, MIT licence in tools\ffx_vk\LICENSE.txt)
for %%D in (build) do if exist "tools\ffx_vk\amd_fidelityfx_vk.dll" copy /Y "tools\ffx_vk\amd_fidelityfx_vk.dll" %%D\ >nul
echo.
echo BUILD SUCCEEDED: build\Resistance3.exe
echo Start the game with Launcher.exe
if /i not "%~1"=="nopause" pause
exit /b 0
:fail
if /i not "%~1"=="nopause" pause
exit /b 1
