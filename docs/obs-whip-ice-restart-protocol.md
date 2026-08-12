# WHIP ICE Restart 协议设计 (obs-whip-ice-restart-protocol v1)

**状态：设计稿，尚未实现。** 本文档只定义协议和分工，不包含代码改动。

## 0. 背景

[whip-output.cpp](../plugins/obs-webrtc/whip-output.cpp) 目前已经上线的止血措施是给
`PeerConnection::State::Disconnected` 加了 `WHIP_DISCONNECT_GRACE_SEC = 10` 秒宽限期
（`StartDisconnectGraceTimer()` / `CancelDisconnectGraceTimer()`），libdatachannel 自己的
ICE agent 会在这 10 秒内悄悄重试；如果 10 秒内没能自愈，或者状态直接变成 `Failed`，
现在的行为是整条 WHIP 会话重来一遍：

```
DELETE <resource_url>          -- 通知服务端释放旧会话
关闭 PeerConnection             -- 销毁 ICE agent + DTLS 关联 + 所有 track/DataChannel
POST <endpoint_url> (新 SDP offer) -- 全新 ICE ufrag/pwd + 全新 DTLS 证书协商
ICE gathering + connectivity check
DTLS handshake
SRTP 密钥派生
（simulcast: 各层 track 重新 addTrack）
```

这一整套下来通常是几百毫秒到几秒的量级（取决于 ICE candidate 收集和 DTLS 往返次数），
而且期间画面完全中断。本文档定义的 **ICE restart** 是 WHIP 规范（RFC 9725）里定义的
标准机制：只重新协商 ICE 层（新的 ice-ufrag/ice-pwd + 重新做 connectivity check），
**复用同一个 DTLS 关联、同一套 SRTP 密钥、同一批 track/SSRC/DataChannel**，把中断时长
从"整条会话重建"压缩到"ICE candidate 重新提名"，理论上可以做到无需重新握手 DTLS，
观感上是一次几十到几百毫秒的卡顿而不是整条流断线重连。

## 1. 触发条件与状态机

ICE restart 和现有的 10 秒宽限期不是二选一，是接力关系：

```
Connected
   │
   │ PeerConnection state → Disconnected
   ▼
[10s 宽限期计时，libdatachannel 自愈中]
   │                              │
   │ 10s 内恢复 Connected          │ 10s 耗尽仍是 Disconnected
   │ (什么都不用做)                 │ 或状态直接变成 Failed
   ▼                              ▼
Connected                    [尝试 ICE restart]（本文档新增）
                                   │
                    ┌──────────────┴──────────────┐
                    │ 成功                          │ 失败 / 超时 / 服务端不支持
                    ▼                              ▼
                Connected                    [现有的全量重建路径]
                                              (DELETE + 新 POST + 全新协商)
```

也就是说：**10 秒宽限期到期后，不要立刻走全量重建，先尝试一次 ICE restart；ICE
restart 本身也要设超时（建议 5 秒），失败了再退回现有的全量重建逻辑**。全量重建
永远是兜底路径，ICE restart 是"尽力而为"的优化，不改变现有的失败语义。

同一次故障只尝试 **一次** ICE restart，不递归重试；如果 ICE restart 之后短时间内
（建议 30 秒内）又掉线，直接走全量重建，不再多花时间尝试第二次 ICE restart——这是
为了避免"restart 本身也在一个坏透的网络上必然失败"的场景下重复浪费时间。

## 2. 线路协议（RFC 9725 定义，OBS/MMX 都要照此实现）

WHIP 规范里 trickle ICE 和 ICE restart 用的是同一个机制：对 `resource_url`
（也就是 `Connect()` 里从 `Location` 头拿到、`resource_url` 成员保存的那个地址）
发 `PATCH`，`Content-Type: application/trickle-ice-sdpfrag`。

