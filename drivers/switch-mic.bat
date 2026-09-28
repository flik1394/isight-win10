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
if "%~1"=="" (set I=0) else (set I=%~1)
if "%~2"=="" (set R=0) else (set R=%~2)
if "%~3"=="" (set J=1) else (set J=%~3)
if "%~4"=="" (set T=0) else (set T=%~4)
if "%~5"=="" (set M=0) else (set M=%~5)

echo [switch-mic] V46 config -^> Intersect=%I Ranges=%R Jack=%J Topo=%T Mono=%M
echo [switch-mic] writing HKLM\SOFTWARE\iSightMic ...
reg add "HKLM\SOFTWARE\iSightMic" /v Intersect /t REG_DWORD /d %I /f >nul 2>&1
reg add "HKLM\SOFTWARE\iSightMic" /v Ranges    /t REG_DWORD /d %R /f >nul 2>&1
reg add "HKLM\SOFTWARE\iSightMic" /v Jack      /t REG_DWORD /d %J /f >nul 2>&1
reg add "HKLM\SOFTWARE\iSightMic" /v Topo      /t REG_DWORD /d %T /f >nul 2>&1
reg add "HKLM\SOFTWARE\iSightMic" /v Mono      /t REG_DWORD /d %M /f >nul 2>&1
if errorlevel 1 (
  echo [switch-mic] could not write the registry -- are you Administrator?
  exit /b 1
)

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
