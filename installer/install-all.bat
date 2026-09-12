@echo off
setlocal EnableExtensions
rem 本文件以 GBK(936) 编码打包，先锁定控制台代码页，避免中文变乱码
chcp 936 >nul 2>&1
rem ============================================================
rem  install-all.bat  -  注册 Apple iSight (FireWire) DirectShow 摄像头
rem  v11：必须右键"以管理员身份运行"
rem
rem  这一版让"画面形状"的调整变得可用、可见：
rem    1. [layout] hosts=Weixin.exe,WeChat.exe —— 这块"缩进矩形"只对
rem       名单里的程序生效，QQ 等宿主仍是完整的 640x480 原帧。
rem    2. [layout] guide=1 —— 直接把标尺画进画面：整帧黄框、矩形红框、
rem       正中白十字。微信窗口里哪些线看得见，就知道它裁掉了多少，
rem       于是 target 一次就能对准。
rem    3. orientation / layout / guide 现在会在通话进行中自动重读
rem       iSightCam.ini（最多半秒一次），改文件即时生效，不用重开微信。
rem
rem  上一版（v10）做了 [layout] target=WxH（把整幅画面缩进宿主实际
rem  显示的那块矩形、四周填黑，宿主裁到的就是这块矩形，场景就回来了）、
rem  [format] types= 子类型排序、yuy2=4 朝向模式，并补上 rcSource/rcTarget。
rem  v9 修"通话中拧掉再拧开相机后所有程序黑屏"（总线监视 + 自动重连）；
rem  v8 修微信画面颠倒与安装脚本中文乱码。
rem
rem  沿用两条保护（曾经把 64 位组件静默漏装过）：
rem    * 拒绝在 32 位 cmd 里运行
rem    * 目标文件被占用时先改名再复制，复制后按版本标记自检
rem ============================================================

set "TAG=ISIGHTFILTER-BUILD-V11-20260912-LAYOUT-LIVE"
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
echo.
echo  画面朝向不对时（例如微信里上下颠倒）：编辑
echo    %LOCALAPPDATA%\iSightCam.ini
echo  把 [orientation] 下的 yuy2 改成 0/2/3 逐个试，
echo  保存后画面一秒内就变，不需要重装或重编译。
echo.
echo  微信里"人太大/画面被切掉"（QQ 正常）时：微信的视频通话窗口
echo  是竖屏的，它会把 4:3 的画面裁掉两边再放大，所以成了"大头照"。
echo  编辑同一个 ini，加上：
echo    [layout]
echo    target=320x480
echo    hosts=Weixin.exe,WeChat.exe
echo    guide=1
echo  guide=1 会画出标尺：整帧黄框、目标矩形红框、正中白十字。
echo  通话中的小窗里如果红框四边都看得见、且没有多余黑边，就说明
echo  目标矩形对了（记得把 guide 改回 0）。还嫌近就把 target 调宽
echo  （360x480、480x480）；两侧出现黑边就调窄（270x480）。
echo  hosts 让这块矩形只对微信生效，QQ 不受影响；target=0 恢复原样。
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
findstr /m /c:"ISIGHTFILTER-BUILD-V11" "%~1" >nul 2>&1
if errorlevel 1 (
    echo     [FAIL] %~1 里没有 v11 标记 —— 这个文件还是旧版！
    exit /b 1
)
for %%A in ("%~1") do echo     [ OK ] 已安装 %~2 位 %%~zA 字节，含 v11 标记
exit /b 0
