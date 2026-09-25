# iSight 虚拟麦克风（virtual-mic）

让 iSight 的麦克风在 Windows 里变成一个**真正的录音设备**，微信 / QQ / OBS /
腾讯会议的设备列表里能直接选到 "iSight Microphone (FireWire)"。

摄像头那条路已经通了（DirectShow filter）。麦克风卡在最后一公里：iSight 的音频
走 FireWire 上一条独立的等时通道，Windows 自带的 1394 驱动栈认不出它，所以系统里
根本没有这个设备。采集链路我们已经用 `isight-audio.exe` 打通（能录到真人说话，
SNR 38 dB），缺的是一个**把处理完的 PCM 交给系统的出口**——这就是本文要做的东西。

## 总体结构

```
   iSight（FireWire）
        │  等时包，960 B/包，'sght' 音频包混在视频流里
        ▼
   CMU 1394cmdr.sys          ← 已安装（1394camera646.exe）
        │
        ▼
   isight-micsvc.exe         ← 用户态服务（本项目新增）
        │  · 收包、拆包
        │  · 丢包隐藏（PLC）
        │  · 降噪 / 门 / 美化
        │
        │  DeviceIoControl：把 48 kHz / 16 bit / 单声道的 PCM 写进环形缓冲
        ▼
   isightmic.sys             ← 内核态 PortCls 微型端口（本项目新增）
        │
        ▼
   Windows 音频引擎 → 微信 / QQ / OBS / 录音机
```

分成两块是为了把风险隔开：**采集和处理都在用户态**（崩了只是服务重启），
**内核里只留一个环形缓冲和 PortCls 的样板代码**（不碰 FireWire、不做 DSP）。

## 为什么必须是内核驱动

用户态没有"制造一个录音设备"的接口。WASAPI、DirectSound、Core Audio 都只能
枚举已存在的端点，而端点由内核里的音频驱动（PortCls / AVStream）注册。所以
虚拟麦克风无捷径，必须有一个 .sys。

代价：需要测试签名（用户机器已开 `testsigning`），且驱动 bug 会 BSOD。因此
内核侧严格保持最小。

## 内核侧：isightmic.sys

基于 MSVAD（Microsoft Virtual Audio Device Driver）样例的 wave-cyclic 微型端口：
- 采样率 **48 kHz**、**16 bit**、**单声道**（iSight 是双声道但两路相关性 0.987，
  实际是同一个麦克风，取 `(L+R)/2` 还能抵消 ±75 的反向直流偏置）
- 一个环形缓冲（约 0.5 s），用户态写、驱动读
- **缓冲读空时输出静音**，绝不重复上一帧（重复会被听成卡顿）
- 通过 IOCTL 暴露：写 PCM、查询缓冲水位、启停

## 用户态：isight-micsvc.exe

### M1（先做，不依赖相机）
用已录好的 WAV（`take2-pr-warm2.wav`）当信号源循环灌进驱动。
目的是**只验证驱动骨架**：设备能不能出现在系统里、能不能被应用识别、录下来的
是不是我们送进去的东西。相机不在也能做。

### M2
换成实时采集：复用 `diags/audio.cpp` 里已经验证过的 CMU 采集参数
（`cap 0xF0020000 <秒> one vrate=0 pkt=960`）。

### M3
实时 DSP（见下）。

## DSP 链（已定型的参数，待移植到 C++）

处理顺序不能改——顺序是踩过坑才定下来的：

