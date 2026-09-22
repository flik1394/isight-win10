@echo off
REM Build isightmic.sys (kernel PortCls capture miniport) with the WDK that ships
REM in the windows-2022 GitHub Actions image (portcls.lib confirmed present).
setlocal
set KIT=C:\Program Files (x86)\Windows Kits\10
set INC=%KIT%\Include\10.0.26100.0
set LIB=%KIT%\Lib\10.0.26100.0\km\x64

cl /nologo /kernel /c /W3 /O2 ^
   /I"%INC%\km" /I"%INC%\km\crt" /I"%INC%\shared" /I"%INC%\um" ^
   isightmic.cpp
if errorlevel 1 exit /b 1

link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 ^
     /LIBPATH:"%LIB%" ^
     portcls.lib ks.lib drmk.lib ntoskrnl.lib hal.lib wmilib.lib wdmsec.lib ^
     /OUT:isightmic.sys isightmic.obj
if errorlevel 1 exit /b 1

dir isightmic.sys
endlocal
