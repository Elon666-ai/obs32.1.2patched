# OBS 绝对时间戳协议 (obs-abs-timestamp-protocol v1)

本协议描述 OBS 客户端如何获取绝对时间、如何把它打进码流、以及 WHIP 推流时如何通过
WebRTC DataChannel 额外广播每帧时间戳，供服务端 / 播放器计算端到端推流延迟。

实现位置：

- NTP 对时：[`libobs/util/ntp-clock.c`](../libobs/util/ntp-clock.c) / [`ntp-clock.h`](../libobs/util/ntp-clock.h)
- SEI/OBU 打点：[`shared/abs-ts/abs-ts.c`](../shared/abs-ts/abs-ts.c) / [`abs-ts.h`](../shared/abs-ts/abs-ts.h)
- WHIP DataChannel：[`plugins/obs-webrtc/whip-output.cpp`](../plugins/obs-webrtc/whip-output.cpp)

## 1. 绝对时间来源 (NTP)

OBS 启动时（`obs_startup()`）会启动一个后台线程，对下列公网 NTP 服务器轮询取时：

```
time.cloudflare.com
time.windows.com
pool.ntp.org
```

- 每个服务器最多采样 4 次，取 RTT（往返时延）最小的一次结果，一旦某次采样 RTT < 30ms
  就提前结束采样。
- 换算方式是简化版 SNTP：假设网络时延对称，`当前 UTC 时间 ≈ 服务器 Transmit
  Timestamp + RTT/2`，不依赖本机墙钟做四时间戳公式。精度在正常网络下一般是个位数
  毫秒级，网络质量差时会变大，但不会导致崩溃或阻塞启动。
- 同步成功后，之后每 **30 分钟** 重新校准一次；如果从未同步成功，改为每 **30 秒**
  重试一次。
- 只要有一次同步成功过，之后即使所有服务器都不可达，也会继续使用上一次校准出的
  offset（而不是每次都退回系统时钟）。
- 如果 NTP 从未同步成功过（例如网络屏蔽了 UDP 123 端口），`ntp_clock_now_ms()` 会
  退回本机系统墙钟（未经校准），保证任何时候都有可用的时间戳，只是精度无法保证。

对外只有一个函数是消费方需要关心的：

```c
uint64_t ntp_clock_now_ms(void); // 自 1970-01-01 UTC 起的毫秒数
```

## 2. 码流内嵌时间戳 (SEI / OBU)

**范围**：所有推流输出（WHIP、RTMP 等），本地录制不打。

**频率**：每一帧视频都打（不是只在关键帧），编码器循环内通过
`obs_output_add_packet_callback()` 挂钩子，与具体输出协议无关。

**payload 格式**：固定 16 字节 UUID + 8 字节大端无符号毫秒时间戳，共 24 字节：

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 16 | UUID，固定为 ASCII 字符串 `"OBS-ABSTS-SEI-v1"` 的字节（见下方十六进制） |
| 16 | 8 | `timestamp_ms`，大端（网络字节序）无符号 64 位整数，`ntp_clock_now_ms()` 的返回值 |

UUID 的十六进制表示（也就是 `"OBS-ABSTS-SEI-v1"` 的 ASCII 编码）：

```
4F 42 53 2D 41 42 53 54 53 2D 53 45 49 2D 76 31
```

之所以直接用可读 ASCII 而不是随机 UUID，是为了在抓包 / hexdump 时能直接肉眼识别。

**按编解码器的封装方式**（和仓库里已有的 `shared/bpm` / 字幕注入完全一致的封装逻辑，
只是换了一个 payload）：

- **H.264 (AVC)**：封装成一条 `sei_type_user_data_unregistered`（SEI payload type 5）
  的 SEI NAL，以 4 字节 Annex-B start code (`00 00 00 01`) 追加在该帧已有 NAL 序列
  **之后**（不是关键帧专属的 SPS/PPS 之后，是紧跟在这一帧的 VCL 数据之后）。