| 步骤 | 作用 | 参数 |
|---|---|---|
| 1. PLC 丢包隐藏 | 按包头 `sample_total` 重建时间轴，洞里填**倒放**的上一音频段 | 洞固定 400/416 帧（丢 2 个包），12.6% |
| 2. 高通 | 压 30 Hz 地环路嗡声 | 4 阶 Butterworth，75 Hz |
| 3. 扩展器 | 压词间噪底 | 窗口 −38..−28 dBFS，floor −18 dB，attack 5 ms / release 80 ms；归一化到 0.71 |
| 4. EQ | 补厚度、收高频 | low-shelf 150 Hz **+4 dB**；peak 700 Hz +3 dB Q1.0；peak 1.8 kHz +3 dB Q0.8；low-pass 6 kHz |
| 5. 激励器 | 从 400–1200 Hz 造谐波补回缺失的高频 | 驱动带 400–1200 Hz，残差再过两道 840 Hz 高通，amount 0.50 |
| 6. 压缩 | 稳定电平 | 阈值 −19 dB，比 1.8:1，attack 16 ms / release 190 ms，makeup +4 dB，软膝 6 dB |
| 7. 扩展器（链尾） | 收尾压噪底 | floor −20 dB，跨度 10 dB，**阈值锚定包络 20% 分位** |
| 8. 软限幅 | 归一化 + 防削波 | 目标 0.89，0.85 满刻度以上 tanh 软弯 |

**几个花了代价才得到的结论，改动前请先看：**

- **倒放填充 vs 周期延续**：周期延续理论上更正确，但 `j % p` 每次回绕就是一个跳变，
  基音估偏就炸出 8909 的台阶。倒放赢在构造上——`fill[j] = out[n-1-j]` 的第一个
  样本恒等于前一帧，接缝天然为零。跳变 106 → 4 次/10 秒。
- **门必须在链尾**：EQ 削减、激励、压缩 makeup 都会移动绝对电平，固定阈值在每一档
  落点都不一样。所以链尾那个扩展器锚定包络分位而不是 dBFS。
- **1 kHz 以上不要猛提**：素材从源头就没有 1 kHz 以上内容（0.7%），而高通之后
  **41% 的噪声在 1 kHz 以上**。提上去的主要是噪声。1k–3k 的目标值 ≤1%，不是越高越好。
  （"clear" 档客观指标全面占优、听感最差，就是因为这个。）
- **15 Hz 突突是插值造出来的**：1333 个洞 ÷ 89 s = 14.97 Hz。填充段必须携带真实
  波形，纯线性斜坡会在包络谱上留下 13.4 倍的峰。
- **回声（43 ms 自相关峰）来自填充段的周期性**，与填什么内容无关——倒放/线性/混合
  三种填充测出来都是 0.137–0.144。只要那 12.6% 是丢的，这个峰就有下限。

## 里程碑

- **M1** 驱动骨架 + WAV 回环：系统里出现设备、能被识别、能录到送进去的音频
- **M2** 实时采集接进驱动
- **M3** 实时 DSP（上表 1–8 步移植到 C++）
- **M4** INF + 测试签名 + 安装脚本 + 服务开机自启

## 进度（2026-09-22 v19）

**M1 完成（已编译 + 已测试签名 + 已打包）**

CI 里有一条独立的 `Build iSight Virtual Mic` 工作流（`.github/workflows/vmic.yml`），
产物 artifact 名为 `isight-vmic`：

| 文件 | 作用 |
|---|---|
| `isightmic.sys` | PortCls WaveCyclic 采集微型端口（x64，26 KB） |
| `isightmic.cat` / `iSightMicTest.cer` | test-sign 目录与自签证书 |
| `isight-micdev.exe` | 建根枚举设备节点并把驱动装上去 |
| `isight-micsvc.exe` | M1 喂音器：把一个 WAV 循环推进驱动 |
| `isight-miccheck.exe` | 自检：开控制设备、看计数器、枚举录音端点、录回 3 秒存 WAV |
| `sample.wav` | 48 kHz / 16 bit / 立体声 3 秒测试音 |

**踩过的坑（vmic 工作流）**：把自签证书导进 runner 的 Root 存储会弹一个没人
按的确认框 → 那一级卡死到 job 超时（run 35746443369 的日志就停在
`[4/6] trusting the cert for this runner only` 之后 7 分钟）。所以 CI **只在
runner 本机创建证书并导出 .cer**，信任动作留给目标机器（`install-mic.bat` 里的
`certutil -addstore`）。另外签名失败不再连累编译产物：失败时有
`isight-vmic-unsigned` 兜底 artifact。

