# LAN Share 设计说明

面向改代码的人。用户怎么装、怎么用、报错怎么办见 [README.md](README.md)。

## 目录结构

```
lan_share.c          服务端（单文件 C99，含内嵌前端）
public/              前端源文件（构建时内嵌进二进制）
  app.js             页面逻辑、信令、传输编排
  p2p-pump.js        发送 pacing / 背压 / 分块（能在 Node 里 require 做单测）
  relay-channel.js   WebSocket 兜底通道（同上）
tools/               构建与测试工具（内嵌、CDP 驱动、单测）
test.js              端到端功能测试（自起服务）
bench.js             服务端吞吐基准
```

## 传输抽象

数据通道只暴露 `send` / `readyState` / `bufferedAmount` / `bufferedAmountLowThreshold` / `addEventListener`，上层的分块、pacing、续传、回执逻辑不感知具体实现。入口是 `ensureChannel(peerId)`，返回 `{mode:'p2p'}` 或 `{mode:'relay'}`。

这样「Wi-Fi 点对点」「有线↔Wi-Fi」「AP 隔离」「mDNS 被拦」「UDP 被禁」不必各写一套逻辑：ICE 建得起来就走 DataChannel，建不起来自动落到 WebSocket 中转。

## 多 lane

单个 `RTCPeerConnection` 对应一条 SCTP 关联，拥塞窗口与接收窗口按关联计算；同一关联内多开 DataChannel 共享同一窗口，没有收益。实测同一链路 TCP 可达 30 MB/s、单条 DataChannel 仅 3.2 MB/s，所以按关联数横向扩展，上限 32 条。

lane 数不是越多越好——最优值随丢包变化（`tools/p2p-e2e.js --sweep`，对端计时）：

| 链路 | 1~4 条 | 16 条 | 32 条 |
| --- | --- | --- | --- |
| 无损 loopback | 26~39 MB/s（够用） | 16~18 | 16~18 |
| 5 ms + 0.5% 丢包 | — | 20.0 | 17.1 |
| 20 ms + 2% 丢包 | — | — | 4.2（明显占优） |

所以并行度直接取**此刻所有已建立的 lane**：lane 的建立与这个数无关（`initiate` 始终建满 32 条），限流只会把已经握好的通道白白闲置。曾经按 `PROBE_LANES=16` 截断并把它冻进 `conn.lanePref`，真机上的表现是"比上一版慢一倍"——第一趟赶上 lane 刚开 8 条就只发 8 条，下一次 `max(8,16)=16`，32 条里永远只跑一半。

| 常量 | 值 | 作用 |
| --- | --- | --- |
| `FIRST_WAVE` / `GAP` | 8 条并发 / 120 ms | 首段并发建链，其余错开逐一发 offer，避免 32 条同时握手产生 60+ 个 mDNS 组播查询被丢弃 |
| `LANE_OPEN_TIMEOUT` | 20 s | 未建立的 lane 退役 |
| `DIRECT_GRACE_MS` | 9 s | 无任何 lane 打开则降级中转 |
| `START_BATCH_LANES` / `START_GRACE_MS` | 16 条 / 1.5 s | 够一批、或首条开好后再等 1.5 s 就开工，不等 32 条齐 |
| 并行度 | 全部已建立 lane | 不按速率分档；`START_BATCH_LANES=16` / `START_GRACE_MS=1500` 只决定什么时候开工 |

文件按字节范围等分成段，段与段首尾相接，接收端按 lane 顺序拼回就是原文件。切段用 `splitRange`，双方用同一个函数算，任何一边少了 lane 段数就会对不齐——所以传输时广告的是「本端活着的 lane 编号」，对端取交集后再切。

## 背压与 pacing

`p2p-pump.js` 里两个水位线：缓冲超过 `DRAIN_LIMIT` 停发，等泄到 `LOW_WATER` 以下继续；`LOW_WATER` 同时用作 `bufferedAmountLowThreshold`。

| 项 | 值 | 说明 |
| --- | --- | --- |
| `DRAIN_LIMIT` | 1 MB | 单通道在途上限 |
| `LOW_WATER` | 512 KB | 恢复发送的低水位 |
| `CHROME_DC_QUEUE_CAP` | 16 MB | Chrome 发送队列实测硬上限，超了 `send()` 直接抛 |

这组值是 128 MB × 32 lane 实测每组 6 次的中位数最优点：

