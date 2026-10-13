# LAN Share — C 版

为小内存嵌入式设备（路由器）写的局域网文件分享服务。**只依赖 libc**，静态链接后约 130 KB，常驻内存约 1.5 MB，在 128 MB 内存、2.6/5.4 内核的老路由器上也能跑。

一台机器跑服务，其他设备用浏览器打开即可，不用装任何东西。

**前端页面已编译进二进制**。`public/` 只是开发时的源文件，**部署时不用一起拷**——把单独一个 `lan-share` 丢到路由器上，`/` 就能正常打开。

> 想要 Node 实现（开发调试、桌面长期运行）？看 [Node 版](../lan-share-node)。

## 两条独立的传输路径

| 路径 | 数据怎么走 | 适合什么 |
| --- | --- | --- |
| **设备直传** | 浏览器之间 WebRTC DataChannel 点对点 | 当场发文件给某台设备，服务端不接触文件内容 |
| **服务端文件库** | 上传落盘到服务端磁盘，再下载 | 存下来慢慢取，或发给不在线的人 |

两条路径都支持断点续传：直传中断后重发会接着上次的字节偏移继续；文件库上传按偏移续写 `.part` 半成品；下载走 HTTP Range，浏览器原生续传。设备名会按 IP 记住并在浏览器本地存一份，刷新页面、重连都不会变回随机名。

**关于速度**：P2P 直传走 WebRTC DataChannel，有两条浏览器硬限制——单条消息不能超过 `sctp.maxMessageSize`（Chrome 默认 256KB），发送队列总共只有约 16MB，`bufferedAmount` 一超过 `send()` 就直接抛 `RTCDataChannel send queue is full`（不是排队，是抛异常）。 pacing 逻辑集中在 `public/p2p-pump.js`：高水位 2MB、低水位 1MB，**窗口没打满就连续发，打满了才等一次**——曾经是每发一块都等一次，而 `bufferedamountlow` 的阈值默认是 0、要等整队泄空才触发，实际放行全靠 50ms 轮询，每块都要赔一个轮询周期，吞吐被钉死在 5MB/s 左右（实测两个版本都卡在 5M/b）。另外一次读 8MB 再切块发送，避免每 256KB 就走一次 Blob IPC 把带宽卡在读速上。

**发送端的进度和速率按对端实收显示**。只统计"交给发送队列的字节"的话，进度条会被各 lane 的高水位吸收（32 条 × 2MB = 64MB 的吸收窗口）先冲到 100% 再卡住，速率在 2~40MB/s 之间荡——荡的是表，不是链路（接收端一直很稳）。接收端每 1MB 回传一次实收字节数，发送端按它画进度、算速率，两边看到的是同一条曲线。progress 在 16/32 条 lane 上各发各的、跨 lane 没有顺序保证，发送端对回执值**只增不减**，晚到的旧值不会把进度拉回去。**"已送达"以对端落盘回执（file-done）为准**，而不是本端把字节交给发送队列就拉满——发送完会先显示"已发出，等待对端保存"，对端真正组装落盘、回执回来后才是"已送达"。

**点对点自适应开 16~32 条并行通道**。一个 `RTCPeerConnection` 只有一个 SCTP 关联，拥塞窗口、接收窗口、消息粒度全是按关联算的——实测同一对设备 TCP（SMB）能跑 30MB/s，而单条 DataChannel 只有 3.2MB/s，瓶颈在每关联的机制里，不在链路上，所以加带宽没用，只能加关联。文件按字节范围切段，每条通道独立推一段（各自的 ICE / DTLS / SCTP / 拥塞窗口），接收端按通道顺序拼回原文件。注意同一条关联上开多个 DataChannel 是没用的，它们共享同一个拥塞窗口。

最优 lane 数随链路丢包变化，这是 CI 双 Chrome + `tc netem` 扫出来的（`tools/p2p-e2e.js --sweep`，速率由对端计时）：