**M2 已接线**：`filter/iSightFilter.cpp`（v19，tag `ISIGHTFILTER-BUILD-V19-20260922-MICFEED`）
在解码每个 `sght` 包时，把 PCM **降混成单声道**后用
`DeviceIoControl(IOCTL_ISIGHTMIC_PUSH)` 推进 `\\.\IsightMicCtl`。
开关是 ini 的 `[audio] mic=1`（默认开）。为什么由滤镜来喂：CMU 一个设备对象
只允许一条等时流，而音频就挂在视频那条流上 —— 谁拿着视频流，谁才听得到麦克风。
所以"先有程序在用摄像头"是前提，这不是权宜之计，是硬约束。

- 降混为什么是免费的改进：两声道是同一个麦克风，且带**反向直流偏置**
  （实测 L≈+75、R≈−73），`(L+R)/2` 正好抵消。
- 驱动不在时（没装）只是日志里写一行 `mic: ... is not there yet`，画面与
  wav 录制完全不受影响；每 5 秒重试一次，所以通话中途装驱动也能立刻生效。


## 画面为什么会花：v17~v19 都判断错了，v20 才修好

> 这一节原来写的是"v18 的帧长算错 614400 → 每帧越界写 165 KB，v19 已修"。
> **那个结论是错的**，2026-09-23 拿到用户本机的原始缓冲后推翻。留在这里
> 是因为推理过程本身值钱：它说明"从日志数字推结构"会推错，"渲染出来看"不会。

### 当时怎么想错的

v18 的日志：

```
audio: strip frame #1: 14 packets / 11424 bytes cut, 449376 bytes of frame data left (a full frame is 614400)
```

`449376 + 11424 = 460800` 恰好等于 640×480 YUV411，而 v18 认为一帧是 614400
（`w*h*2`，YUY2 尺寸）。于是推断"补齐会写 165 KB 越界"。数字都对，
但**结论错**：`cb`（`GetRawFrameBuffer` 返回的 `ulBufferSize`）就是 460800，
v18 的 `want` 只是打印错、并让补齐多写，用户看到的条带另有原因。

### 真相（用本机 dump 量出来的）

```
isight-check/rawview.py   frame-raw-pid*.bin   → 把原始缓冲按 480 行渲染成 PNG
isight-check/slotprobe.py 同一份数据            → 判"一个音频事件占多少字节"
```

- 缓冲 = 恰好一帧：460800 字节 = 480 行 × 960 字节（一行 = 640px YUV411）。
- 音频占 **7 处 × 2 行 = 14 行**，落点在行边界（第 60、122、184、246、308、
  370、432 行，间隔 62 行），每处 **1920 字节 = 两个 960 字节 iso 包槽**
  （负载只填 1680，剩 240 是填充）。三个连续缓冲落点完全一致 → **无漂移**。
- 原始缓冲按 480 行渲染 = **一张连续正常的画面 + 14 行白色噪声条**，
  其余一行都不错位 ⇒ 这 14 行是被覆盖丢弃的，不是"插在字节之间"。
- 所以 v17~v19 "剪掉音频载荷再补齐"必然失败：每处只剪 1632 字节，
  留下 288 字节残渣 ×7 = **2016 字节卡在画面中间**，后面每一行都偏移了
  非整数行 —— 横向条带就是这么来的。

### v20 的修法

帧长一个字节都不动：扫出每个含音频字节的 960 字节槽，用**上一槽**覆盖它
（第二遍做，避免覆盖还没读到的字节）。

```c
const ULONG slot  = (m_width > 0) ? ((ULONG)m_width * 3 / 2) : 0;   // 一行
const ULONG slots = (slot >= 64 && slot <= cb) ? (cb / slot) : 0;   // 480
/* 第一遍：验证 sght 包时，把 [start,end) 覆盖到的槽在 dirty 位图里打标 */
/* 第二遍：dirty 的槽 = memmove(该槽, 上一槽, 960) */
```

- 入长 = 出长 ⇒ 不需要补齐、不需要夹紧、不可能越界；
- 行网格不动 ⇒ 其余 466 行与相机发出的逐字节一致，只有 14 行是"上一行的复制"；
- `want = FrameBytes()` 只留着做模式变化的告警，不再参与改写。

