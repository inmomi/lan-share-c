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
DT_PORT=9331
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

# ---- 可选：AP / 客户端隔离 ----
# 把每个 ns 客户端限制为只能访问其网关（宿主机），于是：
#   - nsA <-> nsB 直连被丢弃（设备间互访失败，对应"Wi-Fi 连不上"）；
#   - 到宿主机（网关）的信令 / 中继仍可达（宿主机自身地址走 INPUT，不走 FORWARD）。
# 这样直连必失败，只剩经宿主机 coturn 中继的路径，严格逼出"必须走中继"。
if [ "$ISOLATE" = "1" ]; then
  iptables -A FORWARD -s "$IP_A" ! -d "$GW_A" -j DROP
  iptables -A FORWARD -s "$IP_B" ! -d "$GW_B" -j DROP
  echo "隔离已启用：$IP_A / $IP_B 只能访问各自网关（宿主机），设备间直连被 DROP"
fi

# ---- DevTools 代理：在每个 ns 内监听该 ns 的 IP，转发到 ns 内回环 127.0.0.1:$DT_PORT ----
# 宿主机（测试进程）用 nsA IP 即可连到在 nsA 里跑的 Chrome 调试端口。
nohup ip netns exec "$NS_A" node "$SCRIPT_DIR/devtools-proxy.js" \
  --listen "$IP_A:$DT_PORT" --target "127.0.0.1:$DT_PORT" >/tmp/proxy-a.log 2>&1 &
nohup ip netns exec "$NS_B" node "$SCRIPT_DIR/devtools-proxy.js" \
  --listen "$IP_B:$DT_PORT" --target "127.0.0.1:$DT_PORT" >/tmp/proxy-b.log 2>&1 &
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
  TURN_JSON="[{\"urls\":\"turn:$GW_A:3478?transport=udp\",\"username\":\"lan\",\"credential\":\"sharepass\"},{\"urls\":\"turn:$GW_A:3478?transport=tcp\",\"username\":\"lan\",\"credential\":\"sharepass\"}]"
  echo "coturn 已启动，TURN 配置已生成（中继地址 $GW_A:3478）"
fi

# ---- 写出 realnet.env ----
{
  echo "SERVER_PORT=$SERVER_PORT"
  echo "SERVER_URL_A=http://$GW_A:$SERVER_PORT"
  echo "SERVER_URL_B=http://$GW_B:$SERVER_PORT"
  echo "WRAP_A=ip netns exec $NS_A"
  echo "WRAP_B=ip netns exec $NS_B"
  echo "DEVTOOLS_A=$IP_A"
  echo "DEVTOOLS_B=$IP_B"
  echo "DEVTOOLS_PORT=$DT_PORT"
  if [ -n "$TURN_JSON" ]; then
    echo "LAN_SHARE_TURN=$TURN_JSON"
  fi
} > realnet.env

echo "realnet.env 已生成："
cat realnet.env
