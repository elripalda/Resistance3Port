@echo off
rem PGO step 3 of 4: merge every recorded profile in pgo\raw into pgo\r3.profdata.
rem Then build the optimised exe with: build_cpu.bat pgo-use  (-> build_pgo\Resistance3.exe)
rem A profile belongs to the source it was recorded with; after larger code changes record a
rem new one (build_cpu.bat pgo-gen + tools\pgo_train.bat). Small drifts only lose the profile
rem for the changed functions.
setlocal
cd /d "%~dp0.."
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not exist pgo\raw\*.profraw (echo No profiles in pgo\raw - run tools\pgo_train.bat first. & pause & exit /b 1)
llvm-profdata merge -o pgo\r3.profdata pgo\raw\*.profraw
if errorlevel 1 (echo Merge FAILED. & pause & exit /b 1)
llvm-profdata show pgo\r3.profdata | findstr /b /c:"Total functions" /c:"Maximum function count"
echo Wrote pgo\r3.profdata. Next: build_cpu.bat pgo-use
pause