- **trickle 一个新 candidate**：body 是 SDP 片段，带 `a=candidate:...` 行。
- **ICE restart**：body 是 SDP 片段，带**新的** `a=ice-ufrag` / `a=ice-pwd`（凭这两个
  值和已有 SDP 里的不一样，服务端识别出"这是要求 restart 而不是普通 trickle"）。

请求示例（OBS 发起）：

```
PATCH /whip/resource/abc123 HTTP/1.1
Host: mmx.example.com
Content-Type: application/trickle-ice-sdpfrag
Authorization: Bearer <bearer_token>

a=ice-ufrag:F7gI
a=ice-pwd:x9cml/YzichV2+XlhiMu8g
```

服务端（MMX）的响应：

- **支持并接受**：`200 OK`，`Content-Type: application/trickle-ice-sdpfrag`，body 是
  服务端**自己新生成**的 ice-ufrag/pwd：

  ```
  HTTP/1.1 200 OK
  Content-Type: application/trickle-ice-sdpfrag

  a=ice-ufrag:9xY2
  a=ice-pwd:8hd93ycMzo1tRnAj7Kf3Za
  ```

  OBS 拿到这个响应后，把这组新凭据设进本地的 remote ICE 参数，双方各自用新
  ufrag/pwd 重新做 connectivity check；DTLS fingerprint 不变，不需要重新握手。

- **不支持 ICE restart**：`501 Not Implemented`（或任何非 2xx）。OBS 收到后直接放弃
  ICE restart，走现有全量重建路径，不重试。

- **资源已不存在**（比如服务端那边已经把这次 session 当死会话清理掉了）：`404 Not
  Found` / `410 Gone`。同样直接走全量重建。

具体字段语法请以当前发布的 RFC 9725 文本为准，上面是按规范内容整理的示意，实现前
建议双方对照 RFC 原文核对一次，不要仅凭本文档字节对字节实现。

## 3. OBS 侧需要做的事

### 3.1 已知的关键阻塞点（先看这个，决定能不能做）

当前仓库里 vendor 的 libdatachannel 版本是 **0.21.0**
（[version.h](../.deps/obs-deps-2025-08-23-x64/include/rtc/version.h)），它的
`rtc::PeerConnection` 公开 API（[peerconnection.hpp](../.deps/obs-deps-2025-08-23-x64/include/rtc/peerconnection.hpp)）**没有** `restartIce()` 或
`setLocalDescription(..., iceRestart=true)` 这类入口——`setLocalDescription()` 只接受
`Description::Type`，没有"重新生成 ICE 凭据"这个参数；ICE ufrag/pwd 是
libjuice（libdatachannel 内部用的 ICE 库）在 ICE transport 创建时生成的，没有暴露
"在不销毁 transport 的前提下重新生成"的公开接口。

所以在动 OBS 端代码之前，**必须先确认下面三条路里哪条能走通**，这是这个方案最大的
不确定性来源：

1. **升级 libdatachannel**，看新版本是否加了 ICE restart 支持（需要去查
   libdatachannel 上游的 changelog/issue，本文档写作时没有联网核实最新情况，需要
   实现前专门确认一次）。如果新版本支持，直接用官方 API，风险最低。
2. **给 libdatachannel 打补丁**：research 一下 libjuice 底层是否已经具备"在同一个
   ICE agent 上更换本地 ufrag/pwd 并重新触发 connectivity check"的内部能力，只是没
   往上透出 C++ API；如果有，写一个薄的补丁层暴露出来，走仓库现有的
   vendor/patch 流程去打包。工作量和风险中等，但能保留同一个 DTLS 关联，是"真"
   ICE restart。
3. **退而求其次，"轻量重建"**：不追求真正复用 ICE transport，而是复用 track/编码器
   状态、跳过 WHIP 的 DELETE+POST 往返（因为服务端已经通过 PATCH 知道要 restart 了，
   不需要整个资源重新创建），只重新走一次 PeerConnection 创建 + ICE + DTLS，但省掉
   HTTP DELETE/POST 这一段 round trip 和 WHIP 资源生命周期管理的开销。这不是真正的
   "只重新协商 ICE"，DTLS 还是要重新握手，收益比方案 1/2 小很多，但实现成本最低、
   不依赖 libdatachannel 改动。

