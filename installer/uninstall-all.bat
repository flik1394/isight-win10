@echo off
rem 取消注册并清理 iSight DirectShow 摄像头组件（管理员运行）
regsvr32 /u /s "%SystemRoot%\System32\iSightCam.ax"
del /f /q "%SystemRoot%\System32\iSightCam.ax" 2>nul
C:\Windows\SysWOW64\regsvr32.exe /u /s "%SystemRoot%\SysWOW64\iSightCam.ax" 2>nul
del /f /q "%SystemRoot%\SysWOW64\iSightCam.ax" 2>nul
echo 已卸载 iSight 摄像头组件（CMU 1394 内核驱动未动，可在控制面板卸载）
pause
