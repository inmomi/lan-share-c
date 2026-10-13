# 测试计划：真实网络（两设备 / Wi-Fi / 中继）覆盖

## 为什么需要这个计划

现有 CI（`e2e-p2p.yml`）只在 `127.0.0.1` 回环上拉起两个 Chrome（同一台机），
而且 `tools/cdp.js` 强制 `--disable-features=WebRtcHideLocalIpsWithMdns`、
且两个浏览器共享本机网络栈。结果是：

- 候选永远是 `127.0.0.1`，从不经过"两台独立设备 / 不同子网 / AP 隔离"的真实路径；
- 所以用户真机问题——**Wi-Fi↔Wi-Fi 直连连不上、中继无效**——CI 完全覆盖不到，
  一直绿却真机失败。这正是 v1.2.x 反复在真机翻车的根因。

要在 GitHub Actions 里重现并守住这些问题，必须在 Runner 上**模拟两台独立主机
（各自独立 IP）**，并能**模拟 AP / 客户端隔离（设备间直连被丢弃）**来逼出
"必须走中继"的路径。

## 模拟方法（GitHub Actions, ubuntu-latest, 需 sudo）

用 Linux 网络命名空间 + 路由（不用 bridge，方便用 iptables 丢包）模拟两台设备：

- 建两个 netns `nsA` / `nsB`，各用一对 veth 接到宿主机做路由：
  - `nsA`：`10.9.1.1/24`，宿主机侧 `10.9.1.254/24`
  - `nsB`：`10.9.2.1/24`，宿主机侧 `10.9.2.254/24`
  - 宿主机开 `ip_forward`，两个 netns 各加默认路由指到宿主机侧网关。
- **服务端**跑在宿主机，监听 `0.0.0.0`，所以 `nsA` 用 `http://10.9.1.254:PORT`、
  `nsB` 用 `http://10.9.2.254:PORT` 连信令（WebSocket）。
- 两个 Chrome 分别用 `ip netns exec nsA` / `nsB` 拉起（复用 `cdp.js` 的
  `launchBrowser`，加 `--chrome-wrap` 前缀）。
- WebRTC 的 host 候选因此变成 `10.9.1.1` / `10.9.2.1`，在宿主机路由上互通
  → 精确模拟"同一 AP 下的两台 Wi-Fi 设备"。
- **AP 隔离模拟**：`iptables -A FORWARD -s 10.9.1.0/24 -d 10.9.2.0/24 -j DROP`
  （双向），但**保留**各自到宿主机（`10.9.x.254`）的链路（信令与中继都走宿主机）。
  这样 host 候选直连必失败，只剩中继可用。

## 测试用例

### T1 — 两主机 LAN（无隔离，happy path）【新增，必过】
- 不丢直连。验证：两端发现 → P2P 建立 → 64MB 文件完整传输 + 逐字节 SHA-256
  校验通过 → 并行 lane 数 > 阈值 → 聚合吞吐达到下限。
- 意义：补上 loopback 漏掉的最基础覆盖——"两台独立设备 + 真实 IP"。

### T2 — AP 隔离 + TURN 中继【新增，关键闸门】
- 开启隔离；CI 内 `apt-get install coturn` 起 TURN 服务，服务端以环境变量
  `LAN_SHARE_TURN` 把中继配置注入页面；`app.js` 的 `makeLane` 把 TURN 加进
  `iceServers`（STUN 保留）。
- 验证：直连被丢弃后，连接仍**通过中继**成功建立、文件完整传输。
- 意义：直接对应"Wi-Fi↔Wi-Fi 连不上 / 中继无效"。该用例在加入 TURN 支持前
  **必然失败**，加入并通过后才满足"通过了才发布"。

### T3 — 吞吐下限（回归）【新增】
- 在 T1（或 T2）上断言聚合吞吐 ≥ 下限（阈值按 CI 基线实测定，避免脆；目标
  明显低于回环、高于单路，例如 LAN 模拟下 ≥ 10 MB/s）。兼顾用户"速度基本达到"
  的诉求，防止回退。

### T5 — 直连彻底不可用 → WebSocket 兜底【新增，保底闸门】
- `ci-realnet.sh setup noudp`：两子网之间只留 TCP，UDP 全丢（WebRTC 的数据面
  是 UDP，ICE/DTLS/SCTP 全在上面，UDP 一断直连无论多少候选都不可能成）。
