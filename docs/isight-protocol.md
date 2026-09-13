# Apple iSight（FireWire）协议笔记

本文件是逆向/实现过程中的权威依据汇总，来源：

1. **Apple 官方**：*The iSight Video Camera*（iSight Programming Guide，已从开发者站点下架，档案库仍有）
   `developer.apple.com/library/archive/documentation/Hardware/Conceptual/iSightProgGuide/`
2. **Linux**：`sound/firewire/isight.c`（Clemens Ladisch 逆向，唯一的上游音频实现）
3. **FreeBSD**：`sys/dev/firewire/fwisound.c`（2026-07 新增，注释中明确"protocol
   reverse-engineered by Clemens Ladisch"，即与 Linux 同源，但**完全不做 IRM 分配**）
4. 本机实测（`diags/audio.cpp` 探针，Windows + CMU 1394 驱动）

---

## 一、四个 Unit Directory（配置 ROM 实测）

`0xFFFFF0000400` 处的配置 ROM 里 root directory 有 4 个 unit directory，
实测解码（关键 0x40 = 单元偏移，单位 quadlet，基址 `0xFFFFF0000000`）：

| Unit | Unit_Spec_ID | Unit_Sw_Version | 0x40 条目 | 基址 | 用途 |
|---|---|---|---|---|---|
| Video | `0x00A02D` | `0x000102` | `0x4000` | `0xFFFFF0010000`（IIDC 寄存器在 `0xFFFFF0F00000`） | IIDC 1.30 摄像头 |
| Audio | `0x000A27` | `0x000010` | `0x8000` | **`0xFFFFF0020000`** | 麦克风 |
| Factory | `0x000A27` | `0x000011` | — | — | Apple 工厂测试用，官方明确"开发者不应访问" |
| **Iris** | `0x000A27` | `0x000012` | `0x080000` | **`0xFFFFF0200000`** | 光圈（镜头环）状态通知 |

> 视频单元是 IIDC 标准 + Apple 私有扩展；root directory 里 `Module_Vendor_Id = 0x080007`
> 且 `Model_Id = 0x000008` 同时存在，即可唯一确定是 iSight。

Apple 原文（介绍部分）：

> Although the audio and video portions of iSight are assembled into a single physical
> enclosure, they operate as **separate logical devices**. Each portion can be operated
> independently of the other.

> The iSight camera sends video packets on **one** FireWire isochronous channel and audio
> packets on **another** FireWire isochronous channel. These channels are allocated by a
> host computer, which then informs the iSight which channels to use. In any single
> FireWire cycle (1/8000 s) the iSight will send **one video packet, or one audio packet,
> or no packet at all --- but never both** in the same cycle. Consequently the most
> efficient isochronous allocation can be obtained by taking the larger of the audio and
> video packet sizes rather than by making independent allocations.

---

## 二、音频单元寄存器（基址 `0xFFFFF0020000`）

Apple 官方表 1-6，与 Linux/FreeBSD 的 `#define` 完全一致，本机实测 11 个寄存器全部应答：

| Offset | 名称 | 读写 | 实测值 | 说明 |
|---|---|---|---|---|
| `0x000` | AudioEnable | W | `0` | 写 `0x80000000` 开启音频发送，`0` 关闭 |
| `0x204` | Default Audio Gain | R | — | 默认增益 |
| `0x210` | Gain: Raw Start | R | `1` | `0x500` 的原始值下界 |
| `0x214` | Gain: Raw End | R | `43` | 原始值上界 |
| `0x218` | Gain: Decibels Start | R | `-30` | 有符号 32 位 |
| `0x21C` | Gain: Decibels End | R | `+12` | 有符号 32 位 |
| `0x280` | Sample Rate Inquiry | R | `0xF0000000` | 采样率位图 |
| `0x300` | Isochronous TX Config | W | `0x00020016` | `通道 + (速度索引 << 16)`，出厂值为通道 22/S400 |
| `0x400` | Sample Rate | W | `0x80000000` | 写 `0x80000000` = 48 kHz |
| `0x500` | Gain | R/W | `31` | 范围见 0x210/0x214 |
| `0x504` | Mute | W | `0` | 写非 0 静音（但**不停止**等时流） |

## 三、音频包格式（表 1-7/1-8）