| 链路 | 4 条 | 8 条 | 16 条 | 32 条 |
|---|---|---|---|---|
| 无损 loopback（TCP 300+ MB/s） | 39 MB/s | 37 | 32 | 32 |
| 5ms + 0.5% 丢包（TCP 26~38） | 6.4 | 11.7 | 11.0 | **13.5** |
| 20ms + 2% 丢包（TCP 3.6~6.7） | 0.9 | 1.6 | 3.1 | **5.1** |

（Node 版同套前端在自己 CI 上的数据更细：无损时 1~2 条反而最快（26~29）、32 条 17.7；0.5% 丢包时 **16 条 20.0 > 32 条 17.1**；2% 丢包时 16 条 3.0 < **32 条 4.2**。）

所以**不硬编码一个数**：每次连接先用 16 条 lane 探 4MB，对端计时聚合 ≥10MB/s 就用 16 条（省 CPU、吸收窗口小、数字稳），慢了才加满到 32 条；探测结果按连接缓存，之后每趟传输的实收速率如果明显变慢会自动升档（只升不降）。任何一条 lane 没打开（20 秒超时）会自动退役，file-offer / file-accept 互换双方活 lane 集合、按交集切段，段数永远对齐。测速页会同时显示链路诊断（候选对类型 host↔host / RTT）和单条通道的速率。

**页面里带了测速页**（第三个标签页）：分别测「服务端下载 / 服务端上传 / 点对点直传」三段，点对点那项还会把对端实际收到的速率一并显示出来。用途是把"慢"拆开定位——服务端慢是磁盘或 HTTP 栈的问题，点对点慢是 Wi-Fi 或 WebRTC SCTP 的问题，两者治法完全不同。测速状态按连接隔离、带 60 秒超时、失败主动发 `speedtest-abort`，不会因为对端没回执就把后续文件分片误吞导致传输卡死。

## 直接下载预编译二进制

每次 push 到主分支，GitHub Actions 都会编译所有目标；打 `v*` tag 时自动发布到 Release。

| 目标 | 文件名 | 适用 |
| --- | --- | --- |
| Windows x86_64 | `lan-share-windows-amd64.exe` | Windows |
| Linux x86_64 | `lan-share-linux-amd64` | 普通 PC、服务器、x86 软路由 |
| Linux ARM64 | `lan-share-linux-arm64` | ARM64 服务器、树莓派 64 位系统 |
| OpenWrt ARMv7l | `lan-share-openwrt-armv7l` | 32 位 ARM 硬浮点路由器（BCM27xx、i.MX6 等） |

Linux 产物全部是 musl 静态链接，任何发行版（含 OpenWrt）都能直接跑，不挑 libc，约 60 KB。

Release 里附 `SHA256SUMS.txt` 可校验。

需要 MIPS 之类没预编译的架构，照下面自己编一条即可，zig 支持 mipsel / mips（大端）/ arm 软浮点等。

## 自己编译

```bash
make                 # 本机编译，产出 ./lan-share
make test            # 编译并跑端到端测试
```

交叉编译（用 OpenWrt SDK 的 toolchain）：

```bash
make CC=mipsel-openwrt-linux-gcc STATIC=-static
mipsel-openwrt-linux-strip lan-share        # 可再小 30%
```

没有 OpenWrt SDK 也能编译。**zig 自带 musl**，一个 40 MB 的下载就能覆盖全部 Linux 目标，CI 用的就是它：

```bash
zig cc -target arm-linux-musleabihf -static -Os -std=c99 -o lan-share lan_share.c
```

| 产物 | zig target |
| --- | --- |
| `lan-share-linux-amd64` | `x86_64-linux-musl` |
| `lan-share-linux-arm64` | `aarch64-linux-musl` |
| `lan-share-openwrt-armv7l` | `arm-linux-musleabihf` |
| （按需）mipsel 软浮点 | `mipsel-linux-musleabi` |
| （按需）mipsel 硬浮点 | `mipsel-linux-musleabihf` |
| （按需）大端 MIPS | `mips-linux-musleabi` |
| （按需）ARM 软浮点 | `arm-linux-musleabi` |

两个坑：

