'use strict';

/* 真实浏览器端到端点对点测试。
 *
 * 前面所有测试都是拿假 DataChannel 在 Node 里跑 pacing 逻辑，能验证边界，
 * 但验证不了"真实 Chrome 的 SCTP 到底能跑多快"——而这正是用户遇到的问题：
 * 同一对设备 SMB 30MB/s，点对点只有 3.2MB/s，4 条并行也只到 5.8MB/s。
 * 瓶颈可能在真实浏览器的 SCTP 栈里，也可能在链路上，猜是猜不出来的。
 *
 * 这个脚本起两个真实浏览器（各自独立进程、独立 profile），都打开应用页面，
 * 让它们互相发现，然后：
 *   1. 跑页面内置的 speedP2p()——和真实传文件共用同一条 pumpFile 通路，
 *      同时报链路诊断（候选对类型 / RTT）和单条 lane 的速率；
 *   2. 真传一个文件（默认 64MB），自动点确认框，等下载落盘，
 *      逐字节校验内容和长度——并行分段拼回去只要错一段，这里就会现形。
 *
 * 用法：
 *   node tools/p2p-e2e.js                       # 自己拉服务端（当前目录的 lan-share）
 *   node tools/p2p-e2e.js --server ./lan-share  # 指定服务端可执行文件
 *   node tools/p2p-e2e.js --url http://127.0.0.1:8099   # 服务端已经起好了
 *   node tools/p2p-e2e.js --file-mb 256 --timeout 300
 *
 * 依赖：只用 Node 内置模块 + tools/cdp.js，不装任何 npm 包。
 * 浏览器：CI 用 ubuntu-latest 自带的 Chrome；本机可以 CHROME_PATH 指过去。
 */

const { spawn } = require('child_process');
const path = require('path');
const os = require('os');
const fs = require('fs');
const crypto = require('crypto');
const { launchBrowser } = require('./cdp.js');

/* ---------------------------------------------------------------- 参数 */

function arg(name, dflt) {
  const i = process.argv.indexOf(`--${name}`);
  if (i < 0) return dflt;
  const v = process.argv[i + 1];
  return v === undefined || v.startsWith('--') ? true : v;
}

const FILE_MB = Number(arg('file-mb', 64));
const TIMEOUT_MS = Number(arg('timeout', 240)) * 1000;
const SERVER_EXE = arg('server', null);
const SERVER_URL = arg('url', null) || arg('server-url', null);
const LABEL = arg('label', 'e2e');
/* 真实双机 / netns 场景：每个浏览器走各自的信令 URL、各自的 netns 包裹前缀、
 * 各自的 DevTools 暴露地址。缺省回退到统一的 SERVER_URL / 同机回环。 */
const SERVER_URL_A = arg('server-url-a', null) || SERVER_URL;
const SERVER_URL_B = arg('server-url-b', null) || SERVER_URL;
const DEVTOOLS_A = arg('devtools-host-a', '127.0.0.1');
const DEVTOOLS_B = arg('devtools-host-b', '127.0.0.1');
/* DevTools 端口：浏览器 A / B 各一个。netns 场景下 ci-realnet.sh 起的转发代理
 * 必须按同样的端口转发（nsA→9331、nsB→9332），两边约定一致才连得上——之前
 * 两个 ns 都按 9331 起代理，B 的 9332 没人转发，于是"DevTools 未就绪"。 */
const DEVTOOLS_PORT_A = Number(arg('devtools-port-a', 9331));
const DEVTOOLS_PORT_B = Number(arg('devtools-port-b', 9332));
/* 吞吐下限（MB/s）：低于此值判失败，用于守住"中继/直连确实能用"的回归。0 表示不检查。 */
const THROUGHPUT_FLOOR = Number(arg('throughput-floor', 0));

