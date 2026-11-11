@echo off
rem CPU-optimised builds of the game, each in its own folder (build\ is not touched).
rem   build_cpu.bat            -> build_cpu\Resistance3.exe       AVX2 everywhere + ThinLTO
rem   build_cpu.bat baseline   -> build_baseline\Resistance3.exe  no AVX (older CPUs; run.bat picks it there)
rem   build_cpu.bat pgo-gen    -> build_pgo_gen\Resistance3.exe   instrumented, for tools\pgo_train.bat
rem   build_cpu.bat pgo-use    -> build_pgo\Resistance3.exe       AVX2 + ThinLTO + PGO from pgo\r3.profdata
rem   build_cpu.bat tracy      -> build_tracy\Resistance3.exe     AVX2 + Tracy zones (connect the Tracy viewer)
rem Add "nopause" as the last argument when calling from a script.
rem The lifted PPU code (1.3 GB of C++, ~75 CPU-minutes) is not recompiled: its objects are
rem copied from build\ (same lift, same flags), except for baseline, which needs them without AVX2.
rem PORT_JOBS (default 4) compile jobs, at below-normal priority. PORT_LTO=0 skips ThinLTO.
rem PORT_PGO_PPU=1 / PORT_LTO_PPU=1 also instrument / LTO the lifted PPU code (compiles it: hours).
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
set "MODE=%~1"
if "%MODE%"=="" set "MODE=avx2"
if /i "%MODE%"=="nopause" (set "MODE=avx2" & set "NOPAUSE=1")
if /i "%~2"=="nopause" set "NOPAUSE=1"
if not defined PORT_JOBS set "PORT_JOBS=4"
set "LTO=ON"
if "%PORT_LTO%"=="0" set "LTO=OFF"
set "PPUX="
if "%PORT_LTO_PPU%"=="1" set "PPUX=%PPUX% -DPORT_LTO_PPU=ON"
if "%PORT_PGO_PPU%"=="1" set "PPUX=%PPUX% -DPORT_PGO_PPU=ON"
set "PREBUILT=1"
if "%PORT_LTO_PPU%"=="1" set "PREBUILT=0"
if "%PORT_PGO_PPU%"=="1" set "PREBUILT=0"
if /i "%MODE%"=="avx2"     (set "BDIR=build_cpu"      & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=%LTO% -DPORT_PGO=off")
if /i "%MODE%"=="baseline" (set "BDIR=build_baseline" & set "OPTS=-DPORT_ARCH=baseline -DPORT_LTO=%LTO% -DPORT_PGO=off" & set "PREBUILT=0")
if /i "%MODE%"=="tracy"    (set "BDIR=build_tracy"    & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=OFF -DPORT_PGO=off -DPORT_TRACY=ON")
if /i "%MODE%"=="pgo-gen"  (set "BDIR=build_pgo_gen"  & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=OFF -DPORT_PGO=gen")
if /i "%MODE%"=="pgo-use" (
  if not exist pgo\r3.profdata (echo pgo\r3.profdata not found - run tools\pgo_train.bat and tools\pgo_merge.bat first. & goto fail)
  set "BDIR=build_pgo" & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=%LTO% -DPORT_PGO=use -DPORT_PGO_PROFILE=%~dp0pgo\r3.profdata"
)
if not defined BDIR (echo Unknown mode "%MODE%": use avx2, baseline, tracy, pgo-gen or pgo-use. & goto fail)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (echo Visual Studio C++ build tools not found - run Setup.bat. & goto fail)
rem vcvars looks for vswhere on PATH (else it prints "'vswhere.exe' is not recognized")
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo Could not set up the Visual Studio build tools. & goto fail)
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
set "FFOPT=-DPS3RECOMP_FFMPEG_RUNTIME=ON -DPS3RECOMP_FFMPEG_INCLUDE_DIR=%~dp0ffmpeg\include"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist build_rpcs3\rpcs3_rsx.lib (echo build_rpcs3\rpcs3_rsx.lib missing - run build.bat once first. & goto fail)

set "PPUOPT=-DPORT_PPU_PREBUILT_OBJ_DIR="
if "%PREBUILT%"=="1" (
  rem snapshot of build\'s lifted PPU objects; refuses ones older than their source
  "%PS%" -NoProfile -Command "$src=(Get-ChildItem build\CMakeFiles\Resistance3.dir -Recurse -Filter ppu_recomp_000.cpp.obj | Select-Object -First 1).DirectoryName; if (-not $src) { exit 2 }; $dst='%BDIR%\ppu_obj'; New-Item -ItemType Directory -Force $dst | Out-Null; $n=0; foreach ($c in Get-ChildItem recompiled -Filter 'ppu_recomp_0*.cpp') { $o=Join-Path $src ($c.Name+'.obj'); if ((Test-Path $o) -and (Get-Item $o).LastWriteTime -gt $c.LastWriteTime) { $d=Join-Path $dst ($c.Name+'.obj'); if (-not (Test-Path $d) -or (Get-Item $d).LastWriteTime -ne (Get-Item $o).LastWriteTime) { Copy-Item $o $d -Force }; $n++ } else { Remove-Item (Join-Path $dst ($c.Name+'.obj')) -ErrorAction SilentlyContinue; Write-Host ('  '+$c.Name+': no up-to-date object in build\ - compiled here') } }; Write-Host ('  '+$n+' lifted PPU object(s) taken from build\')"
  if errorlevel 2 (echo   no lifted PPU objects in build\ - compiling them here ^(long^)) else set "PPUOPT=-DPORT_PPU_PREBUILT_OBJ_DIR=%~dp0%BDIR%\ppu_obj"
)

echo ============================================================
echo  Resistance 3 - CPU build "%MODE%" into %BDIR%\
echo ============================================================
cmake -S project -B %BDIR% -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=Resistance3 -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled" -DR2_RPCS3_RSX_BUILD="%~dp0build_rpcs3" -DPORT_SPU_AVX2=ON %OPTS% %PPUX% "%PPUOPT%" %FFOPT% > logs\configure_%BDIR%.log 2>&1
if errorlevel 1 (echo Configure FAILED - see logs\configure_%BDIR%.log & goto fail)
findstr /c:"CPU target" /c:"LTO:" /c:"PGO:" /c:"Lifted PPU" /c:"Tracy:" logs\configure_%BDIR%.log
echo Building (below-normal priority, %PORT_JOBS% jobs; log logs\build_%BDIR%.log)...
"%PS%" -NoProfile -Command "(Get-Process -Id $PID).PriorityClass = 'BelowNormal'; cmake --build %BDIR% -j %PORT_JOBS% 2>&1 | Tee-Object -FilePath logs\build_%BDIR%.log | Select-String -CaseSensitive -Pattern '^FAILED|error:|^\[[0-9]+/[0-9]+\] Linking' | ForEach-Object { $_.Line }; $c = $LASTEXITCODE; if (Select-String -Path logs\build_%BDIR%.log -Pattern '^FAILED: |^ninja: build stopped' -Quiet) { $c = 1 }; exit $c"
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build_%BDIR%.log & goto fail)

for %%F in (avcodec-61.dll avutil-59.dll swresample-5.dll) do if exist "tools\ffmpeg_bin\%%F" copy /Y "tools\ffmpeg_bin\%%F" %BDIR%\ >nul
for %%F in (r2_dlss.dll nvngx_dlss.dll amd_fidelityfx_vk.dll libxess.dll sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll NvLowLatencyVk.dll) do if exist "build\%%F" copy /Y "build\%%F" %BDIR%\ >nul
echo.
echo BUILD SUCCEEDED: %BDIR%\Resistance3.exe
if /i "%MODE%"=="pgo-gen" echo Next: tools\pgo_train.bat (plays the instrumented build), then tools\pgo_merge.bat, then build_cpu.bat pgo-use
if /i "%MODE%"=="tracy" echo Run it with: set PORT_GAME_EXE=build_tracy\Resistance3.exe ^& run.bat  - then connect tracy-profiler.exe
if /i "%MODE%"=="avx2" echo Run it with: set PORT_GAME_EXE=build_cpu\Resistance3.exe ^& run.bat
if not defined NOPAUSE pause
exit /b 0
:fail
if not defined NOPAUSE pause
exit /b 1
