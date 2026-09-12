@echo off
setlocal EnableExtensions
rem ============================================================
rem  install-all.bat  -  注册 Apple iSight (FireWire) DirectShow 摄像头
rem  v7：必须右键"以管理员身份运行"
rem
rem  这一版多做三件事，因为上一版把 64 位组件漏装了（表现为
rem  "微信/QQ 还是老样子"，实际它们加载的是 System32 里的旧文件）：
rem    1. 拒绝在 32 位 cmd 里运行——那会让对 System32 的写入被重定向到
rem       SysWOW64，"64 位没装上"却毫无提示
rem    2. 目标文件被占用时先改名再复制（Windows 允许改名已映射的 DLL，
rem       不允许直接覆盖，这就是上次 copy 静默失败的原因）
rem    3. 复制后按内置版本标记校验，装没装上一眼可见
rem ============================================================

set "TAG=ISIGHTFILTER-BUILD-V7-20260912-PINCAT"
set "SRC64=%~dp0iSightCam64.ax"
set "SRC32=%~dp0iSightCam32.ax"
set "DST64=%SystemRoot%\System32\iSightCam.ax"
set "DST32=%SystemRoot%\SysWOW64\iSightCam.ax"

echo ============================================================
echo  Apple iSight (FireWire) DirectShow 滤镜 安装程序
echo  目标版本标记: %TAG%
echo ============================================================
echo.

rem ---------- 0. 管理员 ----------
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] 请右键本文件，选择"以管理员身份运行"
    pause
    exit /b 1
)

rem ---------- 1. 必须 64 位命令解释器 ----------
if not "%PROCESSOR_ARCHITEW6432%"=="" (
    echo [!] 当前是 32 位 cmd.exe：对 System32 的写入会被重定向到 SysWOW64，
    echo     64 位组件将不会被安装。请从 64 位的文件管理器/终端重新运行。
    pause
    exit /b 1
)

rem ---------- 2. 源文件检查 ----------
if not exist "%SRC64%" ( echo [!] 找不到 %SRC64% & pause & exit /b 1 )
if not exist "%SRC32%" ( echo [!] 找不到 %SRC32% & pause & exit /b 1 )
echo [0] 源文件:
echo     64 位 %SRC64%
echo     32 位 %SRC32%
echo.

rem ============================================================
rem  64 位
rem ============================================================
echo [1] 安装 64 位组件  -^>  %DST64%

call :ReplaceFile "%SRC64%" "%DST64%"
if errorlevel 1 ( echo. & echo [!] 64 位组件安装失败，见上面错误 & pause & exit /b 1 )

regsvr32 /s "%DST64%"
if errorlevel 1 (
    echo [!] 64 位 regsvr32 失败（code %errorlevel%）
    echo     请确认已安装 CMU 1394 驱动（1394camera646.exe）并重启过电脑
    pause & exit /b 1
)
call :VerifyFile "%DST64%" "64"
echo.

rem ============================================================
rem  32 位
rem ============================================================
echo [2] 安装 32 位组件  -^>  %DST32%

call :ReplaceFile "%SRC32%" "%DST32%"
if errorlevel 1 (
    echo [!] 32 位组件安装失败（64 位已可用；但 32 位 QQ 等会看不到摄像头）
) else (
    "%SystemRoot%\SysWOW64\regsvr32.exe" /s "%DST32%"
    if errorlevel 1 (
        echo [!] 32 位 regsvr32 失败（code %errorlevel%）
    ) else (
        call :VerifyFile "%DST32%" "32"
    )
)
echo.

rem ============================================================
rem  3. 注册表指向核对
rem ============================================================
echo [3] 注册表 InprocServer32 指向:
reg query "HKLM\SOFTWARE\Classes\CLSID\{73912CE1-84DD-4BF4-8693-FF4603D7369F}\InprocServer32" /ve 2>nul | findstr /i "iSightCam.ax"
reg query "HKLM\SOFTWARE\Classes\CLSID\{73912CE1-84DD-4BF4-8693-FF4603D7369F}\InprocServer32" /reg:32 /ve 2>nul | findstr /i "iSightCam.ax"
echo.

echo ============================================================
echo  安装完成。设备管理器 / 系统摄像头列表里应出现
echo  "Apple iSight (FireWire)"。
echo.
echo  验证版本：运行 isight-graphtest.exe，日志里会打印实际
echo  加载的 build tag（应为 %TAG%）。
echo ============================================================
pause
exit /b 0

rem ============================================================
rem  子过程：替换文件（被占用时先改名再复制）
rem ============================================================
:ReplaceFile
rem %1=源 %2=目标
copy /y "%~1" "%~2" >nul 2>&1
if not errorlevel 1 exit /b 0

echo     - 直接覆盖失败（文件正被运行中的程序占用），改为先改名再复制
move /y "%~2" "%~2.old" >nul 2>&1
if errorlevel 1 (
    echo     - 改名也失败：目标被独占。请关闭所有摄像头程序后重试
    exit /b 1
)
copy /y "%~1" "%~2" >nul 2>&1
if errorlevel 1 (
    echo     - 复制仍然失败
    exit /b 1
)
del /f /q "%~2.old" >nul 2>&1
echo     - 已通过改名方式替换成功
exit /b 0

rem ============================================================
rem  子过程：按版本标记校验
rem ============================================================
:VerifyFile
rem %1=目标 %2=位数标签
findstr /m /c:"ISIGHTFILTER-BUILD-V7" "%~1" >nul 2>&1
if errorlevel 1 (
    echo     [FAIL] %~1 里没有 v7 标记 —— 这个文件还是旧版！
    exit /b 1
)
for %%A in ("%~1") do echo     [ OK ] 已安装 %~2 位 %%~zA 字节，含 v7 标记
exit /b 0