```
0x000  1394 包头发送（含长度与通道号）        <- OHCI 接收时已去掉
0x004  包头 CRC                              <- OHCI 接收时已去掉
0x008  Audio Sample Count                    <- 本包样本数
0x00C  Audio Signature = "sght" (0x73676874) <- 用来识别音频包
0x010  Audio Sample Total                    <- 之前所有样本累计（用于检测丢包）
0x014  Reserved
0x018+ Audio Data                             <- 双声道，每样本 4 字节
```
- 每个样本 4 字节 = 左声道 int16 + 右声道 int16，**大端**。
- 一包最多 `475` frames（Linux `MAX_FRAMES_PER_PACKET`）= 1900 字节数据，整包 ≈1916 字节。
- 采样率固定 48 kHz / 16 bit / 立体声。
- 音频以**大块**发送，填在视频包的"空隙周期"里；选 640×480 YUV 4:1:1 30fps 时，音频包约为其他模式的
  2 倍大、1/2 数量。

## 四、视频单元（`0x00A02D/0x000102`）关键寄存器

基址 `0xFFFFF0F00000`，IIDC 1.30 + Apple 扩展。CMU 驱动把它当"相对偏移 0x000"的起点：

| Offset | 名称 | 说明 |
|---|---|---|
| `0x600` | FRAME_RATE | 位 31-29 为帧率档 |
| `0x604` | VIDEO_MODE | 位 31-29 为模式档 |
| `0x608` | VIDEO_FORMAT | 位 31-29 为格式档 |
| `0x60C` | **ISOCHRONOUS_DATA_CHANNEL** | 1394a：通道在位 31-28、速度索引在位 27-24；1394b（位 15 置位）：通道在位 13-8、速度在位 3-0 |
| `0x614` | **ISO_ENABLE** | 写 `0x80000000` 启动视频发送，写 `0` 停止 |
| `0x800-0x8FC` | IIDC 关键寄存器 | 曝光等；Apple 在 `0xD00-0xDFC` 提供只读默认值副本 |
| `0xFBC` / `0xFC0` | Edge Enhancement Inquiry / Edge Enhancement | Apple 私有 |
| `0xF00` / `0xF10-0xF24` / `0xF90` | CIF 模式查询与选择 | Apple 私有 CIF 格式（128×96 / 176×144 / 352×288） |

> 官方提示：上电时 iSight 会把曝光设为"已知良好值"；主机驱动**不应**擅自改动，需要时可从
> `0xD00-0xDFC` 只读副本恢复。

## 五、Iris 单元（`0x000A27/0x000012`）—— 关键安全机制

| 项 | 值 |
|---|---|
| Iris Status Address 寄存器地址 | `0xFFFFF0200000`（CSR 基址 + `0x200000`） |
| 访问方式 | **只写**，必须用**单次 8 字节块写** |
| 内容 | 主机侧 64 位 1394 地址（总线号通常 `0x3ff`，主机内存地址 ≥ `0x0001.0000.0000`） |
| 状态回报 | 立即回一次（确认生效 + 当前状态），之后状态变化时再送；quadlet write，`1`=光圈开，`0`=光圈关 |
| 总线复位 | 该寄存器**被复位作废**，且相机不跟踪主机 node id 变化 → 复位后主机必须重写 |

**安全机制（这条最重要）**：

> When a host has stored an address in this register, iSight will accept IIDC and audio
> **register writes only from the host node ID** stored in the register. Writes from other
> nodes will be rejected with `response_conflict_error`. ... If, after a bus reset, no host
> has stored an address in this register, then writes to IIDC and audio registers are
> accepted from any FireWire node.

**电源/流开关（Apple 原文）**：

> Closing the iris turns off isochronous audio and video streams. The FireWire interface
> remains fully active, except that it will not send any isochronous data until the iris is
> opened and the data flow is re-enabled by a host computer.

Iris 状态（macOS 侧属性值）：`pending` / `open` / `closed` / `login failed`
（最后一个 = 总线上已有别的节点取得控制权）。

## 六、Linux / FreeBSD 的启动序列（"标准答案"）

Linux `isight_start_streaming()`：

```
1. write REG_SAMPLE_RATE (0x400) = 0x80000000
2. isight_connect():
     ch = fw_iso_resources_allocate(&resources, sizeof(struct audio_payload)=1916,
                                    device->max_speed)      <- 经 IRM 分配通道+带宽
     write REG_ISO_TX_CONFIG (0x300) = ch | (max_speed << 16)
3. write REG_AUDIO_ENABLE (0x000) = 0x80000000
4. fw_iso_context_create(RECEIVE, resources.channel, ...) + queue 20 packets + start
```