教训（都值得复用）：
1. **缓冲区改写，目标长度只能来自缓冲区自身（`cb`）**，不能来自"我以为这个模式多大"。
2. **别用日志数字推内存布局**——`rawdump=1` 存原始缓冲 + 渲染成图，十分钟定案；
   我按日志推了两轮，两轮都错。
3. **抓"同一构建、只差一个开关"的两份输出**（本机 dump 里
   `pid18884`=开音频 / `pid26988`=关音频）是最省事的 A/B。

## 装不上的原因：设备实例名传错了（2026-09-23 修）

首次在真机上装 v20 驱动包，卡在这一步：

```
INF: ...\isightmic.inf
  staged in driver store       C:\WINDOWS\INF\oem275.inf
  class                        Media
  create device info           FAILED (0xe0000205)
```

证书、驱动库暂存都过了，只有建节点失败，错误码 `0xE0000205`
（`SPAPI_E_INVALID_DEVINST_NAME`，SetupAPI 的错误，`FormatMessage` 查不到，
所以日志里是空的）。

**根因**：`SetupDiCreateDeviceInfo` 的 `DeviceName` 参数在带 `DICD_GENERATE_ID`
时**不是硬件 ID**，而是**裸的根枚举设备 ID** —— 不带 `枚举器\` 前缀，也不带
实例后缀（微软文档给的例子是 `*PNP0500`）。原来的代码传的是硬件 ID：

```c
SetupDiCreateDeviceInfoW(h, L"ISIGHTMIC\\Mic", ..., DICD_GENERATE_ID, &did);  /* 错 */
```

`ISIGHTMIC\Mic` 里有反斜杠，Windows 就把它读成"枚举器为 ISIGHTMIC 的设备实例
ID"，而系统里没有这个枚举器 → 非法实例名。硬件 ID 有反斜杠是正常的
（INF 的 `[Models]` 就写成 `ISIGHTMIC\Mic`），**只有这里这个参数不能有**。

**修法**（`tools/micdev.cpp`，tag `ISIGHT-MICDEV-BUILD-V21-20260923-ROOTID`）：

1. 候选表按序试，取第一个被接受的写法，并把选中的写进日志，不再靠猜：
   `"iSightMic"` → `"*ISIGHTMIC"` → `"Root\iSightMic"` → `"ISIGHTMIC\Mic"`。
2. 顺手把顺序改成 devcon 的做法：设 `SPDRP_HARDWAREID` → `DIF_REGISTERDEVICE`
   → `UpdateDriverForPlugAndPlayDevices`（让 PnP 自己挑驱动），失败才回落手
   动 `DIF_SELECTBESTCOMPATDRV` + `DIF_INSTALLDEVICE`。
3. 建完用 `SetupDiGetDeviceInstanceId` 打出真正生成的实例 ID。

**附带的一处连带错误**：`install-mic-v20.bat` 里验收查的是
`...\Enum\ISIGHTMIC` —— 那是按"枚举器 = ISIGHTMIC"想的。根枚举设备的节点在
`Enum\ROOT\...` 下面，所以这条即使装成功了也会报 `[X]`。已改成在
`Enum\ROOT` 下按硬件 ID 递归搜。

**怎么确认的**：非管理员跑 `SetupDiCreateDeviceInfo` 一律返回 `0x5`
（`ERROR_ACCESS_DENIED`），这个 API 要求 Administrators 组，所以在
普通权限下没法用"试名字"的办法定位，只能靠文档 + 错误码。见
`isight-check/probe_devname.py`（留档，需要在管理员下才有意义）。

## 装上了但用不了：子设备交给了 miniport，PortCls 要的是 port（2026-09-23 修）

上一节修好之后，设备节点建起来了、驱动也装上了，但系统里**看不到录音端点**：

```
实例 ID:      ROOT\ISIGHTMIC\0000
设备描述:     iSight Microphone (FireWire)
状态:         问题
问题代码:     10 (0x0A) [CM_PROB_FAILED_START]
问题状态:     0xC000000D
驱动程序名称: oem275.inf            <- pnputil /enum-devices /problem
```

`0xC000000D` = `STATUS_INVALID_PARAMETER`。`setupapi.dev.log` 里的同一件事：

```
dvi:  Start: ROOT\ISIGHTMIC\0000
!     Device 'ROOT\ISIGHTMIC\0000' not started:
      Device has problem: 0x0a (CM_PROB_FAILED_START), problem status: 0xc000000d.
