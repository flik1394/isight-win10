# iSightCam — 让 2003 年的 Apple FireWire iSight 在 Windows 上当普通摄像头用

![Build iSightCam](https://github.com/flik1394/isight-win10/actions/workflows/build.yml/badge.svg)

把 Apple iSight（2003 年那台铝筒 FireWire 摄像头）接到 Windows 10 / 11 上，
在**微信、QQ、腾讯会议、Zoom、OBS** 等任何标准程序里像普通 USB 摄像头一样直接用。
**零内核代码改动、不需要驱动签名。**

> **当前版本：v1.0.0**（2026-09-13）
> 视频链路完整可用。麦克风尚未支持 —— 见下方[「已知限制」](#已知限制)与[路线图](#路线图)。

---

## 下载安装

1. 到 [**Releases**](../../releases/latest) 下载 `iSightCam-1.0.0-win.zip`，解压。
2. **双击 `1394camera646.exe`** — 安装 CMU 1394 内核驱动（官方签名版），装完**必须重启电脑**。
3. 接好硬件：PCIe FireWire 扩展卡 + 6-pin FireWire 线，接到 iSight 上。
   - 先打开开始菜单里的 **1394Camera Demo** 确认能看到画面（可选，但强烈建议）。
4. **右键 `install-all.bat` → 以管理员身份运行**。
5. 打开微信 / QQ / OBS / 腾讯会议，摄像头列表里选 **"Apple iSight (FireWire)"**。

卸载：管理员运行 `uninstall-all.bat`。

> 包内的 `1394camera646.exe` 是 CMU 1394 Digital Camera Driver 6.4.6（签名版），
> 由 CMU 官方发布，本项目仅做转发与引用。

## 已验证可用

| 程序 | 状态 | 备注 |
|---|---|---|
| OBS / AmCap / 系统「相机」 | ✅ | 640×480 YUV422 @30fps |
| 微信（Windows 4.x）视频通话 | ✅ | 默认出完整 4:3 画面；微信自己会裁成竖屏，见下方 FAQ |
| QQ 视频通话 | ✅ | 首次检测到设备可能要等几秒，是 QQ 自己的枚举行为 |
| 腾讯会议 / Zoom / 钉钉 | ✅ | 走标准 DirectShow 采集 |
| 32 位程序 | ✅ | 安装包同时注册 32 位与 64 位组件 |

## 功能特性

- **原生 640×480 @30fps**，输出 RGB24 / YUY2 / RGB32 可选（`[format] types=`）。
- **画面朝向可调**：不同 FireWire 卡/线序下 iSight 的 YUV 排列可能不同，
  用 ini 里的 `[orientation]` 一键翻转，通话中改也**一秒内生效**，不用重装。
- **总线自愈**：通话中相机被拔/断电（拧一下底座），5–10 秒内自动重连；
  也可以手动 `isight-diag.exe reset` 复位 1394 总线。
- **取流不被"停死"**（v1.0 的关键修复）：DirectShow 基类的取流循环只要
  `Deliver()` 返回过一次非 `S_OK`（宿主渲染器切状态、偶发拒收样本）就会
  永久退出工作线程 —— 表现为「设备在列表里，画面永远是黑的，零帧」。
  本版接管该循环，被拒样本重试而不是当场判死，并在日志里喊
  `STREAM PARKED`，黑屏从此可诊断。
- **微信画面适配**（`[layout]`）：微信通话窗是竖屏的，会把 4:3 画面裁掉两边再放大，
  于是成了"大头照"。可用 ini 指定一个只对微信生效的裁剪矩形（`hosts=Weixin.exe`），
  QQ 等宿主仍收到完整原帧；`guide=1` 会把标尺画进画面方便校准。
- **完整日志**：`%LOCALAPPDATA%\iSightCam.log`，含宿主进程名、布局是否生效、
  取流循环状态。出问题先看它。

## 已知限制

- **无麦克风**。iSight 的麦克风走 FireWire 上一条**独立的音频通道**（不是 AV/C、不是 USB audio），
  Windows 自带的 1394 驱动栈只做视频，认不出它。可用其它麦克风代替；
  音频链路的技术验证见[路线图](#路线图)。
- **分辨率固定 640×480**，这是 iSight 的硬件规格。
- **Windows 11 24H2 及以后**的内核驱动策略更严，CMU 驱动可能装不上；
  建议以 Windows 10 为主，装不上时参考仓库文档在设备管理器里手动指定 1394 兼容驱动。
- 需要**一张能用的 FireWire 卡**。已验证 TI 双芯片卡（800 口经 800→400 线）与
  常见的 OHCI 1394 卡；部分廉价卡（尤其 NEC/VIA 桥接方案）可能不识别。

## 常见问题

**画面上下颠倒 / 转了 90°**
编辑 `%LOCALAPPDATA%\iSightCam.ini`，把 `[orientation]` 下的 `yuy2` 依次试 `0 / 2 / 3`，
保存后一秒内生效。（默认值是 `2`。）

**微信视频通话里"人特别大"，画面被切掉（QQ 和 Demo 正常）**
这是微信把 4:3 画面裁成竖屏再放大造成的。编辑同一个 ini：

```ini
[layout]
target=320x480
hosts=Weixin.exe,WeChat.exe
guide=1
```

`guide=1` 会把标尺画进画面（整帧黄框、目标矩形红框、正中白十字）。通话小窗里若红框
四边都看得见又没有多余黑边，说明矩形正好；还嫌近就把 `target` 调宽（`360x480`、
`480x480`），两侧出现黑边就调窄（`270x480`）。校准完把 `guide` 改回 `0`。
`target=0` 完全关闭。

**QQ 找不到摄像头 / 要等一会儿才检测到**
QQ 有自己的设备枚举与缓存逻辑，首次识别可能滞后几秒到几十秒（本项目的日志显示
这段时间里 QQ 根本没加载滤镜）。先在 QQ 设置里手动刷新设备列表，或退出重进。

**装完看不到设备**
1. 确认第 2 步装完 CMU 驱动后**真的重启过**；
2. 确认 1394Camera Demo 能出图（出不了图 → 是驱动/硬件问题，不是本项目的问题）；
3. 看 `%LOCALAPPDATA%\iSightCam.log` 里有没有 `=== iSightCam 1.0.0 ...` 这行，
   没有就是滤镜没被加载。

**通话中拧动相机后画面不出来**
等 5–10 秒会自动恢复；卡住就运行 `isight-diag.exe reset`。

## 想自己编译

仓库自带 GitHub Actions 工作流：把仓库 fork 或上传到自己的 GitHub，
云端 Windows 机器会自动编译，产物在 Actions → Artifacts 里
（`iSightCam-package` = 可直接安装的完整包，`isight-diag` = 诊断工具集）。
本地编译用 `build.cmd x64` / `build.cmd x86`（需 VS 2022 生成工具 + DirectShow BaseClasses）。

打一个 `v*` 开头的 tag 就会自动发布一个 GitHub Release，并把安装包挂上去。

## 工作原理

CMU 驱动分两层：官方签名的内核驱动 `1394Camera.sys` 负责 FireWire 总线取流
（由 `1394camera646.exe` 安装）；本项目把 CMU 开源的用户态库 `C1394Camera`
与一个新写的 DirectShow Source Filter（`filter/iSightFilter.cpp`）一起编译成
`iSightCam.ax`，注册进系统的视频采集设备类别。视频格式为 iSight 原生的
640×480 YUV422 @ 30fps，经 CMU 库转换为 RGB24 输出。**全程零内核代码改动**，
因此不需要驱动签名。

## 路线图

- **v2.0 — 麦克风（研究中）**。已经用诊断工具摸清了 iSight 麦克风的全部协议细节：
  它在配置 ROM 里作为一个独立单元被枚举（`spec 0x000A27 / ver 0x000010`，
  与 Linux 内核 `sound/firewire/isight.c` 的 `SW_ISIGHT_AUDIO` 一致），
  CSR 基址 `0xFFFFF0020000`，11 个寄存器全部可读（增益 −30…+12 dB、48 kHz、
  采样率位图都已确认）。目前卡在两点：① CMU 驱动一个设备对象只允许一条等时流，
  视频跑起来后音频接收会被拒（`ERROR_GEN_FAILURE`）；② 相机配置完成后仍不发流，
  怀疑缺 1394 IRM 带宽预留锁。两条备选路线（自写音频小驱动 / 让 CMU 驱动同时绑定
  音频节点）正在评估。
- 多分辨率/多帧率（受硬件限制，希望不大）
- 更省事的适配：自动按宿主窗口比例选择 `[layout]`

## 许可与致谢

- `cmu/` — CMU 1394 Digital Camera Driver，LGPL 2.1（见 `cmu/lgpl.txt`）
- `filter/`、`build.cmd`、`diags/`、CI 配置 — 可自由使用与修改
- 感谢 [Andrew-Dyachenko/apple-firewire-isight-on-windows](https://github.com/Andrew-Dyachenko/apple-firewire-isight-on-windows)
  保存并公开了 CMU 6.4.6 签名驱动
- 音频协议参考 Linux 内核 `sound/firewire/isight.c`
