@echo off
REM =====================================================================
REM  sweep-mic.bat  --  V46 empirical matrix runner for the iSight virtual
REM  microphone (isightmic.sys).  Each configuration is reloaded by
REM  switch-mic.bat (no recompile, no reboot), and one miccheck.txt is
REM  collected per combo, plus a SWEEP_SUMMARY.txt with the decisive lines.
REM
REM  A configuration is 5 bits, in this order:
REM      Intersect  Ranges  Jack  Topo  Mono
REM  (what each bit does is documented in switch-mic.bat's header.)
REM
REM  Decisive signal in miccheck:
REM    [4] GetMixFormat failed (0x88890008)  -> still BROKEN (symptom itself)
REM        (when that happens, ProbeInitFormats also prints [4b] lines that
REM         show which hard-coded formats the audio engine WOULD accept)
REM    [4] endpoint format : ...             -> GetMixFormat succeeded
REM    [4] captured N samples                -> endpoint fully works (FIXED)
REM
REM  Sanity signal, checked FIRST when reading a combo:
REM    CFGCODE: expect N / V46 config code N (...)
REM    -> the driver's own report of which switches it actually loaded.
REM       If "expect" and the reported number differ, the registry never
REM       reached the driver (permissions / wrong native view / stale image)
REM       and this combo's verdict is meaningless -- do NOT conclude "this
REM       combination does not help".  CfgCode = Intersect + 2*Ranges +
REM       4*Jack + 8*Topo + 16*Mono.
REM
REM  MUST be run as Administrator, from the unzipped isight-vmic package dir.
REM  Output:  sweep\sweep_<label>.txt  (full miccheck, one per combo)
REM           sweep\SWEEP_SUMMARY.txt  (decisive lines + [4b] detail)
REM =====================================================================
setlocal
set "SWEEP_DIR=sweep"
if not exist "%SWEEP_DIR%" mkdir "%SWEEP_DIR%"
set "SUM=%SWEEP_DIR%\SWEEP_SUMMARY.txt"
echo V46 sweep  %DATE% %TIME% > "%SUM%"
echo combos: baseline, ranges_dual, mono, no_jack, intersect_echo, topo_full, ranges_mono, ranges_mono_nojack, full_mono >> "%SUM%"
echo. >> "%SUM%"

REM ---- trust THIS package's test certificate before touching the device --
REM Every CI build ships a fresh throwaway cert (CN=iSightMic Test), and
REM switch-mic.bat only remove/installs the device node -- it never imports a
REM cert.  Run straight into the sweep on a freshly downloaded package and the
REM new .sys cannot load, so the sweep would quietly measure whatever older
REM trusted driver is still installed.  Re-importing is harmless and idempotent.
if exist iSightMicTest.cer (
  echo [sweep] importing this package's test certificate ...
  certutil -addstore Root             iSightMicTest.cer >nul 2>&1
  certutil -addstore TrustedPublisher iSightMicTest.cer >nul 2>&1
) else (
  echo [sweep] WARNING: iSightMicTest.cer not found -- run this from the unzipped package dir.
)

REM ---- curated matrix: call :run <label> <I> <R> <J> <T> <M> ----
call :run baseline_V45       0 0 1 0 0
call :run ranges_dual        0 1 1 0 0
call :run mono               0 0 1 0 1
call :run no_jack            0 0 0 0 0
call :run intersect_echo     1 0 1 0 0
call :run topo_full          0 0 1 1 0
call :run ranges_mono        0 1 1 0 1
call :run ranges_mono_nojack 0 1 0 0 1
call :run full_mono          0 1 1 1 1

REM ---- leave the machine in the DEFAULT (V45-parity) config ----
echo [sweep] restoring DEFAULT config (0 0 1 0 0) ...
call switch-mic.bat 0 0 1 0 0

echo. >> "%SUM%"
echo V46 sweep complete.  Per-combo reports: %SWEEP_DIR%\sweep_*.txt >> "%SUM%"
echo Machine left in DEFAULT config (0 0 1 0 0) -- re-apply the winning >> "%SUM%"
echo combo with:  switch-mic.bat I R J T M >> "%SUM%"

echo.
echo ================================================================
echo  SWEEP COMPLETE.  Send the whole "%SWEEP_DIR%" folder to flik.
echo  (SWEEP_SUMMARY.txt has the decisive lines; sweep_*.txt are full.)
echo  Machine left in DEFAULT config (0 0 1 0 0).
echo ================================================================
endlocal
goto :eof

REM ---- subroutine: reload one combo, capture miccheck, extract verdict ----
:run
set "LBL=%1"
set "I=%2"
set "R=%3"
set "J=%4"
set "T=%5"
set "M=%6"
set "L_DEAD="
set "L_OURS="
set "L_MIX="
set "L_CAP="
set "L_CFG="
echo.
echo ================================================================
echo [sweep] combo "%LBL%"   I=%I% R=%R% J=%J% T=%T% M=%M%
echo ================================================================

call switch-mic.bat %I% %R% %J% %T% %M%
if not exist miccheck.txt (
  echo [sweep] ERROR: miccheck.txt missing after "%LBL%" >&2
  echo ---- combo %LBL% (I=%I% R=%R% J=%J% T=%T% M=%M%) >> "%SUM%"
  echo   DEVICE-DEAD: miccheck.txt not produced (re-run this combo alone) >> "%SUM%"
  echo. >> "%SUM%"
  goto :eof
)
copy /y miccheck.txt "%SWEEP_DIR%\sweep_%LBL%.txt" >nul

for /f "delims=" %%a in ('findstr /i /l "NOT reachable" miccheck.txt') do set "L_DEAD=%%a"
for /f "delims=" %%a in ('findstr /i /l "OURS" miccheck.txt') do set "L_OURS=%%a"
for /f "delims=" %%a in ('findstr /l "GetMixFormat failed" miccheck.txt') do set "L_MIX=%%a"
if not defined L_MIX (
  for /f "delims=" %%a in ('findstr /l "endpoint format :" miccheck.txt') do set "L_MIX=%%a"
)
for /f "delims=" %%a in ('findstr /l "[4] captured" miccheck.txt') do set "L_CAP=%%a"
for /f "delims=" %%a in ('findstr /l "config code" miccheck.txt') do set "L_CFG=%%a"
REM CfgCode = Intersect + 2*Ranges + 4*Jack + 8*Topo + 16*Mono
set /a "EXPECT=%I% + 2*%R% + 4*%J% + 8*%T% + 16*%M%"

echo ---- combo %LBL% (I=%I% R=%R% J=%J% T=%T% M=%M%) >> "%SUM%"
if defined L_CFG (
  echo   CFGCODE: expect %EXPECT% / %L_CFG% >> "%SUM%"
) else (
  echo   CFGCODE: expect %EXPECT% / (driver did NOT report -- combo inconclusive) >> "%SUM%"
)
if defined L_DEAD (echo   CONTROL: device NOT reachable >> "%SUM%") else (echo   CONTROL: ok >> "%SUM%")
if defined L_OURS (echo   ENDPOINT: %L_OURS% >> "%SUM%") else (echo   ENDPOINT: OURS not found >> "%SUM%")
echo   MIX: %L_MIX% >> "%SUM%"
if defined L_CAP (echo   CAP: %L_CAP% >> "%SUM%") else (echo   CAP: (none) >> "%SUM%")
for /f "delims=" %%a in ('findstr /l "[4b]" miccheck.txt') do echo   [4b] %%a >> "%SUM%"
echo. >> "%SUM%"
goto :eof
