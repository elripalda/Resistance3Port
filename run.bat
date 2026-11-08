@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
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
build\Resistance3.exe "%~dp0game\EBOOT.elf" 2>&1 | "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "$input | Tee-Object -FilePath logs\run.log"
if exist shadercache robocopy shadercache "%SHC_BACKUP%" /E /XO /R:0 /W:0 /NJH /NJS /NFL /NDL /NP >nul
