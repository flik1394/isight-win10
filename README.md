# iSightCam — 让 2003 年的 Apple FireWire iSight 变成 Windows 标准摄像头

**你不需要会写代码，也不需要安装任何开发工具。**
把本项目上传到 GitHub 后，GitHub 的云端服务器会自动帮你编译出成品，你只需要下载、解压、双击安装。

---

## 第一步：上传到 GitHub（约 5 分钟）

1. 注册 / 登录 [github.com](https://github.com)（免费）
2. 右上角点 **`+`** → **New repository**
   - Repository name 填：`isight-dshow`
   - 选 **Public**（必须，否则免费账号无法运行构建）
   - 点 **Create repository**
3. 在新页面点击 **"uploading an existing file"** 链接
4. 把本文件夹里的**全部内容**（包括隐藏的 `.github` 文件夹）拖进上传区
   - 在文件选择器里进入本文件夹，全选（Ctrl+A）后拖入即可
   - 确保 `.github/workflows/build.yml` 也被上传（这是自动编译的"说明书"）
5. 拉到页面底部点 **Commit changes**

## 第二步：等待云端自动编译（约 5–10 分钟）

1. 点击仓库顶部的 **Actions** 标签
2. 左侧 "Build iSightCam" → 右侧最新一次运行
3. 等它变成 **绿色 ✔**（灰色圆点是还在跑）
4. 点进该次运行，拉到页面底部 **Artifacts** 区域
5. 下载 **iSightCam-package**，解压到本地

> ❌ 如果变成红色 ✖：点进去复制报错信息，发给我（AI 助手），我来修。

## 第三步：安装（约 10 分钟，含重启）

解压出的 `iSightCam-package` 文件夹里：

1. **双击 `1394camera646.exe`** — 安装 CMU 内核驱动，装完**必须重启电脑**
2. 接好摄像头（PCIe FireWire 卡 + FireWire 线），可先打开开始菜单里的
   **1394Camera Demo** 确认能看到画面
3. **右键 `install-all.bat` → 以管理员身份运行**
4. 完成！打开 OBS / Zoom / 微信 / 腾讯会议，摄像头列表里选择
   **"Apple iSight (FireWire)"**

---

## 包内是什么

| 文件 | 作用 |
|---|---|
| `1394camera646.exe` | CMU 1394 内核驱动（官方签名版） |
| `iSightCam64.ax` / `iSightCam32.ax` | 本项目编译产物：DirectShow 摄像头组件，让系统把 iSight 当成普通网络摄像头 |
| `install-all.bat` / `uninstall-all.bat` | 一键注册 / 卸载 |
| `README-install.txt` | 给装机时的简要说明 |

## 技术原理（一段话版）

CMU 驱动分两层：签名的内核驱动 `1394Camera.sys` 负责 FireWire 总线取流
（由 `1394camera646.exe` 安装）；本项目把 CMU 开源的用户态库 `C1394Camera`
与一个新写的 DirectShow Source Filter（`filter/iSightFilter.cpp`）一起编译成
`iSightCam.ax`，注册到系统的视频采集设备类别中。视频格式为 iSight 原生的
640×480 YUV422 @ 30fps，经 CMU 库转换为 RGB24 输出。**全程零内核代码改动**，
因此不需要驱动签名。

## 已知限制

- 无麦克风（iSight 麦克风走 FireWire 独立音频通道，Windows 不支持），请用其他麦克风
- 分辨率固定 640×480（iSight 硬件规格）
- Windows 11 24H2+ 的内核策略较严，建议以 Windows 10 为主；装不上 CMU 驱动时
  参考仓库文档在设备管理器手动添加 1394 兼容驱动

## 许可

- `cmu/` 目录：CMU 1394 Digital Camera Driver，LGPL 2.1（见 `cmu/lgpl.txt`）
- `filter/`、`build.cmd`、CI 配置：可自由使用与修改
