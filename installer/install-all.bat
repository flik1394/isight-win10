@echo off
rem ============================================================
rem  install-all.bat  -  注册 Apple iSight (FireWire) DirectShow 摄像头
rem  请右键选择"以管理员身份运行"
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] 请右键本文件，选择"以管理员身份运行"
    pause
    exit /b 1
)

echo === 安装 64 位摄像头组件 ===
copy /y "%~dp0iSightCam64.ax" "%SystemRoot%\System32\iSightCam.ax"
regsvr32 /s "%SystemRoot%\System32\iSightCam.ax"
if %errorlevel% neq 0 (
    echo [!] 64 位组件注册失败，请确认已安装 CMU 1394 驱动并重启过电脑
    pause & exit /b 1
)

echo === 安装 32 位摄像头组件 ===
copy /y "%~dp0iSightCam32.ax" "%SystemRoot%\SysWOW64\iSightCam.ax"
C:\Windows\SysWOW64\regsvr32.exe /s "%SystemRoot%\SysWOW64\iSightCam.ax"
if %errorlevel% neq 0 (
    echo [!] 32 位组件注册失败（不影响 64 位使用，可忽略）
)

echo.
echo === 完成！系统摄像头列表中应出现 "Apple iSight (FireWire)" ===
echo    提示：需要先装好 CMU 1394 驱动（1394camera646.exe）并重启
pause