/* 全局硬超时：任何意外的永久挂起（比如兜底超时之前、或收尾阶段卡住）都不能
 * 让 job 一直吃到 GitHub 的 30 分钟墙。内部上传循环有 TIMEOUT_MS 上限，
 * 这里在它之外再给足余量后强制退出。 */
const HARD_MS = TIMEOUT_MS + 180000;
setTimeout(() => {
  log(`全局硬超时 ${HARD_MS}ms，强制退出（疑似永久挂起）`);
  process.exit(1);
}, HARD_MS).unref();

/* 把 "ip netns exec nsA" 这样的空格分隔串拆成命令前缀数组。 */
function splitWrap(s) {
  if (!s || typeof s !== 'string') return [];
  return s.trim().split(/\s+/).filter(Boolean);
}
const WRAP_A = splitWrap(arg('chrome-wrap-a', null));
const WRAP_B = splitWrap(arg('chrome-wrap-b', null));
/* lane 数量扫描：--sweep 1,2,4,8,16。回答"并行到底能不能线性扩展"——
 * 4 条在 Runner 上合计约 26MB/s、单条约 6.5MB/s，如果是每关联的上限，
 * 加 lane 就该继续涨；如果到顶了，说明瓶颈是共享的，加 lane 是徒劳。
 * 每一轮都走真实 pumpFile + 对端回执，速率由对端计时，不受发送队列干扰。 */
const SWEEP = arg('sweep', null);
const SWEEP_MB = Number(arg('sweep-mb', 16));
const ROOT = path.join(__dirname, '..');
const PORT = 8471;

/* 文件内容用确定性图案生成，Node 侧能用同样的规则复算哈希，
 * 这样"拼回去对不对"就是逐字节可比对的，而不是只看长度。 */
const PATTERN = (i) => ((i * 7 + (i >>> 3)) & 0xFF);

function expectedBuffer(size) {
  const b = Buffer.allocUnsafe(size);
  for (let i = 0; i < size; i++) b[i] = PATTERN(i);
  return b;
}

function log(...a) {
  /* 直接写 fd 1：管道下游是 CI 日志或被 timeout 杀掉时，console.log 的缓冲
   * 会整段丢失，排查起来全是空的。 */
  fs.writeSync(1, a.join(' ') + '\n');
}
function fail(msg) { log(`FAIL  ${msg}`); process.exitCode = 1; }

/* ------------------------------------------------------------ 服务端 */

function startServer() {
  let exe = SERVER_EXE;
  if (!exe) {
    /* 没指定就按平台找当前目录编出来的二进制 */
    const guesses = process.platform === 'win32'
      ? ['lan-share.exe', 'lan-share']
      : ['lan-share', 'lan-share.exe'];
    exe = guesses.map((g) => path.join(ROOT, g)).find((p) => fs.existsSync(p));
  }
  if (!exe) throw new Error('找不到服务端可执行文件，用 --server 指定');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lanshare-e2e-'));
  const child = spawn(exe, ['-p', String(PORT), '-d', dir, '-w', path.join(ROOT, 'public')], {
    stdio: 'ignore',
  });
  child.on('error', (err) => { throw new Error(`服务端启动失败: ${err.message}`); });
  return child;
}

async function waitServer(url, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    try {
      const r = await fetch(url);
      if (r.ok) return;
    } catch (_) { /* 还没起来 */ }
    if (Date.now() > deadline) throw new Error('服务端 20 秒内未就绪');
    await new Promise((r) => setTimeout(r, 200));
  }
}

/* ------------------------------------------------------------ 浏览器侧帮助 */

const READY = `(function(){
  if (typeof state === 'undefined') return 'no-state';
  if (!state.myId) return 'no-id';
  return state.peers.length ? 'ready' : 'no-peer';
})()`;

