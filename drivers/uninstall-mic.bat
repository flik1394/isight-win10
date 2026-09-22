@echo off
REM =====================================================================
REM  uninstall-mic.bat  --  remove the iSight virtual microphone driver
REM  (rollback / uninstall).  Run as Administrator.
REM =====================================================================
setlocal

echo [iSight Mic] removing the device node ...
isight-micdev.exe remove

echo [iSight Mic] deleting the driver from the driver store ...
pnputil /delete-driver isightmic.inf /uninstall >nul 2>&1

echo [iSight Mic] driver removed.
echo [iSight Mic] To re-enable normal driver signing enforcement (optional):
echo [iSight Mic]   bcdedit /set testsigning off
echo [iSight Mic]   (then reboot)
endlocal
exit /b 0
