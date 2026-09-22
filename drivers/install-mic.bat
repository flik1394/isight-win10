@echo off
REM =====================================================================
REM  install-mic.bat  --  install the iSight virtual microphone driver
REM  (isightmic.sys, a PortCls WaveCyclic capture miniport)
REM
REM  MUST be run as Administrator (right-click -> Run as administrator).
REM
REM  One-time prerequisite (needs a reboot, do it once):
REM      bcdedit /set testsigning on
REM  then reboot.  After that, test-signed drivers load and you can run
REM  this script any number of times without rebooting.
REM =====================================================================
setlocal

echo [iSight Mic] installing "iSight Microphone (FireWire)" ...
echo [iSight Mic] step 1/3: import the test certificate
certutil -addstore Root           iSightMicTest.cer >nul 2>&1
certutil -addstore TrustedPublisher iSightMicTest.cer >nul 2>&1
if errorlevel 1 (
  echo [iSight Mic] could not import the certificate -- are you Administrator?
  exit /b 1
)

echo [iSight Mic] step 2/3: create the device node and install the driver
isight-micdev.exe install isightmic.inf
if errorlevel 1 (
  echo [iSight Mic] install failed.
  echo [iSight Mic] Did you boot once with test-signing ON?
  echo [iSight Mic]   bcdedit /set testsigning on
  echo [iSight Mic]   (then reboot)
  exit /b 1
)

echo [iSight Mic] step 3/3: done.
echo [iSight Mic] "iSight Microphone (FireWire)" should now appear under
echo [iSight Mic] Control Panel - Sound - Recording.
echo [iSight Mic] Start the feeder to push audio into it, e.g.:
echo [iSight Mic]   isight-micsvc.exe sample.wav
endlocal
exit /b 0