如果 1、2 都不可行，本协议的价值退化为"跳过 DELETE+POST 往返"（方案 3），仍然优于
现状，但达不到"完全不中断媒体"的理想效果。**建议先花小半天时间调研方案 1/2 的可行性
再决定要不要投入实现**，不要直接假设能做到"真"ICE restart。

### 3.2 假设方案 1 或 2 可行时，OBS 侧的具体改动

- `WHIPOutput` 新增一个方法，大致对应现有 `Connect()` 的角色，但走 `PATCH` 而不是
  `POST`：

  ```cpp
  bool AttemptIceRestart(uint64_t generation);
  ```

  在 `StartDisconnectGraceTimer()` 的超时分支里，先调用它，只有它返回 `false` 才继续
  现在的 `MarkDisconnected() + Stop(false) + obs_output_signal_stop(...)` 全量重建路径；
  `Failed` 分支同理，先尝试一次 `AttemptIceRestart()`。

- `AttemptIceRestart()` 大致流程（复用 `Connect()` 里已有的 curl 基础设施）：
  1. 生成一组新的本地 ICE ufrag/pwd（具体 API 取决于 3.1 选的方案）。
  2. 构造 `application/trickle-ice-sdpfrag` 格式的 PATCH body，`curl_easy_setopt`
     `CURLOPT_CUSTOMREQUEST = "PATCH"`，URL 用现有的 `resource_url` 成员。
  3. 超时建议 5 秒（比现有 `Connect()` 用的 8 秒短，因为 ICE restart 应该比首次建连
     快，等太久不如直接走全量重建）。
  4. 200 且 body 合法：解析出服务端新 ufrag/pwd，设进本地的 remote ICE 参数，等
     `onStateChange` 重新回到 `Connected`（复用现有回调，`MarkConnected()` /
     `CancelDisconnectGraceTimer()` 不用改）。
  5. 非 200（含超时/网络错误/501/404/410）：返回 `false`，调用方走现有全量重建。

- **不要**在 `AttemptIceRestart()` 里碰 `active_generation`、`videoLayerStates`、
  `audio_track`/`video_track`/`timestamp_channel` ——这些都应该原样保留，这正是
  ICE restart 相对全量重建的意义所在。

- 需要新增一个"这次是不是 restart 尝试中"的状态位（类似现有的
  `disconnect_grace_*` 那一组成员），避免 `AttemptIceRestart()` 还没返回时，
  `onStateChange` 又并发触发另一次 restart 或全量重建；具体做法可以参考现有
  `disconnect_grace_cancel` + `condition_variable` 的写法，或者更简单地用一个
  `std::atomic<bool> ice_restart_in_progress`。

- 日志：每次 restart 尝试、成功、失败都要打 `do_log(LOG_INFO, ...)`，方便后续和
  第 2 项（getStats/丢包证据）的排查对上时间线。

### 3.3 和现有 primary/backup failover（`ShouldFallback`）的关系

不冲突，但顺序要理清：ICE restart 只在**同一个 origin（同一个 WHIP endpoint）**内部
尝试恢复；`ShouldFallback()` 的 30 秒计时是"连续失败多久该换到备用服务器"。建议
`fail_since_ns`/`fail_since_set` 的计时起点不因为"正在尝试 ICE restart"而重置——也就是
说，`MarkDisconnected()` 仍然在进入 `Disconnected`/`Failed` 时正常调用，ICE restart
只是在全量重建之前多插一步尝试，不影响 failover 计时器的语义。如果 ICE restart
反复失败导致本地一直没有真正调用到 `Stop(false)`（因为每次都在 restart 分支里
提前 return），需要确保 `fail_since_ns` 计时依然在跑，否则会导致该切备用服务器时
切不过去——具体实现时这是一个容易漏掉的点，务必测试到。

