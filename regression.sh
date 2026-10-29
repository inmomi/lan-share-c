#!/bin/bash
# 回归测试：逐条验证报告里修过的问题没有被改回去。
# 用法：bash regression.sh   （先 make，脚本会自己找同目录的产物）
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT" || exit 1
BIN=./lan-share
TMP=$(mktemp -d)
PASS=0; FAIL=0
ck() { if [ "$1" = "$2" ]; then echo "PASS  $3"; PASS=$((PASS+1)); else echo "FAIL  $3 (期望=$2 实际=$1)"; FAIL=$((FAIL+1)); fi; }
cleanup() { kill $S1 $S2 $S3 $S4 $S5 $S6 $S7 $S8 $S9 $S10 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
pkill -f "$BIN -p 83" 2>/dev/null; sleep 0.3

echo "===== Bug D: 产物静态链接 ====="
# 不用 file(1)：直接看 ELF 里有没有 PT_INTERP（有=动态链接）
LINK=$(python3 - "$BIN" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\x7fELF':
    print('not-elf'); raise SystemExit
phoff = struct.unpack_from('<Q', d, 0x20)[0]
phentsize = struct.unpack_from('<H', d, 0x36)[0]
phnum = struct.unpack_from('<H', d, 0x38)[0]
has_interp = any(struct.unpack_from('<I', d, phoff + i * phentsize)[0] == 3 for i in range(phnum))
print('dynamically' if has_interp else 'statically')
PY
)
ck "$LINK" "statically" "Bug D: 产物为静态链接"

echo "===== Bug A: 分块上传超 size 应拒绝 ====="
rm -rf "$TMP/up" && mkdir -p "$TMP/up"
$BIN -p 8301 -d "$TMP/up" > "$TMP/s1.log" 2>&1 &
S1=$!; sleep 1
head -c 200 /dev/urandom > "$TMP/p200"
CODE=$(curl -s -o "$TMP/resp_a" -w '%{http_code}' -X POST -H "X-Lan-Share-Request:1" --data-binary @"$TMP/p200" "http://127.0.0.1:8301/api/upload?name=over.bin&size=100&offset=0")
ck "$CODE" "400" "Bug A: 声明size=100发200字节应返回400"
ck "$(stat -c %s "$TMP/up/over.bin" 2>/dev/null || echo 0)" "0" "Bug A: 被拒后文件不落盘"
# 错误响应体必须是完整 JSON：曾把长度写死成 13，导致只发出 {"error":"bad
BODYOK=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print('OK' if d.get('error') else 'BAD')" "$TMP/resp_a" 2>/dev/null || echo "FAIL")
ck "$BODYOK" "OK" "Bug A: 400 响应体是合法 JSON"
echo "  响应: $(cat "$TMP/resp_a")"

echo "===== Bug A 正向 + 全流程(上传/列表/下载/删除) ====="
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Lan-Share-Request:1" --data-binary @"$TMP/p200" "http://127.0.0.1:8301/api/upload?name=ok.bin&size=200&offset=0")
ck "$CODE" "200" "上传: 声明200发200应成功"
ck "$(stat -c %s "$TMP/up/ok.bin" 2>/dev/null || echo 0)" "200" "上传: 文件200字节"
curl -s "http://127.0.0.1:8301/api/files" | grep -q '"ok.bin"' && ck 1 1 "列表: 含ok.bin" || ck 0 1 "列表: 含ok.bin"
DL=$(curl -s "http://127.0.0.1:8301/api/download?name=ok.bin" | wc -c)
ck "$DL" "200" "下载: 200字节"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE -H "X-Lan-Share-Request:1" "http://127.0.0.1:8301/api/files?name=ok.bin")
ck "$CODE" "200" "删除: ok.bin返回200"
curl -s "http://127.0.0.1:8301/api/files" | grep -q '"ok.bin"' && ck 0 1 "删除后: 不在列表" || ck 1 1 "删除后: 不在列表"

