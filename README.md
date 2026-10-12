# LAN Share — C 版

为小内存嵌入式设备（路由器）写的局域网文件分享服务。**只依赖 libc**，静态链接后约 70 KB，常驻内存约 1.5 MB，在 128 MB 内存、2.6/5.4 内核的老路由器上也能跑。

一台机器跑服务，其他设备用浏览器打开即可，不用装任何东西。

> 想要 Node 实现（开发调试、桌面长期运行）？看 [Node 版](../lan-share-node)。

## 两条独立的传输路径

| 路径 | 数据怎么走 | 适合什么 |
| --- | --- | --- |
| **设备直传** | 浏览器之间 WebRTC DataChannel 点对点 | 当场发文件给某台设备，服务端不接触文件内容 |
| **服务端文件库** | 上传落盘到服务端磁盘，再下载 | 存下来慢慢取，或发给不在线的人 |

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

```bash
scp lan-share root@192.168.1.1:/usr/bin/lan-share
scp -r public root@192.168.1.1:/usr/share/lan-share/
ssh root@192.168.1.1 chmod +x /usr/bin/lan-share
```

运行：

```bash
lan-share -p 8080 -d /mnt/sda1/share -w /usr/share/lan-share
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-p` | `8080` | 监听端口 |
| `-d` | `./uploads` | 文件库落盘目录（**建议指到挂载的 U 盘/硬盘**） |
| `-w` | `./public` | 前端静态文件目录 |
| `-h` | — | 显示帮助 |

### 开机自启（OpenWrt）

创建 `/etc/init.d/lan_share`：

```sh
#!/bin/sh /etc/rc.common
START=95
STOP=10

start() {
    service_start /usr/bin/lan-share \
        -p 8080 \
        -d /mnt/sda1/share \
        -w /usr/share/lan-share
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

`test.js` 会自己拉起服务、跑完 38 项断言再关掉它，覆盖：静态托管、目录穿越防护（明文与编码两种）、上传/下载/删除、中文与特殊文件名往返、同名自动重命名、5 MB 文件逐字节完整性、WebSocket 信令全链路（设备发现、改名广播、信令转发、无效目标回执、断线收缩）。

需要本机有 Node ≥ 21（仅用于跑测试，运行服务不需要）。也可以指向已跑起来的服务：

```bash
BASE=http://192.168.1.1:8080 node test.js
```

CI 在 Ubuntu 和 macOS 上跑完整功能测试，并编译 Windows / Linux x64 / Linux ARM64 / OpenWrt ARMv7l 四个目标的二进制，见 [build.yml](.github/workflows/build.yml)。

## 实现说明

- **只依赖 libc**。WebSocket 需要的 SHA-1 与 Base64 都在文件内实现，不链接 OpenSSL。
- **不配 STUN/TURN**。纯内网场景下 WebRTC 用本机内网 IP 的 host candidate 就能直连，完全离线可用。
- **响应头与文件体分流**。下载时 HTTP 头走高优先级缓冲先发出，再流式读盘，避免两者互相覆盖。
- **UTF-8 文件名在 Windows 上走宽字符 API**。Windows 的 ANSI 版 `open`/`fopen` 会拿当前代码页（简体中文是 GBK 936）解释 UTF-8 字节，只要恰好拼出非法的 GBK 双字节组合就失败——症状很随机，例如"最.txt"能建而"中.txt"不能。所以 Windows 分支一律用 `_wopen` / `_wfopen` / `_wstat64` / `FindFirstFileW`。Linux 下路径是原始字节、内核不解释编码，无需转换。
- **文件名安全**。上传前剥离路径、过滤控制字符，并校验最终路径仍在存储目录内，防止目录穿越。

## HTTP 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/files` | 文件列表，`{ files: [{name,size,sizeText,mtime}], total }` |
| POST | `/api/upload?name=xxx` | 请求体为原始文件流，落盘；同名自动追加 `(1)` |
| GET | `/api/download?name=xxx` | 下载，带 UTF-8 文件名的 `Content-Disposition` |
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