```

**根因**：`StartDevice` 把 **miniport** 传给了 `PcRegisterSubdevice`：

```c
CMiniportWaveCyclic* w = new(...) CMiniportWaveCyclic(NULL);
wave = (PUNKNOWN)(IMiniportWaveCyclic*)w;
st = PcRegisterSubdevice(DeviceObject, (PWSTR)L"Wave", wave);   /* 错 */
```

而官方文档对第三个参数写得很死：

> `Unknown` — Pointer to the **IPort** interface of **the port driver object**
> that is bound to the subdevice.

PortCls 拿到对象后去 QI 它的 IPort，miniport 的 `QueryInterface` 回
`STATUS_INVALID_PARAMETER`（这是 PortCls 系列 miniport 表示"没这个接口"的惯例，
MSVAD 也这么写），PortCls 把这个 status 原样抛出来 → `StartDevice` 失败 →
一条子设备都没枚举出来 → 端点当然没有。

而 `\\.\IsightMicCtl` 一直是好的，因为控制设备是 `DriverEntry` 建的，
和这套装配流程完全无关 —— 这个"活着一半"的状态最容易把人带偏。

**修法**：按 "Subdevice Creation" 那一节把四件东西绑起来（port / miniport /
资源列表 / 引用字符串）：

```c
PPORT port = NULL;
NTSTATUS st = PcNewPort(&port, PortClassId);            /* CLSID_PortWaveCyclic */
if (!NT_SUCCESS(st)) return st;
st = port->Init(DeviceObject, Irp, Miniport, NULL, ResourceList);
if (NT_SUCCESS(st)) st = PcRegisterSubdevice(DeviceObject, Name, port);
port->Release();                       /* PcRegisterSubdevice 自己 AddRef 了 */
```

- `UnknownAdapter` 传 `NULL` 是允许的，文档原话："This pointer is optional and
  can be specified as NULL."
- `CLSID_PortWaveCyclic` / `CLSID_PortTopology` 在 portcls.h 里，配合已有的
  `#define INITGUID` 可直接用。
- 子设备名必须和 INF 里 `KSNAME_*` 的引用字符串一致（"Wave"/"Topology"），
  且那块缓冲区要活到设备对象销毁 —— 驱动 `.rdata` 里的字面量满足。

**DriverVer 必须跟着抬**：`1.0.0.0` → `1.0.1.0`。同版本号的包
`SetupCopyOEMInf` 会回"already staged"，**一个字节都不替换**，然后我们会拿着
旧 `.sys` 白测一轮。

### 这一轮暴露的方法问题：验收判据不够硬

`install-mic-v20.bat` 原来验收三条 —— 驱动文件落盘 / 设备节点在 /
服务项在。**这三条在驱动启动失败时照样全过**，所以它报了 `[OK] 虚拟麦克风已安装`，
而实际上端点根本不存在。从"装上了"到"能用"之间那段，是靠不住的。

现在加了第四条，也是唯一真正的判据：**`isight-micdev.exe list` 的输出里必须
出现 `iSight Microphone`**。失败时还会把
`pnputil /enum-devices /class Media` 和 `/enum-devices /problem` 追加进报告 ——
`/problem` 会用英文直接点名 `CM_PROB_FAILED_START` 和状态码，这次就是靠它一眼定案的。

顺带加了 `IOCTL_ISIGHTMIC_GETBUILD`：驱动自报构建标记
（`ISIGHTMIC-BUILD-V21-20260923-PORTCLS`），`miccheck` 会打印出来。
单独一个 IOCTL 而不是往 `ISIGHTMIC_STATUS` 加字段，是为了不把 DirectShow 滤镜
也拖进一次重建。

## 2026-09-24：设备正常、接口全在，却一个录音端点都没有