echo "===== 断点续传全流程 ====="
head -c 300 /dev/urandom > "$TMP/p300"
head -c 100 "$TMP/p300" > "$TMP/chunk1"
tail -c 200 "$TMP/p300" > "$TMP/chunk2"
curl -s -o /dev/null -X POST -H "X-Lan-Share-Request:1" --data-binary @"$TMP/chunk1" "http://127.0.0.1:8301/api/upload?name=chunk.bin&size=300&offset=0"
STATUS=$(curl -s "http://127.0.0.1:8301/api/upload-status?name=chunk.bin&size=300")
echo "$STATUS" | grep -q '"received":100' && ck 1 1 "续传: 首块后已收100" || ck 0 1 "续传: 首块后已收100 ($STATUS)"
curl -s -o /dev/null -X POST -H "X-Lan-Share-Request:1" --data-binary @"$TMP/chunk2" "http://127.0.0.1:8301/api/upload?name=chunk.bin&size=300&offset=100"
ck "$(stat -c %s "$TMP/up/chunk.bin" 2>/dev/null || echo 0)" "300" "续传: 最终300字节"
curl -s "http://127.0.0.1:8301/api/download?name=chunk.bin" > "$TMP/dl_chunk"
cmp "$TMP/p300" "$TMP/dl_chunk" && ck 1 1 "续传: 内容逐字节一致" || ck 0 1 "续传: 内容逐字节一致"
# 超长（且需要转义的）文件名：upload-status 的 body 曾被截断后按 snprintf 返回值发送，越界读栈
LONGNAME=$(python3 -c "print('q'*240)")
STATUS=$(curl -s "http://127.0.0.1:8301/api/upload-status?name=$LONGNAME&size=1000000")
echo "$STATUS" | python3 -c "import json,sys; json.loads(sys.stdin.read()); print('OK')" >/dev/null 2>&1 \
  && ck 1 1 "upload-status: 超长文件名仍返回合法 JSON" || ck 0 1 "upload-status: 超长文件名仍返回合法 JSON"

echo "===== 剪贴板全流程 ====="
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Lan-Share-Request:1" -H "Content-Type: text/plain" --data-binary '你好世界' "http://127.0.0.1:8301/api/clipboard?kind=text&mime=text/plain")
ck "$CODE" "200" "剪贴板: 写入文本"
CBDATA=$(curl -s "http://127.0.0.1:8301/api/clipboard/data")
ck "$CBDATA" "你好世界" "剪贴板: 读回内容"

kill $S1 2>/dev/null; sleep 0.5