- **zig 不在 GitHub Release 上**。0.14 起 ziglang 只把源码包放 GitHub，预编译二进制在
  `https://ziglang.org/download/<版本>/zig-x86_64-linux-<版本>.tar.xz`。写成 GitHub 的 URL 会 404。
- **MIPS 必须写全 ABI**（`musleabi` / `musleabihf`）。写裸 `mipsel-linux-musl` 会报 `unable to provide libc`。

`-Os` 比 `-O2` 更省空间，小 flash 设备优先用它（Makefile 默认已是 `-Os`）。

## 部署到路由器

只拷二进制就够了，前端在它里面：

```bash
scp lan-share root@192.168.1.1:/usr/bin/lan-share
ssh root@192.168.1.1 chmod +x /usr/bin/lan-share
```

运行：

```bash
lan-share -p 8080 -d /mnt/sda1/share
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-p` | `8080` | 监听端口 |
| `-d` | `./uploads` | 文件库落盘目录（**建议指到挂载的 U 盘/硬盘**） |
| `-w` | `./public` | 前端静态文件目录，**可省略**，找不到时自动用二进制里内嵌的页面 |
| `-h` | — | 显示帮助 |

想改前端再重新内嵌（需要 Node，改完重新 `make`）：

```bash
node tools/embed-web.js        # public/ -> web_assets.h
```

### 开机自启（OpenWrt）

创建 `/etc/init.d/lan_share`：

```sh
#!/bin/sh /etc/rc.common
START=95
STOP=10

start() {
    service_start /usr/bin/lan-share \
        -p 8080 \
        -d /mnt/sda1/share
}

stop() {
    service_stop /usr/bin/lan-share
}
```

```bash
chmod +x /etc/init.d/lan_share
/etc/init.d/lan_share enable
/etc/init.d/lan_share start
```

## 内存与并发

- 常驻内存约 **1.5 MB**，其中 1.3 MB 是 24 个客户端槽位的静态数组。设备不会超过 24 台就不用改；想再省内存，把 `lan_share.c` 里的 `MAX_CLIENTS` 调小（如 8），数组占用按比例降到约 0.4 MB。
- 上传是流式落盘，不整包进内存，传多大的文件都不会把内存吃满。
- 路由器的 flash 擦写次数有限，`-d` 务必指到外接存储。

## 测试

```bash
make test
```

`test.js` 会自己拉起服务、跑完 114 项断言再关掉它，覆盖：

- 静态托管、目录穿越防护（明文与编码两种）
- 上传 / 下载 / 删除、中文与特殊文件名往返、同名自动重命名
- 5 MB 与 16 MB 文件逐字节完整性
- WebSocket 信令全链路（设备发现、改名广播、信令转发、无效目标回执、断线收缩）
- **上传断点续传**：分块写入、未完结不进文件库、`/api/upload-status` 查进度、续传后内容逐字节一致、跳过未上传区域返回 409
- **下载 Range**：`Accept-Ranges: bytes`、206 + `Content-Range`、开区间与闭区间内容校验、越界返回 416
- **设备名记忆**：改名回执、断开重连后名字不变
- **P2P 发送 pacing**（不起浏览器）：用假 DataChannel 跑 `public/p2p-pump.js` 的真代码，
  复现 Chrome 那 16MB 发送队列上限，验证 64MB 文件一次发完、不抛"队列满"、
  缓冲从不越界、慢通道与续传起点都正确
- **测速接口**：整块 POST / 下载 / 删除（这条路径会让头和 body 挤在同一个 TCP 段到达）

### 真实浏览器点对点测试

假 DataChannel 验证不了"真实 Chrome 的 SCTP 到底能跑多快"——而这正是排查
点对点慢的关键。`tools/p2p-e2e.js` 拉起两个独立 Chrome 进程（零依赖，直接用
Node 内置 WebSocket 走 CDP，不装 puppeteer），让它们互相发现，然后：

1. **TCP 基线**：同机服务端上传/下载——点对点数字只有和它比才有意义；
2. **内置测速**：冷连接、热连接各跑一次 `speedP2p`，报候选对类型与 RTT；
3. **真实文件传输**（默认 64MB）：自动点接收确认框、等下载落盘、逐字节 SHA-256
   校验——并行 lane 分段拼回去只要错一段就会现形。