async function waitReady(page, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const s = await page.evaluate(READY);
    if (s === 'ready') return;
    if (s === 'no-state' || s === 'no-id') {
      if (Date.now() > deadline) throw new Error(`页面未就绪: ${s}`);
    } else if (Date.now() > deadline) {
      throw new Error('等不到对端出现（两台浏览器没有互相发现）');
    }
    await new Promise((r) => setTimeout(r, 250));
  }
}

const peerIds = (page) => page.evaluate('state.peers.map(p => p.id)');
const peerNames = (page) => page.evaluate('state.peers.map(p => p.name)');

/* 等 WebRTC 连接真正 established。app 在发现对端后会自动建 lane（每条一个
 * RTCPeerConnection），这里轮询每条 lane 的 pc.connectionState，全部 connected
 * 才继续；failed 或超时则把 ICE 候选对诊断打出来，让 CI 日志直接告诉我们
 * "直连/中继到底通没通"，而不是默默卡死等到 job 被掐。 */
async function waitConnected(page, peerId, timeoutMs, minConnected) {
  /* minConnected：至少连上几条才算通（默认 1）。LANES 是 32，全连上要受
   * initiate 的 150ms 逐条建链、以及 TURN 逐条分配中继端口影响，拿"全部连上"
   * 当闸门会误杀；能通不能通看至少 1 条，快不快交给后面的吞吐下限去卡。 */
  const need = minConnected || 1;
  const deadline = Date.now() + timeoutMs;
  let last = { found: false, lanes: [] };
  for (;;) {
    const st = await page.evaluate(`(function(){
      const conn = state.conns.get(${JSON.stringify(peerId)});
      if (!conn) return { found: false };
      return { found: true, lanes: conn.lanes.map(function(l){
        return { cs: l.pc ? l.pc.connectionState : 'no-pc', ice: l.pc ? l.pc.iceConnectionState : 'no-pc' };
      }) };
    })()`);
    last = st;
    if (st.found && st.lanes.length) {
      /* 不把瞬时 failed 判死：app 的 retireLane 会把 failed 的 lane 换成全新
       * 的 pc（回到 new/connecting 重新建链），这种可自愈的抖动不该让闸门失败。 */
      const n = st.lanes.filter((l) => l.cs === 'connected').length;
      if (n >= need) return { ok: true, connected: n, total: st.lanes.length, lanes: st.lanes };
    }
    if (Date.now() > deadline) {
      const lanes = st.found ? st.lanes : [];
      return {
        ok: false, timeout: true,
        connected: lanes.filter((l) => l.cs === 'connected').length,
        total: lanes.length, lanes,
      };
    }
    await new Promise((r) => setTimeout(r, 300));
  }
}

/* 从 getStats 挖出每条 lane 选中的候选对（类型 + 地址）。悬而未连时这份
 * 信息能直接定位是"host 候选没互通"还是"中继根本没用上"。 */
async function dumpConnDiag(page, peerId) {
  try {
    return await page.evaluate(`(async () => {
      const conn = state.conns.get(${JSON.stringify(peerId)});
      if (!conn) return { noConn: true };
      const out = [];
      for (const l of conn.lanes) {
        const pc = l.pc;
        /* ss=signalingState：区分"offer 已发出在等 answer"（have-local-offer）
         * 和"压根没建链"（stable/new）——连不上时这是最关键的一条信息。 */
        const info = {
          cs: pc ? pc.connectionState : null,
          ice: pc ? pc.iceConnectionState : null,
          ss: pc ? pc.signalingState : null,
        };
        if (pc && pc.getStats) {
          try {
            const stats = await pc.getStats();
            let pair = null, local = null, remote = null;
            stats.forEach((s) => {
              if (s.type === 'candidate-pair' && s.state === 'succeeded' && !pair) pair = s;
              if (s.type === 'local-candidate' && !local && s.candidateType) local = s;
              if (s.type === 'remote-candidate' && !remote && s.candidateType) remote = s;
            });
            if (pair) info.pair = { type: pair.candidateType || pair.type };
            if (local) info.local = { type: local.candidateType, addr: (local.address || '') + ':' + (local.port || '') };
            if (remote) info.remote = { type: remote.candidateType, addr: (remote.address || '') + ':' + (remote.port || '') };
          } catch (e) { info.statsErr = String(e); }
        }
        out.push(info);
      }
      return out;
    })()`);
  } catch (e) { return { err: e.message }; }
}