echo "===== Bug B + Bug E: 长 -d 目录 ====="
LONGDIR="$TMP/$(printf 'a%.0s' {1..200})/$(printf 'b%.0s' {1..200})/$(printf 'c%.0s' {1..200})"
mkdir -p "$LONGDIR"
$BIN -p 8302 -d "$LONGDIR" > "$TMP/s2.log" 2>&1 &
S2=$!; sleep 1
RESP=$(curl -s "http://127.0.0.1:8302/api/info")
PJ=$(echo "$RESP" | python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print('OK' if isinstance(d.get('peers'),int) and isinstance(d.get('uploadDir'),str) else 'BAD')" 2>/dev/null || echo "FAIL")
ck "$PJ" "OK" "Bug B: 长-d目录 /api/info JSON可解析"
echo "$RESP" | grep -q '}$' && ck 1 1 "Bug B: JSON以}结尾" || ck 0 1 "Bug B: JSON以}结尾"
echo "  响应长度: ${#RESP}"
head -c 100 /dev/urandom > "$TMP/p100"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Lan-Share-Request:1" --data-binary @"$TMP/p100" "http://127.0.0.1:8302/api/upload?name=e.bin&size=200&offset=0")
ck "$CODE" "200" "Bug E: 长-d目录分块上传成功"
ls "$LONGDIR/.lanshare-parts/"*.part >/dev/null 2>&1 && ck 1 1 "Bug E: .part写入正确位置" || ck 0 1 "Bug E: .part写入正确位置"
kill $S2 2>/dev/null; sleep 0.5

echo "===== Bug B: -d 含双引号必须转义 ====="
QDIR="$TMP/q\"x"
mkdir -p "$QDIR"
$BIN -p 8307 -d "$QDIR" > "$TMP/s7.log" 2>&1 &
S7=$!; sleep 1
QJ=$(curl -s "http://127.0.0.1:8307/api/info" | python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print('OK' if d.get('uploadDir')==sys.argv[1] else 'BAD')" "$QDIR" 2>/dev/null || echo "FAIL")
ck "$QJ" "OK" "Bug B: 含引号目录 /api/info 可解析且值正确"
kill $S7 2>/dev/null; sleep 0.3

echo "===== Bug C: 多并发连接不崩溃 ====="
$BIN -p 8303 -d "$TMP/up" > "$TMP/s3.log" 2>&1 &
S3=$!; sleep 1
OK=0
for i in $(seq 1 20); do curl -s -o /dev/null "http://127.0.0.1:8303/api/info" && OK=$((OK+1)); done
ck "$OK" "20" "Bug C: 20并发连接全部正常"
pgrep -f "$BIN -p 8303" >/dev/null && ck 1 1 "Bug C: 服务未崩溃" || ck 0 1 "Bug C: 服务未崩溃"
kill $S3 2>/dev/null; sleep 0.5

echo "===== Bug H: TURN 注入只放行合法 JSON ====="
LAN_SHARE_TURN='[{"urls":"turn:x"}]' $BIN -p 8304 -d "$TMP/up" > "$TMP/s4.log" 2>&1 &
S4=$!; sleep 1
grep -q 'TURN 中继:    已启用' "$TMP/s4.log" && ck 1 1 "Bug H: 合法TURN被接受" || ck 0 1 "Bug H: 合法TURN被接受"
kill $S4 2>/dev/null; sleep 0.3
LAN_SHARE_TURN='[</script>]' $BIN -p 8306 -d "$TMP/up" > "$TMP/s6.log" 2>&1 &
S6=$!; sleep 0.5
grep -q '已忽略' "$TMP/s6.log" && ck 1 1 "Bug H: 含<>TURN被拒" || ck 0 1 "Bug H: 含<>TURN被拒"
kill $S6 2>/dev/null; sleep 0.3

echo "===== 日志级别：默认只出错 ====="
$BIN -p 8308 -d "$TMP/up" > "$TMP/s8.log" 2>&1 &
S8=$!; sleep 1
curl -s -o /dev/null "http://127.0.0.1:8308/api/info"
curl -s -o /dev/null "http://127.0.0.1:8308/api/files"
ck "$(grep -c 'api/info\|api/files' "$TMP/s8.log")" "0" "日志: 默认不输出访问日志"
ck "$(grep -c '浏览器打开' "$TMP/s8.log")" "1" "日志: 启动横幅始终输出"
kill $S8 2>/dev/null; sleep 0.3
$BIN -p 8309 -d "$TMP/up" -v > "$TMP/s9.log" 2>&1 &
S9=$!; sleep 1
curl -s -o /dev/null "http://127.0.0.1:8309/api/info"
ck "$(grep -c 'GET /api/info' "$TMP/s9.log")" "1" "日志: -v 输出访问日志"
kill $S9 2>/dev/null; sleep 0.3

echo "===== 口令：登录页 + Cookie 认证 ====="
$BIN -p 8310 -d "$TMP/up" --token 'r3gress!' > "$TMP/s10.log" 2>&1 &
S10=$!; sleep 1
curl -s "http://127.0.0.1:8310/" | grep -q '需要访问口令' && ck 1 1 "口令: 未认证首页是登录页" || ck 0 1 "口令: 未认证首页是登录页"
ck "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:8310/app.js")" "302" "口令: 未认证静态资源 302"
ck "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:8310/api/files")" "401" "口令: 未认证接口 401"
ck "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'X-Lan-Share-Request: 1' "http://127.0.0.1:8310/api/login?token=wrong")" "401" "口令: 错误口令 401"
rm -f "$TMP/ck.txt"
ck "$(curl -s -c "$TMP/ck.txt" -o /dev/null -w '%{http_code}' -X POST -H 'X-Lan-Share-Request: 1' "http://127.0.0.1:8310/api/login?token=r3gress%21")" "200" "口令: 正确口令登录 200"
grep -q 'ls_auth' "$TMP/ck.txt" && ck 1 1 "口令: 下发认证 Cookie" || ck 0 1 "口令: 下发认证 Cookie"
curl -s -b "$TMP/ck.txt" "http://127.0.0.1:8310/" | grep -q '局域网文件分享' && ck 1 1 "口令: 带Cookie进入主页面" || ck 0 1 "口令: 带Cookie进入主页面"
ck "$(curl -s -b "$TMP/ck.txt" -o /dev/null -w '%{http_code}' "http://127.0.0.1:8310/api/files")" "200" "口令: 带Cookie接口 200"
ck "$(curl -s -o /dev/null -w '%{http_code}' -H 'Cookie: ls_auth=deadbeef' "http://127.0.0.1:8310/api/files")" "401" "口令: 伪造Cookie 401"
kill $S10 2>/dev/null

echo ""
echo "========== 回归结果: PASS=$PASS  FAIL=$FAIL =========="
[ $FAIL -eq 0 ] && echo "全部通过" || echo "存在失败"
[ $FAIL -eq 0 ]