## 4. MMX 侧需要做的事

MMX 用 Pion（`ppmmx/internal/protocols/webrtc/peer_connection.go`），Pion 从 v3 起
`webrtc.PeerConnection` 原生支持 `RestartICE()`（重新生成本地 ICE 凭据，下一次
`CreateAnswer`/`SetLocalDescription` 会带上新凭据），比 OBS 侧的处境好得多，不存在
libdatachannel 那种"库不支持"的阻塞。具体方法名请对照当前 vendor 的 Pion 版本文档
核实一次。

### 4.1 WHIP 资源的 PATCH 处理器

MMX 现在的 WHIP HTTP 层应该已经在处理 trickle candidate 的 PATCH（如果还没有，这个
是前置依赖，要先做）。在这个处理器里新增分支：

1. 解析收到的 `application/trickle-ice-sdpfrag` body。
2. 如果 body 里带的是 `a=candidate` 行 → 走现有 trickle candidate 逻辑，走
   `AddICECandidate`。
3. 如果 body 里带的是 `a=ice-ufrag` / `a=ice-pwd`，且和当前记录的（上一次协商用的）
   ufrag/pwd **不同** → 识别为 ICE restart 请求，走下面 4.2 的流程。
4. 其他情况（比如 ufrag/pwd 和当前一致，纯粹重复请求）→ 直接 `200 OK` 空响应，
   幂等处理，不重复触发 restart。

### 4.2 执行 restart

1. 用收到的新 ufrag/pwd 构造一个最小 SDP，调用 Pion 的 `SetRemoteDescription`（或
   Pion 是否需要先显式调用 `RestartICE()` 再 `SetRemoteDescription`，取决于 Pion
   版本的具体语义，实现时需要查一下当前 vendor 版本的行为——有的版本是
   `SetRemoteDescription` 探测到 ufrag 变化会自动触发 restart，不需要显式调用）。
2. 生成本地新的 answer（带新的本地 ufrag/pwd），从中提取出
   `a=ice-ufrag`/`a=ice-pwd`，拼成响应 body。
3. 返回 `200 OK`，`Content-Type: application/trickle-ice-sdpfrag`，body 是上一步提取
   出的凭据。
4. DTLS/SRTP：**不要**重新走证书协商或密钥派生，Pion 在只做 ICE restart（没有
   `a=fingerprint` 变化）时应该自动保留现有 DTLS 关联；这是需要专门写单测/集成测试
   验证的点，不能想当然。
5. 现有的 media track、RTP 接收状态（jitter buffer、SSRC 映射）不动。

### 4.3 超时与并发保护

- 给这个 PATCH handler 设一个处理超时（建议 3~5 秒），超时或内部出错就回
  `500`/`501`，让 OBS 那边走全量重建，不要让 OBS 卡死在等 restart 响应上。
- 同一个 resource 同时只处理一个 restart 请求，用现有 session 对象上的锁/状态位
  防止并发 PATCH 把 ICE agent 状态搞乱。

### 4.4 不支持时的显式降级

如果决定分阶段上线（比如先只上 MMX 侧，OBS 侧还没实现，或者反过来），未实现的一侧
必须让另一侧能明确识别到"不支持"从而走回全量重建，不能让请求卡住或返回歧义状态：

- MMX 没实现：收到未知的 PATCH body（不是合法 candidate 也不是合法
  ufrag/pwd 格式），返回 `501 Not Implemented`。
- OBS 没实现：不会发起 PATCH，MMX 侧该功能自然不会被触发，无需额外处理。

## 5. 边界情况 / 需要专门测试验证的点

- **DataChannel（`obs-timestamp`）能不能扛过 ICE restart**：DataChannel 建立在
  SCTP over DTLS 之上。理论上 DTLS 关联不变的话 SCTP 关联也应该能继续用，但这是
  libdatachannel/Pion 各自 SCTP 实现的具体行为，不是 JSEP 规范强保证的东西，
  **必须实测**：ICE restart 完成后立即检查 `timestamp_channel->isOpen()`，如果发现
  实测中 DataChannel 没能存活，需要补一个"ICE restart 成功后重新创建
  DataChannel"的分支（这个比重建整个 PeerConnection 便宜得多，可以接受）。
