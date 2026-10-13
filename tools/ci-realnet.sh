#!/usr/bin/env bash
# 在 GitHub Actions (ubuntu-latest, root) 上用 Linux network namespace 模拟
# 两台独立设备，并可模拟 AP / 客户端隔离，逼出"必须走中继"的路径。
# 同时起 coturn（可选）与 DevTools TCP 代理（每个 ns 一个），并写出 realnet.env
# 供测试脚本读取。
#
# 用法（在已具有 root 的 CI step 内）：
#   bash tools/ci-realnet.sh setup            # 两机直连（T1 happy path）
#   bash tools/ci-realnet.sh setup isolate    # 隔离，设备间直连被丢弃（T2 需中继）
#
# 环境变量：
#   COTURN=1        安装并起 coturn，并在 realnet.env 写入 LAN_SHARE_TURN
#   SERVER_PORT     服务端端口（默认 8471）
#
# 网络布局：
#   nsA  10.9.1.1/24   宿主机侧 10.9.1.254   （veth 对：vetha <-> vetha-h）
#   nsB  10.9.2.1/24   宿主机侧 10.9.2.254   （veth 对：vethb <-> vethb-h）
#   宿主机开 ip_forward，两个 ns 各加默认路由指向宿主机侧网关。
#   两个 Chrome 分别 `ip netns exec nsA/nsB` 拉起 → host 候选是 10.9.1.1 / 10.9.2.1，
#   经宿主机路由互通，精确模拟"同一 AP 下的两台 Wi-Fi 设备"。
#   隔离：把每个 ns 客户端限制为只能访问其网关（宿主机），于是 nsA<->nsB 直连
#   被丢弃，只剩经宿主机的中继（coturn）可用。

set -euo pipefail

NS_A=nsA
NS_B=nsB
SUBNET_A=10.9.1.0/24
SUBNET_B=10.9.2.0/24
IP_A=10.9.1.1
IP_B=10.9.2.1
GW_A=10.9.1.254
GW_B=10.9.2.254
VETH_A=vetha
VETH_B=vethb
# 两个浏览器的 DevTools 端口，必须与 p2p-e2e.js 分配给 A / B 的调试端口一致
# （A=9331、B=9332）。两个 ns 各有独立网络栈，Chrome 在各自 ns 内的 127.0.0.1
# 上监听，代理在各自 ns 的 IP 上按**同一个端口**转发出来——之前两个 ns 都按
# 9331 起代理，B 的 9332 没人转发，于是 B 的 DevTools 永远连不上。
DT_PORT_A=9331
DT_PORT_B=9332
SERVER_PORT="${SERVER_PORT:-8471}"
COTURN="${COTURN:-0}"
ISOLATE=0
for a in "$@"; do [ "$a" = "isolate" ] && ISOLATE=1; done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# 清理可能残留的命名空间 / 进程（幂等，方便重复跑）
cleanup_leftover() {
  set +e
  ip netns del "$NS_A" 2>/dev/null || true
  ip netns del "$NS_B" 2>/dev/null || true
  pkill -f "devtools-proxy.js" 2>/dev/null || true
  pkill -f "turnserver" 2>/dev/null || true
}
cleanup_leftover

# ---- 建 netns + veth + 路由 ----
ip netns add "$NS_A"
ip netns add "$NS_B"

ip link add "$VETH_A" type veth peer name "${VETH_A}-h"
ip link set "${VETH_A}-h" netns "$NS_A"
ip addr add "$GW_A/24" dev "$VETH_A"
ip link set "$VETH_A" up
ip netns exec "$NS_A" ip addr add "$IP_A/24" dev "${VETH_A}-h"
ip netns exec "$NS_A" ip link set "${VETH_A}-h" up
ip netns exec "$NS_A" ip link set lo up
ip netns exec "$NS_A" ip route add default via "$GW_A"