```bash
cc -O2 -std=c99 -Wall -o lan-share lan_share.c
node tools/p2p-e2e.js --server ./lan-share --file-mb 64 --timeout 240
```

注意区分两个速率：**交付速率**只算到发送端字节入队（DataChannel 队列上限
16MB，能吸收好几秒数据，会虚高），**端到端速率**算到对端文件落盘，才是真实值。

CI 每次 push 都跑（[e2e-p2p.yml](.github/workflows/e2e-p2p.yml)，64MB 与 16MB
两档）：Runner 上的 Chrome 走真实内核 loopback，没有开发机沙箱虚拟网络的干扰。

## 服务端吞吐基准

怀疑服务端是瓶颈时，先跑这个再下结论（不起浏览器，直连 HTTP）：

```bash
node bench.js c 8123      # 测 C 版
node bench.js node 8124   # 测 Node 版
```

会分别打出 4MB 分块上传、整块上传、下载三个吞吐。本机实测 C 版约
190 / 450 / 270 MB/s，Node 版约 92 / 130 / 136 MB/s——**服务端从来不是瓶颈**，
看到几 MB/s 时应该去看浏览器到路由器那段链路。

页面里也有一个「网速测试」标签，分服务端下载、服务端上传、点对点三段测：
三段都慢就是局域网链路的问题（2.4G Wi-Fi、百兆口常见），只有点对点慢才是
DataChannel 的问题。

需要本机有 Node ≥ 21（仅用于跑测试，运行服务不需要）。也可以指向已跑起来的服务：

```bash
BASE=http://192.168.1.1:8080 node test.js
```

CI 在 Ubuntu 和 macOS 上跑完整功能测试，并编译 Windows / Linux x64 / Linux ARM64 / OpenWrt ARMv7l 四个目标的二进制，见 [build.yml](.github/workflows/build.yml)。

## 实现说明

- **只依赖 libc**。WebSocket 需要的 SHA-1 与 Base64 都在文件内实现，不链接 OpenSSL。
- **读事件一次读完**。`client_readable` 循环 `recv` 到 `EWOULDBLOCK`，缓冲 64KB。
  原来一次只 `recv` 8KB：客户端一个 4MB 分块要几十万次 select 唤醒才吃得完，
  单核 MIPS/ARM 直接被打满。三种模式收多少还不一样——HTTP 态只收"头缓冲还放得下"的
  量（`HEAD_BUF` 只有 8KB，多收的 body 塞进去会被当成畸形请求直接关连接，
  症状是客户端 `socket hang up`），WS 态按 `WS_BUF` 剩余空间收，UPLOAD 态直接写盘、
  放心按 64KB 收。
