# LAN Share

局域网内传文件：一台机器跑起来，其他设备用浏览器打开就能互传。

服务端只做三件事——托管页面、转发 WebRTC 信令、在直连建不起来时中转字节。正常的点对点传输里，文件内容不经过服务器。

- **零依赖**：只依赖 libc，musl 静态链接后约 230 KB；前端资源内嵌在二进制里，部署只需拷一个文件。
- **免安装**：对端只要有浏览器，不需要客户端、账号或外网。
- **点对点优先**：多通道并行传输；直连被 AP 隔离或 mDNS 拦截时自动降级为服务器中转，功能不中断。
- **省资源**：冷启动常驻内存约 8 MB，其中约 7.9 MB 是 24 个连接槽位的静态缓冲，与负载无关。

四个标签页：设备直传、共享剪贴板、服务端文件库、网速测试。

```
┌──────────┐   浏览器   ┌──────────┐
│  设备 A  │◀─────────▶│  设备 B  │   直连：WebRTC DataChannel，多 lane 并行
└────┬─────┘            └────┬─────┘   兜底：直连不可用时经服务端 WebSocket 中转
     │  HTTP + WebSocket     │
     └───────────┬───────────┘
          ┌──────┴──────┐
          │  lan-share  │   单进程、select 事件循环、24 个连接槽位
          └─────────────┘
```

## 怎么运行

### 拿到可执行文件

