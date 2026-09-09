@echo off
rem ============================================================
rem  build.cmd <x64|x86>
rem  Requires: Visual Studio Build Tools environment (cl.exe on PATH)
rem  Produces: iSightCam64.ax / iSightCam32.ax  at repo root
rem ============================================================
setlocal

set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
if /i "%ARCH%"=="x64" (set OUT=iSightCam64.ax) else (set OUT=iSightCam32.ax)

echo === [1/4] compiling DirectShow BaseClasses (%ARCH%) ===
if not exist baseclasses\streams.h (
    echo [ERROR] baseclasses folder not found - see .github/workflows/build.yml
    exit /b 1
)
if not exist obj\bc-%ARCH% mkdir obj\bc-%ARCH%
cd obj\bc-%ARCH%
cl /nologo /c /O2 /MD /EHsc /GR /W3 /FS /Zc:twoPhase- ^
   /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS ^
   /DUNICODE /D_UNICODE ^
   /I..\..\baseclasses ..\..\baseclasses\*.cpp
if errorlevel 1 ( cd ..\.. & exit /b 1 )
lib /nologo /OUT:strmbase-%ARCH%.lib *.obj
if errorlevel 1 ( cd ..\.. & exit /b 1 )
rem remove base-classes dllentry.obj: we provide our own DLL entry points
lib /nologo strmbase-%ARCH%.lib /REMOVE:dllentry.obj
if errorlevel 1 ( cd ..\.. & exit /b 1 )
cd ..\..

echo === [2/4] compiling CMU 1394 camera library (%ARCH%) ===
if not exist obj\cmu-%ARCH% mkdir obj\cmu-%ARCH%
cd obj\cmu-%ARCH%
cl /nologo /c /O2 /MD /EHsc /GR /W3 /FS /Zc:twoPhase- ^
   /D_CRT_SECURE_NO_WARNINGS /DMY1394CAMERA_EXPORTS ^
   /I..\..\cmu\1394camera ^
   ..\..\cmu\1394camera\1394Camera.cpp ^
   ..\..\cmu\1394camera\1394CamAcq.cpp ^
   ..\..\cmu\1394camera\1394CamCap.cpp ^
   ..\..\cmu\1394camera\1394CamRGB.cpp ^
   ..\..\cmu\1394camera\1394CamReg.cpp ^
   ..\..\cmu\1394camera\1394CamMem.cpp ^
   ..\..\cmu\1394camera\1394CamPIO.cpp ^
   ..\..\cmu\1394camera\1394CamSIO.cpp ^
   ..\..\cmu\1394camera\1394CamFMR.cpp ^
   ..\..\cmu\1394camera\1394CameraControl.cpp ^
   ..\..\cmu\1394camera\1394CameraControlSize.cpp ^
   ..\..\cmu\1394camera\1394CameraControlStrobe.cpp ^
   ..\..\cmu\1394camera\1394CameraControlTrigger.cpp ^
   ..\..\cmu\1394camera\ControlWrappers.cpp ^
   ..\..\cmu\1394camera\1394main.c ^
   ..\..\cmu\1394camera\isochapi.c ^
   ..\..\cmu\1394camera\tables.c ^
   ..\..\cmu\1394camera\debug.c
if errorlevel 1 ( cd ..\.. & exit /b 1 )
cd ..\..

echo === [3/4] compiling filter (%ARCH%) ===
if not exist obj\flt-%ARCH% mkdir obj\flt-%ARCH%
cd obj\flt-%ARCH%
cl /nologo /c /O2 /MD /EHsc /GR /W3 /FS /Zc:twoPhase- ^
   /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DMY1394CAMERA_EXPORTS ^
   /I..\..\cmu\1394camera /I..\..\baseclasses ^
   ..\..\filter\iSightFilter.cpp
if errorlevel 1 ( cd ..\.. & exit /b 1 )
cd ..\..

echo === [4/4] linking %OUT% (%ARCH%) ===
if /i "%ARCH%"=="x64" (set MACHINE=X64) else (set MACHINE=X86)
link /nologo /DLL /OUT:%OUT% /DEF:filter\iSightCam.def /MACHINE:%MACHINE% ^
  /OPT:REF /OPT:ICF ^
  obj\flt-%ARCH%\iSightFilter.obj ^
  obj\cmu-%ARCH%\*.obj ^
  obj\bc-%ARCH%\strmbase-%ARCH%.lib ^
  ole32.lib oleaut32.lib uuid.lib strmiids.lib winmm.lib advapi32.lib ^
  setupapi.lib shlwapi.lib user32.lib gdi32.lib version.lib
if errorlevel 1 exit /b 1

echo === OK: %OUT% built ===
endlocal
exit /b 0
