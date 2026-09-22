# 音视频合并（v2.0 研究）

把 iSight 的麦克风和摄像头合成**一套东西**：程序只需要打开摄像头，
画面和声音都来自这台 2003 年的铝筒。

## 为什么现在能做：音频和视频走的是同一条流

之前麦克风一直被当成"第二条等时通道"来攻克，而 CMU 驱动**一个设备对象只给一条
等时流**（README「已知限制」里写的那堵墙），所以视频一跑起来音频就收不到。

`diags/audio.cpp`（v9–v12）把这堵墙拆掉了，实测结论是：

- iSight 的音频单元**不拥有自己的等时通道**——它发在**视频引擎那条通道上**，
  和视频包交错；v9 的 variant 3 一次抓到 1050 个 `sght` 音频包和**同一份 YUV 画面**。
- 把它挪到别的通道（v10 的五种变体）结果是**一个字节都没有**。
- 音频占到的比例只取决于视频让出多少总线周期：视频开着时约 43%，
  丢包是相机侧确定性的 1/8（每 67 ms 丢 2 个包），主机侧怎么调都降不下来。

推论很直接：**音频不需要第二条流**。谁持有那条唯一的等时流，谁就同时拥有画面和声音。
而 DirectShow 滤镜正好就是那个持有者——所以"合并"不是把两个设备捆在一起，
而是把 filter 已经拿到的那一份数据拆成两路用掉。

## 架构

```
   iSight（FireWire）
        │  一条等时流：DCAM 视频包 + 'sght' 音频包交错
        ▼
   1394cmdr.sys（CMU，官方签名，不改）
        │
        ▼
   iSightCam.ax  ← 唯一的采集者
        ├── video pin      → 640x480 RGB24/YUY2（已有）
        ├── audio pin      → 48 kHz / 16 bit PCM（v17）
        └── PCM → 共享内存 → isight-micsvc.exe → isightmic.sys → 系统麦克风（v18）
```

三个出口共用一个采集者，是因为那条等时流只有一个。好处：

- 音视频**天然同步**（同一个缓冲、同一个时钟，不需要再去对齐漂移）；
- 麦克风不额外占总线带宽；
- 不需要第二条 1394 通道，也就绕开了 CMU 驱动的限制。

### 出口 1：DirectShow audio pin（v17）

给 filter 加第二个 capture pin，`MEDIATYPE_Audio` / `MEDIASUBTYPE_PCM`。
支持它的宿主（OBS、GraphEdit、部分会议软件）一挂上摄像头就同时拿到麦克风。
缺点：微信 / QQ / 腾讯会议各自单独选麦克风（走 Core Audio），不保证会用这个 pin。

### 出口 2：虚拟麦克风设备（v18，`drivers/isightmic` + `service/feed.cpp` 已写好）

PortCls wave-cyclic 微型端口 + 用户态喂音服务。任何程序都能在麦克风列表里选到
"iSight Microphone (FireWire)"。代价：需要测试签名（本机已开 `testsigning`）。

### 出口 3：WAV 落盘（v16，已实现）

`[audio] wav=1` 时把解出的 PCM 写进 `%LOCALAPPDATA%\iSightAudio.wav`。
它既是取证手段（到底有没有声音、丢包多少），也是后面调 DSP 的素材来源。

## v16：先回答"能不能在视频路径里拿到音频"

v16 不碰 DirectShow 的 pin，只做两件事：

1. 采集起来之后，按 Linux 的顺序点亮音频单元
   （`SAMPLE_RATE=48 kHz` → `ISO_TX_CONFIG=视频通道|速率` → `AUDIO_ENABLE=1`）；
2. 每帧 `AcquireImageEx()` 之后扫描同一块采集缓冲，按 `sght` 重新同步，
   解出 `sample_count` / `sample_total` 和 16 bit 大端立体声样本。

日志会给出判断所需的一切：

```
audio: unit start -- channel=2 speed=2 (0x60C=0x...) rate=0 tx=0 gain=0 enable=0
audio: 100 packets, 20000 frames (0.4 s of audio in 2.1 s of video),
       lost 2800 frames (12.3%), peak 1002, 640 video frames scanned, 3 bad
```

要盯的三个数：**包数**（0 = 相机没发，或通道不对）、**lost%**（应落在 12% 左右，
与诊断工具一致就说明链路正常）、**peak**（几百以上才是真有声音，个位数是静音）。

### 风险：音频包会不会把画面搞坏 —— 会的，v17 已修复

v16 实测：画面出现 ~10 条水平错位缝 + 底部 11 KB 彩虹噪声带。原因不是带宽，
而是 **1394 内核驱动把音频 payload 当视频字节拼进连续 UYVY 流**
（`getRGB`/`getDIB` 从 `pFrameStart` 连续读 614400 字节，不做任何包分拣）。
每帧 ~14 个音频包 × 816 字节 ≈ 11 KB 视频被挤出缓冲。

v17 的修复不需要自收流：解码音频的同一遍循环里，把每个验证过的音频包
`[start, end)` 从缓冲中剔除并 `memmove` 前移压缩——剔除后缓冲前部正好是
完整的 614400 字节纯视频，画面和声音兼得。

- 关键坑：`GetRawFrameBuffer` 必须返回 `pFrameStart`（DMA 写入和视频转换
  都从这里读），返回 `pDataBuf` 会让每一刀都偏 1~4 KB；
- `[audio] strip=1` 默认开，`strip=0` 回 v16 行为，`enable=0` 仍是 v15 紧急开关。

回退永远是 `[audio] enable=0`：音频单元根本不点亮，视频路径与 v15 完全一致。

## 音频包格式（已验证）

```
struct {                       /* 16 bytes */
    __be32 sample_count;       /* 本包帧数                     */
    __be32 signature;          /* 0x73676874 == "sght"         */
    __be32 sample_total;       /* 累计帧数，用来算丢包          */
    __be32 reserved;
} header;
__be32 samples[];              /* sample_count * 2 通道, S16_BE, 48 kHz */
```

`isight-check/audio2wav.py` 就是按这个格式把抓包解成 WAV 的，音质链路
（PLC → 高通 → 门 → 美化，见 `docs/virtual-mic.md`）也是在它的输出上定型的。

## 里程碑

- **v16** 在视频采集路径里解出音频 + 落盘取证 ✅（音频链路全通：丢包 12.4%、peak 9433）
- **v17** 把音频包从画面里剔除（strip）+ audio pin：画面恢复干净、filter 直接交 PCM
- **v18** 出口 2：喂音服务 + `isightmic.sys`，全系统可用
- **v19** 实时 DSP（把 `docs/virtual-mic.md` 那张 8 步表移植进 C++）