FreeBSD `fwisound_iso_start()`（**不做任何 IRM 分配**，固定通道 `iso_channel`，默认 1）：

```
1. 挂载时已经 write REG_SAMPLE_RATE (0x400) = 0x80000000
2. fw_open_isodma() + 设置接收 DMA 的通道 tag = iso_channel
3. write REG_ISO_TX_CONFIG (0x300) = iso_channel | (speed << 16)
4. write REG_AUDIO_ENABLE (0x000) = 0x80000000
5. 启动 IR DMA
6. 收到的包：len >= 20 且 signature == "sght"，sample_count*4 <= len-16 才算有效
```

两条实现都验证了同一结论：**相机不需要 IRM 预留就能开始发音频**（FreeBSD 那条尤其干净）。

补充（2026-09-13 复核 Linux 源码）：

- `isight_probe()` 阶段**不写任何寄存器**；`REG_GAIN(0x500)` / `REG_MUTE(0x504)` 只在用户空间调
  mixer 时才写，**启动音频前不需要碰**；`REG_DEF_AUDIO_GAIN(0x204)` 定义了但整份驱动从未使用。
- 因此"启动音频"的最小寄存器集合就只有三个：`0x400` → `0x300` → `0x000`。我们写的完全一样。
- Linux 对 `0x300` 的打包是 `ch | (max_speed << 16)`（**通道在低 16 位、速度在高 16 位**），
  并且这次写用 `FW_FIXED_GENERATION | generation` 锁代次：**总线复位会作废这次配置并整段重来**。

## 七、本机实测状态（2026-09-13）

- 音频单元定位成功，11 个寄存器应答且数值与官方表一致 → 协议层无悬念。
- **IRM 假设已排除**：IRM 寄存器（`0x220/0x224/0x228`）通过 CMU 可读（4915 单位、32 通道全空闲），
  但不可写；而 FreeBSD 实现根本不做这一步照样能收流。
- **v8 纠正了一条被当真的错误结论**：v7 把"传显式通道 → 回填 `0xFFFFFFFF`"解释成
  CMU 的 `ACQ_SUBSCRIBE_ONLY` 语义（"不分配，只按你说的通道接收"）。这个解释**没有被任何证据支持**——
  CMU 的 subscribe 分支靠 `m_AcquisitionFlags & ACQ_SUBSCRIBE_ONLY` 走，而那个 flag 根本没经
  `IOCTL_ISOCH_SETUP_STREAM` 传给驱动（`ISOCH_STREAM_PARAMS` 里**没有 flag 字段**，只有
  `nMaxBytesPerFrame` 高位偷了个双包标志）。所以 `0xFFFFFFFF` 更可能就是**分配失败**。
  v8 因此改问一个能被检验的问题：**让驱动自己分配通道**（`vidlisten auto`），
  这才是唯一被证实可用的路径。

## 八、接收通路自检（v8 的设计）

在断言"相机不发流"之前，必须先证明**我们能收**。自检用相机自己的 DCAM 视频引擎当已知发射源：

1. `vidlisten auto 4 4096` —— 让驱动**分配**通道，再把该通道写进相机 `0x60C`、`0x614=0x80000000`，
   听 4 秒。这是与 `isight-diag.exe`（实测 90 帧 / 0 超时）**同构**的路径，必须收到字节。
2. `vidlisten 3 4 4096` —— 显式通道对比组。若 1 成功而 2 失败，则"显式通道 = 订阅"不成立，
   结论是**驱动不支持订阅**；此时音频实验必须走"分配 + 告诉相机"这条路（`cap <base> auto` 正是如此）。
3. `cap 0xF0020000 4 auto` —— 四段递进，最后一段是"**先**把视频引擎拉起来、再开音频"，
   因为 Apple 明确说音频包只填视频包的**空隙周期**，音频时钟很可能来自视频引擎。

音视频**永远用不同等时通道**；而 CMU 驱动**一个设备对象只允许一条等时流**，
所以音频实验期间我们只收音频通道、不试图同时收视频。

## 九、v9 的决定性发现：接收**缓冲几何**（不是相机的问题）

这是整件事里最容易被误判的一处。`1394cmdr` 驱动对缓冲参数的容忍度是**假的**：

| 缓冲配置 | 驱动返回 | 自检报告 pending | 实际收到 |
|---|---|---|---|
| 1 × 131072 字节，`nNumberOfBuffers=2` | `success` | 8 个 pending | **0 字节** |
| 8 × 460800 字节，`nNumberOfBuffers=9` | `success` | 8 个 pending | **9 完成 / 4147200 字节** |