ip link add "$VETH_B" type veth peer name "${VETH_B}-h"
ip link set "${VETH_B}-h" netns "$NS_B"
ip addr add "$GW_B/24" dev "$VETH_B"
ip link set "$VETH_B" up
ip netns exec "$NS_B" ip addr add "$IP_B/24" dev "${VETH_B}-h"
ip netns exec "$NS_B" ip link set "${VETH_B}-h" up
ip netns exec "$NS_B" ip link set lo up
ip netns exec "$NS_B" ip route add default via "$GW_B"

# 主机开启转发（让两个 ns 经主机互访，模拟同一 AP 下两台设备）
sysctl -w net.ipv4.ip_forward=1 >/dev/null
# 反向路径过滤可能把经 veth 转发的包丢掉，关掉省得再排查一轮
sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null 2>&1 || true
sysctl -w net.ipv4.conf.default.rp_filter=0 >/dev/null 2>&1 || true

# ---- 转发 / 隔离规则 ----
# GitHub Runner 预装 Docker，而 Docker 默认把 FORWARD 链策略设成 DROP。
# 不处理的话 nsA <-> nsB 一律不通（宿主机自己的地址走 INPUT 不受影响，于是
# 表现为"到网关通、到对端不通"，第一次跑就被这个假象骗了）。
# 一律用 -I FORWARD 1 插到链首，确保优先于 Docker 留下的规则生效。
if [ "$ISOLATE" = "1" ]; then
  # AP / 客户端隔离：每个 ns 只能访问自己的网关（宿主机），于是
  #   - nsA <-> nsB 直连被丢弃（设备间互访失败，对应"Wi-Fi 连不上"）；
  #   - 到宿主机（网关）的信令 / 中继仍可达（宿主机自身地址走 INPUT，不走 FORWARD）。
  # 这样直连必失败，只剩经宿主机 coturn 中继的路径，严格逼出"必须走中继"。
  iptables -I FORWARD 1 -s "$IP_A" ! -d "$GW_A" -j DROP
  iptables -I FORWARD 1 -s "$IP_B" ! -d "$GW_B" -j DROP
  echo "隔离已启用：$IP_A / $IP_B 只能访问各自网关（宿主机），设备间直连被 DROP"
else
  # 直连：显式放行两个子网互访，绕开 Docker 设的 FORWARD DROP 策略。
  iptables -I FORWARD 1 -s "$SUBNET_A" -d "$SUBNET_B" -j ACCEPT
  iptables -I FORWARD 1 -s "$SUBNET_B" -d "$SUBNET_A" -j ACCEPT
  echo "直连已放行：$SUBNET_A <-> $SUBNET_B 经宿主机转发（绕开 Docker 的 FORWARD DROP）"
fi

# ---- DevTools 代理：在每个 ns 内监听该 ns 的 IP，转发到 ns 内回环的同端口 ----
# 宿主机（测试进程）用 nsX 的 IP 即可连到在 nsX 里跑的 Chrome 调试端口。
# 端口必须和浏览器一一对应：nsA→9331（浏览器 A）、nsB→9332（浏览器 B）。
nohup ip netns exec "$NS_A" node "$SCRIPT_DIR/devtools-proxy.js" \
  --listen "$IP_A:$DT_PORT_A" --target "127.0.0.1:$DT_PORT_A" >/tmp/proxy-a.log 2>&1 &
nohup ip netns exec "$NS_B" node "$SCRIPT_DIR/devtools-proxy.js" \
  --listen "$IP_B:$DT_PORT_B" --target "127.0.0.1:$DT_PORT_B" >/tmp/proxy-b.log 2>&1 &
sleep 1

