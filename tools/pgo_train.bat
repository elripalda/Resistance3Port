@echo off
rem PGO step 2 of 4: play the instrumented build (build_cpu.bat pgo-gen) to record a profile.
rem Play normally for 10-20 minutes and cover what matters for speed: the heaviest gameplay
rem (large fights, the helicopter / vehicle sections, busy outdoor levels), a level load, a
rem checkpoint reload, cutscenes and the menus. The instrumented build runs slower than normal
rem (roughly 10-30%); that does not matter for the profile. Counters are written every
rem PORT_PGO_DUMP_S seconds (default 30) and when the game exits, to pgo\raw\r3-<pid>.profraw
rem (a level hand-off to a new process writes a second file; all of them are merged).
rem Several sessions are fine: run this again and everything in pgo\raw is merged.
rem Then: tools\pgo_merge.bat, then build_cpu.bat pgo-use.
setlocal
cd /d "%~dp0.."
if not exist build_pgo_gen\Resistance3.exe (echo build_pgo_gen\Resistance3.exe not found - run build_cpu.bat pgo-gen first. & pause & exit /b 1)
if not exist pgo\raw mkdir pgo\raw
set "LLVM_PROFILE_FILE=%CD%\pgo\raw\r3-%%p.profraw"
set "PORT_GAME_EXE=build_pgo_gen\Resistance3.exe"
if not defined PORT_PGO_DUMP_S set "PORT_PGO_DUMP_S=30"
echo Recording a PGO profile with build_pgo_gen\Resistance3.exe -> pgo\raw\
call run.bat
echo.
dir /b pgo\raw\*.profraw 2>nul || echo No .profraw written - check logs\run.log for the "[pgo]" line.
echo Next: tools\pgo_merge.bat
pause