/* ------------------------------------------------------------ 主流程 */

(async () => {
  let server = null;
  let pageA = null;
  let pageB = null;
  const dlDir = fs.mkdtempSync(path.join(os.tmpdir(), 'lanshare-dl-'));

  try {
    const urlA = SERVER_URL_A || `http://127.0.0.1:${PORT}`;
    const urlB = SERVER_URL_B || `http://127.0.0.1:${PORT}`;
    /* 只有完全没给任何 URL 时才自己拉服务端（默认同机回环）。
     * 给了 --server-url* 就认为服务端已经由外部（CI 步骤）起好。 */
    if (!SERVER_URL_A && !SERVER_URL_B && !SERVER_URL) {
      server = startServer();
      await waitServer(urlA, 20000);
    } else {
      await waitServer(urlA, 20000);
    }
    log(`服务端: A→${urlA}  B→${urlB}`);

    log('拉起两个浏览器…');
    pageA = await launchBrowser({
      port: DEVTOOLS_PORT_A, wrap: WRAP_A,
      devtoolsHost: DEVTOOLS_A, devtoolsPort: DEVTOOLS_PORT_A,
    });
    pageB = await launchBrowser({
      port: DEVTOOLS_PORT_B, wrap: WRAP_B,
      devtoolsHost: DEVTOOLS_B, devtoolsPort: DEVTOOLS_PORT_B,
    });
    await pageA.navigate(urlA);
    await pageB.navigate(urlB);
    await waitReady(pageA, 30000);
    await waitReady(pageB, 30000);

    const idsA = await peerIds(pageA);
    const idsB = await peerIds(pageB);
    const namesB = await peerNames(pageB);
    log(`A 看到对端: ${idsA.join(',')}（${namesB.join('、')}）`);
    if (!idsA.length || !idsB.length) throw new Error('两端没有互相发现，无法继续');
    const targetFromA = idsA[0];

    /* ---------------------------------------------------- 0. TCP 基线 */
    /* 同一对进程、同一台机器上服务端 HTTP 上传/下载能跑多快。
     * P2P 数字只有和这个基线比才有意义：基线 300MB/s 而 P2P 5MB/s，
     * 瓶颈在 WebRTC/SCTP 侧；基线也只有 10MB/s，那就是机器/网络的问题。 */
    log('\n--- TCP 基线（服务端上传/下载，同机对照）---');
    try {
      const tcp = await pageA.evaluate(`(async () => {
        const up = await speedUpload();
        const down = await speedDownload();
        return { up: up.mbps, down: down.mbps };
      })()`);
      log(`  上传: ${tcp.up.toFixed(1)} MB/s · 下载: ${tcp.down.toFixed(1)} MB/s`);
    } catch (err) {
      log(`  TCP 基线测速失败（不影响 P2P 结论）: ${err.message}`);
    }

    /* ---------------------------------------------------- 连接闸门
     * 先显式等连接建立再跑测速：否则一旦 netns 直连或中继不通，speedP2p 会
     * 永远 await，整个 job 只能被 GitHub 掐掉。这里 60s 内没 connected 就直接
     * 失败并打印 ICE 诊断，日志里一眼能看出是"host 候选不通"还是"中继没用上"。 */
    log('\n--- 等待 WebRTC 连接建立（闸门，60s）---');
    /* 必须先用 app 自己的 initiate() 把连接发起：getConn 是懒创建的，不调它
     * state.conns 里根本没有这条连接，直接等连接状态只会空等超时（第一次跑就
     * 得到 {"noConn":true}）。initiate 幂等，已建过的 lane 会跳过。 */
    await pageA.evaluate(
      `(async () => { await initiate(${JSON.stringify(targetFromA)}); return true; })()`, 30000);
    const connState = await waitConnected(pageA, targetFromA, 60000, 1);
    if (!connState.ok) {
      /* 两侧都要看：A 停在 have-local-offer 说明 offer 发出去了但没等到 answer，
       * 而 B 侧 noConn 就说明 offer 根本没送到——两种原因修法完全不同。 */
      const myId = await pageA.evaluate('state.myId');
      log(`连接未建立: ${JSON.stringify(connState)}`);
      log(`A 侧（发给 ${targetFromA}）: ${JSON.stringify(await dumpConnDiag(pageA, targetFromA))}`);
      log(`B 侧（发给 ${myId}）: ${JSON.stringify(await dumpConnDiag(pageB, myId))}`);
      throw new Error('WebRTC 连接未在 60s 内建立（netns 直连或中继路径不通）');
    }
    log(`连接已建立: ${connState.connected}/${connState.total} 条 lane connected`);

    /* ---------------------------------------------------- 1. 内置测速 */
    log('\n--- 内置测速（speedP2p，与真实传输同一条 pumpFile 通路）---');
    /* 连跑两次：第一次连接是刚建好的（冷启动，SCTP 拥塞窗口从零开始爬），
     * 第二次连接已经热了。两次差很多就说明慢在冷启动而不是链路。 */
    const runs = [];
    for (let i = 0; i < 2; i++) {
      const t0 = Date.now();
      const sp = await pageA.evaluate(`(async () => {
        const r = await speedP2p(${JSON.stringify(targetFromA)});
        return JSON.parse(JSON.stringify(r));
      })()`);
      const spSecs = ((Date.now() - t0) / 1000).toFixed(1);
      runs.push(sp);
      log(`  [第 ${i + 1} 次${i === 0 ? '（冷连接）' : '（热连接）'}]`);
      log(`    链路诊断 : ${sp.diag ? sp.diag.text : '（无）'}`);
      log(`    本端交付 : ${sp.mbps.toFixed(2)} MB/s（含背压等待，${spSecs}s 墙钟）`);
      log(`    对端收到 : ${(sp.echoed ? sp.echoed.received : 0) / 1048576} MB`
        + ` @ ${sp.peerMbps.toFixed(2)} MB/s`);
      log(`    并行 lane: ${sp.lanes} 条（单条约 ${(sp.mbps / sp.lanes).toFixed(2)} MB/s）`);
    }
    const sp = runs[runs.length - 1];

    /* ---------------------------------------------------- 2. 真实文件传输 */
    log(`\n--- 真实文件传输（${FILE_MB}MB，自动确认接收，逐字节校验）---`);
    await pageB.enableDownloads(dlDir);
    /* 接收端的确认框是自定义 DOM 弹窗（不是 window.confirm），CDP 的对话框
     * 自动应答管不到它。这里挂个 MutationObserver，弹窗一出现就点"确定"。 */
    const hooked = await pageB.evaluate(`(function(){
      const modal = document.getElementById('modal');
      const yes = document.getElementById('modalYes');
      if (!modal || !yes) return 'no-modal';
      const obs = new MutationObserver(() => {
        if (!modal.classList.contains('hidden')) yes.click();
      });
      obs.observe(modal, { attributes: true, attributeFilter: ['class'] });
      window.__autoAccept = obs;
      return 'hooked';
    })()`);
    if (hooked !== 'hooked') fail(`接收端弹窗挂钩失败: ${hooked}`);
    const fileName = `e2e-${LABEL}-${FILE_MB}mb.bin`;

    const sendStart = Date.now();
    await pageA.evaluate(`(async () => {
      const size = ${FILE_MB} * 1024 * 1024;
      const buf = new Uint8Array(size);
      for (let i = 0; i < size; i++) buf[i] = ((i * 7 + (i >>> 3)) & 0xFF);
      const file = new File([buf], ${JSON.stringify(fileName)});
      enqueueSend(${JSON.stringify(targetFromA)}, file);
      return true;
    })()`);

    /* 等发送端记录变成"已送达" */
    let upRec = null;
    const upDeadline = Date.now() + TIMEOUT_MS;
    let lastDump = 0;
    for (;;) {
      upRec = await pageA.evaluate(`(function(){
        for (const r of state.transfers.values()) {
          /* 记录里没有 name 字段（名字只写进 DOM），发送端带 file 对象，按它匹配 */
          if (r.dir === 'up' && r.file && r.file.name === ${JSON.stringify(fileName)}) return { status: r.status, sent: r.sent, size: r.size, done: !!r.done, failed: !!r.failed };
        }
        return null;
      })()`);
      if (upRec && (upRec.done || upRec.failed)) break;
      /* 卡住的时候最需要知道两端各自停在哪：发送记录、对端连接状态、
       * 待确认的 accept、接收端 incoming。每 3 秒打一次。 */
      if (Date.now() - lastDump > 3000) {
        lastDump = Date.now();
        const dump = await pageA.evaluate(`(function(){
          const conns = [];
          for (const c of state.conns.values()) {
            conns.push(c.lanes.map((l) => l.dc ? l.dc.readyState : 'no-dc').join('/'));
          }
          return {
            queue: state.queue === Promise.resolve() ? 'idle' : 'busy',
            pending: state.pendingAccept.size,
            conns,
            incoming: state.incoming ? { name: state.incoming.name, received: state.incoming.received, lanesDone: state.incoming.lanesDone, expected: state.incoming.expectedLanes } : null,
          };
        })()`);
        log(`  …发送端 ${JSON.stringify(upRec)} | A: ${JSON.stringify(dump)}`);
        const dumpB = await pageB.evaluate(`(function(){
          const conns = [];
          for (const c of state.conns.values()) {
            conns.push(c.lanes.map((l) => l.dc ? l.dc.readyState : 'no-dc').join('/'));
          }
          return {
            pending: state.pendingAccept.size,
            conns,
            modalHidden: document.getElementById('modal') ? document.getElementById('modal').classList.contains('hidden') : 'no-modal',
            incoming: state.incoming ? { name: state.incoming.name, received: state.incoming.received, lanesDone: state.incoming.lanesDone, expected: state.incoming.expectedLanes } : null,
            down: [...state.transfers.values()].filter((r) => r.dir === 'down').map((r) => ((r.file && r.file.name) || r.tid) + ':' + r.status),
          };
        })()`);
        log(`  …接收端 ${JSON.stringify(dumpB)}`);
      }
      if (Date.now() > upDeadline) break;
      await new Promise((r) => setTimeout(r, 300));
    }
    const sendSecs = (Date.now() - sendStart) / 1000;

    if (!upRec || !upRec.done) {
      fail(`发送未完成: ${JSON.stringify(upRec)}`);
    } else {
      log(`  发送端: ${(upRec.sent / 1048576).toFixed(1)} MB / ${(upRec.size / 1048576).toFixed(1)} MB`
        + `，${sendSecs.toFixed(1)}s → ${(upRec.size / 1048576 / sendSecs).toFixed(2)} MB/s`);
    }

    /* 等接收端把文件存下来 */
    let transferOk = false;
    let e2eRate = 0;
    const saved = await pageB.waitForDownload(fileName, TIMEOUT_MS);
    /* 真正的端到端速率要算到"文件落盘"为止。发送端的 done 现在代表对端
     * 已组装落盘（收到 file-done 回执），与 waitForDownload 命中的时间点
     * 几乎重合，所以下面的"交付速率"和"端到端"会收敛到同一个数。 */
    const e2eSecs = (Date.now() - sendStart) / 1000;
    if (!saved) {
      fail('接收端没有出现下载文件');
    } else {
      const got = fs.readFileSync(saved);
      const want = expectedBuffer(FILE_MB * 1024 * 1024);
      const hashGot = crypto.createHash('sha256').update(got).digest('hex');
      const hashWant = crypto.createHash('sha256').update(want).digest('hex');
      if (got.length !== want.length) {
        fail(`文件长度不对: ${got.length} != ${want.length}`);
      } else if (hashGot !== hashWant) {
        /* 找出第一个不一致的偏移，直接告诉你是哪一段拼错了 */
        let firstBad = -1;
        for (let i = 0; i < want.length; i++) {
          if (got[i] !== want[i]) { firstBad = i; break; }
        }
        fail(`文件内容不一致（第一个差异在 ${firstBad} 字节，即第 ${Math.floor(firstBad / (FILE_MB * 1024 * 1024 / 4))} 段附近）`);
      } else {
        log(`  接收端: ${(got.length / 1048576).toFixed(1)} MB 落盘，SHA-256 一致 ✓`);
        log(`  交付速率 : ${(FILE_MB / sendSecs).toFixed(2)} MB/s（发起到对端回执 done）`);
        log(`  端到端   : ${(FILE_MB / e2eSecs).toFixed(2)} MB/s（发起到对端落盘，这才是真实速率）`);
        transferOk = true;
        e2eRate = FILE_MB / e2eSecs;
      }
    }

    /* 吞吐下限：守住"直连 / 中继确实能跑通且达到可用速率"的回归。 */
    if (transferOk && THROUGHPUT_FLOOR > 0) {
      if (e2eRate < THROUGHPUT_FLOOR) {
        fail(`端到端吞吐 ${e2eRate.toFixed(2)} MB/s 低于下限 ${THROUGHPUT_FLOOR} MB/s`);
      } else {
        log(`  吞吐下限 : ${e2eRate.toFixed(2)} MB/s ≥ ${THROUGHPUT_FLOOR} MB/s ✓`);
      }
    }

    /* ---------------------------------------------------- 3. 接收端状态 */
    const downRec = await pageB.evaluate(`(function(){
      /* 接收端记录不带 file，测试里只有一个文件，按 size 匹配 */
      for (const r of state.transfers.values()) {
        if (r.dir === 'down' && r.size === ${FILE_MB * 1024 * 1024}) return { status: r.status, sent: r.sent, size: r.size };
      }
      return null;
    })()`);
    log(`  接收端记录: ${JSON.stringify(downRec)}`);

    /* ---------------------------------------------------- 4. lane 数量扫描 */
    if (SWEEP) {
      log(`\n--- lane 数量扫描（热连接，每轮 ${SWEEP_MB}MB，速率由对端计时）---`);
      const counts = String(SWEEP).split(',').map((s) => Number(s.trim()))
        .filter((n) => Number.isFinite(n) && n > 0);
      const rounds = await pageA.evaluate(`(async () => {
        const conn = state.conns.get(${JSON.stringify(targetFromA)});
        const out = [];
        const blob = new Blob([new Uint8Array(${SWEEP_MB} * 1024 * 1024)]);
        for (const n of ${JSON.stringify(counts)}) {
          if (n > conn.lanes.length) { out.push({ n, error: 'lane 不够（只有 ' + conn.lanes.length + ' 条）' }); continue; }
          const lanes = conn.lanes.slice(0, n);
          const ranges = splitRange(0, ${SWEEP_MB} * 1024 * 1024, n);
          /* 和 speedP2p 同一套握手：逐 lane 发 speedtest、等 ready、再泵数据。
           * 丢包链路上控制消息本身会丢（2% 丢包下几十次握手必中几次），
         * 所以每 2 秒重发一次，别一丢就判死。 */
          await Promise.all(lanes.map((lane, i) => new Promise((resolve, reject) => {
            let tries = 0;
            const send = () => {
              tries++;
              try {
                lane.dc.send(JSON.stringify({ t: 'speedtest', bytes: ranges[i].to - ranges[i].from, ch: lane.index }));
              } catch (err) { clearInterval(rep); clearTimeout(to); reject(err); return; }
            };
            const rep = setInterval(send, 2000);
            const to = setTimeout(() => {
              clearInterval(rep);
              lane.dc.removeEventListener('message', onMsg);
              reject(new Error('lane ' + i + ' 对端没有回应测速请求（重发 ' + tries + ' 次）'));
            }, 30000);
            const onMsg = (e) => {
              if (typeof e.data !== 'string') return;
              let m; try { m = JSON.parse(e.data); } catch (_) { return; }
              if (m.t === 'speedtest-ready' && Number(m.ch) === lane.index) {
                clearInterval(rep); clearTimeout(to);
                lane.dc.removeEventListener('message', onMsg); resolve();
              }
            };
            lane.dc.addEventListener('message', onMsg);
            send();
          })));
          const echoes = lanes.map((lane) => new Promise((resolve) => {
            const to = setTimeout(() => resolve(null), 60000);
            const onMsg = (e) => {
              if (typeof e.data !== 'string') return;
              let m; try { m = JSON.parse(e.data); } catch (_) { return; }
              if (m.t === 'speedtest-result' && Number(m.ch) === lane.index) {
                clearTimeout(to); lane.dc.removeEventListener('message', onMsg); resolve(m);
              }
            };
            lane.dc.addEventListener('message', onMsg);
          }));
          await Promise.all(ranges.map((r, i) =>
            pumpFile(lanes[i].dc, blob.slice(r.from, r.to), 0, { chunk: chunkSizeFor(lanes[i]) })
              .then(() => { lanes[i].dc.send(JSON.stringify({ t: 'speedtest-end', ch: lanes[i].index })); })));
          const results = await Promise.all(echoes);
          const received = results.reduce((a, r) => a + (r ? r.received : 0), 0);
          /* 并行时取最慢那条 lane 的耗时当分母，偏保守但不会虚高 */
          const slowest = results.reduce((mx, r) => Math.max(mx, r ? r.ms : 0), 0);
          out.push({
            n, received, slowest,
            mbps: slowest > 0 ? received / 1048576 / (slowest / 1000) : 0,
            perLane: results.map((r) => (r && r.ms > 0 ? (r.received / 1048576) / (r.ms / 1000) : 0)),
          });
        }
        return out;
      })()`);
      for (const r of rounds) {
        if (r.error) { log(`  ${String(r.n).padStart(2)} 条: ${r.error}`); continue; }
        log(`  ${String(r.n).padStart(2)} 条 lane: 合计 ${r.mbps.toFixed(1)} MB/s`
          + `（单条 ${r.perLane.map((x) => x.toFixed(1)).join(' / ')} MB/s）`);
      }
    }

    log(`\n===== ${LABEL}: 测速 ${sp.mbps.toFixed(1)} MB/s`
      + `（对端 ${sp.peerMbps.toFixed(1)}）· 端到端传输 ${(FILE_MB / e2eSecs).toFixed(1)} MB/s =====`);
  } catch (err) {
    fail(`运行异常: ${err.stack || err.message}`);
  } finally {
    if (pageA) await pageA.close();
    if (pageB) await pageB.close();
    if (server) server.kill();
    try { fs.rmSync(dlDir, { recursive: true, force: true }); } catch (_) {}
  }
})().then(() => {
  /* 明确退出：失败后残留的 WebSocket / 浏览器子进程会把事件循环一直撑住，
   * 只设 process.exitCode 而不退出的话，job 会空转到全局硬超时才结束
   * （第一次跑就是 08:09 失败、08:16 才被硬超时收掉）。 */
  process.exit(process.exitCode || 0);
});