- **RTP 序列号/SSRC 连续性**：ICE restart 不涉及 `videoLayerStates`，序列号应该
  是连续的（没有像全量重建那样清零/重新分配 SSRC），这对
  [obs-abs-timestamp-protocol.md](obs-abs-timestamp-protocol.md) 里
  `frame_no`（RTP 序列号语义）是好事——不会因为一次 ICE restart 就出现 frame_no
  从某层的计数器重新从 0 开始的情况，服务端如果拿 frame_no 做时间序列分析不用
  特殊处理"restart 导致计数器归零"这种情况。
- **Simulcast**：多层视频各自的 track 都要在 restart 后确认还能正常收发；OBS 侧
  `videoLayerStates` 不会被 ICE restart 触碰，理论上没问题，但要连同多层一起测，
  不能只测单层配置。
- **和 SEI/OBU 时间戳注入的关系**：[shared/abs-ts](../shared/abs-ts/abs-ts.c) 挂在
  `obs_output` 层的 packet callback，和 WHIP/ICE 层完全解耦，ICE restart 不会影响它。

## 6. 测试计划

1. **单元/集成测试**（各自代码库内）：
   - OBS 侧：模拟 `Disconnected` 状态触发、验证 `AttemptIceRestart()` 在
     mock/本地信令服务器上能正确发出 PATCH 并解析 200 响应。
   - MMX 侧：对 PATCH handler 做单测，覆盖"合法 restart 请求"、"重复请求
     （ufrag/pwd 不变）"、"格式非法"、"resource 不存在"几种输入。
2. **本地故障注入**：在 OBS 和 MMX 之间人为制造网络中断（`tc netem` 丢包/延迟，
   或者直接短暂 `iptables DROP` 相关端口几秒），验证：
   - 短暂中断（< 10s）：现有宽限期机制生效，什么都不用做。
   - 中等中断（10s ～ 数十秒，取决于恢复速度）：触发 ICE restart，验证画面恢复
     时间明显短于全量重建（用现有的连接耗时日志 `connect_time_ms` 和新增的
     restart 耗时日志对比）。
   - 长时间/彻底中断：验证最终正确回退到全量重建，不会卡死在 restart 重试循环里。
3. **真实 Lightsail 场景回归**：拿到"14:56:24 那次 10 秒断流"类似场景的网络条件后
   （配合前面第 2 项的 mtr/getStats 证据），实测 ICE restart 能不能命中并显著缩短
   中断时间；如果这条链路本身问题在于持续限速/丢包（而不是短暂抖动），ICE restart
   本身可能也扛不住，这种情况应该导向"换 Origin"而不是继续加固 restart 逻辑。

## 7. 实施建议顺序

1. 先做 §3.1 的 libdatachannel 可行性调研（不写产品代码，就是花时间确认能不能做），
   这决定了后面工作量是"中等"还是"这个方案在 OBS 侧目前做不到、只能等库支持"。
2. MMX 侧的 PATCH handler 可以独立先做（Pion 支持良好，不依赖调研结果），做完后
   即使 OBS 暂时用不上，也不影响现有功能（未知 PATCH 走 `501`，MVP 阶段 OBS 本来就
   不会发）。
3. OBS 侧确认可行后再实现 3.2，双方联调，走第 6 节的测试计划。
4. 如果 §3.1 调研结论是"目前做不到真正的 ICE restart"，评估是否值得做方案 3
   （跳过 DELETE/POST 往返的轻量重建），或者干脆搁置，靠已经上线的 10 秒宽限期 +
   有需要时的 primary/backup failover（`ShouldFallback`）过渡。

## 8. 版本 / 变更

- v1（本文档）：首个版本，设计稿，未实现。