| 组合 | 中位耗时 | 中位吞吐 | 停顿 |
| --- | --- | --- | --- |
| 1 MB / 512 KB | 2.29 s | 56 MB/s | 0 次 |
| 1 MB / 256 KB | 2.83 s | 45 MB/s | 低水位太深，等得太久 |
| 2 MB / 1 MB | 3.60 s | 36 MB/s | 收尾更容易被单条慢 lane 拖住 |

水位线再往上没有收益：发送端 `bufferedAmount` 全程只有 2~3 MB（32 lane 合计），瓶颈不在队列深度，而在搬字节的速率。窗口开大只会多攒内存，并让「入队速率」更虚高。

三条硬约束（改动前先读）：

1. 高水位加一块必须明显小于 16 MB，否则传输中途会抛 `RTCDataChannel send queue is full`。
2. 单条消息不能超过对端协商的 `maxMessageSize`（Chrome 默认 256 KB，RFC 8841 缺省 64 KB）。
3. 不能每块都 `file.slice().arrayBuffer()`：每次都走 Blob IPC，读延迟会串在发送循环前面。所以按 `READ_BLOCK` 预读大块，并在内存里切块；读块必须在本 lane 的段末截止，否则每条 lane 会把后面别的 lane 的数据也读进来（128 MB / 32 lane 会白分配 ~500 MB）。

发送节奏是「没到高水位就连续发，到了才等一次」，而不是每块都 `await waitDrain`：后者的放行粒度受 50 ms 轮询限制，吞吐会被钉在「块大小 / 50 ms」≈ 5 MB/s。

## 接收与落盘

| 条件 | 行为 |
| --- | --- |
| 大小 ≤ 32 MiB，或页面非安全上下文 | 下载到内存后触发保存 |
| 大小 > 32 MiB，且为安全上下文（HTTPS / localhost），且浏览器支持 File System Access API | 用户选择保存位置，边收边写，不驻留内存 |

流式路径用 `STREAM_ACK_WINDOW`（4 MiB）做全局在途窗口：发送端领先对端已写盘字节超过该值就等回执，避免把接收端和服务端内存吃光。

**同一对端同时只接收一个文件**：`state.incoming` 是单槽位。因此发送端的 `sendQueues` 不只串行化"发送"，每个 `sendOne` 还会等到对端的 `file-done`（最长 60 s）才返回——否则第二个文件会在第一个还没落盘时开传，接收端只能中断前一个。真被顶掉时接收端会回一条 `file-abort`，发送端据此把该笔标记为失败，不会永远停在"已发出，等待对端保存"。多文件真并发需要把接收状态按 tid 重构。

## 兜底通道

WebSocket 单帧上限 `WS_BUF`=128 KB，帧内还要扣 1 字节 id 长度和 id 本身，所以客户端按 64 KB 切块。二进制帧（opcode `0x2`）复用信令连接，格式：

```
[1 字节对端 id 长度][id][payload]
```

接收端每收 512 KB（`RELAY_ACK_EVERY`）回一次确认，用来泄发送端的 inflight 窗口。DataChannel 那边有 Chrome 的 16 MB 队列兜着，这条通道没有，`bufferedAmount` 必须如实反映对端进度。

## 局域网 STUN

Chrome 默认把 host 候选伪装成 mDNS `.local` 名称，依赖组播解析；家用路由器普遍开 IGMP Snooping，无线侧组播常被丢弃，候选因此解析不出来。

服务端在同一端口号另开 UDP socket 实现最小 STUN：应答 Binding Request，返回携带客户端真实内网 IP 的 `srflx` 候选（magic cookie `0x2112A442`）。Chrome 不对 `srflx` 做 mDNS 伪装，直连因此不依赖组播。

只实现 STUN，不实现 TURN，数据不经第三方；外部 TURN 可通过 `window.__LAN_SHARE_TURN__` 注入。

## 进度与完成语义

只统计本端入队字节会被各 lane 的高水位（32 × 1 MB）吸收，进度提前到 100% 后停滞。所以进度按接收端每 1 MB 回传的实收字节数计算；回执跨 lane 无顺序保证，发送端只增不减。完成态以对端 `file-done` 回执为准——本端把字节交给发送队列不算送达。

## 服务端模型

`select` 单线程，24 个静态客户端槽位（`MAX_CLIENTS`）。读事件循环至 `EWOULDBLOCK`；各模式下读入量受 `HEAD_BUF`（8 KB）/ `WS_BUF`（128 KB）/ 写盘缓冲约束。输出缓冲为追加语义，避免连续两次排队覆盖整帧。WebSocket 分片按 RFC 6455 通用处理（浏览器发大消息时二进制帧同样分片）。Windows 文件名操作统一用 `_wopen` / `_wfopen` / `_wstat64` / `FindFirstFileW`。