- **前端内嵌进二进制**。`tools/embed-web.js` 把 `public/` 的 HTML/JS/CSS 转成 `web_assets.h` 字节数组，启动时先找磁盘上的 `-w` 目录、找不到就回退到内嵌副本。这样拷一个二进制到路由器（`public/` 没跟着过去）也不会 404。改了前端记得重新生成，CI 每轮都会自己跑一遍并断言「无 `public/` 目录时 `/` 仍返回 200」。
- **输出缓冲是追加而不是覆盖**。单线程 `select` 循环里 `send` 只是写进内核缓冲，真正 flush 往往在下一轮，连续两次排队（改名回执 `self` + `peers` 广播）会把前一条整帧丢掉。`queue_mem()` 因此先把未发送的部分挪到开头再追加。
- **配了一个 STUN（不配 TURN）**。`iceServers` 现在是 `[{ urls: 'stun:stun.l.google.com:19302' }]`。原因：纯 host 候选会被 Chrome 默认伪装成 mDNS `.local` 名字，两台 Windows PC 在真实局域网里经常解析失败，导致绝大多数并行 lane 的 ICE 连通性检查过不了、只剩下 1 条能开，速度就卡在单关联的 ~5MB/s（回环上却能开满 32 条）。加了 STUN 后浏览器还会收集 srflx 候选（可达的 IPv4 UDP 端点），绕开 mDNS/IPv6 主机候选的不可达问题。**只加 STUN、不加 TURN，所以数据绝不经第三方中转。** 纯离线局域网下 STUN 请求会超时，但 host 候选照常使用、只是 ICE 稍慢一点；想彻底不连外网就把这行改回 `[]` 并重新 `embed-web.js`。
- **32 条 lane 逐条错峰建立（每条间隔 150ms）**。32 条 `RTCPeerConnection` 同时握手，会瞬间灌出 60+ 个 mDNS 组播查询，操作系统大量丢弃 → 多数候选永远解析不出、对应 lane 的 ICE 连不上（真机只开 1~4 条、速度卡 5MB/s 的典型成因）。逐条来让 mDNS 查询错开，几乎每条都能解析出真实 IP、32 条基本全开；而且每条走干净直连，单关联速度也从"mDNS 降级路径"回到正常的几十 MB/s——少量 lane 就能跑满千兆。传输界面会直接显示「N 路并行」，方便确认到底开了几条。
- **响应头与文件体分流**。下载时 HTTP 头走高优先级缓冲先发出，再流式读盘，避免两者互相覆盖。
- **UTF-8 文件名在 Windows 上走宽字符 API**。Windows 的 ANSI 版 `open`/`fopen` 会拿当前代码页（简体中文是 GBK 936）解释 UTF-8 字节，只要恰好拼出非法的 GBK 双字节组合就失败——症状很随机，例如"最.txt"能建而"中.txt"不能。所以 Windows 分支一律用 `_wopen` / `_wfopen` / `_wstat64` / `FindFirstFileW`。Linux 下路径是原始字节、内核不解释编码，无需转换。
- **文件名安全**。上传前剥离路径、过滤控制字符，并校验最终路径仍在存储目录内，防止目录穿越。

## HTTP 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/files` | 文件列表，`{ files: [{name,size,sizeText,mtime}], total }` |
| POST | `/api/upload?name=xxx` | 请求体为原始文件流，落盘；同名自动追加 `(1)` |
| POST | `/api/upload?name=xxx&size=N&offset=M` | 分块上传。先写 `uploads/.lanshare-parts/<sha1(name\|size)>.part`，收满 `size` 字节才改名进文件库；返回 `done:false,received:N` 表示还没完。跳过未上传区域返回 409 |
| GET | `/api/upload-status?name=xxx&size=N` | 查询该半成品已收字节数 `{ received }`，续传前用它定位起点 |
| GET | `/api/download?name=xxx` | 下载，带 UTF-8 文件名的 `Content-Disposition`。支持 `Range: bytes=...`，回 206 + `Content-Range`，越界回 416，始终带 `Accept-Ranges: bytes` |
| DELETE | `/api/files?name=xxx` | 删除 |
| GET | `/api/info` | 在线设备数、存储目录、`engine:"c"` |

WebSocket 服务端在 `/ws`，消息格式：

```jsonc
{"type":"self",  "id":"dev-1-abcd", "name":"蓝色海豚"}   // 连上时告诉你是谁
{"type":"peers", "peers":[{"id":"...","name":"...","ip":"..."}]}  // 在线设备列表变化
{"type":"signal","from":"...","to":"...","data":{...}}   // 转发 WebRTC offer/answer/ICE
{"type":"signal-lost"}                                    // 目标设备已离线
```

客户端发送的：`{"type":"rename","name":"新名字"}`、`{"type":"signal","to":"...","data":{...}}`。

## 注意

- 服务端文件库默认无鉴权，同一局域网内任何人都能上传下载删除。只在可信网络里用。
- 路由器开启 **AP 隔离** 会让设备之间的点对点连接失败（文件库功能不受影响）。
- 用手机系统分享菜单、PWA 安装或消息通知需要 HTTPS；局域网 HTTP 下传文件正常。
- WebRTC 接收的文件先攒在浏览器内存里再落盘，几百 MB 以上没问题，超过 2 GB 建议走文件库路径。

## 许可

MIT