从 [Releases](https://github.com/inmomi/lan-share-c/releases/latest) 下载对应平台的产物，并用同目录的 `SHA256SUMS.txt` 校验：

| 平台 | 文件名 | 目标三元组 |
| --- | --- | --- |
| Windows x86_64 | `lan-share-windows-amd64.exe` | `x86_64-windows-gnu` |
| Linux x86_64 | `lan-share-linux-amd64` | `x86_64-linux-musl` |
| Linux ARM64 | `lan-share-linux-arm64` | `aarch64-linux-musl` |
| OpenWrt ARMv7l | `lan-share-openwrt-armv7l` | `arm-linux-musleabihf` |

```bash
sha256sum -c SHA256SUMS.txt      # Windows 用 certutil -hashfile 或 7-Zip 校验
```

仓库里的 `dist/` 目录放了同样这四个产物和 `SHA256SUMS.txt`，克隆下来即可直接用，不必等发布页。

也可以用 [构建](#构建) 里的命令自己编，产物就是单个可执行文件。

### 启动

```bash
./lan-share -p 8080 -d /mnt/sda1/share
```

启动后终端会打印监听地址，例如 `http://192.168.1.100:8080`。其他设备在浏览器中打开该地址，页面会列出在线设备；点设备卡片选文件，或把文件直接拖到卡片上即可发送。

首次打开时服务端为设备分配名称 `主机1`、`主机2`……（跳过在线设备已占用的编号），名称保存在浏览器 `localStorage` 中，刷新不变；也可以在页面顶栏手动改名。浏览器读不到主机名，设备标识只能由编号或手动命名提供。

### 命令行参数

```
用法: lan-share [-p 端口] [-d 文件库目录] [-w 前端目录] [--token 口令] [--quota MiB] [-v|-vv]
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-p` | `8080` | 监听端口；同一端口号的 UDP 兼作局域网 STUN |
| `-d` | `./uploads` | 文件库目录，路由器上应指向外接存储 |
| `-w` | `./public` | 前端静态目录；目录不存在时回退到内嵌资源 |
| `--token` | 关闭 | 可选访问口令，开启后先进入登录页，认证通过才能看到主界面 |
| `--quota` | `4096` | 文件库总配额（MiB），`0` 表示不限制 |
| `-v` / `--verbose` | 关闭 | 输出常规日志（连接、上传、下载、访问记录）；默认只输出错误 |
| `-vv` | 关闭 | 在 `-v` 基础上再加逐帧、逐块、ICE 候选等调试细节 |
| `-h` | — | 显示帮助 |

```bash
./lan-share -p 8080 -d /mnt/sda1/share --token '你的口令' --quota 8192
```

### 设为常驻

OpenWrt 开机自启（`/etc/init.d/lan_share`，`chmod +x` 后 `enable`）：

```sh
#!/bin/sh /etc/rc.common
START=95
STOP=10

start() { service_start /usr/bin/lan-share -p 8080 -d /mnt/sda1/share; }
stop()  { service_stop /usr/bin/lan-share; }
```

Linux 用 systemd 时可参考（路径按实际修改）：

```ini
[Unit]
Description=LAN Share
After=network.target

[Service]
ExecStart=/usr/local/bin/lan-share -p 8080 -d /var/lib/lan-share --quota 4096
Restart=on-failure
DynamicUser=yes
StateDirectory=lan-share

[Install]
WantedBy=multi-user.target
```

`StateDirectory=lan-share` 会创建 `/var/lib/lan-share` 并授予动态用户写权限，`-d` 必须指向同一路径。若改用固定用户，删掉 `DynamicUser` 并按需 `chown` 数据目录。

### 开启访问口令

浏览器访问根路径只会看到登录页（`/login.html`），其它静态资源在认证前一律重定向回登录页——未认证的人看不到主界面，也拿不到 `app.js`。输入正确口令后服务端下发 `HttpOnly + SameSite=Strict` 的 Cookie 并跳转到正常页面，刷新不必重输。

也可以用 `http://<IP>:8080/?token=你的口令` 直接打开，等价于先认证；口令会记入 `localStorage` 并自动从地址栏移除，避免留在历史记录中。

## 怎么用

| 标签页 | 说明 |
| --- | --- |
| 设备直传 | 浏览器之间点对点传输。文件按字节范围切段，并行度取此刻所有已建立的通道（上限 32 条）；通道中断后退役重建，重发同一文件从断点续传 |
| 共享剪贴板 | 服务端保存文字或图片（PNG / JPEG / GIF / WebP），保留最近 32 条，按更新时间淘汰；单项上限 16 MiB，总量上限 128 MiB |
| 服务端文件库 | 上传落盘，任意设备下载。分块写入 `.lanshare-parts/*.part`，收满后改名入库；支持 `Range` 与断点续传 |
| 网速测试 | 分别测量服务端下载、服务端上传、点对点三段吞吐 |

接收大文件时按条件选择保存方式：

| 条件 | 行为 |
| --- | --- |
| 大小 ≤ 32 MiB，或页面非安全上下文 | 下载到内存后触发保存 |
| 大小 > 32 MiB，且为安全上下文（HTTPS / localhost），且浏览器支持 File System Access API | 用户选择保存位置，边收边写，不驻留内存 |

进度按对端确认接收的字节数计算，而不是本端入队量；状态显示"已送达"表示对端已落盘。

网速测试用来区分瓶颈位置：三段都低指向局域网链路（2.4 GHz Wi-Fi、百兆口）；只有点对点低指向 WebRTC / SCTP 层；点对点显示"服务器中转"则说明直连没建起来，吞吐受中转路径限制。

## 出错怎么办

排查第一步：加 `-v` 重跑，日志写 stderr，重定向到文件即可长期保留。

```bash
./lan-share -p 8080 -d /data -v > /var/log/lan-share.log 2>&1
tail -f /var/log/lan-share.log
```

默认只输出错误（内存不足、落盘失败、上传中断、`select` 异常、目录路径过长等），正常使用不打印访问日志；`-v` 每行以 `[时:分:秒]` 开头，`-vv` 再加逐帧、逐块、ICE 候选。

| 现象 | 原因与处理 |
| --- | --- |
| 打不开页面 / 端口被占用 | 换端口 `-p 8081`；确认防火墙放行该 TCP 端口（UDP 同端口用于 STUN，能开更好） |
| 设备可见但传不了，一直「连接中」 | 卡片标注「服务器中转（直连未建立）」即已降级。恢复直连需关掉路由器的 AP / 客户端隔离，或排除 mDNS 组播拦截（IGMP Snooping） |
| 吞吐明显低于预期 | 先跑网速测试定位是哪一段慢；`-vv` 看「候选」行类型：`host` 为局域网直连，`srflx` 为 NAT 映射地址，`relay` 为中转 |
| 传输中途停滞 | 看接收端状态原文。「已中断（N MB），可续传」表示通道断开，重发同一文件自动续传；持续「接收中」表示对端没消费 |
| 传输到一半提示「等待浏览器写入文件超时」 | 接收端磁盘慢或浏览器写盘卡住，换保存位置重试 |
| 一台设备反复提示「目标设备已离线」 | 对端刷新或重连后连接 id 变更，客户端会自动丢弃失效连接；持续出现则查该设备的网络是否频繁断开 |
| 大文件没有弹保存框，内存暴涨 | 非安全上下文（HTTP + 非 localhost）或浏览器不支持 File System Access API，只能整块下到内存。用 HTTPS 或 localhost 打开即可走流式写盘 |
| 上传返回 507 | 文件库配额满（默认 4 GiB），清理文件库或调 `--quota` |
| 登录页反复跳回 | Cookie 被拦截（隐私模式 / SameSite 策略）；改用 `?token=口令` 访问，或换普通窗口 |
| 手机端功能受限 | 系统分享、PWA 安装、通知要求 HTTPS；局域网 HTTP 下传文件正常 |
| 走公网穿透后只能中转且很慢 | cloudflared / ngrok / devtunnel 只转发 HTTP 与 WebSocket，不转发 UDP，WebRTC 必然连不上。跨公网直连要用虚拟组网（Tailscale / EasyTier / ZeroTier）给两端分配可达 IP |
| 重启后 `.part` 半成品消失 | 超过 24 小时无进展的半成品会在启动时与运行中自动清理 |

## 构建

```bash
make                        # 本机编译，产出 ./lan-share
make test                   # 编译并运行端到端测试
node tools/embed-web.js     # 修改 public/ 后重新内嵌前端（make 也会自动触发）
```

前端位于 `public/`，构建时由 `tools/embed-web.js` 生成 `web_assets.h` 并编入二进制；`public/` 只是开发源文件，部署无需分发。

交叉编译使用 zig（自带 musl）：

```bash
zig cc -target arm-linux-musleabihf -static -Os -std=c99 -o lan-share lan_share.c
```

| 产物 | zig target |
| --- | --- |
| `lan-share-linux-amd64` | `x86_64-linux-musl` |
| `lan-share-linux-arm64` | `aarch64-linux-musl` |
| `lan-share-openwrt-armv7l` | `arm-linux-musleabihf` |
| `lan-share-windows-amd64.exe` | `x86_64-windows-gnu`（链接 `-lws2_32`） |
| mipsel 软 / 硬浮点 | `mipsel-linux-musleabi` / `mipsel-linux-musleabihf` |
| 大端 MIPS | `mips-linux-musleabi` |

推 `v*` 标签会触发 `.cnb.yml` 里的发布流水线：先用 `dist/SHA256SUMS.txt` 校验产物，再建 Release 并把四个文件和校验清单挂成附件。

约束：zig 预编译包仅发布于 `ziglang.org/download/<版本>/`；MIPS 目标须写全 ABI（裸 `mipsel-linux-musl` 会报 `unable to provide libc`）；体积优先时用 `-Os` 而非 `-O2`。发布产物需 `-s` 去除符号，并重新生成 `SHA256SUMS.txt`。

## 测试

```bash
make test                    # 端到端，144 项断言
node tools/test-relay.js     # 兜底通道 17 项单测，无需服务端与浏览器
node tools/cdp-scenarios.js  # 双浏览器全场景 70 项：直传/剪贴板/文件库/测速/口令/配额/断线重连
node bench.js 8123           # 服务端吞吐基准（自带起停）
```

`cdp-scenarios.js` 需要 Chrome / Edge，用 `CHROME_PATH` 指定可执行文件；它会自己起三个服务端实例（普通 / 带口令 / 配额 1 MiB），把用户能点到的路径全走一遍，并校验落盘字节、协议状态码与全程无 JS 异常。

真实浏览器端到端由 `tools/p2p-e2e.js` 驱动两个独立 Chrome 实例（CDP，无外部依赖）：

```bash
node tools/p2p-e2e.js --server ./lan-share --file-mb 64 --timeout 240
node tools/p2p-e2e.js --url https://xxxx.trycloudflare.com --require-mode relay
```

## 设计

传输抽象、多 lane 与背压水位线、局域网 STUN、进度语义、HTTP / WebSocket 接口、日志种类、测试拓扑模拟等内部细节见 [DESIGN.md](DESIGN.md)。

## 许可

MIT
