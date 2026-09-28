@echo off
REM =====================================================================
REM  switch-mic.bat  --  reload the iSight virtual microphone with a
REM  different V46 runtime configuration, WITHOUT recompiling and WITHOUT
REM  rebooting.  The .sys binary is unchanged; only the registry signature
REM  that DriverEntry reads changes, so cycling the device is enough.
REM
REM  Usage:  switch-mic.bat [Intersect] [Ranges] [Jack] [Topo] [Mono]
REM    each argument is 0 or 1; omitted args keep the V46 default (0 0 1 0 0):
REM      Intersect  0 = hand intersection to PortCls (STATUS_NOT_IMPLEMENTED)
REM                 1 = echo the client's proposed format (original V40)
REM      Ranges     0 = advertise a single WAVEFORMATEX range (V43/V45)
REM                 1 = also advertise a WAVEFORMATEXTENSIBLE range (V40-V42)
REM      Jack       0 = no topology automation table (pre-V45)
REM                 1 = publish KSPROPERTY_JACK_DESCRIPTION (V45)
REM      Topo       0 = single MIC node (current)
REM                 1 = MIC + VOLUME node (the "full" reference topology)
REM      Mono       0 = stereo channel config + stereo jack mapping
REM                 1 = mono channel config + mono jack mapping
REM
REM  MUST be run as Administrator, from the unzipped isight-vmic package dir.
REM =====================================================================
setlocal

REM ---- elevation self-check + self-relaunch --------------------------------
REM Everything this script does (reg add HKLM, net stop Audiosrv, remove and
REM re-install the device node) needs an elevated token.  Run non-elevated it
REM used to limp on: the first "reg add" returns access-denied, :regfail fires
REM and the script exits before miccheck ever runs -- so the caller sees a
REM missing report and reads it as a driver fault.  Re-launch ourselves with
REM the UAC prompt instead, so double-clicking this file just works.
net session >nul 2>&1
if not errorlevel 1 goto :elevated
echo [switch-mic] not elevated -- asking for Administrator (click YES) ...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%1 %2 %3 %4 %5' -WorkingDirectory '%~dp0' -Verb RunAs"
exit /b 1
:elevated

if "%~1"=="" (set I=0) else (set I=%~1)
if "%~2"=="" (set R=0) else (set R=%~2)
if "%~3"=="" (set J=1) else (set J=%~3)
if "%~4"=="" (set T=0) else (set T=%~4)
if "%~5"=="" (set M=0) else (set M=%~5)

REM NOTE: every variable reference here must be CLOSED (%I%, not %I).  An
REM unclosed %I makes cmd treat "%I Ranges=%" as one variable name (it happily
REM includes the space), which is undefined and expands to nothing -- the
REM echoed line then loses whole words and, worse, "reg add /d" receives no
REM value and silently swallows "64" from the trailing /reg:64.
echo [switch-mic] V46 config -^> Intersect=%I% Ranges=%R% Jack=%J% Topo=%T% Mono=%M%
echo [switch-mic] writing HKLM\SOFTWARE\iSightMic ...
REM /reg:64 is load-bearing: a 32-bit reg.exe silently redirects HKLM\SOFTWARE
REM to Wow6432Node, while the driver's ZwOpenKey reads the NATIVE view --
REM the switches would then never be seen and every combo would silently run
REM with the compiled-in defaults.  Force the 64-bit (native) view.
reg add "HKLM\SOFTWARE\iSightMic" /v Intersect /t REG_DWORD /d %I% /f /reg:64 >nul 2>&1 || goto :regfail
reg add "HKLM\SOFTWARE\iSightMic" /v Ranges    /t REG_DWORD /d %R% /f /reg:64 >nul 2>&1 || goto :regfail
reg add "HKLM\SOFTWARE\iSightMic" /v Jack      /t REG_DWORD /d %J% /f /reg:64 >nul 2>&1 || goto :regfail
reg add "HKLM\SOFTWARE\iSightMic" /v Topo      /t REG_DWORD /d %T% /f /reg:64 >nul 2>&1 || goto :regfail
reg add "HKLM\SOFTWARE\iSightMic" /v Mono      /t REG_DWORD /d %M% /f /reg:64 >nul 2>&1 || goto :regfail

REM Read back and show what is actually stored, so a redirect/permission
REM problem is visible here instead of masquerading as "this combo does not
REM help" in the sweep results.
echo [switch-mic] registry now reads:
reg query "HKLM\SOFTWARE\iSightMic" /reg:64
goto :wrotereg

:regfail
echo [switch-mic] could not write the registry -- are you Administrator?
exit /b 1

:wrotereg

echo [switch-mic] stopping audio services + killing audiodg.exe ...
net stop AudioEndpointBuilder /y >nul 2>&1
net stop Audiosrv /y >nul 2>&1
taskkill /f /im audiodg.exe >nul 2>&1

echo [switch-mic] removing the device node (unloads the .sys) ...
isight-micdev.exe remove
echo [switch-mic] re-installing the device node (reloads the .sys, reads registry) ...
isight-micdev.exe install isightmic.inf
if errorlevel 1 (
  echo [switch-mic] device install reported an error; still restarting services.
)

echo [switch-mic] starting audio services ...
net start Audiosrv >nul 2>&1
net start AudioEndpointBuilder >nul 2>&1
timeout /t 3 /nobreak >nul

echo [switch-mic] running miccheck to report the live config + endpoint ...
isight-miccheck.exe 2
echo [switch-mic] done. miccheck.txt has the full report.
endlocal
exit /b 0
