# LAN Share

单文件局域网文件传输服务。仅依赖 libc，静态链接后约 180 KB，常驻内存约 1.5 MB。一台设备运行服务端，其余设备用浏览器访问即可互传，无需安装客户端、登录或连接外网。

```
┌─────────┐   浏览器    ┌─────────┐
│ 设备 A  │◀──────────▶│ 设备 B  │      直连：WebRTC DataChannel，多 lane 并行
└────┬────┘             └────┬────┘      兜底：直连不可用时经服务端 WebSocket 中转
     │  HTTP + WebSocket     │
     └───────────┬───────────┘
            ┌────┴─────┐
            │ lan-share │
            └───────────┘
```

前端资源经 `tools/embed-web.js` 内嵌进二进制；`public/` 仅为开发源文件，部署时无需分发。服务端只转发信令与中转帧，不解析 SDP/ICE，直连时不接触文件内容。

## 快速开始

从 [Releases](https://github.com/inmomi/lan-share-c/releases/latest) 下载单个可执行文件：

| 平台 | 文件名 |
| --- | --- |
| Windows x86_64 | `lan-share-windows-amd64.exe` |
| Linux x86_64 | `lan-share-linux-amd64` |
| Linux ARM64 | `lan-share-linux-arm64` |
| OpenWrt ARMv7l | `lan-share-openwrt-armv7l` |

Linux 产物为 musl 静态链接，不依赖发行版 libc。Release 附 `SHA256SUMS.txt`。

```bash
lan-share-windows-amd64.exe -p 8080 -d D:\share          # Windows
./lan-share-linux-amd64 -p 8080 -d /mnt/sda1/share       # Linux / 路由器
```

启动后打印监听地址，例如 `http://192.168.1.100:8080`。其余设备用浏览器打开该地址，页面显示在线设备列表。

文件接收路径按条件选择：

| 条件 | 行为 |
| --- | --- |
| 大小 ≤ 32 MiB，或非安全上下文 | 浏览器内存下载回退 |
| 大小 > 32 MiB 且为安全上下文（HTTPS / localhost），且支持 File System Access API | 用户选择保存位置，边收边写，不驻留内存 |

进度条显示对端实际接收速率，非本端入队速率；状态为「已送达」表示对端已落盘。

## 功能

| 标签页 | 说明 |
| --- | --- |
| 设备直传 | 浏览器间点对点。文件按字节范围切段，最多 32 条通道并行；通道中断后自动退役，重发同一文件从断点续传 |
| 共享剪贴板 | 服务端保存的文字或图片（PNG/JPEG/GIF/WebP），保留最近 32 条，按 `updated` 淘汰最旧；单项上限 16 MiB，总量上限 128 MiB |
| 服务端文件库 | 上传落盘，任意设备下载。分块写入 `.lanshare-parts/*.part`，收满后改名入库；支持 HTTP `Range` 与断点续传 |
| 网速测试 | 分别测量服务端下载、服务端上传、点对点传输三段吞吐 |

网速测试用于定位瓶颈：三段均低属局域网链路问题（2.4G Wi-Fi、百兆口常见）；仅点对点低属 WebRTC / SCTP 层问题。

## 命令行参数

```
用法: lan-share [-p 端口] [-d 文件库目录] [-w 前端目录]
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-p` | `8080` | 监听端口；同端口 UDP 兼作局域网 STUN |
| `-d` | `./uploads` | 文件库目录。路由器上应指向外接存储 |
| `-w` | `./public` | 前端静态目录，可省略；缺省使用内嵌资源 |
| `-h` | — | 显示帮助 |

## 部署

```bash
scp lan-share-openwrt-armv7l root@192.168.1.1:/usr/bin/lan-share
ssh root@192.168.1.1 'chmod +x /usr/bin/lan-share && /usr/bin/lan-share -p 8080 -d /mnt/sda1/share'
```

OpenWrt 开机自启：写入 `/etc/init.d/lan_share`，然后 `chmod +x` 并 `enable`。

```sh
#!/bin/sh /etc/rc.common
START=95
STOP=10

start() { service_start /usr/bin/lan-share -p 8080 -d /mnt/sda1/share; }
stop()  { service_stop /usr/bin/lan-share; }
```

### 编译

```bash
make                        # 本机编译，产出 ./lan-share
make test                   # 编译并运行端到端测试
node tools/embed-web.js     # 修改 public/ 后重新内嵌前端
```

交叉编译使用 zig（自带 musl，CI 采用此方式）：

```bash
zig cc -target arm-linux-musleabihf -static -Os -std=c99 -o lan-share lan_share.c
```

| 产物 | zig target |
| --- | --- |
| `lan-share-linux-amd64` | `x86_64-linux-musl` |
| `lan-share-linux-arm64` | `aarch64-linux-musl` |
| `lan-share-openwrt-armv7l` | `arm-linux-musleabihf` |
| mipsel 软 / 硬浮点 | `mipsel-linux-musleabi` / `mipsel-linux-musleabihf` |
| 大端 MIPS | `mips-linux-musleabi` |

约束：zig 预编译包仅发布于 `ziglang.org/download/<版本>/`；MIPS 目标须写全 ABI（裸 `mipsel-linux-musl` 报 `unable to provide libc`）；`-Os` 优先于 `-O2`。

## 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/files` | `{ files: [{name,size,sizeText,mtime}], total }` |
| GET | `/api/clipboard` | 最新一条置于顶层，历史见 `items[]`；无内容返回 404 |
| GET | `/api/clipboard/data?id=N` | 下载第 N 条，省略 `id` 取最新 |
| POST | `/api/clipboard?kind=text\|image&mime=&name=` | 新增一条，单项上限 16 MiB |
| DELETE | `/api/clipboard?id=N` | 删除第 N 条，省略 `id` 清空 |
| POST | `/api/upload?name=xxx` | 请求体为原始文件流，同名追加 `(1)` |
| POST | `/api/upload?name=xxx&size=N&offset=M` | 分块上传至 `.lanshare-parts/<sha1(name\|size)>.part`，收满后入库；返回 `done:false,received:N`；跳过未上传区域返回 409 |
| GET | `/api/upload-status?name=&size=` | 半成品已收字节数 `{ received }` |
| GET | `/api/download?name=xxx` | 支持 `Range`（206/416），恒带 `Accept-Ranges: bytes`，UTF-8 文件名 `Content-Disposition` |
| DELETE | `/api/files?name=xxx` | 删除文件 |
| GET | `/api/info` | 在线设备数、存储目录、`engine` |

WebSocket 位于 `/ws`，服务端按 id 转发：

```jsonc
{"type":"self",  "id":"dev-1-abcd", "name":"蓝色海豚"}
{"type":"peers", "peers":[{"id":"...","name":"...","ip":"..."}]}
{"type":"signal","from":"...","to":"...","data":{...}}
{"type":"signal-lost"}
```

客户端发送 `{"type":"rename","name":"..."}` 与 `{"type":"signal","to":"...","data":{...}}`。兜底通道二进制帧（opcode `0x2`）复用同一连接，格式 `[1 字节对端 id 长度][id][payload]`，单帧上限 64 KB。

## 设计说明

**传输抽象**。数据通道只暴露 `send` / `readyState` / `bufferedAmount` / `bufferedAmountLowThreshold` / `addEventListener`，上层分块、pacing、续传、回执逻辑不感知具体实现。入口为 `ensureChannel(peerId)`，返回 `{mode:'p2p'}` 或 `{mode:'relay'}`。实现在 `public/relay-channel.js`，独立成文件以便 Node 环境单测。

**多 lane**。单个 `RTCPeerConnection` 对应一条 SCTP 关联，拥塞窗口与接收窗口按关联计算；同关联内多开 DataChannel 共享同一窗口，无收益。实测同一链路 TCP 30 MB/s、单条 DataChannel 3.2 MB/s，故按关联数横向扩展，上限 32 条。

| 常量 | 值 | 作用 |
| --- | --- | --- |
| lane 建链间隔 | 150 ms | 避免 32 条同时握手产生 60+ 个 mDNS 组播查询被丢弃 |
| `LANE_OPEN_TIMEOUT` | 20 s | 未建立的 lane 退役 |
| `DIRECT_GRACE_MS` | 9 s | 无任何 lane 打开则降级中转 |
| `LANE_SETTLE_MS` / `MAX` | 1.5 s / 12 s | 已有 lane 打开后的收敛等待 |
| `WS_BUF` | 128 KB | 服务端单帧上限，客户端按 64 KB 切块 |
| pacing 高 / 低水位 | 2 MB / 1 MB | 兜底通道背压 |
| 接收端回执间隔 | 1 MB | 进度上报 |
| 背压确认间隔 | 512 KB | 兜底通道流控 |

**局域网 STUN**。Chrome 默认将 host 候选伪装为 mDNS `.local` 名称，依赖组播解析；家用路由器普遍启用 IGMP Snooping，无线侧组播常被丢弃，导致候选无法解析。服务端在同端口号另开 UDP socket 实现最小 STUN，应答 Binding Request 并返回携带客户端真实内网 IP 的 `srflx` 候选（magic cookie `0x2112A442`）。Chrome 不对 `srflx` 做 mDNS 伪装，直连因此不依赖组播。仅实现 STUN，不实现 TURN，数据不经第三方；外部 TURN 配置可通过 `window.__LAN_SHARE_TURN__` 注入。

**进度语义**。仅统计入队字节会被各 lane 高水位（32 × 2 MB）吸收，进度提前到达 100% 后停滞。改为按接收端每 1 MB 回传的实际字节数计算；回执跨 lane 无顺序保证，发送端只增不减。完成态以对端 `file-done` 回执为准。

**服务端**。`select` 单线程，24 个静态客户端槽位；读事件循环至 `EWOULDBLOCK`，各模式下读入量受 `HEAD_BUF` / `WS_BUF` / 写盘缓冲约束；输出缓冲为追加语义，避免连续两次排队覆盖整帧；WebSocket 分片按 RFC 6455 通用处理（浏览器发送大消息时二进制帧同样分片）；Windows 文件名操作统一使用 `_wopen` / `_wfopen` / `_wstat64` / `FindFirstFileW`。

## 测试

```bash
make test                   # 端到端，149 项断言
node tools/test-relay.js    # 兜底通道 17 项单测，无需服务端与浏览器
node bench.js c 8123        # 服务端吞吐基准
```

`test.js` 自启服务并在结束后关闭，覆盖静态托管与目录穿越防护、上传/下载/删除、剪贴板文字与图片往返（含 32 条历史、淘汰、按 id 取删）、中文与特殊文件名、5 MB 与 16 MB 文件逐字节校验、WebSocket 信令全链路、上传断点续传、下载 `Range`、设备名记忆，以及用假 DataChannel 驱动 `p2p-pump.js` 验证 64 MB 单次发送不触发队列满异常。

`bench.js` 本机实测 190 / 450 / 270 MB/s（4 MB 分块上传 / 整块上传 / 下载）。

真实浏览器端到端由 `tools/p2p-e2e.js` 完成，经 CDP 驱动两个独立 Chrome 实例（无外部依赖）：TCP 基线、冷热连接测速与候选对类型、真实文件传输逐字节校验。

```bash
node tools/p2p-e2e.js --server ./lan-share --file-mb 64 --timeout 240
node tools/p2p-e2e.js --url https://xxxx.trycloudflare.com --require-mode relay
```

交付速率计至字节入队，端到端速率计至对端落盘，两者不可混用。

CI：`build.yml` 运行功能测试与四目标编译，并以 netns 模拟三种网络拓扑——两主机直连、AP 隔离 + TURN、UDP 全丢（断言降级至兜底通道）；`e2e-p2p.yml` 运行双 Chrome 与 `tc netem` 丢包下的 lane 数扫描。推送 `v*` tag 触发发布。

## 限制

- 文件库与共享剪贴板无鉴权，同网络内可读写删。仅用于可信网络。
- 路由器启用 AP 隔离时设备间无法直连，自动降级至中转；文件库功能不受影响。
- 大文件流式写入要求 HTTPS 或 localhost 且浏览器支持 File System Access API（Chrome / Edge），否则回退至内存下载。
- 公网穿透工具（cloudflared / ngrok / devtunnel 等）仅转发 HTTP 与 WebSocket，不提供 UDP；服务端 STUN 返回的内网地址对公网对端不可达。此类部署下 WebRTC 直连必然失败，仅走中转通道，吞吐受隧道带宽限制。跨公网直连需虚拟组网方案（Tailscale / EasyTier / ZeroTier）为两端分配可达 IP。

## 故障定位

| 现象 | 排查方向 |
| --- | --- |
| 设备可见但无法传输，停留在「连接中」 | 卡片标注「服务器中转（直连未建立）」即已降级。恢复直连需关闭 AP 隔离，或排除 mDNS 组播拦截（IGMP Snooping） |
| 传输中途停滞 | 查看接收端状态原文。「已中断（N MB），可续传」表示通道断开，重发同一文件自动续传；持续「接收中」表示对端未消费 |
| 吞吐显著低于预期 | 先运行网速测试区分段位；服务端各段正常而点对点低时，属 WebRTC 层问题 |
| 手机端功能受限 | 系统分享、PWA 安装、通知要求 HTTPS；局域网 HTTP 下页面传输文件正常 |

## 许可

MIT
