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
  * 画面上下颠倒 / 旋转了 → 编辑 %LOCALAPPDATA%\iSightCam.ini 的
    [orientation] 段，把 yuy2 依次试 0 / 2 / 3，保存后**一秒内**画面就变
    （v11 起通话中也会自动重读 ini，不用重开微信）
  * 微信视频通话里人特别大、画面被切掉（QQ 和 Demo 正常）→ 微信的通话窗口
    是竖屏的，它会把 4:3 的画面裁掉两边再放大，所以成了"大头照"。同样编辑
    iSightCam.ini，加上：
        [layout]
        target=320x480
        hosts=Weixin.exe,WeChat.exe
        guide=1
    guide=1 会把标尺画进画面：整帧黄框、目标矩形红框、正中白十字。通话中的
    小窗里若红框四边都看得见、四周没有多余黑边，说明矩形正好；还嫌近就把
    target 调宽（360x480 / 480x480），两侧出现黑边就调窄（270x480）。
    校准完把 guide 改回 0。hosts 让这块矩形**只对微信生效**，QQ 等宿主仍是
    完整的 640x480 原帧；target=0 表示完全关闭。
  * 视频通话中给相机断电（拧一圈）后画面不出来 → 5-10 秒内会自动恢复；
    若卡住可运行 isight-diag.exe reset 手动复位 1394 总线
  * 想看滤镜到底在做什么 → %LOCALAPPDATA%\iSightCam.log（v11 的选项行会打印
    host='Weixin.exe' ... -> layout ACTIVE/inactive，一眼看出矩形有没有生效）
  * 视频通话中给相机断电（拧一圈）后画面不出来 → 5-10 秒内会自动恢复；
    若卡住可运行 isight-diag.exe reset 手动复位 1394 总线
  * 想看滤镜到底在做什么 → %LOCALAPPDATA%\iSightCam.log

注意：本组件不提供麦克风（iSight 的麦克风走独立的 FireWire 音频通道，
Windows 驱动栈不支持），请使用其他麦克风。