## 设备命名

名称按连接分配，与源 IP 无关——经端口转发或隧道接入时所有设备源 IP 相同，按 IP 记忆会导致多台设备同名。客户端把名称存进 `localStorage` 并在握手时通过 `?name=` 回传，因此刷新不变；服务端另按 IP 记忆一份，仅在名称未被在线设备占用时兜底复用。

## HTTP 与 WebSocket 接口

`--token` 开启后，静态资源也受保护：未认证时只有 `/login.html` 可见，其余路径 302 回 `/`，首页直接返回登录页；`/api/*` 与 `/ws` 未认证返回 401。

```
POST /api/login?token=<口令>          # 需带 X-Lan-Share-Request: 1
  → 200 {"ok":true} + Set-Cookie: ls_auth=<sha1(口令)>; HttpOnly; SameSite=Strict
  → 401 {"error":"unauthorized"}
```

Cookie 里存 `sha1(口令)` 而非原文，避免口令里的特殊字符破坏 Cookie 语法。拿不到 Cookie 的客户端（curl、脚本）仍可用下面两种方式：

- `--token` 开启后，请求需带 `X-Lan-Share-Token: <口令>`；无法设置请求头的场景（`<img>`、`<a download>`、WebSocket）改用 `?token=<口令>`。
- 所有 mutating 请求（`POST` / `PUT` / `PATCH` / `DELETE`）还必须带 `X-Lan-Share-Request: 1`，用于阻断浏览器跨站请求。
- WebSocket 握手校验 `Origin` 与 `Host` 一致；非浏览器客户端不带 `Origin` 时放行，仍受可选口令保护。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/info` | 在线设备数与存储目录 |
| GET | `/api/files` | `{ files: [{name,size,sizeText,mtime}], total }` |
| GET | `/api/download?name=xxx` | 支持 `Range`（206/416），恒带 `Accept-Ranges: bytes`，UTF-8 文件名 `Content-Disposition` |
| POST | `/api/upload?name=xxx` | 请求体为原始文件流，同名追加 `(1)` |
| POST | `/api/upload?name=xxx&size=N&offset=M` | 分块上传至 `.lanshare-parts/<sha1(name\|size)>.part`，收满后入库；返回 `done:false, received:N`；跳过未接收区域返回 409 |
| GET | `/api/upload-status?name=&size=` | 半成品已收字节数 `{ received }` |
| DELETE | `/api/files?name=xxx` | 删除文件 |
| GET | `/api/clipboard` | 最新一条置于顶层，历史见 `items[]`；带 `ETag`，匹配 `If-None-Match` 返回 304；无内容返回 404 |
| GET | `/api/clipboard/data?id=N` | 下载第 N 条，省略 `id` 取最新 |
| POST | `/api/clipboard?kind=text\|image&mime=&name=` | 新增一条，单项上限 16 MiB |
| DELETE | `/api/clipboard?id=N` | 删除第 N 条，省略 `id` 清空 |

请求体只支持 `Content-Length`，不解析 `Transfer-Encoding: chunked`：带 chunked 的请求直接回 411，避免"静默建出 0 字节文件还回 200"。另外，提前拒绝（401 / 403 / 409 / 507 / 411）时请求体往往还没读完，服务端会把它排水掉再解析下一个请求——不排水的话残留字节会被当成下一条请求行，整条 keep-alive 连接从此错位（症状是"配额超限之后所有上传都网络错误"）。超过 8 MiB 的残体不值得排水，响应发完直接关连接。

信令位于 `/ws`，握手参数 `?token=<口令>&name=<设备名>`，服务端按 id 转发：

```jsonc
{"type":"self",        "id":"dev-1-abcd", "name":"主机1"}
{"type":"peers",       "peers":[{"id":"...","name":"...","ip":"..."}]}
{"type":"signal",      "from":"...", "to":"...", "data":{...}}
{"type":"signal-lost", "to":"dev-1-abcd"}           // 目标已离线，附带目标 id
```

客户端发送 `{"type":"rename","name":"..."}` 与 `{"type":"signal","to":"...","data":{...}}`。

## 日志

启动横幅写 stdout，运行日志写 stderr。默认只输出错误，`-v` / `-vv` 逐级加详细。

```
[19:29:30] WS 就绪 设备=主机1 ip=192.168.1.5（当前在线 2 台）
[19:29:41] POST /api/upload?name=a.bin&size=1048576&offset=0 -> 200 0.08 KB 23ms ip=192.168.1.5
[19:29:41] 上传 完成 name=a.bin 1.0 MB
[19:29:42] 下载 name=a.bin 1.0 MB
[19:29:42] 候选 主机1→主机2 udp 192.168.1.5:52075 类型=host
[19:29:42] 中转 开始 设备=主机1→主机2（直连未建立，字节走服务器）
[19:29:43] 信令丢失 主机1→dev-11-4f4f（对方不在线）
[19:29:45] 连接关闭 设备=主机2 mode=WS ip=192.168.1.6 原因=收到对端关闭帧 未发=8 待发=0/0 中转 收=0 发=0
```

| 行类型 | 级别 | 含义 |
| --- | --- | --- |
| `queue_mem OOM` / `select 错误` / `上传 中断` / `半成品目录路径过长` | 错误（默认） | 异常与失败，始终输出 |
| 访问日志 | `-v` | `方法 目标 -> 状态码 数据量 耗时ms ip`，一行一个请求 |
| `上传 完成` / `上传 中断` | `-v` / 默认 | 文件名与已收数据量；中断时给出进度与原因 |
| `下载` | `-v` | 文件名与数据量（流式下载不经过响应队列，单独记录） |
| `候选` | `-vv` | 双方交换的 ICE 候选。`host` 为局域网地址直连，`srflx` 为 NAT 映射地址，`relay` 为 TURN 中转 |
| `中转 开始` | `-v` | 直连未建立，字节经服务器转发 |
| `信令丢失` | `-v` | 目标设备已断开（常见于对端刷新后更换连接 id），每条连接最多记录 3 条 |
| `连接关闭` | `-v` | 设备名、模式、原因与未发字节；普通 HTTP 连接正常关闭不记录，避免刷屏 |

数据量按 1024 进制换算，最小单位 KB（不足 1 KB 保留两位小数，如 `0.08 KB`），向上为 MB / GB / TB；页面上的文件大小与速率用同一套规则。

取舍：`304` 响应（剪贴板轮询的「无变化」）不记录；查询串里的 `token` 一律打码为 `***`；ICE 候选与帧级日志每条连接有上限，超出后停止打印并提示。

## 测试与拓扑模拟

`test.js` 自起服务，覆盖静态托管、目录穿越、上传/下载/删除、剪贴板、中文名、大文件逐字节校验、信令全链路、断点续传、`Range`、设备名记忆，以及用假 DataChannel 驱动 `p2p-pump.js` 验证背压与分块边界。交付速率计至字节入队，端到端速率计至对端落盘，两者不可混用。

`tools/cdp-scenarios.js` 是功能层的全场景回归：两个真浏览器 + 三个服务端实例（普通 / 口令 / 配额），覆盖空文件、Unicode 与特殊名、拖拽、拒绝、同对端并发、双向同时传、40MB 流式落盘、剪贴板文字与图片及超限、文件库上传/下载/Range/目录穿越/断点续传、网速测试三段、断开重连、口令与配额，并采集全程未捕获 JS 异常。

`tools/ci-realnet.sh`（需 root）用 Linux network namespace 模拟两机直连 / AP 隔离 + coturn / 禁 UDP 三种拓扑，再交给 `tools/p2p-e2e.js --require-mode relay` 断言降级到兜底通道；丢包拓扑下的 lane 数扫描用 `tools/p2p-e2e.js --sweep`，配合 `tc netem` 注入丢包与延迟。

## 安全与限制

- 默认无鉴权，同网络内可读写删；不可信网络请启用 `--token`。CSRF 与 Origin 校验只防浏览器跨站，不替代鉴权。
- 默认文件库配额 4 GiB（`--quota` 调整），超限上传返回 507；超过 24 小时无进展的 `.part` 会在启动时与运行中自动清理。
- 超时策略：请求头 20 秒、上传 60 秒无进展、WebSocket 120 秒无数据即断开，避免慢连接占满 24 个槽位。
- 路由器启用 AP 隔离时设备间无法直连，自动降级至中转；文件库功能不受影响。
- 大文件流式写入要求 HTTPS 或 localhost，且浏览器支持 File System Access API（Chrome / Edge），否则回退至内存下载。
- 公网穿透工具（cloudflared / ngrok / devtunnel 等）只转发 HTTP 与 WebSocket，不转发 UDP；服务端 STUN 返回的内网地址对公网对端不可达，直连必然失败。跨公网直连需虚拟组网（Tailscale / EasyTier / ZeroTier）为两端分配可达 IP。
