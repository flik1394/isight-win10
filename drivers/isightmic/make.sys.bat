@echo off
REM Build isightmic.sys (kernel PortCls capture miniport) with the WDK that ships
REM in the windows-2022 GitHub Actions image (portcls.lib confirmed present).
setlocal
set KIT=C:\Program Files (x86)\Windows Kits\10
set INC=%KIT%\Include\10.0.26100.0
set LIB=%KIT%\Lib\10.0.26100.0\km\x64

REM /D_AMD64_ : the SDK's shared\ntdef.h needs a target-architecture macro and
REM aborts with C1189 "No Target Architecture" without one.  The WDK's MSBuild
REM toolset injects it; driving cl directly means we have to.
REM /Fo : cl writes the object to the current directory, not next to the source.
REM /GS- : keep the user-mode CRT's GS handler (LIBCMT gs_report.obj) out of a
REM kernel binary.
cl /nologo /kernel /c /W3 /O2 /D_AMD64_ /GS- /Fo:isightmic.obj ^
   /I"%INC%\km" /I"%INC%\km\crt" /I"%INC%\shared" /I"%INC%\um" ^
   isightmic.cpp
if errorlevel 1 exit /b 1

link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 ^
     /NODEFAULTLIB:LIBCMT /NODEFAULTLIB:LIBCMTD /NODEFAULTLIB:MSVCRT ^
     /LIBPATH:"%LIB%" ^
     portcls.lib ks.lib ksguid.lib drmk.lib ntoskrnl.lib hal.lib ^
     wmilib.lib wdmsec.lib BufferOverflowK.lib ^
     /OUT:isightmic.sys isightmic.obj
if errorlevel 1 exit /b 1

dir isightmic.sys
endlocal
