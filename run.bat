@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
rem settings and controls are also kept in Saved Games\Resistance 3\config, so they survive a reinstall or update:
rem missing files are restored from there before the game starts and saved back after it closes. portable.txt turns this off.
set "CFG_USER=%USERPROFILE%\Saved Games\Resistance 3\config"
set "CFG_FILES=settings.ini settings.bat controls.txt mouselook.cfg"
if not exist portable.txt if exist "%CFG_USER%" robocopy "%CFG_USER%" . %CFG_FILES% /XO /XN /XC /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
if not exist build\Resistance3.exe (echo The game is not built yet - run Setup.bat first. & pause & exit /b 1)
rem ---- settings the port needs (Launcher.exe's choices come after, from settings.bat) ----
set "SPURS_PM_YIELD=0"
set "SPURS_IDLE_POLL=0"
set "FREEZE_RELAUNCH=900"
set "PS3_MAPPER_BASE=30000000"
rem skip the logo + "story so far" recap movie (plays during the first load)
set "SKIP_INTRO=1"
rem your copy of the game (Setup.bat put it here)
set "PS3_VFS_ROOT=%~dp0gamefiles"
rem Launcher.exe writes settings.bat (display mode, frame limit, vsync, filtering, keys)
if exist "%~dp0settings.bat" call "%~dp0settings.bat"
rem crash dumps are opt-in: set R2_DUMPS=1 to have Windows write minidumps to logs\dumps (newest 3 kept)
set "R2_WER=HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Resistance3.exe"
if "%R2_DUMPS%"=="1" (
  if not exist logs\dumps mkdir logs\dumps
  reg add "%R2_WER%" /v DumpFolder /t REG_EXPAND_SZ /d "%~dp0logs\dumps" /f >nul
  reg add "%R2_WER%" /v DumpType /t REG_DWORD /d 1 /f >nul
  reg add "%R2_WER%" /v DumpCount /t REG_DWORD /d 3 /f >nul
) else (
  reg delete "%R2_WER%" /f >nul 2>&1
)
rem shader cache backup (outside the game folder, so Setup/updates can't wipe it): restore missing files before, save new ones after
set "SHC_BACKUP=%LOCALAPPDATA%\Resistance3Port\shadercache"
if exist "%SHC_BACKUP%" robocopy "%SHC_BACKUP%" shadercache /E /XO /XN /XC /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
echo Running... (log: %~dp0logs\run.log). Close the game window to stop.
rem which build: PORT_GAME_EXE (e.g. build_cpu\Resistance3.exe) wins; on a CPU without AVX2 (the launcher
rem sets PORT_CPU_V3=0) the build_baseline one when it has been built (build_cpu.bat baseline)
if not defined PORT_GAME_EXE set "PORT_GAME_EXE=build\Resistance3.exe"
if "%PORT_CPU_V3%"=="0" if exist build_baseline\Resistance3.exe set "PORT_GAME_EXE=build_baseline\Resistance3.exe"
rem output goes to logs\run.log only (Launcher.exe starts this file hidden, so there is no console window)
"%PORT_GAME_EXE%" "%~dp0game\EBOOT.elf" > logs\run.log 2>&1
rem the game can hand over to a new process (level loads, freeze restart): wait until none is left
:waitgame
for %%E in (Resistance3.exe) do tasklist /FI "IMAGENAME eq %%E" 2>nul | find /i "%%E" >nul && (ping -n 3 127.0.0.1 >nul & goto waitgame)
if exist shadercache robocopy shadercache "%SHC_BACKUP%" /E /XO /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
if not exist portable.txt robocopy . "%CFG_USER%" %CFG_FILES% /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