- 验证：`--require-mode relay` 断言确实降级到了兜底通道，且文件仍完整传到、
  SHA-256 一致。
- 意义：回答"最坏情况下还能不能传"。没有这一档，前面几档全绿也可能只是
  "恰好这条网能直连"。

## 最大的盲区：测试自己关掉了 mDNS（2026-10-13 补）

`tools/cdp.js` 里一直有 `--disable-features=WebRtcHideLocalIpsWithMdns`。
Chrome 默认把 host 候选伪装成 `xxx.local` 的 mDNS 名字、靠组播解析；这条 flag
把伪装关掉，ICE 就能直接拿到真实内网 IP——**于是测试永远复现不出真机上的
故障**：

- 真机：同路由器同 SSID 的两台设备能看到对方（信令是 TCP，通），但直连建不起来；
- 测试：因为关了伪装，直连永远建得起来。

修法两头一起：
1. `launchBrowser` 新增 `mdnsBypass`（默认开，显式传 false 就是按真实浏览器的
   默认行为跑）；`p2p-e2e.js` 用 `--no-mdns-bypass` 触发。T1 现在就带着它跑。
2. 服务端自带局域网 STUN（UDP 同端口），给客户端发"自己真实内网 IP"的
   srflx 候选。Chrome 不对 srflx 做 mDNS 伪装，所以直连不再依赖组播。

配套：加了 `--require-mode p2p|relay`，断言最终走的是哪一层——否则兜底通道
一上来，直连用例会退化成"永远通过"。

### T4 — 回归（保留现有）
- loopback 双 Chrome 完整传输 + SHA 校验（现有 `p2p-e2e`）。
- lossy 扫描（`tc netem` 2% / 0.5% 丢包下 lane 数与聚合随丢包上升）——保留。

## 需要的代码改动

1. **`public/app.js` `makeLane`**：读取 `window.__LAN_SHARE_TURN__`（服务端注入），
   有则加入 `iceServers`（STUN 仍保留）。无则行为不变（纯 LAN 直连）。
2. **`lan_share.c`**：读环境变量 `LAN_SHARE_TURN`，serve `/` 时把占位符
   `/*__TURN_CONFIG__*/ null` 替换为 TURN 配置 JSON（未设置则留 null）。
3. **`tools/cdp.js` `launchBrowser`**：支持 `opts.wrap`（命令前缀数组），用于
   `ip netns exec nsX` 包裹 Chrome。
4. **`tools/p2p-e2e.js`**：支持 `--server-url <url>`（覆盖 `SERVER_URL`）、
   `--chrome-wrap "<prefix>"`（传给 `launchBrowser` 的 `wrap`）。
5. **`tools/ci-realnet.sh`**（新增）：建 netns + 路由、可选隔离、可选起 coturn，
   打印 `SERVER_A` / `SERVER_B` / `WRAP_A` / `WRAP_B` / `TURN_URL` 等变量。
6. **`.github/workflows/build.yml`**：新增 `two-host-lan`、`isolated-relay` 两个
   job（同样在 push:main 跑给反馈、在 `v*` tag 跑用于闸门），并把
   `release.needs` 改为 `[test, build, two-host-lan, isolated-relay]`。

## 通过标准（release gate）

- T1、T2、T3、T5 全绿，**特别是 T2（中继）和 T5（兜底）必须通过**才能发版。
- `release` 仅在打 `v*` tag 且上述全绿时执行。

## 里程碑

1. **T1 + T3**：不需改 app，先补最基础的"两设备 + 真实 IP"覆盖并跑绿。
2. **TURN 支持（app + C server）+ T2**：修"Wi-Fi/中继无效"，跑绿。
3. 接上 `release` 闸门，发版（计划 v1.3.0）。

## 不在本计划内

- 不引入对外部公共 TURN/STUN 的强制依赖；TURN 为**可选**配置，缺省仍走
  STUN + host（纯内网离线可用）。
- 不做 Wi-Fi Direct / Aware 这类操作系统级直连（Web 应用层做不到，且路由器的
  基础设施 Wi-Fi 本就是同 AP 互通，问题在隔离而非 Wi-Fi 本身）。
