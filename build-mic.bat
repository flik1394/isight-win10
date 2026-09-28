@echo off
REM =====================================================================
REM  build-mic.bat -- LOCAL one-shot build of isightmic.sys (+ user-mode
REM  tools + test-signed micpkg).  Equivalent to .github/workflows/vmic.yml
REM  but running on YOUR machine: no GitHub round-trip, no waiting, and
REM  every compiler error is printed right here in this window.
REM
REM  One-time prerequisites:
REM    * Visual Studio 2022 Build Tools with the "Desktop development with
REM      C++" workload  (gives cl.exe / link.exe / vswhere.exe)
REM    * Windows Driver Kit (WDK) 10.0.26100.0 or newer, WITH the kernel-mode
REM      headers  (gives km\ntddk.h, portcls.lib, inf2cat.exe, signtool.exe)
REM
REM  Run from the repo root in a normal cmd window:
REM      build-mic.bat
REM
REM  Output:  isightmic.sys, isight-micsvc.exe, isight-micdev.exe,
REM           isight-miccheck.exe, micpkg\  (inf + sys + signed cat)
REM
REM  NOTE on the signing cert: this script reuses ONE persistent local cert
REM  ("CN=iSightMic Test Local") so you only have to trust
REM  micpkg\iSightMicLocal.cer on the target machine ONCE -- unlike the CI
REM  package, whose throwaway cert changes every build and silently falls
REM  back to an older trusted driver if you forget to re-trust it.
REM =====================================================================
setlocal

REM ---- 1. Visual Studio (cl.exe / link.exe) ---------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [build-mic] vswhere.exe not found.
  echo             Install Visual Studio 2022 Build Tools with the
  echo             "Desktop development with C++" workload first.
  exit /b 1
)
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo [build-mic] no Visual Studio with the C++ x64 toolset found.
  exit /b 1
)
echo [build-mic] Visual Studio : %VSROOT%
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (
  echo [build-mic] vcvarsall x64 failed.
  exit /b 1
)

REM ---- 2. WDK (kernel headers + libs) ----------------------------------------
set "KITROOT=%ProgramFiles(x86)%\Windows Kits\10"
set "KITVER="
if exist "%KITROOT%\Include\10.0.26100.0\km\ntddk.h" set "KITVER=10.0.26100.0"
if not defined KITVER (
  for /f "delims=" %%d in ('dir /b /ad "%KITROOT%\Include" 2^>nul') do (
    if not defined KITVER if exist "%KITROOT%\Include\%%d\km\ntddk.h" set "KITVER=%%d"
  )
)
if not defined KITVER (
  echo [build-mic] no WDK with km\ntddk.h found under %KITROOT%\Include.
  echo             Install the Windows Driver Kit (kernel headers) first.
  exit /b 1
)
set "KITINC=%KITROOT%\Include\%KITVER%"
set "KITLIB=%KITROOT%\Lib\%KITVER%"
echo [build-mic] WDK            : %KITVER%

REM ---- 3. inf2cat / signtool (anywhere under the WDK bin tree) ---------------
set "INF2CAT="
set "SIGNTOOL="
for /r "%KITROOT%\bin" %%f in (inf2cat.exe) do if not defined INF2CAT set "INF2CAT=%%f"
for /r "%KITROOT%\bin" %%f in (signtool.exe) do if not defined SIGNTOOL set "SIGNTOOL=%%f"
if not defined INF2CAT echo [build-mic] WARNING: inf2cat.exe not found, skipping catalog.
if not defined SIGNTOOL echo [build-mic] WARNING: signtool.exe not found, skipping signing.

REM ---- 4. kernel driver (EXACT same flags as vmic.yml) ------------------------
echo [build-mic] compiling isightmic.cpp ...
if exist isightmic.obj del /q isightmic.obj
cl /nologo /kernel /c /W3 /O2 /D_AMD64_ /GS- /Fo:isightmic.obj ^
   /I"%KITINC%\km" ^
   /I"%KITINC%\km\crt" ^
   /I"%KITINC%\shared" ^
   /I"%KITINC%\um" ^
   drivers\isightmic\isightmic.cpp
if errorlevel 1 (
  echo.
  echo [build-mic] COMPILE FAILED -- the real errors are printed above.
  exit /b 1
)
echo [build-mic] linking isightmic.sys ...
link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 ^
   /NODEFAULTLIB:LIBCMT /NODEFAULTLIB:LIBCMTD /NODEFAULTLIB:MSVCRT ^
   /LIBPATH:"%KITLIB%\km\x64" ^
   portcls.lib ks.lib ksguid.lib drmk.lib ntoskrnl.lib hal.lib ^
   wmilib.lib wdmsec.lib BufferOverflowK.lib ^
   /OUT:isightmic.sys isightmic.obj
if errorlevel 1 (
  echo.
  echo [build-mic] LINK FAILED -- the real errors are printed above.
  exit /b 1
)
echo [build-mic] isightmic.sys built.
dir isightmic.sys | findstr isightmic

REM ---- 5. user-mode tools (non-fatal) ----------------------------------------
echo [build-mic] building user-mode tools ...
cl /nologo /O2 /W3 /EHsc /MD /I"drivers\isightmic" service\feed.cpp /Fe:isight-micsvc.exe >nul 2>&1
cl /nologo /O2 /W3 /EHsc /MD /DUNICODE /D_UNICODE tools\micdev.cpp setupapi.lib newdev.lib /Fe:isight-micdev.exe >nul 2>&1
cl /nologo /O2 /W3 /EHsc /MD /I"drivers\isightmic" tools\miccheck.cpp ole32.lib winmm.lib /Fe:isight-miccheck.exe >nul 2>&1

REM ---- 6. package + test-sign with ONE persistent local cert ------------------
if exist micpkg rmdir /s /q micpkg
mkdir micpkg
copy /y drivers\isightmic\isightmic.inf micpkg\ >nul
copy /y isightmic.sys micpkg\ >nul

if not defined INF2CAT goto :skipcat
if not defined SIGNTOOL goto :skipcat

powershell -NoProfile -Command "$c = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.Subject -like '*iSightMic Test Local*' } | Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=iSightMic Test Local' -CertStoreLocation 'Cert:\CurrentUser\My' -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(10) }; Export-Certificate -Cert $c -FilePath micpkg\iSightMicLocal.cer | Out-Null; $c.Thumbprint" > "%TEMP%\ism_thumb.txt"
set /p CERTTHUMB=<"%TEMP%\ism_thumb.txt"
echo [build-mic] signing cert  : %CERTTHUMB%  (persistent, trust micpkg\iSightMicLocal.cer once)

"%INF2CAT%" /driver:micpkg /os:10_X64
if errorlevel 1 (
  echo [build-mic] inf2cat FAILED.
  exit /b 1
)
"%SIGNTOOL%" sign /fd SHA256 /sha1 %CERTTHUMB% micpkg\isightmic.sys
if errorlevel 1 ( echo [build-mic] sign sys FAILED. & exit /b 1 )
"%SIGNTOOL%" sign /fd SHA256 /sha1 %CERTTHUMB% micpkg\isightmic.cat
if errorlevel 1 ( echo [build-mic] sign cat FAILED. & exit /b 1 )

:skipcat
echo.
echo [build-mic] ================================================================
echo [build-mic]  DONE.  micpkg\ is ready to install.
echo [build-mic]  If this is the FIRST local build, trust the cert once:
echo [build-mic]      certutil -addstore Root micpkg\iSightMicLocal.cer
echo [build-mic]  then install with your usual install/reload script.
echo [build-mic] ================================================================
endlocal
exit /b 0