前两个 bug 修完（设备实例名、port/miniport）之后，设备**启动正常**了：
`ROOT\ISIGHTMIC\0000` 的 `ConfigFlags=0`、没有 `Problem` 键，
四个 `KSCATEGORY_*` 接口全部注册在 `DeviceClasses` 下，
`isight-micdev.exe list` 也把 `iSight Microphone (FireWire)` 列了出来。

但 "声音" 和微信里什么都没有：

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture
    共 21 个端点，其中 iSight 的 0 个
```

**关键区分**：`KSCATEGORY_*` 是 INF 里 `AddInterface` 注册的**内核流接口**，注册了
就一直在；"声音"面板/微信/WASAPI 读的是 `MMDevices\Audio\Capture` 下由
`AudioEndpointBuilder` 服务建出来的 **MMDevice 端点**。**接口注册 ≠ 端点存在**，
两个工具（`micdev list` 查 KS，`miccheck` 查 MMDevice）结论矛盾时就是这个原因。

### 定位手段：`isight-check/ksprobe.py`

枚举 `KSCATEGORY_AUDIO` 的每个过滤器，`CreateFile` 打开，对 pin 0/1 扫一遍
`KSPROPERTY_PIN_*`（`KSPROPSETID_Pin`）的 id 并打印原始字节。对照结果：

| 设备 | `id10 KSPROPERTY_PIN_PHYSICALCONNECTION` |
|---|---|
| Realtek 内置麦克风（真实硬件，有端点） | **270 字节**，`Size=0x10e Pin=0 "\??\HDAUDIO..."` |
| iSight 的每一个 pin | **完全没有响应** |

### 根因与修复

`StartDevice` 注册完两个子设备就结束了，**从来没调
`PcRegisterPhysicalConnection`**。PortCls 因此不知道 wave 过滤器的桥钉和
topology 过滤器的桥钉是硬连着的，`KSPROPERTY_PIN_PHYSICALCONNECTION` 无内容，
而 SysAudio / AudioEndpointBuilder 正是靠这个属性把两个过滤器配对成一张图 ——
配不出来，端点就不存在。

修复（`repo/drivers/isightmic/isightmic.cpp`）：

```c
// InstallSubdevice 现在把 port 交还给调用方（原来在函数里就 Release 了）
InstallSubdevice(..., L"Wave",     CLSID_PortWaveCyclic, wave, &wavePort);
InstallSubdevice(..., L"Topology", CLSID_PortTopology,   topo, &topoPort);

// 两个子设备都注册完之后，登记它们之间的硬连线。
// 方向：From = 数据源（输出钉脚）= topology 的桥钉 DATAFLOW_OUT
//       To   = 数据接收（输入钉脚）= wave 的桥钉     DATAFLOW_IN
PcRegisterPhysicalConnection(DeviceObject,
                             topoPort, KSPIN_TOPO_WAVE_BRIDGE,
                             wavePort, KSPIN_WAVE_BRIDGE);
```

`DriverVer` 1.0.1.0 → **1.0.2.0**，构建标记 `ISIGHTMIC-BUILD-V22-20260924-PHYSCONN`。

### 安装脚本随之改了两处

- **装之前先摘掉所有 `ROOT\ISIGHTMIC\*` 节点**。原来的工具用
  `DICD_GENERATE_ID`，每跑一次就新建一个实例，重启后会出现好几个同名麦克风；
  而且摘掉旧节点能让 Windows **当场卸载**旧驱动，新节点才可能立刻绑定新的
  `.sys`，省掉一次重启。
- **端点验收改成轮询**（最多 5 次、约 12 秒）。设备启动后音频服务建端点不是
  瞬时的，验收写死了会误报失败。

## 已知限制

- 需要测试签名（`testsigning`），Secure Boot 机器装不上
- 丢包 12.6% 是相机侧的确定性行为，主机侧已排除（视频尺寸、进程优先级、
  `pkt` 大小、`sample_total` 解析都试过，见 `isight-check/loss-experiments.md`）。
  由它派生的极轻微回声无法完全消除。
- 延迟目标 < 50 ms（采集约 5 ms/包 + DSP + 驱动缓冲）