- **H.265 (HEVC)**：封装成 **Suffix SEI**（NAL type = 40），同样是
  `user_data_unregistered`，以 3 字节 start code 追加在该帧数据之后。Suffix SEI 在
  规范里就是设计给"跟在它所属的 VCL NAL 之后"的，所以这个位置是标准合规的。
- **AV1**：封装成一个 metadata OBU，`metadata_type = 6`
  （`METADATA_TYPE_USER_PRIVATE_6`，AV1 规范里 6~31 是 user private 区间），同样
  追加在该帧的 OBU 序列之后。

解析建议：不管哪种编解码器，服务端 demux 出 NAL/OBU 之后，找到
`user_data_unregistered` SEI（或对应的 user-private OBU），比对前 16 字节是否等于
上面的 UUID，命中后读后续 8 字节大端整数即为该帧的推流时间戳（ms，UTC）。同一帧的
所有 simulcast 层（WHIP 情况下）都会各自带上这份时间戳。

## 3. WHIP WebRTC DataChannel 时间戳广播

仅 WHIP 推流时启用，作为码流内 SEI 时间戳之外的旁路通道，方便服务端不用解码/解析
码流也能拿到时间戳。

- **DataChannel label**：`"obs-timestamp"`，OBS 侧在建立 PeerConnection 时主动创建
  （`createDataChannel`），不需要服务端 `onDataChannel` 之外做任何应答；如果服务端
  不接这个 channel，OBS 侧发送只是静默失败（`isOpen()` 为 false 时直接跳过发送），
  不影响正常推流。
- **消息格式**：每一帧视频（每个 simulcast 层各发一条）发送一条 UTF-8 文本消息，
  内容是如下结构的 JSON：

  ```json
  {"frame_no": 1234, "timestamp": 1733500000123, "rid": "0"}
  ```

  字段说明：

  | 字段 | 类型 | 含义 |
  |---|---|---|
  | `frame_no` | uint16 (0~65535，会绕回) | 这一帧第一个 RTP 包在**该 simulcast 层自己的序列号空间**里的 RTP sequence number。和该层实际发出的 RTP 包的 `sequenceNumber` 字段是同一个计数器，只是取的是"这一帧起始"那个值 |
  | `timestamp` | uint64，毫秒 | `ntp_clock_now_ms()` 在发送这一帧时的返回值，和第 2 节码流内嵌的是同一个时钟源、同一时刻取的值 |
  | `rid` | string | simulcast 层标识，取值就是 RTP `rid` 扩展里用的 `"0"`、`"1"`、`"2"`……用来消歧：不同层各自独立计数，`frame_no` 在跨层时不是全局唯一的，必须配合 `rid` 才能对应到具体是哪个层的哪一帧 |

  **重要**：`frame_no` 不是全局递增的"第几帧"计数器，而是 RTP 序列号本身（每层从 0
  开始各自累加，一个编码帧可能被分片成多个 RTP 包，`frame_no` 取的是这些分片里
  **第一个**包的序列号）。服务端如果要用 `frame_no` 去关联具体收到的 RTP 包，用它
  匹配该分片序列里最小的那个 RTP sequence number 即可；如果服务端本来就要处理 RTP
  jitter buffer，这个字段可以直接复用已有的 RTP 序列号比对逻辑，不需要单独维护一套
  帧计数。

### 延迟计算示例

拿到某一帧在 SEI（或 DataChannel）里的 `timestamp`，用播放器本地收到 / 渲染这一帧
时的本地 UTC 时间（需要播放端也做 NTP 或至少用足够准的系统时钟）相减，就是端到端
推流延迟：

```
延迟(ms) = 播放端收到该帧的本地 UTC 毫秒时间 - 该帧携带的 timestamp
```

## 4. 版本 / 变更

- v1（本文档）：首个版本。UUID、payload 布局、DataChannel JSON schema 均为固定值；
  后续如需变更字段，请升级 UUID 或在 JSON 里加版本号字段，不要原地破坏性修改这份
  schema。
