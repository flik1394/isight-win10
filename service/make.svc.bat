@echo off
REM Build isight-micsvc.exe (user-mode feeder, M1 = WAV loopback).
REM Uses the plain MSVC toolchain (no WDK needed for user mode).
setlocal
cl /nologo /O2 /W3 /EHsc /MD ^
   /I"..\drivers\isightmic" ^
   feed.cpp /Fe:isight-micsvc.exe
if errorlevel 1 exit /b 1
dir isight-micsvc.exe
endlocal