# ---- 可选：coturn 中继 ----
TURN_JSON=""
if [ "$COTURN" = "1" ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq coturn
  cat > /tmp/turn.conf <<EOF
listening-ip=0.0.0.0
listening-port=3478
relay-ip=0.0.0.0
min-port=49152
max-port=65535
user=lan:sharepass
realm=lan-share.local
no-cli
EOF
  nohup turnserver -c /tmp/turn.conf >/tmp/turn.log 2>&1 &
  sleep 2
  # TURN 预检：从 nsA 里真跑一次中继分配，确认 coturn 可达且凭据可用。
  # Chrome 侧 ICE 失败是静默的（只表现为连不上），先在这里把
  # "coturn 本身有问题" 和 "WebRTC 路径有问题" 区分开。
  if command -v turnutils_uclient >/dev/null 2>&1; then
    if ip netns exec "$NS_A" timeout 20 turnutils_uclient -u lan -w sharepass "$GW_A" \
        >/tmp/turn-probe.log 2>&1; then
      echo "TURN 预检 OK：nsA 能向 $GW_A 成功分配中继"
    else
      echo "::warning::TURN 预检失败（nsA 分配中继失败）"
      echo "---- /tmp/turn-probe.log ----"
      tail -n 20 /tmp/turn-probe.log 2>/dev/null || true
      echo "---- /tmp/turn.log ----"
      tail -n 20 /tmp/turn.log 2>/dev/null || true
    fi
  else
    echo "（未找到 turnutils_uclient，跳过 TURN 预检）"
  fi
  TURN_JSON="[{\"urls\":\"turn:$GW_A:3478?transport=udp\",\"username\":\"lan\",\"credential\":\"sharepass\"},{\"urls\":\"turn:$GW_A:3478?transport=tcp\",\"username\":\"lan\",\"credential\":\"sharepass\"}]"
  echo "coturn 已启动，TURN 配置已生成（中继地址 $GW_A:3478）"
fi

# ---- netns 连通自检（秒级，先于浏览器测试；路由接错立刻现形）----
echo "=== netns 连通自检 ==="
if ip netns exec "$NS_A" ping -c1 -W2 -q "$GW_A" >/dev/null 2>&1; then echo "  nsA -> 网关 $GW_A: OK"; else echo "  nsA -> 网关 $GW_A: FAIL"; fi
if ip netns exec "$NS_B" ping -c1 -W2 -q "$GW_B" >/dev/null 2>&1; then echo "  nsB -> 网关 $GW_B: OK"; else echo "  nsB -> 网关 $GW_B: FAIL"; fi
if [ "$ISOLATE" = "1" ]; then
  if ip netns exec "$NS_A" ping -c1 -W2 -q "$IP_B" >/dev/null 2>&1; then echo "  隔离异常: nsA -> nsB($IP_B) 竟可达"; else echo "  隔离 OK: nsA -> nsB($IP_B) 被 DROP（符合预期）"; fi
else
  if ip netns exec "$NS_A" ping -c1 -W2 -q "$IP_B" >/dev/null 2>&1; then echo "  直连 OK: nsA -> nsB($IP_B) 可达（符合预期）"; else echo "  直连异常: nsA -> nsB($IP_B) 不可达（netns 路由坏了）"; fi
fi

# ---- 写出 realnet.env（值含空格的必须加引号，否则 source 时会被当成命令执行）----
{
  echo "SERVER_PORT=$SERVER_PORT"
  echo "SERVER_URL_A='http://$GW_A:$SERVER_PORT'"
  echo "SERVER_URL_B='http://$GW_B:$SERVER_PORT'"
  echo "GW_A='$GW_A'"
  echo "GW_B='$GW_B'"
  echo "ISOLATE='$ISOLATE'"
  echo "WRAP_A='ip netns exec $NS_A'"
  echo "WRAP_B='ip netns exec $NS_B'"
  echo "DEVTOOLS_A='$IP_A'"
  echo "DEVTOOLS_B='$IP_B'"
  echo "DEVTOOLS_PORT_A='$DT_PORT_A'"
  echo "DEVTOOLS_PORT_B='$DT_PORT_B'"
  if [ -n "$TURN_JSON" ]; then
    echo "LAN_SHARE_TURN='$TURN_JSON'"
  fi
} > realnet.env

echo "realnet.env 已生成："
cat realnet.env
