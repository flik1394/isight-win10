iSightCam 安装说明（Apple iSight FireWire 摄像头 → Windows 标准摄像头）
==========================================================================

包内文件：
  1394camera646.exe    CMU 1394 内核驱动（官方签名版，必装）
  iSightCam64.ax       64 位 DirectShow 摄像头组件
  iSightCam32.ax       32 位 DirectShow 摄像头组件
  install-all.bat      一键注册（管理员运行）
  uninstall-all.bat    一键卸载（管理员运行）

安装步骤（顺序不能乱）：
  1. 双击 1394camera646.exe，安装 CMU 驱动，装完【重启电脑】
  2. 重启后接好摄像头（PCIe FireWire 卡 + 6-pin 线）
  3. 打开 1394Camera Demo 确认能看到画面（可选但建议）
  4. 右键 install-all.bat → 以管理员身份运行
  5. 打开 OBS / Zoom / 微信 / 腾讯会议，摄像头列表选择
     "Apple iSight (FireWire)"

常见问题：
  * OBS 里看不到摄像头 → 确认第 1 步已重启；确认 1394Camera Demo 能出图
  * 某些应用看不到 → 那个应用可能是 32 位的，本包已同时注册 32/64 位
  * 想恢复原状 → 管理员运行 uninstall-all.bat

注意：本组件不提供麦克风（iSight 的麦克风走独立的 FireWire 音频通道，
Windows 驱动栈不支持），请使用其他麦克风。