也就是说：**驱动会"成功"地接受一个永远收不到数据的配置**。之前所有"相机不发流"的结论
全部作废——真正的原因是我们在用错误的几何收。

v9 因此完全照抄 `C1394Camera::StartImageAcquisitionEx(8, 2000, ACQ_START_VIDEO_STREAM)`
的参数：`InitResources()` 里 `nNumberOfBuffers = (acquisitionBuffers × subBuffers) + 1`，
每个 acquisition buffer 是**一整帧** `640×480 YUV 4:1:1 = 460800` 字节，
`dc1394BuildAcquisitonBuffer(460800, dmaMax, 960, idx)`；attach 全部 8 个后 `Sleep(50)`，
再逐个确认仍是 `ERROR_IO_INCOMPLETE`（CMU 把"已 ready"当成硬失败），最后才 `t1394IsochListen`。

**修复后立刻收到音频**：`sght` 包 1050 个 / 210000 帧 / 4.38 秒，48 kHz 16 bit 立体声大端，
**非静音**（peak 1002、rms 208）。这是这块 iSight 麦克风第一次在 Windows 上出声。

但只有约 **43%** 的帧：dump 里 56% 是 YUV 视频像素块。原因见下节。

## 十、v9 为什么"半成功"：`-1` 是 `VID_SAME`，不是 `VID_OTHER`

v9 variant 3 给视频通道传的是 `-1`，本意"另一个通道"，但宏定义是：

```c
#define VID_SAME  (-1)   // 用我们自己的接收通道
#define VID_OTHER (-2)   // 用另一个通道
```

于是**音频和视频都被放到了同一个接收通道 0**（日志：`0x60C = 0x02000000 (video channel 0,
we listen on 0)`，与相机出厂值相同、写等于没写）。相机每个 1393 周期只能发一个包，
两个流只能交替 → 音频丢一半。

## 十一、v10 把结论整个反过来了，v11 因此改成"一次只变一个变量"

v10 严格按 Apple 的说明"音视频必须用不同通道"，把视频引擎搬到通道 1
（`0x60C = 0x12000000`，读回确认写入生效），结果**五个 variant 全部 0 字节**，
连 `0x614` 读出 `0x80000000`（streaming）的那两个也不例外。

所以 v10 的前提至少有一个方向是错的，剩下两种解释无法靠读文档区分：

- **(A) 音频单元遵守 `0x300`**：v9 成功只是因为"我们告诉它的通道"和"视频通道"**恰好同一个数**。
  那么把视频留在出厂通道 0、把音频告诉通道 1、我们在通道 1 上收，就能拿到
  **完全不含视频的干净音频**。
- **(B) 音频单元忽略 `0x300`，永远跟着视频单元走**（Apple 说音频时钟派生自视频引擎，
  这一条与 (B) 一致）：那么路由权不在我们手里，**唯一可用的旋钮是带宽**——
  降视频帧率、换更小的 format/mode，因为音频只填视频剩下的周期。

v11 的 `cap` 因此变成**单配置模式**：命令行给任意一个
`ach=N` / `vidch=same|other|N` / `vid=0|1` / `aud=0|1` / `vrate=N` / `fmt=N` / `mode=N`
（或裸词 `one`），就只跑**一次**、只测一个假设，不再"逐段升级、遇到第一个出字节的就停"
（那种写法会把若干设置混在一起——v9 的"胜出配置"正是被 `-1` 误解出来的）。

两条判决性命令：

```
isight-audio.exe cap 0xF0020000 12 1 one vidch=0 ach=1   # (A) 音频告诉 ch1，视频留 ch0，我们在 ch1 收
isight-audio.exe cap 0xF0020000 12 one vrate=1           # (B) v9 原配置，视频帧率降到 3.75 fps
```

顺带修掉一个静默陷阱：`cap` 的位置参数现在**扫描**而不是按索引取。
以前 `cap <base> 12 one` 会把 `one` 喂给 `atoi()` 得到 0，**静默变成"显式通道 0"**——
和"自动分配"是完全不同的实验（见 §七：显式通道的回填不可信）。

`cap` 的递进路径也收缩成三段，终点就是 v9 的配置（视频在**我们**的通道上）：
v10 已经证明，**把视频通道搬走会直接掐死整条流**，那不是可用的方向。
