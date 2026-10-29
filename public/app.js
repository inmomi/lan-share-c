'use strict';

/* LAN Share client
 * - Signalling over WebSocket (server only relays offers/answers/ICE).
 * - File bytes travel over a WebRTC DataChannel straight between browsers.
 * - The vault tab uploads to / downloads from the server disk.
 * - DataChannel 的发送 pacing 在 p2p-pump.js（LSPump），浏览器里以全局变量暴露。
 *
 * 背压与分块策略在 p2p-pump.js（LSPump）。注意 DRAIN_LIMIT 必须远小于 Chrome
 * DataChannel 发送队列约 16MB 的硬上限，超了 send() 会直接抛队列满异常。
 */
const { LOW_WATER, LANES, splitRange, chunkSizeFor, pumpFile,
  speedtestBegin, speedtestCount, speedtestFinish, speedtestAbort } = LSPump;

/* 文件库上传的分块大小。走 HTTP，没有 256KB 的限制，取大一点减少往返。 */
const UP_CHUNK = 4 * 1024 * 1024;
/* 进度条刷新间隔。原来每收到一块就写一次 DOM，100MB 文件要动 1600 次，
 * 光重排就把主线程拖住了。改成 120ms 批量刷一次。 */
const UI_TICK = 120;
const STREAM_SAVE_THRESHOLD = 32 * 1024 * 1024;
const STREAM_ACK_WINDOW = 4 * 1024 * 1024;

/* v2：服务端改成"主机1、主机2…"编号，旧的随机动物名（安静鲸鱼之类）在
 * 同一个出口 IP 下会重复，换 key 让老浏览器重新分配一次。 */
const NAME_KEY = 'lanshare.deviceName.v2';

const state = {
  myId: null,
  myName: '',
  peers: [],
  conns: new Map(),
  sendQueues: new Map(),
  incoming: null,
  reconnectDelay: 2000,
  /* 非空表示本连接正在接收对端的测速流量，此时二进制消息只计数不当文件。
   * 计数状态按连接存（conn.speedtest），不是全局——挂在全局会让一个连接的
   * 测速流量把另一个连接的文件分块吞掉。 */
  pendingAccept: new Map(),
  transfers: new Map(),
  targetPeer: null,
  clipboardUpdated: null,
  clipboardSelectedId: null,
  /* 共享框当前显示的内容，供常驻的复制按钮使用；两者最多一个非 null */
  clipboardText: null,
  clipboardImageUrl: null,
  /* 上次 /api/clipboard 的 ETag，没变化时服务端直接回 304 */
  clipboardEtag: null,
  /* 可选访问口令；null 表示服务端没开或还没输入 */
  token: null,
  tokenPrompting: false,
  /* 剪贴板预览的渲染序号：并发的两次刷新只有最后一次能落 DOM */
  clipboardRenderSeq: 0,
  modalResolve: null,   /* 当前正在显示的弹窗的 resolve */
  modalAccept: null,    /* 当前正在显示的弹窗的 onAccept */
  modalQueue: [],       /* 待显示的弹窗（FIFO），见 confirmDialog */
  /* 接收方未完成的半成品，按 fileId 存。通道断了不清空，
   * 对端重发同一个文件时用它里的 received 作为续传起点。 */
  resume: new Map(),
  dirty: new Set(),
};

const MAX_RESUME_SLOTS = 8;
function resumeSet(fileId, inc) {
  if (state.resume.size >= MAX_RESUME_SLOTS && !state.resume.has(fileId)) {
    const oldest = state.resume.keys().next().value;
    if (oldest !== undefined) state.resume.delete(oldest);
  }
  state.resume.set(fileId, inc);
}

const $ = (id) => document.getElementById(id);
const makeId = () => Math.random().toString(36).slice(2, 10) + Date.now().toString(36);

/* ------------------------------------------------------------- 访问口令 */

const TOKEN_KEY = 'lanshare.token';

/* 支持一次性 ?token=xxx 链接：存起来并从地址栏抹掉，避免留在历史记录里。
 * 平时从 localStorage 读，刷新页面不用重新输入。 */
function loadToken() {
  try {
    const url = new URL(location.href);
    const fromUrl = url.searchParams.get('token');
    if (fromUrl) {
      url.searchParams.delete('token');
      history.replaceState(null, '', url.pathname + url.search + url.hash);
      localStorage.setItem(TOKEN_KEY, fromUrl);
      return fromUrl;
    }
    return localStorage.getItem(TOKEN_KEY) || null;
  } catch (_) {
    return null;
  }
}
state.token = loadToken();

/* 所有 /api/* 请求统一带上 CSRF 头和可选口令。用包装 fetch 而不是改几十个
 * 调用点，新增接口也不会漏。 */
const nativeFetch = window.fetch.bind(window);
window.fetch = (input, init) => {
  const url = typeof input === 'string' ? input : (input && input.url) || '';
  if (!url.startsWith('/api/')) return nativeFetch(input, init);
  const opts = Object.assign({}, init);
  const headers = new Headers(opts.headers || undefined);
  headers.set('X-Lan-Share-Request', '1');
  if (state.token) headers.set('X-Lan-Share-Token', state.token);
  opts.headers = headers;
  return nativeFetch(input, opts).then((res) => {
    if (res.status === 401 && state.token) {
      state.token = null;
      try { localStorage.removeItem(TOKEN_KEY); } catch (_) { /* ignore */ }
      showTokenModal();
    }
    return res;
  });
};

/* <img> / <a download> / XMLHttpRequest 不能带自定义头，只能把口令放查询串。 */
function apiUrl(url) {
  if (!state.token) return url;
  return `${url}${url.includes('?') ? '&' : '?'}token=${encodeURIComponent(state.token)}`;
}

function esc(text) {
  return String(text).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

/* 单位跟随数量级：最小单位 KB，往上是 MB / GB / TB（1024 进制）。
 * 个位数保留两位小数，否则 84 字节会显示成 "0.0 KB"，看不出量级。 */
function humanSize(bytes) {
  const n = Math.max(0, Number(bytes) || 0);
  if (n === 0) return '0 KB';
  const units = ['KB', 'MB', 'GB', 'TB'];
  let value = n / 1024;
  let idx = 0;
  while (value >= 1024 && idx < units.length - 1) {
    value /= 1024;
    idx += 1;
  }
  const digits = value < 1 ? 2 : (value < 100 ? 1 : 0);
  return `${value.toFixed(digits)} ${units[idx]}`;
}

/* id → 名字的缓存。对端一掉线 peers 里就没有它了，但提示需要说出是谁，
 * 不然只能显示"已断开的设备"。只保留最近 64 条，别无限长。 */
const peerNameById = new Map();

function rememberPeerName(id, name) {
  if (!id) return;
  peerNameById.delete(id);
  peerNameById.set(id, name);
  while (peerNameById.size > 64) {
    peerNameById.delete(peerNameById.keys().next().value);
  }
}

function peerName(id) {
  const peer = state.peers.find((p) => p.id === id);
  if (peer) return peer.name;
  return peerNameById.get(id) || '已断开的设备';
}

/* 设备名持久化。
 * 服务端每次新连接都会重新随机取名，光靠它刷新一次就换一个名字，
 * 对端根本认不出这台机器是谁。所以本机记住名字，连上就报给服务端。
 * 服务端那边另外按 IP 记一层，作为换浏览器/清缓存时的兜底。 */
function loadSavedName() {
  try {
    return localStorage.getItem(NAME_KEY) || '';
  } catch (_) {
    return '';
  }
}

function saveName(name) {
  try {
    localStorage.setItem(NAME_KEY, name);
  } catch (_) {
    /* 隐私模式下写不了 localStorage，名字这次会话内有效即可 */
  }
}

/** 续传的标识：同名同大小才算同一个文件。带 lastModified 防止文件内容变了却续上。 */
function fileIdOf(name, size, lastModified) {
  return `${name}|${size}|${lastModified || 0}`;
}

function humanSpeed(bytesPerSecond) {
  return `${humanSize(bytesPerSecond)}/s`;
}

/* ------------------------------------------------------------------ toasts */

/* 同一句话 4 秒内只弹一次。对端掉线时，各条 lane 会各自重发一次信令，
 * 服务端每条都回 signal-lost——不合并的话一次断线能连弹十几个相同的提示。 */
const toastRecent = new Map();

function toast(message, ms = 3200) {
  const now = Date.now();
  if ((toastRecent.get(message) || 0) + 4000 > now) return;
  toastRecent.set(message, now);
  const el = document.createElement('div');
  el.className = 'toast';
  el.textContent = message;
  $('toasts').appendChild(el);
  setTimeout(() => el.remove(), ms);
}

/* ------------------------------------------------------------------- modal */

/* 弹窗 DOM 只有一个，但请求可能同时来好几个（多台设备同时发文件）。
 * 用真正的 FIFO 队列：来一个排一个，关掉当前再显示下一个。
 * 曾用"把上一个 resolve 串起来"的写法，那只能串两个：第三个进来时
 * show() 会把 modalResolve 覆盖成自己的，中间的请求就永远等不到结果。 */
function showNextModal() {
  const item = state.modalQueue[0];
  if (!item) {
    state.modalResolve = null;
    state.modalAccept = null;
    return;
  }
  $('modalTitle').textContent = item.title;
  $('modalBody').textContent = item.body;
  $('modal').classList.remove('hidden');
  state.modalResolve = item.resolve;
  state.modalAccept = item.onAccept;
}

function confirmDialog(title, body, onAccept) {
  return new Promise((resolve) => {
    state.modalQueue.push({ title, body, onAccept: onAccept || null, resolve });
    if (state.modalQueue.length === 1) showNextModal();
  });
}

function closeModal(result) {
  const item = state.modalQueue.shift();
  const resolve = item ? item.resolve : state.modalResolve;
  state.modalResolve = null;
  state.modalAccept = null;
  $('modal').classList.add('hidden');
  /* 先显示下一个再 resolve：这样 Promise 的后续逻辑（发 file-accept 等）
   * 不会和弹窗切换抢同一个时序，用户看到的是"关掉一个立刻弹出下一个"。 */
  if (state.modalQueue.length) showNextModal();
  if (resolve) resolve(result);
}

$('modalYes').addEventListener('click', async () => {
  const onAccept = state.modalAccept;
  if (!onAccept) {
    closeModal(true);
    return;
  }
  $('modalYes').disabled = true;
  try {
    const fileHandle = await onAccept();
    closeModal({ accepted: true, fileHandle });
  } catch (err) {
    if (err.name !== 'AbortError') toast(`无法选择保存位置：${err.message}`);
    closeModal(false);
  } finally {
    $('modalYes').disabled = false;
  }
});
$('modalNo').addEventListener('click', () => closeModal(false));

/* ------------------------------------------------------------- 口令弹窗 */

function showTokenModal() {
  if (state.tokenPrompting) return;
  state.tokenPrompting = true;
  $('tokenModal').classList.remove('hidden');
  $('tokenInput').value = '';
  setTimeout(() => $('tokenInput').focus(), 30);
}

function hideTokenModal() {
  state.tokenPrompting = false;
  $('tokenModal').classList.add('hidden');
}

async function submitToken() {
  const value = $('tokenInput').value.trim();
  if (!value) return;
  state.token = value;
  const res = await fetch('/api/info', { cache: 'no-store' });
  if (!res.ok) {
    state.token = null;
    try { localStorage.removeItem(TOKEN_KEY); } catch (_) { /* ignore */ }
    toast('口令错误，请重试');
    $('tokenInput').select();
    return;
  }
  try { localStorage.setItem(TOKEN_KEY, value); } catch (_) { /* ignore */ }
  hideTokenModal();
  if (ws) {
    try { ws.close(); } catch (_) { /* ignore */ }
    ws = null;
  }
  connect();
  refreshVault();
  refreshClipboard(true);
}

$('tokenSubmit').addEventListener('click', submitToken);
$('tokenInput').addEventListener('keydown', (event) => {
  if (event.key === 'Enter') submitToken();
});

/* --------------------------------------------------------------- signalling */

let ws = null;

function connect() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const params = new URLSearchParams();
  if (state.token) params.set('token', state.token);
  /* 名字随握手一起带上：服务端据此保持同一个"主机N"，刷新不换号。
   * 第一次来没有存过名字，服务端会分配一个新编号。 */
  const savedName = loadSavedName();
  if (savedName) params.set('name', savedName);
  const qs = params.toString();
  ws = new WebSocket(`${proto}//${location.host}/ws${qs ? '?' + qs : ''}`);
  /* 兜底数据通道直接收 ArrayBuffer，省掉一次 Blob→ArrayBuffer 的异步转换 */
  ws.binaryType = 'arraybuffer';

  ws.addEventListener('open', () => {
    state.reconnectDelay = 2000;
    const saved = loadSavedName();
    if (saved) {
      state.myName = saved;
      if (document.activeElement !== $('myName')) $('myName').value = saved;
      send({ type: 'rename', name: saved });
    }
  });

  ws.addEventListener('message', (event) => {
    /* 二进制帧 = 兜底数据通道的文件字节。服务端按帧首的 id 转过来的，
     * 这里按同一个 id 找到对端的兜底 lane，交给和 DataChannel 完全相同的
     * onChannelMessage 处理——上层不区分数据是从哪一层来的。 */
    if (typeof event.data !== 'string' && event.data) {
      const frame = LSRelay.parseRelayFrame(event.data);
      if (frame && frame.payload.byteLength) {
        const lane = ensureRelay(frame.from);
        const inc = state.incoming;
        const deferAck = inc && inc.writer && inc.lanes && inc.lanes.has(lane);
        if (!deferAck) {
          lane.ackPending += frame.payload.byteLength;
          if (lane.ackPending >= RELAY_ACK_EVERY) {
            sendSignal(frame.from, { ch: 0, relayAck: lane.ackPending });
            lane.ackPending = 0;
          }
        }
        lane.dc.deliver(frame.payload);
      }
      return;
    }
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch (_) {
      return;
    }
    if (msg.type === 'self') {
      state.myId = msg.id;
      const saved = loadSavedName();
      if (saved) {
        /* 本机定过的名字优先，服务端给的随机名不能覆盖它 */
        state.myName = saved;
        if (document.activeElement !== $('myName')) $('myName').value = saved;
        if (msg.name !== saved) send({ type: 'rename', name: saved });
      } else {
        /* 第一次来：接受服务端名字并记住，此后刷新就一直是它 */
        state.myName = msg.name;
        saveName(msg.name);
        if (document.activeElement !== $('myName')) $('myName').value = msg.name;
      }
      renderPeers();
    } else if (msg.type === 'peers') {
      state.peers = msg.peers.filter((p) => p.id !== state.myId);
      for (const p of state.peers) rememberPeerName(p.id, p.name);
      /* 对端刷新/重连后 id 会变。旧 id 的连接不扔掉的话，它上面的 lane 还会
       * 继续往一个不存在的 id 发信令，服务端每条都回 signal-lost——这就是
       * "一台设备一直弹目标设备已离线"的来源。 */
      const online = new Set(state.peers.map((p) => p.id));
      for (const id of [...state.conns.keys()]) {
        if (!online.has(id)) dropConn(id);
      }
      renderPeers();
    } else if (msg.type === 'signal') {
      handleSignal(msg.from, msg.data);
    } else if (msg.type === 'signal-lost') {
      const who = msg.to ? peerName(msg.to) : '';
      dropConn(msg.to);
      renderPeers();
      toast(who ? `${who} 已离线` : '目标设备已离线');
    }
  });

  ws.addEventListener('close', (event) => {
    /* 记下关闭原因。信令一断兜底通道就跟着废，正在跑的传输会以"连接中断"
     * 收场——但那条报错完全不说明是谁断的。code 1006（异常断开）/ 1009
     * （消息过大）/ 1001（对端离开）分别指向完全不同的原因，没有它只能猜。 */
    window.__wsClose = {
      code: event ? event.code : -1,
      reason: event ? event.reason : '',
      clean: event ? event.wasClean : null,
      at: Date.now(),
      sent: ws ? ws.bufferedAmount : -1,
    };
    /* 也往控制台打一份：出问题时把页面 console 和服务端日志按时间轴对一遍，
     * 就知道"谁在哪一刻断的"，不用两边猜。 */
    console.log('[lan-share] 信令已断:', JSON.stringify(window.__wsClose),
                '我=', state.myId, '已有连接=', state.conns.size);
    state.peers = [];
    /* 兜底通道是挂在信令 WebSocket 上的：连接一断它就废了。必须显式关掉，
     * 否则正在跑的传输会卡在 waitDrain 上（readyState 还是 open，却永远
     * 发不出去），只能等 15s 硬超时。 */
    for (const conn of state.conns.values()) {
      if (conn.relay) {
        conn.relay.dc.close();
        conn.relay = null;
      }
    }
    renderPeers();
    const delay = state.reconnectDelay || 2000;
    state.reconnectDelay = Math.min((state.reconnectDelay || 2000) * 2, 30000);
    setTimeout(() => {
      /* 口令弹窗期间不要空转重连：错误口令会把服务端日志刷满 */
      if (!state.tokenPrompting) connect();
    }, delay);
  });
  ws.addEventListener('error', () => {
    console.log('[lan-share] 信令 WebSocket 错误');
  });
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function sendSignal(to, data) {
  send({ type: 'signal', to, data });
}

/* ==========================================================================
 * 兜底数据通道（WebSocket 中转）——统一传输抽象的第二层
 *
 * 实现在 public/relay-channel.js（独立模块，能在 Node 里直接单测）。上层
 * （file-offer / pumpFile / lane-end / progress）只认一套 DataChannel 式的
 * 接口：send(string|ArrayBuffer) / readyState / bufferedAmount /
 * bufferedAmountLowThreshold / addEventListener('message' | 'bufferedamountlow')。
 * 底下是 WebRTC DataChannel 还是这条 WS 中转，对上层完全一样——所以
 * 「Wi-Fi 点对点」「有线↔Wi-Fi」「AP 隔离」「mDNS 组播被拦」「UDP 被禁」
 * 这些情况不必各写一套逻辑：ICE 建得起来就走 DataChannel，建不起来自动落到
 * 这一层。
 *
 * 为什么必须有这一层：Chrome 默认把 host 候选伪装成 mDNS .local 名字，要靠
 * 组播解析；家用路由器默认开 IGMP Snooping，无线侧经常收不到组播，候选就
 * 永远解析不出来，ICE 一条都建不起来。而 WebSocket 走单播 TCP，且信令本来
 * 就已经通了（页面就是从服务端加载的），所以它一定能通——代价只是多一跳、
 * 没有多 lane 并行。
 * ========================================================================== */

const RELAY_ACK_EVERY = LSRelay.RELAY_ACK_EVERY;
/* 直连宽限期：这段时间内一条 lane 都没开，就认定直连不可用、降级到兜底。
 * 不能等到 LANE_OPEN_TIMEOUT(20s)——用户只会看到界面没反应。 */
const DIRECT_GRACE_MS = 9000;
/* 首包只等"够用的一批"lane：32 条全部错开发完 offer 要 3s 上下，等齐了再传，
 * 用户看到的就是"点了发送没反应"（实测 16MB 只要 0.17s 传完，整单却要 6.7s，
 * 全耗在等 lane）。够 START_BATCH_LANES 条、或首条开好后再等 START_GRACE_MS
 * 就开工；剩下的 lane 在后台继续建，下一次传输直接用。 */
const START_BATCH_LANES = 16;
const START_GRACE_MS = 1500;

/* 兜底通道的出站口：控制消息走信令 JSON，数据帧直接投给 WebSocket。
 * 单独抽出来是为了让 RelayChannel 本身不依赖页面全局（ws / send），
 * 这样它能在 Node 里被单测。 */
function makeRelaySink(peerId) {
  return {
    peerId,
    send(data) {
      if (typeof data === 'string') {
        /* file-offer / accept / lane-end / progress 走原来的信令通道，
         * 服务端原样转发 JSON，不用额外开一条链路。 */
        send({ type: 'signal', to: peerId, data: { ch: 0, relay: data } });
        return;
      }
      if (!ws || ws.readyState !== WebSocket.OPEN) throw new Error('连接中断');
      ws.send(data);
    },
    get bufferedAmount() {
      return ws && ws.readyState === WebSocket.OPEN ? ws.bufferedAmount : 0;
    },
  };
}

/** 从本地 SDP 里数各类 ICE 候选，顺带看 host 候选是不是被伪装成 .local。
 *  降级时把它打出来，一眼就能区分"根本没收集到可用的真实 IP 候选"和
 *  "候选有了但对端不可达"——这两种的修法完全不同，之前只能靠猜。 */
function candidateTypeStats(pc) {
  const sdp = (pc && pc.localDescription && pc.localDescription.sdp) || '';
  const types = {};
  let obfuscated = 0;
  for (const line of sdp.split('\r\n')) {
    if (!line.startsWith('a=candidate:')) continue;
    const f = line.split(' ');
    const typ = f[f.length - 2];
    const addr = f[4];
    if (typ) types[typ] = (types[typ] || 0) + 1;
    if (addr && /\.local$/.test(addr)) obfuscated++;
  }
  const counts = Object.keys(types).map((k) => `${k}:${types[k]}`).join(' ');
  return {
    text: (counts || '无候选') + (obfuscated ? ` · ${obfuscated} 条伪装成 .local` : ''),
    types, obfuscated,
  };
}

/** 拿到（或建好）到某个对端的兜底 lane。它不在 conn.lanes 里——那 32 条是
 *  WebRTC 的，会被 retireLane 换掉；兜底 lane 与 WebSocket 同生命周期。 */
function ensureRelay(peerId) {
  const conn = getConn(peerId);
  if (conn.relay) return conn.relay;
  const dc = new LSRelay.RelayChannel(peerId, makeRelaySink(peerId));
  const lane = {
    session: conn, index: 0, peerId,
    pc: null, dc, ready: Promise.resolve(dc),
    role: 'relay', relay: true, speedtest: null,
    pendingCands: [], ackPending: 0,
  };
  /* 和 attachChannel 的做法保持一致：收到的数据派发成 dc 上的 'message' 事件，
   * 再由它进 onChannelMessage。这样 waitAccept / laneSpeedtest 挂在 dc 上的
   * 应答监听器，在兜底通道上和 DataChannel 上行为完全相同——否则对端回了
   * speedtest-ready 也没人听见，表现就是"对端没有回应测速请求"。 */
  dc.addEventListener('message', (e) => onChannelMessage(lane, e.data));
  /* 信令 WS 一断，这条兜底通道就废了。和 attachChannel 里的 DataChannel 一样
   * 挂 close：否则传输会一直卡在"接收中"，既等不到 lane-end 也没有失败提示。 */
  dc.addEventListener('close', () => {
    if (conn.relay === lane) conn.relay = null;
    markInterrupted(lane);
    renderPeers();
  });
  conn.relay = lane;
  return lane;
}

/* ------------------------------------------------------- peer connections */

/* 每个对端一个 session，里面 LANES 条并行的 lane。
 *
 * 为什么不是一条：一个 RTCPeerConnection 只有一个 SCTP 关联，拥塞窗口、接收
 * 窗口、消息粒度全是按关联算的。实测同一对设备 TCP（SMB）能跑 30MB/s，而单条
 * DataChannel 只有 3.2MB/s——瓶颈在每关联的机制里，不在链路上，所以加带宽
 * 没用，只能加关联。每条 lane 有独立的 pc / DataChannel，也就有独立的
 * ICE + DTLS + SCTP + 拥塞窗口。
 *
 * 反过来，同一条关联上开多个 DataChannel 是没用的：它们共享同一个拥塞窗口，
 * 实测不会有任何提升。 */
function getConn(peerId) {
  let conn = state.conns.get(peerId);
  if (conn) return conn;

  conn = { peerId, lanes: [] };
  for (let i = 0; i < LANES; i++) conn.lanes.push(makeLane(conn, i));
  state.conns.set(peerId, conn);
  return conn;
}

/** 丢掉某个对端的连接状态（对端离线，或刷新后换了 id）。
 * 不清理的话，旧 pc 上的 ICE/DTLS 还会继续往旧 id 发信令，服务端每收到
 * 一条就回一条 signal-lost，界面表现就是"一直弹对方已离线"。
 * *不* 负责渲染，调用方自己决定什么时候重画。 */
function dropConn(peerId) {
  if (!peerId) return;
  const conn = state.conns.get(peerId);
  if (conn) {
    for (const lane of conn.lanes.slice()) {
      try { if (lane.pc) lane.pc.close(); } catch (_) { /* 已经关了 */ }
    }
    if (conn.relay) {
      try { conn.relay.dc.close(); } catch (_) { /* 已经关了 */ }
      conn.relay = null;
    }
    state.conns.delete(peerId);
  }
  if (state.targetPeer === peerId) state.targetPeer = null;
}

/* 构造 ICE 服务器列表：基础 STUN 永远在；若页面被服务端注入了 TURN 配置
 * （window.__LAN_SHARE_TURN__，由服务端按 LAN_SHARE_TURN 环境变量透传），
 * 把其中每一条都加进来。返回新数组，避免污染全局。
 * TURN 为可选：缺省（null）时纯 STUN + host，数据绝不经第三方；一旦服务端
 * 配了 TURN，AP / 客户端隔离下设备间直连被丢弃时即可走中继，不再"中继无效"。 */
function buildIceServers() {
  const servers = [];
  /* 服务端在同一个端口上兼任局域网 STUN（UDP），给客户端发"自己真实内网 IP"
   * 的自反射候选。Chrome 只对 host 候选做 mDNS 伪装，srflx 是真实 IP——
   * 所以只要这个 STUN 可达，直连就不再依赖组播解析。
   *
   * 原来写死 stun.l.google.com 有两个问题：一是国内基本不可达，ICE 要等它超时
   * 才继续（白等几秒）；二是即使可达，同一个局域网里两台设备拿到的 srflx 都是
   * 路由器公网 IP，要靠 NAT 回环才能互通，对局域网直连毫无帮助。 */
  if (typeof location !== 'undefined' && location.hostname) {
    const host = location.hostname.includes(':')
      ? `[${location.hostname}]`
      : location.hostname;
    const port = location.port || (location.protocol === 'https:' ? 443 : 80);
    servers.push({ urls: `stun:${host}:${port}` });
  } else {
    servers.push({ urls: 'stun:stun.l.google.com:19302' });
  }
  const extra = (typeof window !== 'undefined') && window.__LAN_SHARE_TURN__;
  if (Array.isArray(extra)) {
    for (const s of extra) {
      if (s && s.urls) {
        servers.push({
          urls: s.urls,
          username: s.username || '',
          credential: s.credential || '',
        });
      }
    }
    console.log('[lan-share] 已启用 TURN 中继，共 ' + servers.length + ' 个 ICE 服务器');
  }
  return servers;
}

function makeLane(session, index) {
  /* 必须带 STUN：纯 host 候选会被 Chrome 默认伪装成 mDNS .local 名字，两台
   * Windows PC 在真实局域网里经常解析失败，于是 32 条 lane 里绝大多数 ICE
   * 连通性检查都过不了——表现就是回环(loopback)能开 32 条、真机千兆只开 1 条、
   * 速度卡在单关联 ~5MB/s。加 STUN 后浏览器还会收集 srflx 候选（可达的 IPv4 UDP
   * 端点），绕开 mDNS/IPv6 主机候选的不可达问题。默认只加 STUN、不加 TURN
   * （数据绝不经第三方）；若服务端注入了 TURN（见 buildIceServers），则一并加入。
   * 纯离线局域网下 STUN 请求会超时，但 host 候选照常使用，只是 ICE 稍慢一点。 */
  const pc = new RTCPeerConnection({ iceServers: buildIceServers() });
  const lane = {
    session, index, peerId: session.peerId,
    pc, dc: null, ready: null, role: null, speedtest: null,
    pendingCands: [],   // 远端描述还没 set 之前到的候选，先攒着，setRemote 后补加
  };

  /* 信令带 ch，告诉对端这是哪一条 lane 的 offer / candidate */
  pc.addEventListener('icecandidate', (event) => {
    if (event.candidate) sendSignal(session.peerId, { ch: index, candidate: event.candidate });
  });

  pc.addEventListener('datachannel', (event) => attachChannel(lane, event.channel));

  pc.addEventListener('connectionstatechange', () => {
    if (pc.connectionState === 'failed' || pc.connectionState === 'closed') {
      retireLane(lane);
    } else {
      renderPeers();
    }
  });

  return lane;
}

/* 这条 lane 废了：换一个干净的 pc，下次传输时重新建链。
 * 不能把整个 session 扔掉——其他 lane 可能还是好的，正在传的文件也靠它们续。 */
function retireLane(lane) {
  markInterrupted(lane);
  try { lane.pc.close(); } catch (_) { /* 已经关了 */ }
  lane.session.lanes[lane.index] = makeLane(lane.session, lane.index);
  renderPeers();
}

function attachChannel(lane, dc) {
  lane.dc = dc;
  dc.binaryType = 'arraybuffer';
  dc.bufferedAmountLowThreshold = LOW_WATER;
  lane.ready = new Promise((resolve) => {
    dc.addEventListener('open', () => resolve(dc), { once: true });
  });
  /* lane.index 决定这一条上的字节属于文件的哪一段，接收端靠它拼回去 */
  dc.addEventListener('message', (event) => onChannelMessage(lane, event.data));
  dc.addEventListener('close', () => {
    markInterrupted(lane);
    if (lane.dc === dc) {
      lane.dc = null;
      lane.ready = null;
      lane.role = null;
    }
    renderPeers();
  });
  renderPeers();
}

async function initiate(peerId) {
  const conn = getConn(peerId);
  const pending = conn.lanes.filter((l) => !l.dc);
  /* 分两段建，兼顾"首包快"和"别把 mDNS 打爆"：
   *   - 第一段 8 条并发（8 个 offer ≈ 十几个 mDNS 查询，系统扛得住），
   *     首条 lane 通常在 100~300ms 内 open，传输立刻就能开工；
   *   - 其余继续按 120ms 错开发 offer（32 条同时建会一口气灌出 60+ 个组播
   *     查询，被内核丢弃后多数候选解析不出来，真机只剩 1~4 条 lane）。
   * 注意这里只等 offer 发完，不等 ICE 连通——连通由 ensureChannel 轮询。 */
  const FIRST_WAVE = 8;
  const GAP = 120;
  await Promise.all(pending.slice(0, FIRST_WAVE).map(
    (l) => initiateLane(conn, l).catch(() => {})
  ));
  for (const l of pending.slice(FIRST_WAVE)) {
    initiateLane(conn, l).catch(() => {});
    await new Promise((r) => setTimeout(r, GAP));
  }
  return conn;
}

async function initiateLane(session, lane) {
  if (lane.dc) return;
  lane.role = 'offerer';
  const dc = lane.pc.createDataChannel('files', { ordered: true });
  attachChannel(lane, dc);
  const offer = await lane.pc.createOffer();
  await lane.pc.setLocalDescription(offer);
  sendSignal(session.peerId, { ch: lane.index, desc: lane.pc.localDescription });
}

async function handleSignal(from, data) {
  /* 兜底通道的两类消息走的是同一条信令链路，但不属于任何一条 WebRTC lane，
   * 所以必须在按 ch 取 lane 之前先分流：
   *   relay    = 对端发来的控制消息（file-offer / accept / lane-end / progress）
   *   relayAck = 对端回的收字节确认，用来泄发送端的 inflight 窗口 */
  if (data && typeof data.relay === 'string') {
    ensureRelay(from).dc.deliver(data.relay);
    return;
  }
  if (data && data.relayAck) {
    ensureRelay(from).dc.ack(Number(data.relayAck) || 0);
    return;
  }

  const conn = getConn(from);
  const ch = Math.max(0, Math.min(LANES - 1, Number(data.ch) || 0));
  const lane = conn.lanes[ch];

  if (data.desc && data.desc.type === 'offer') {
    // Simple glare handling: if I already sent an offer and my id sorts
    // first, ignore theirs and let mine win. 每条 lane 各自判断。
    if (lane.role === 'offerer' && state.myId && state.myId < from) return;
    lane.role = 'answerer';
    try {
      await lane.pc.setRemoteDescription(data.desc);
      await flushPendingCands(lane);
      const answer = await lane.pc.createAnswer();
      await lane.pc.setLocalDescription(answer);
      sendSignal(from, { ch, desc: lane.pc.localDescription });
    } catch (err) {
      /* 双方同时 offer 时状态机会打架，输了的一方扔掉这条重来 */
      retireLane(lane);
    }
    return;
  }

  if (data.desc && data.desc.type === 'answer') {
    try {
      await lane.pc.setRemoteDescription(data.desc);
      await flushPendingCands(lane);
    } catch (_) { /* 冲突时对方迟到的 answer，忽略 */ }
    return;
  }

  if (data.candidate) {
    try {
      if (lane.pc.remoteDescription && lane.pc.remoteDescription.type) {
        await lane.pc.addIceCandidate(data.candidate);
      } else {
        /* 远端描述还没 set（offer/answer 还在路上，或 glare 互抢时候选比
         * 描述先到），先缓存起来。真实网络下这很常见——候选一旦被直接
         * addIceCandidate 且远端还没 set，会抛错被吞掉、这条候选就永久丢了，
         * 该关联 ICE 就再也连不上。32 条里只剩 1 条能开的现象多半由此而来。 */
        lane.pendingCands.push(data.candidate);
      }
    } catch (err) {
      /* stale candidate, safe to ignore */
    }
  }
}

/* 远端描述 set 完成后，把缓存的候选补加进去。错峰建链 + 缓存候选一起，
 * 让 32 条 lane 在真实局域网里也能基本全部连上。 */
async function flushPendingCands(lane) {
  if (!lane.pendingCands.length) return;
  const q = lane.pendingCands;
  lane.pendingCands = [];
  for (const c of q) {
    try { await lane.pc.addIceCandidate(c); } catch (_) { /* stale */ }
  }
}

/* 单条 lane 等待打开的上限。32 条 lane 里坏一条不该让整个连接永远等下去
 * （丢包链路上 ICE/DTLS 握手失败的概率随 lane 数线性增长）。超时的 lane
 * 会被退役，下次 ensureChannel 时重建，本次传输只用活着的 lane。 */
const LANE_OPEN_TIMEOUT = 20000;

async function ensureChannel(peerId) {
  let conn = state.conns.get(peerId);
  if (!conn) conn = getConn(peerId);
  /* initiate 里首段 8 条并发 offer，其余按 120ms 错开（避免 mDNS 查询风暴），
   * 全部发完要约 3s。不能等它全部 settle 再判断能不能直连——那样光"发现
   * 直连不可用"就要二十几秒。这里让它跑着，同时轮询已经 open 的 lane。 */
  let initiating = Promise.resolve(conn);
  if (conn.lanes.some((l) => !l.dc)) initiating = initiate(peerId);

  /* 后台把剩下的 lane 开完，失败不影响本次传输 */
  initiating.catch(() => {});

  const graceEnd = Date.now() + DIRECT_GRACE_MS;
  const batchEnd = Date.now() + START_GRACE_MS;
  let live = [];
  for (;;) {
    live = conn.lanes.filter((l) => l.dc && l.dc.readyState === 'open');
    if (live.length >= START_BATCH_LANES) break;      /* 够一批，开工 */
    if (live.length > 0 && Date.now() >= batchEnd) break;  /* 首条已开，等到期就开工 */
    if (Date.now() >= graceEnd) break;                /* 一条都没有，走兜底 */
    await new Promise((r) => setTimeout(r, 60));
  }

  if (!live.length) {
    /* 一条都没建起来：典型是 Chrome 把 host 候选伪装成 mDNS .local 名字、
     * 而 AP / 有线无线桥接把组播拦了，候选永远解析不出来。原来这里直接抛
     * "点对点通道未就绪"，用户看到的就是"传不了"——现在落到 WebSocket 兜底
     * 通道，照样能传，只是单路、多一跳。 */
    await initiating.catch(() => {});
    /* 降级不是终点，得说清楚为什么：候选是根本没有、还是被伪装成 .local 之后
     * 解析不出来。分别对应"UDP/端口被封"和"mDNS 组播不通"两种完全不同的网络。 */
    const st = conn.lanes[0] ? candidateTypeStats(conn.lanes[0].pc) : { text: '无 pc' };
    const ice = conn.lanes[0] && conn.lanes[0].pc ? conn.lanes[0].pc.iceConnectionState : '?';
    console.log(`[lan-share] 直连未就绪（ice=${ice}，本地候选 ${st.text}），改用 WebSocket 兜底通道`);
    toast('直连未建立，已改用服务器中转');
    const relay = ensureRelay(peerId);
    return { conn, live: [relay], mode: 'relay' };
  }

  /* 已经够一批了，不再等其余 lane——它们还在后台握手，下次传输直接受益。
   * 只退役明确失败/已关闭的 lane：还在 connecting 的留着，否则刚谈好就被
   * 我们自己掐掉，永远凑不满一批。 */
  live = conn.lanes.filter((l) => l.dc && l.dc.readyState === 'open');
  for (const l of conn.lanes.slice()) {
    const st = l.pc ? l.pc.connectionState : 'closed';
    if (st === 'failed' || st === 'closed') retireLane(l);
  }
  if (!live.length) throw new Error('点对点通道未就绪');
  return { conn, live, mode: 'p2p' };
}

/* ------------------------------------------------------------ data channel */

/** 对端回应接收请求。resolve 出 { ok, offset }，offset 是对方已收到的字节数。 */
function waitAccept(tid) {
  return new Promise((resolve) => {
    state.pendingAccept.set(tid, resolve);
    setTimeout(() => {
      if (state.pendingAccept.has(tid)) {
        state.pendingAccept.delete(tid);
        resolve({ ok: false, offset: 0 });
      }
    }, 60000);
  });
}

/** 单块大小。Chrome 的 sctp.maxMessageSize 默认 262144，超过 send() 会直接抛错。 */

async function sendOne(peerId, file) {
  const { conn, live } = await ensureChannel(peerId);
  const tid = makeId();
  const fileId = fileIdOf(file.name, file.size, file.lastModified);

  const rec = addTransfer({
    tid,
    name: file.name,
    size: file.size,
    dir: 'up',
    peerId,
    peerLabel: peerName(peerId),
    file,
    retry: () => enqueueSend(peerId, file),
  });
  rec.fileId = fileId;

  /* 不在真实文件前跑测速流。只广告真正会用的 lane——对端按交集数段数，
   * 广告 32 条却只用 16 条，对端会永远等不到收尾。 */
  /* 并行度 = 此刻所有已建立的 lane。曾经按 PROBE_LANES(16) 截断，还把这
   * 个数字写进 conn.lanePref 冻结下来：第一次传输赶上 lane 刚开 8 条，就
   * 只发 8 条；下一次 max(8,16)=16，32 条里永远只跑一半——真机上表现为
   * "比上一版慢一倍"。lane 的建立本来就不受这个数影响（initiate 始终建满
   * 32 条），这里没有理由不用。 */
  const want = live.length === 1 && live[0].relay ? 1 : live.length;
  const chosen = live.slice(0, want);

  /* 控制消息走第一条活 lane，数据随后按范围铺到所用 lane 上。
   * lanes 带上本端活着的 lane 编号：对端用自己的活lane 集合取交集，
   * 两边段数才能对齐（任何一边少一条 lane，写死 LANES 就会永远等不到收尾）。 */
  chosen[0].dc.send(JSON.stringify({
    t: 'file-offer', tid, fileId, name: file.name, size: file.size,
    lanes: chosen.map((l) => l.index),
  }));

  const accept = await waitAccept(tid);
  if (!accept.ok) {
    updateTransfer(tid, { status: '对方拒绝', failed: true });
    return;
  }

  const start = accept.offset || 0;
  /* 双方活 lane 的交集才是真能用的：发送端活着但接收端已死的 lane，
   * 字节发过去直接进黑洞，接收端永远等不到那条 lane-end。
   * 注意是在 chosen（本次实际使用的子集）里取交集，不是全部活 lane。 */
  const use = accept.lanes
    ? chosen.filter((l) => accept.lanes.includes(l.index))
    : chosen;
  /* 实际并行路数直接显示出来：如果这里长期只有 1~4 路，速度上不去的
   * 根因就是真机上多数 lane 没建起来（典型是 mDNS 候选解析失败），而不是
   * 数据分发的问题。 */
  console.log('[lan-share] 发送端活 lane:', live.length, '本次使用:', use.length, 'chosen:', chosen.length);
  if (!use.length) throw new Error('对端没有可用的传输通道');
  /* 把 [start, size) 均分给活 lane。段与段首尾相接，接收端按 lane 顺序
   * 拼回去就是原文件——所以这里切的顺序不能乱，也不能漏段。
   * 每条 lane 是独立的 SCTP 关联，拥塞窗口互不影响：丢包链路上单关联
   * 的窗口会坍缩，多关联并联是对抗手段（tools/p2p-e2e.js --sweep：2% 丢包下
   * 4 条 0.9MB/s、32 条 5.1MB/s）。 */
  /* 走的是哪一层必须让用户看见：直连 = "N 路并行"，兜底 = "服务器中转"。
   * 之前只有"点对点通道未就绪"一句报错，用户既不知道为什么、也没有退路。 */
  const laneLabel = (use.length === 1 && use[0].relay)
    ? '1 路 · 服务器中转（直连未建立）'
    : use.length + ' 路并行';
  const ranges = splitRange(start, file.size, use.length);
  /* 空文件（size=0）或"上次已经全收完"（start=size）时 splitRange 返回空数组：
   * 没有任何 lane 会发数据，也就没有任何 lane 会发 lane-end，接收端永远等不到
   * 收尾，发送端一直停在"已发出，等待对端保存"。这里补一条 lane-end 让对端
   * 走完收尾流程——接收端的 expectedLanes 也是 0，收到即齐。 */
  if (!ranges.length && use[0].dc && use[0].dc.readyState === 'open') {
    try {
      use[0].dc.send(JSON.stringify({ t: 'lane-end', tid, ch: 0 }));
    } catch (_) { /* 通道刚关：下面照样进入等待送达，失败由回执超时兜底 */ }
  }
  const laneSent = use.map(() => 0);
  const sentTotal = () => start + laneSent.reduce((a, b) => a + b, 0);
  const report = () => {
    updateTransfer(tid, { sent: sentTotal(), status: '传输中 · ' + laneLabel });
  };
  const waitForFileWriteAck = accept.stream ? async () => {
    const deadline = Date.now() + 120000;
    while (sentTotal() - Math.max(start, rec.acked || start) >= STREAM_ACK_WINDOW) {
      if (rec.failed || Date.now() > deadline) throw new Error('等待浏览器写入文件超时');
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
  } : null;

  const sendStart = Date.now();
  await Promise.all(ranges.map((range, i) => {
    if (range.to <= range.from) return Promise.resolve();
    const lane = use[i];
    /* 字节搬运在 p2p-pump.js 的 pumpFile 里：分块读 + 预读 + 背压 + 队列满重试。
     * 那边可以用假 DataChannel 在 Node 里跑回归测试。end 让每条 lane 只发自己那段。 */
    return pumpFile(lane.dc, file, range.from, {
      chunk: chunkSizeFor(lane),
      end: range.to,
      waitForAck: waitForFileWriteAck,
      onProgress: (sent) => {
        laneSent[i] = sent - range.from;
        report();
      },
    }).then(() => {
      /* 这一段发完了。接收端按 lane 计数，齐了才组装文件——不能用 lane 0 上的
       * 一条 file-end 收尾，跨通道没有顺序保证，它会追过别的 lane 的数据。 */
      lane.dc.send(JSON.stringify({ t: 'lane-end', tid, ch: i }));
    });
  }));

  /* 全部 lane-end 都已发出，本端不再发数据。但"已送达"要等对端 file-done
   * 回执才算数——此刻对端可能还有几十 MB 在队列里没落盘，这里把进度拉满、
   * acked 也拉满，会让发送端显示冲到接收端前面一大截、速率飙上天（就是
   * 用户看到的"做过山车"）。所以只标 sent，进度仍由对端回执驱动。 */
  updateTransfer(tid, { status: '已发出，等待对端保存', sent: file.size });

  /* 对端同一时刻只接收一个文件（state.incoming 是单槽位）。这里等"已送达"
   * 回执再返回，sendQueues 才能把同一对端的多次发送真正串起来——否则第二个
   * 文件会在第一个还没落盘时开传，接收端只能中断前一个，发送端则永远停在
   * "已发出，等待对端保存"。回执迟到不应卡死队列，超时就放行。 */
  const ackDeadline = Date.now() + 60000;
  while (!rec.done && !rec.failed && Date.now() < ackDeadline) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}

function enqueueSend(peerId, file) {
  /* 按对端分组队列：给 A 发大文件不再阻塞给 B 发文件 */
  const q = state.sendQueues.get(peerId) || Promise.resolve();
  state.sendQueues.set(peerId, q.then(() => sendOne(peerId, file)).catch((err) => {
    const rec = [...state.transfers.values()].reverse().find(
      (r) => r.file === file && !r.done
    );
    if (rec) updateTransfer(rec.tid, { status: `发送失败：${err.message}`, failed: true });
    toast(`发送「${file.name}」失败：${err.message}`);
  }));
}

function onChannelMessage(lane, data) {
  if (typeof data === 'string') {
    let msg;
    try {
      msg = JSON.parse(data);
    } catch (_) {
      return;
    }
    /* 一旦进入正式的文件协议，就认为这条 session 上的测速状态作废。
     * 双保险：除了 deadline 超时，也要保证测速残留不会把文件分块吞掉。 */
    if (msg.t === 'file-offer' || msg.t === 'file-accept'
      || msg.t === 'file-reject' || msg.t === 'lane-end') {
      for (const l of lane.session.lanes) speedtestAbort(l);
    }
    if (msg.t === 'file-offer') {
      askReceive(lane, msg);
    } else if (msg.t === 'progress') {
      /* 对端汇报实收字节数。发送端的进度条和速率以它为准——
       * 本端的 sent 只是"交给发送队列"的字节，会被高水位吸收而虚高。
       * 对端在 16/32 条 lane 上各发各的 progress，跨 lane 没有顺序保证：
       * 晚到的旧值会把 acked 拉回去（进度条缩水、速率算出负数再被压成 0），
       * 显示就"做过山车"。所以 acked 只增不减。 */
      const pr = state.transfers.get(msg.tid);
      const received = Number(msg.received) || 0;
      if (pr && (pr.acked == null || received >= pr.acked)) {
        updateTransfer(msg.tid, { acked: received });
      }
    } else if (msg.t === 'file-done') {
      /* 对端已组装落盘，发送端这才算真正送达——而不是本端把字节交给
       * 发送队列就当完事（那种假完成会让发送端进度冲到接收端前面一大截）。 */
      updateTransfer(msg.tid, { acked: Number(msg.received) || 0, status: '已送达', done: true });
    } else if (msg.t === 'file-abort') {
      /* 对端明确放弃了这次接收（被新任务顶掉等）。不标失败的话发送端会一直
       * 停在"已发出，等待对端保存"，用户以为还在传。 */
      const ab = state.transfers.get(msg.tid);
      if (ab && !ab.done && !ab.failed) {
        updateTransfer(msg.tid, {
          status: `对端中断接收：${msg.reason || '被新任务打断'}`, failed: true,
        });
      }
    } else if (msg.t === 'file-accept' || msg.t === 'file-reject') {
      const resolve = state.pendingAccept.get(msg.tid);
      if (resolve) {
        state.pendingAccept.delete(msg.tid);
        resolve({
          ok: msg.t === 'file-accept',
          offset: Number(msg.offset) || 0,
          stream: Boolean(msg.stream),
        });
      }
      if (msg.t === 'file-reject') toast('对方拒绝了接收');
    } else if (msg.t === 'lane-end') {
      /* 每条 lane 发完自己那一段就报一次。段数是双方用同一个 splitRange
       * 算出来的，所以齐了就是真齐了，不会早也不会晚。 */
      const inc = state.incoming;
      if (inc && inc.tid === msg.tid) {
        inc.lanesDone = (inc.lanesDone || 0) + 1;
        if (inc.lanesDone >= inc.expectedLanes) finishReceive(lane);
      }
    } else if (msg.t === 'speedtest') {
      /* 对端在测链路速度：接下来这条连接上的二进制只计数，不弹窗、不进文件库。
       * 计数挂在 lane 上并有硬超时，万一 speedtest-end 永远不来也不会
       * 把后续真实文件的分块吞掉（那种症状就是传输无限卡死）。 */
      speedtestBegin(lane, msg.bytes, Date.now());
      lane.dc.send(JSON.stringify({ t: 'speedtest-ready', ch: lane.index }));
    } else if (msg.t === 'speedtest-end') {
      const st = speedtestFinish(lane, Date.now());
      if (st) {
        lane.dc.send(JSON.stringify({
          t: 'speedtest-result',
          ch: lane.index,
          received: st.received,
          ms: st.ms,
        }));
      }
    } else if (msg.t === 'speedtest-abort') {
      /* 发送方没能正常收尾，明确告诉接收端作废计数状态 */
      speedtestAbort(lane);
    }
    return;
  }

  /* 测速进行中：只累加字节数。放在 state.incoming 判断之前，
   * 保证测速数据永远不会被误当成文件内容。 */
  if (speedtestCount(lane, data.byteLength, Date.now())) return;

  const inc = state.incoming;
  if (inc && inc.writer) {
    const part = inc.laneOffsets[lane.index];
    if (!part) return;
    if (part.received + data.byteLength > part.length) {
      markInterrupted(lane);
      return;
    }
    const position = part.from + part.received;
    part.received += data.byteLength;
    inc.writeQueue = inc.writeQueue.then(async () => {
      if (inc.writeError) return;
      await inc.writer.write({ type: 'write', position, data });
      part.saved += data.byteLength;
      if (lane.relay) {
        lane.ackPending += data.byteLength;
        if (lane.ackPending >= RELAY_ACK_EVERY) {
          sendSignal(lane.peerId, { ch: 0, relayAck: lane.ackPending });
          lane.ackPending = 0;
        }
      }
      recordReceivedProgress(lane, inc, data.byteLength);
    }).catch((err) => {
      inc.writeError = err;
      if (state.incoming === inc) markInterrupted(lane);
    });
    return;
  }
  if (inc && inc.laneBufs[lane.index]) {
    /* 这一条 lane 对应文件的一段，分块按到达顺序进本段的缓冲。
     * lane 内部是有序的（ordered:true），段与段之间靠 splitRange 的顺序拼。 */
    inc.laneBufs[lane.index].push(data);
    recordReceivedProgress(lane, inc, data.byteLength);
  }
}

function recordReceivedProgress(lane, inc, bytes) {
  inc.received += bytes;
  updateTransfer(inc.tid, { sent: inc.received });
  const now = Date.now();
  if (inc.received - (inc.lastAck || 0) >= 1024 * 1024
    || now - (inc.lastAckTs || 0) >= 500) {
    inc.lastAck = inc.received;
    inc.lastAckTs = now;
    try {
      lane.dc.send(JSON.stringify({ t: 'progress', tid: inc.tid, received: inc.received }));
    } catch (_) { /* 通道正在关，下一条数据也到不了了 */ }
  }
}

async function askReceive(lane, msg) {
  const from = peerName(lane.peerId);
  /* 同一个文件之前收到一半就断了：用已收字节数作为续传起点，
   * 让对端从断点接着发，而不是从 0 重来。 */
  let prior = msg.fileId ? state.resume.get(msg.fileId) : null;
  if (prior && prior.writerClose) await prior.writerClose;
  if (prior && prior.writeError) {
    state.resume.delete(msg.fileId);
    prior = null;
  }
  const already = prior ? prior.received : 0;

  const canStreamSave = msg.size >= STREAM_SAVE_THRESHOLD
    && window.isSecureContext && typeof window.showSaveFilePicker === 'function';
  const resumeStream = Boolean(prior && prior.streamBacked && prior.fileHandle);
  const resumeHandle = resumeStream ? prior.fileHandle : null;
  const picker = canStreamSave && !resumeHandle
    ? () => window.showSaveFilePicker({ suggestedName: msg.name })
    : null;
  const saveHint = canStreamSave || resumeHandle
    ? '\n大文件将直接写入你选择的位置，减少内存占用。'
    : (msg.size >= STREAM_SAVE_THRESHOLD
      ? '\n当前浏览器/HTTP 页面不支持流式写盘，接收期间可能占用较多内存。'
      : '');
  let accepted;
  if (already > 0 && already < msg.size) {
    accepted = await confirmDialog(
      '继续接收',
      `${from} 继续发送「${msg.name}」(${humanSize(msg.size)})，`
        + `本机已收到 ${humanSize(already)}，将从断点续传。${saveHint}`,
      picker
    );
  } else {
    accepted = await confirmDialog(
      '收到传输请求',
      `${from} 想发送「${msg.name}」(${humanSize(msg.size)})${saveHint}`,
      picker
    );
  }
  if (!accepted || (typeof accepted === 'object' && !accepted.accepted)) {
    lane.dc.send(JSON.stringify({ t: 'file-reject', tid: msg.tid }));
    return;
  }
  const fileHandle = (typeof accepted === 'object' && accepted.fileHandle) || resumeHandle || null;
  let writer = null;
  try {
    if (prior && prior.writerClose) await prior.writerClose;
    if (prior && prior.writeQueue) await prior.writeQueue;
    writer = prior && prior.writer ? prior.writer : null;
    if (!writer && fileHandle) {
      writer = await fileHandle.createWritable({ keepExistingData: resumeStream });
    }
    if (writer && resumeStream) await writer.truncate(already);
    if (writer && !resumeStream && prior && already > 0) await writer.truncate(0);
  } catch (err) {
    lane.dc.send(JSON.stringify({ t: 'file-reject', tid: msg.tid }));
    toast(`无法打开保存文件：${err.message}`);
    return;
  }

  /* 期望的段数 = 双方活 lane 的交集大小（和发送端算的是同一个交集：
   * 它拿本端活集合和回传的集合取交集后切段）。写死 LANES 会在 lane 死亡
   * 或文件很小、段数不足时永远等不到收尾。
   * 老版本对端不发 lanes 字段时退回本端数量（双方都全活时相等）。 */
  /* 兜底通道不在 session.lanes 里（它是 conn.relay），且整条链路只有它一条：
   * 直接按 [0] 走，别拿 32 条 WebRTC lane 的开合状态去和它取交集——
   * 那边可能一条都没开，交集算出来是 0，收端会误判成"没有可用通道"而拒绝。 */
  const myLive = lane.relay
    ? [0]
    : lane.session.lanes
      .filter((l) => l.dc && l.dc.readyState === 'open')
      .map((l) => l.index);
  console.log('[lan-share] 接收端活 lane:', myLive.length, '对端要约:', (msg.lanes || []).length);
  const sharedLive = Array.isArray(msg.lanes) && msg.lanes.length
    ? myLive.filter((i) => msg.lanes.includes(i))
    : myLive;
  const useCount = Array.isArray(msg.lanes) && msg.lanes.length
    ? sharedLive.length
    : LANES;
  if (useCount <= 0) {
    lane.dc.send(JSON.stringify({ t: 'file-reject', tid: msg.tid }));
    return;
  }
  const inc = {
    tid: msg.tid,
    fileId: msg.fileId,
    name: msg.name,
    size: msg.size,
    from,
    /* 续传时上一段已经收到的字节。上一段的 lane 缓冲按 lane 顺序摊平就是
     * [0, already)，直接接在新数据前面，不需要重新分组。 */
    priorChunks: writer ? [] : ((prior && already < msg.size) ? prior.laneBufs.flat() : []),
    laneBufs: lane.session.lanes.map(() => []),
    writer,
    streamBacked: Boolean(writer),
    fileHandle,
    writerClose: null,
    writeQueue: Promise.resolve(),
    writeError: null,
    laneOffsets: Object.create(null),
    resumeFrom: already,
    lanesDone: 0,
    /* 这次传输实际在用的通道。判"中断"时必须拿它过滤——直连建不起来时文件走
     * 兜底通道，而同期那 32 条 WebRTC lane 还在陆续 ICE 失败被判死刑，它们的
     * 死活跟这次传输毫无关系（详见 markInterrupted）。 */
    lanes: new Set(lane.relay ? [lane] : sharedLive.map((i) => lane.session.lanes[i])),
    expectedLanes: splitRange(already, msg.size, useCount).length,
    received: already,
    /* progress 回执的节流状态：每 1MB 或 500ms 报一次实收字节数 */
    lastAck: already,
    lastAckTs: 0,
  };
  if (writer && !resumeStream && prior && already > 0) {
    let position = 0;
    try {
      for (const chunk of prior.laneBufs.flat()) {
        await writer.write({ type: 'write', position, data: chunk });
        position += chunk.byteLength;
      }
    } catch (err) {
      await writer.abort().catch(() => {});
      lane.dc.send(JSON.stringify({ t: 'file-reject', tid: msg.tid }));
      toast(`无法写入续传数据：${err.message}`);
      return;
    }
  }
  const transferRanges = splitRange(already, msg.size, useCount);
  transferRanges.forEach((range, index) => {
    const laneIndex = lane.relay ? 0 : (sharedLive[index] ?? myLive[index] ?? 0);
    inc.laneOffsets[laneIndex] = {
      from: range.from,
      length: range.to - range.from,
      received: 0,
      saved: 0,
    };
  });
  /* 并发保护：已有未完成的接收时先妥善中断它（存为可续传），
   * 避免被新文件无条件覆盖导致静默数据损坏。真并发需按 tid 重构，此处先止损。 */
  if (state.incoming && state.incoming !== inc && !state.incoming.done) {
    const preempted = state.incoming;
    markInterrupted(null);
    /* 必须告诉发送端这个 tid 已被顶掉，否则它永远停在"等待对端保存"。 */
    if (lane.dc && lane.dc.readyState === 'open') {
      try {
        lane.dc.send(JSON.stringify({
          t: 'file-abort', tid: preempted.tid, reason: '对端开始接收新文件',
        }));
      } catch (_) { /* 通道正在关，发送端靠回执超时兜底 */ }
    }
  }
  state.incoming = inc;
  if (inc.fileId) resumeSet(inc.fileId, inc);

  addTransfer({
    tid: msg.tid, name: msg.name, size: msg.size, dir: 'down', peerId: lane.peerId, peerLabel: from,
  });
  updateTransfer(msg.tid, { status: '接收中 · ' + myLive.length + ' 路', sent: inc.received });
  /* 回传本端活着的 lane：发送端取交集后切段，段数和这边才对得上 */
  lane.dc.send(JSON.stringify({
    t: 'file-accept', tid: msg.tid, fileId: msg.fileId, offset: inc.received,
    lanes: myLive, stream: Boolean(writer),
  }));
}

/** 通道中途断开：保留已收到的分块，等对方重发同一个文件后续上。 */
function markInterrupted(lane) {
  /* 通道断了，这条 lane 上的测速计数状态也必须作废 */
  if (lane) speedtestAbort(lane);
  const inc = state.incoming;
  if (!inc) return;

  /* 死掉的这条 lane 不是本次传输在用的通道，那就跟它无关，不能顺着往下判中断。
   *
   * 这是个必现的竞态，不是偶发：直连建不起来时文件走兜底通道，而同一时间轴上
   * 那 32 条 WebRTC lane 还在各自跑 ICE，失败后由 retireLane 依次调到这里。
   * 老代码不看 lane 归属，于是"第一批判死刑"的那一刻就把正在传的传输写成
   * 「已中断（N MB），可续传」，同时 state.incoming 被置空——后续字节全部没人
   * 接，界面就停在传了几 MB 然后中断，对端却还在发（最终"发送未完成"）。
   *
   * 断点落在哪完全由两条时间轴的相对快慢决定（兜底吞吐 × 首条 lane 判死刑的
   * 时刻），所以真机上看起来是"19.7MB 与 32.6MB 之间随机断"，自动化测试里则
   * 表现为同一份代码时绿时红。 */
  if (lane && inc.lanes && !inc.lanes.has(lane)) return;

  state.incoming = null;
  if (inc.writer) {
    const writer = inc.writer;
    inc.writerClose = inc.writeQueue
      .then(async () => {
        if (inc.writeError) {
          const safeOffset = contiguousSavedOffset(inc);
          await writer.truncate(safeOffset);
          await writer.close();
          inc.received = safeOffset;
        } else {
          await writer.close();
          inc.received = contiguousSavedOffset(inc);
        }
      })
      .then(() => {
        inc.writer = null;
        if (inc.fileId && inc.received > 0 && inc.received < inc.size) {
          resumeSet(inc.fileId, inc);
          updateTransfer(inc.tid, {
            status: `已中断（${humanSize(inc.received)}），可续传`, failed: true,
          });
        } else {
          updateTransfer(inc.tid, { status: '已中断', failed: true });
        }
      })
      .catch((err) => {
        inc.writeError = err;
        inc.writer = null;
        updateTransfer(inc.tid, { status: `保存失败：${err.message}`, failed: true });
      });
    return;
  }
  if (inc.fileId && inc.received > 0 && inc.received < inc.size) {
    resumeSet(inc.fileId, inc);
    updateTransfer(inc.tid, { status: `已中断（${humanSize(inc.received)}），可续传`, failed: true });
  } else {
    updateTransfer(inc.tid, { status: '已中断', failed: true });
  }
}

function contiguousSavedOffset(inc) {
  let offset = inc.resumeFrom || 0;
  const parts = Object.values(inc.laneOffsets || {}).sort((a, b) => a.from - b.from);
  for (const part of parts) {
    if (part.from !== offset || part.saved < part.length) break;
    offset = part.from + part.length;
  }
  return offset;
}

async function finishReceive(lane) {
  const inc = state.incoming;
  if (!inc) return;
  if (inc.writer) {
    try {
      await inc.writeQueue;
      if (inc.writeError) throw inc.writeError;
      await inc.writer.close();
      inc.writer = null;
    } catch (err) {
      inc.writeError = err;
      if (inc.writer) {
        try {
          const safeOffset = contiguousSavedOffset(inc);
          await inc.writer.truncate(safeOffset);
          await inc.writer.close();
          inc.received = safeOffset;
        } catch (_) {
          await inc.writer.abort().catch(() => {});
        }
        inc.writer = null;
      }
      state.incoming = null;
      if (inc.fileId && inc.received > 0) state.resume.set(inc.fileId, inc);
      else if (inc.fileId) state.resume.delete(inc.fileId);
      updateTransfer(inc.tid, { status: `保存失败：${err.message}`, failed: true });
      return;
    }
  }
  state.incoming = null;
  state.resume.delete(inc.fileId);
  if (inc.streamBacked) {
    inc.priorChunks.length = 0;
    inc.laneBufs.forEach((b) => { b.length = 0; });
    updateTransfer(inc.tid, { status: '已保存', sent: inc.size, done: true });
    if (lane && lane.dc && lane.dc.readyState === 'open') {
      try {
        lane.dc.send(JSON.stringify({ t: 'file-done', tid: inc.tid, received: inc.size }));
      } catch (_) { /* 忽略：收尾回执失败不致命 */ }
    }
    toast(`已保存「${inc.name}」`);
    return;
  }
  /* 拼接顺序：先续传前已经收到的字节，再按 lane 0..N-1 的顺序把各段接上。
   * splitRange 保证各段首尾相接，所以这个顺序就是文件原始顺序。 */
  const blob = new Blob([...inc.priorChunks, ...inc.laneBufs.flat()],
    { type: 'application/octet-stream' });
  inc.priorChunks.length = 0;
  inc.laneBufs.forEach((b) => { b.length = 0; });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = inc.name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
  updateTransfer(inc.tid, { status: '已保存', sent: inc.size, done: true });
  /* 回执发送端：本机已落盘。它据此把发送端状态从"已发出"改成"已送达"，
   * 而不是发送端自己把队列清空就当完事。通道若正好在关，对端靠 progress
   * 到 size 也能判送达（见 updateTransfer 的收口）。 */
  if (lane && lane.dc && lane.dc.readyState === 'open') {
    try {
      lane.dc.send(JSON.stringify({ t: 'file-done', tid: inc.tid, received: inc.size }));
    } catch (_) { /* 忽略：收尾回执失败不致命 */ }
  }
  toast(`已接收「${inc.name}」`);
}

/* ---------------------------------------------------------------- transfers */

function addTransfer(info) {
  const el = document.createElement('div');
  el.className = 'transfer';
  el.innerHTML = '<div class="t-head"><span class="t-name"></span><span class="t-status"></span></div>'
    + '<div class="bar"><i></i></div><div class="t-sub"></div>'
    + '<button class="mini t-retry hidden">继续</button>';
  el.querySelector('.t-name').textContent = info.name;
  $('transferList').prepend(el);

  const rec = {
    tid: info.tid,
    el,
    size: info.size,
    sent: 0,
    /* 对端实收字节（发送端才有，progress 回执刷新）。null 表示还没收到
     * 第一条回执——那之前只能先显示本端交付量。 */
    acked: null,
    status: '准备中',
    dir: info.dir,
    peerId: info.peerId || null,
    peerLabel: info.peerLabel || '',
    file: info.file || null,
    retry: info.retry || null,
    speed: 0,
  };
  state.transfers.set(info.tid, rec);
  el.querySelector('.t-retry').addEventListener('click', () => {
    if (rec.retry) rec.retry();
  });
  updateTransfer(info.tid, {});
  return rec;
}

/* 进度更新改成节流。原来每收/发一块就写一次 DOM，100MB 的文件要动一千多次，
 * 光重排就把主线程占住了，反而拖慢传输。统一 120ms 批量刷一次。
 * 速率按"显示值"（发送端是 acked，接收端是 sent）的变化算，见 renderTransfer。 */
function updateTransfer(tid, patch) {
  const rec = state.transfers.get(tid);
  if (!rec) return;
  const hadAck = rec.acked != null;
  Object.assign(rec, patch);

  /* 发送端：对端实收达到文件大小才算真正送达。progress 到 size 或
   * file-done 都会走到这，统一在这里收口，不要各写一份。 */
  if (rec.acked != null && rec.size && rec.acked >= rec.size && !rec.done && !rec.failed) {
    rec.done = true;
    if (rec.status !== '已送达') rec.status = '已送达';
  }

  const shown = rec.acked != null ? rec.acked : rec.sent;
  if (typeof patch.sent === 'number' || typeof patch.acked === 'number') {
    /* 第一次收到对端回执：速率基线从"入队字节"切到"实收字节"。之前 sent
     * 是虚高的入队速率，直接沿用会把切过去那一瞬算成负数速率。 */
    if (rec.acked != null && !hadAck) rec.lastTs = 0;
    const now = Date.now();
    if (!rec.lastTs) {
      rec.lastTs = now;
      rec.lastSent = shown;
    } else {
      const dt = (now - rec.lastTs) / 1000;
      if (dt >= 0.4) {
        const inst = Math.max(0, (shown - rec.lastSent) / dt);
        /* 指数平滑压掉单次 progress 跳变：实收速率本身很稳，但 progress 是
         * 1MB 一条、又在多条 lane 上乱序到，瞬时值会跳。 */
        rec.speed = rec.speed > 0 ? rec.speed * 0.6 + inst * 0.4 : inst;
        rec.lastTs = now;
        rec.lastSent = shown;
      }
    }
  }
  if ((rec.done || rec.failed) && !rec._cleanupScheduled) {
    rec._cleanupScheduled = true;
    setTimeout(() => { state.transfers.delete(tid); if (rec.el) rec.el.remove(); }, 5 * 60 * 1000);
  }
  state.dirty.add(tid);
}

function renderTransfer(tid) {
  const rec = state.transfers.get(tid);
  if (!rec) return;
  /* 发送端有对端回执时按实收显示：进度条不再被发送队列吸收而虚高，
   * 速率也不再荡。没有回执（刚发起、对端是老版本）才退回本端交付量。 */
  const shown = rec.acked != null ? rec.acked : rec.sent;
  const pct = rec.size ? Math.min(100, Math.round((shown / rec.size) * 100)) : (rec.done ? 100 : 0);
  const who = rec.peerLabel || (rec.peerId ? peerName(rec.peerId) : '');
  const speed = rec.speed > 0 ? ` · ${humanSpeed(rec.speed)}` : '';

  rec.el.querySelector('.bar i').style.width = `${pct}%`;
  rec.el.querySelector('.t-status').textContent = rec.status || `${pct}%`;
  rec.el.querySelector('.t-sub').textContent = rec.dir === 'down'
    ? `来自 ${who} · ${humanSize(shown)} / ${humanSize(rec.size)}${speed}`
    : `发往 ${who} · ${humanSize(shown)} / ${humanSize(rec.size)}${speed}`;
  rec.el.classList.toggle('done', Boolean(rec.done));
  rec.el.classList.toggle('fail', Boolean(rec.failed));
  rec.el.querySelector('.t-retry').classList.toggle('hidden', !(rec.failed && rec.retry));
}

setInterval(() => {
  if (!state.dirty.size) return;
  for (const tid of state.dirty) renderTransfer(tid);
  state.dirty.clear();
}, UI_TICK);

/* ------------------------------------------------------------------ peers */

function renderPeers() {
  const grid = $('peerGrid');
  const others = state.peers;
  $('peerCount').textContent = `${others.length} 台设备`;

  if (!others.length) {
    grid.innerHTML = '<div class="empty">还没有其他设备。在另一台设备的浏览器打开同一个地址即可出现。</div>';
    return;
  }

  grid.innerHTML = '';
  for (const peer of others) {
    const conn = state.conns.get(peer.id);
    let dotClass = 'dot';
    let note = '未连接';
    if (conn) {
      if (conn.relay) {
        /* 兜底通道不在 conn.lanes 里。走到这一层说明直连没建起来——明说出来，
         * 别让用户看到一个"连接中"然后不知道到底连上没有。 */
        dotClass = 'dot busy';
        note = '服务器中转（直连未建立）';
      } else {
        const open = conn.lanes.filter((l) => l.dc && l.dc.readyState === 'open').length;
        if (open === conn.lanes.length) {
          dotClass = 'dot live';
          note = `已直连 ×${open}`;
        } else if (open > 0) {
          dotClass = 'dot busy';
          note = `连接中 ${open}/${conn.lanes.length}`;
        } else {
          dotClass = 'dot busy';
          note = '连接中';
        }
      }
    }

    const card = document.createElement('div');
    card.className = 'peer-card';
    card.dataset.peer = peer.id;
    card.innerHTML = `<div class="avatar">${esc(peer.name.slice(0, 1))}</div>`
      + `<div class="pname">${esc(peer.name)}</div>`
      + `<div class="pmeta"><span class="${dotClass}"></span>${note}</div>`;

    card.addEventListener('click', () => {
      state.targetPeer = peer.id;
      $('peerPicker').click();
    });

    card.addEventListener('dragover', (event) => {
      event.preventDefault();
      card.classList.add('dragover');
    });
    card.addEventListener('dragleave', () => card.classList.remove('dragover'));
    card.addEventListener('drop', (event) => {
      event.preventDefault();
      card.classList.remove('dragover');
      for (const file of event.dataTransfer.files) enqueueSend(peer.id, file);
    });

    grid.appendChild(card);
  }
}

$('peerPicker').addEventListener('change', (event) => {
  const target = state.targetPeer;
  const files = [...event.target.files];
  event.target.value = '';
  if (!target) return;
  for (const file of files) enqueueSend(target, file);
});

$('myName').addEventListener('change', (event) => {
  const value = event.target.value.trim().slice(0, 24);
  if (!value) {
    event.target.value = state.myName;
    return;
  }
  state.myName = value;
  saveName(value);
  send({ type: 'rename', name: value });
});

/* ------------------------------------------------------------------ vault */

async function refreshVault() {
  let data;
  try {
    const res = await fetch('/api/files', { cache: 'no-store' });
    data = await res.json();
  } catch (_) {
    $('vaultMeta').textContent = '无法连接服务端';
    return;
  }
  const files = data.files || [];
  $('vaultMeta').textContent = files.length
    ? `共 ${files.length} 个文件 · ${humanSize(data.total || 0)}`
    : '文件库是空的';

  const list = $('vaultList');
  list.innerHTML = '';
  for (const file of files) {
    const li = document.createElement('li');
    li.className = 'vault-item';
    const when = new Date(file.mtime).toLocaleString('zh-CN', { hour12: false });
    li.innerHTML = `<span class="vname">${esc(file.name)}</span>`
      + `<span class="vmeta">${esc(file.sizeText)} · ${esc(when)}</span>`
      + '<span class="vact"><button data-act="dl">下载</button><button data-act="rm" class="danger">删除</button></span>';

    li.querySelector('[data-act="dl"]').addEventListener('click', () => {
      const a = document.createElement('a');
      a.href = apiUrl(`/api/download?name=${encodeURIComponent(file.name)}`);
      a.download = file.name;
      document.body.appendChild(a);
      a.click();
      a.remove();
    });

    li.querySelector('[data-act="rm"]').addEventListener('click', async () => {
      if (!window.confirm(`确定删除「${file.name}」？`)) return;
      try {
        const res = await fetch(`/api/files?name=${encodeURIComponent(file.name)}`, { method: 'DELETE' });
        const body = await res.json();
        toast(body.ok ? '已删除' : `删除失败：${body.error || '未知错误'}`);
        refreshVault();
      } catch (err) {
        toast('删除失败：网络错误');
      }
    });

    list.appendChild(li);
  }
}

function clipboardForm(kind, mime, name) {
  return `/api/clipboard?kind=${encodeURIComponent(kind)}&mime=${encodeURIComponent(mime)}`
    + `&name=${encodeURIComponent(name || '')}`;
}

async function publishClipboard(kind, mime, name, blob) {
  const status = $('clipboardStatus');
  /* 空内容没有意义，发上去只会给所有人留一条空记录；服务端也会拒。 */
  if (!blob || blob.size === 0) {
    status.textContent = '';
    toast('内容为空，未发布');
    return false;
  }
  status.textContent = '正在发布';
  try {
    const res = await fetch(clipboardForm(kind, mime, name), {
      method: 'POST',
      headers: { 'Content-Type': mime },
      body: blob,
    });
    if (!res.ok) {
      const error = await res.json().catch(() => ({}));
      throw new Error(error.error || `HTTP ${res.status}`);
    }
    state.clipboardDirty = false;
    toast(kind === 'text' ? '文字已发布到共享剪贴板' : '图片已发布到共享剪贴板');
    await refreshClipboard(true);
    return true;
  } catch (err) {
    status.textContent = '发布失败';
    toast(`共享剪贴板发布失败：${err.message}`);
    return false;
  }
}

function renderClipboardHistory(items, selectedId) {
  const list = $('clipboardHistory');
  list.replaceChildren();
  $('clipboardHistoryCount').textContent = `${items.length} 条`;
  if (!items.length) {
    const empty = document.createElement('li');
    empty.className = 'clipboard-history-empty';
    empty.textContent = '暂无历史记录';
    list.appendChild(empty);
    return;
  }
  for (const item of items) {
    const row = document.createElement('li');
    row.className = 'clipboard-history-item';
    if (Number(item.id) === Number(selectedId)) row.classList.add('selected');
    const select = document.createElement('button');
    select.type = 'button';
    select.className = 'clipboard-history-select';
    select.textContent = `${item.kind === 'image' ? '图片' : '文字'} · ${item.name}`
      + ` · ${humanSize(item.size)} · ${new Date(item.updated).toLocaleString()}`;
    select.addEventListener('click', () => showClipboardItem(item, items));
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'clipboard-history-delete';
    remove.textContent = '删除';
    remove.addEventListener('click', async () => {
      try {
        const res = await fetch(`/api/clipboard?id=${item.id}`, { method: 'DELETE' });
        if (!res.ok) {
          toast('删除历史记录失败');
          return;
        }
        state.clipboardUpdated = null;
        await refreshClipboard(true);
      } catch (err) {
        toast('删除历史记录失败：网络错误');
      }
    });
    row.append(select, remove);
    list.appendChild(row);
  }
}

async function showClipboardItem(meta, items) {
  const status = $('clipboardStatus');
  /* 每次渲染领一个序号，等 await 回来时只有"最后一次"有权动 DOM。
   * 切换标签页会 refreshClipboard(true)，2 秒轮询又会自己来一发，两次
   * 渲染同时在飞是常态；没有这个守卫，两边各 append 一个 <pre>，
   * 预览区就会出现两份内容（netns T1 的浏览器 e2e 就是这么红的）。 */
  const seq = ++state.clipboardRenderSeq;
  try {
    const data = await fetch(`/api/clipboard/data?id=${meta.id}&v=${meta.updated}`, { cache: 'no-store' });
    if (!data.ok) throw new Error(`HTTP ${data.status}`);
    /* 正文也先读干净，再一次性改 DOM：replaceChildren 与 appendChild 之间
     * 不能再夹 await，否则旧的那次渲染会把新的一次清掉又补一份。 */
    const text = meta.kind === 'text' ? await data.text() : null;
    if (seq !== state.clipboardRenderSeq) return;
    state.clipboardSelectedId = Number(meta.id);
    state.clipboardText = meta.kind === 'text' ? text : null;
    state.clipboardImageUrl = meta.kind === 'image'
      ? apiUrl(`/api/clipboard/data?id=${meta.id}&v=${meta.updated}`) : null;
    $('copyClipboardText').textContent = meta.kind === 'image' ? '复制图片' : '复制文字';
    renderClipboardHistory(items || [], state.clipboardSelectedId);
    $('clipboardPreview').replaceChildren();
    $('downloadClipboardImage').classList.add('hidden');
    if (meta.kind === 'text') {
      const pre = document.createElement('pre');
      pre.className = 'clipboard-text-preview';
      pre.textContent = text;
      $('clipboardPreview').appendChild(pre);
    } else if (meta.kind === 'image') {
      const image = document.createElement('img');
      image.className = 'clipboard-image-preview';
      image.src = apiUrl(`/api/clipboard/data?id=${meta.id}&v=${meta.updated}`);
      image.alt = meta.name;
      $('clipboardPreview').appendChild(image);
      const link = $('downloadClipboardImage');
      link.href = image.src;
      link.download = meta.name || 'shared-image';
      link.classList.remove('hidden');
    }
    status.textContent = `更新于 ${new Date(meta.updated).toLocaleTimeString()}`;
  } catch (_) {
    status.textContent = '无法读取历史记录';
  }
}

async function refreshClipboard(force) {
  const status = $('clipboardStatus');
  try {
    const headers = {};
    if (!force && state.clipboardEtag) headers['If-None-Match'] = state.clipboardEtag;
    const res = await fetch('/api/clipboard', { cache: 'no-store', headers });
    if (res.status === 304) return;      /* 没有新内容，连 JSON 都不用解析 */
    if (res.status === 401) return;      /* 口令弹窗由 fetch 包装层负责 */
    if (res.status === 404) {
      state.clipboardEtag = null;
      state.clipboardUpdated = null;
      state.clipboardSelectedId = null;
      state.clipboardText = null;
      state.clipboardImageUrl = null;
      $('copyClipboardText').textContent = '复制文字';
      renderClipboardHistory([], null);
      $('clipboardPreview').replaceChildren();
      $('downloadClipboardImage').classList.add('hidden');
      status.textContent = '剪贴板为空';
      return;
    }
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    state.clipboardEtag = res.headers.get('ETag') || null;
    const meta = await res.json();
    /* 历史按 updated 倒序，最新一条永远在栈顶；服务端已排好序，这里再排一次，
     * 避免任何响应顺序问题导致旧记录跑到前面。 */
    const items = (Array.isArray(meta.items) ? meta.items : [meta])
      .slice()
      .sort((a, b) => Number(b.updated) - Number(a.updated));
    /* 没有新内容时只重画历史列表，保留用户手动点开的旧记录；一旦有新内容
     * 或强制刷新，共享框始终切回最新一条。 */
    if (!force && meta.updated === state.clipboardUpdated) {
      renderClipboardHistory(items, state.clipboardSelectedId);
      return;
    }
    state.clipboardUpdated = meta.updated;
    state.clipboardSelectedId = items[0].id;
    renderClipboardHistory(items, state.clipboardSelectedId);
    await showClipboardItem(items[0], items);
  } catch (_) {
    status.textContent = '无法连接服务端';
  }
}

$('publishClipboardText').addEventListener('click', async () => {
  const input = $('clipboardText');
  if (!input.value.trim()) {
    toast('文字为空，未发布');
    return;
  }
  const published = await publishClipboard('text', 'text/plain', 'shared-text.txt', new Blob([input.value], {
    type: 'text/plain;charset=utf-8',
  }));
  if (published) input.value = '';
});
/* 浏览器剪贴板的图片普遍只接受 image/png，JPEG/GIF/WebP 先经 canvas 转 PNG。 */
function imageBlobToPng(blob) {
  if (blob.type === 'image/png') return Promise.resolve(blob);
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(blob);
    const image = new Image();
    image.onload = () => {
      URL.revokeObjectURL(url);
      const canvas = document.createElement('canvas');
      canvas.width = image.naturalWidth;
      canvas.height = image.naturalHeight;
      const ctx = canvas.getContext('2d');
      if (!ctx) {
        reject(new Error('no canvas'));
        return;
      }
      ctx.drawImage(image, 0, 0);
      canvas.toBlob((png) => (png ? resolve(png) : reject(new Error('encode failed'))), 'image/png');
    };
    image.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new Error('decode failed'));
    };
    image.src = url;
  });
}

/* 复制按钮常驻在共享框下方，复制的是共享框当前显示的那一条：文字写文本，
 * 图片转成 PNG 后写剪贴板。 */
$('copyClipboardText').addEventListener('click', async () => {
  if (state.clipboardImageUrl) {
    if (!navigator.clipboard || !navigator.clipboard.write || typeof ClipboardItem === 'undefined') {
      toast('当前环境不支持复制图片（需 HTTPS 或 localhost）');
      return;
    }
    try {
      /* 直接把 Promise 交给 ClipboardItem：写剪贴板要求用户手势，先 await
       * fetch 再写的话手势可能已失效，浏览器会拒绝。 */
      const png = fetch(state.clipboardImageUrl, { cache: 'no-store' })
        .then((res) => {
          if (!res.ok) throw new Error(`HTTP ${res.status}`);
          return res.blob();
        })
        .then(imageBlobToPng);
      await navigator.clipboard.write([new ClipboardItem({ 'image/png': png })]);
      toast('图片已复制到本机剪贴板');
    } catch (_) {
      toast('复制图片失败，请检查剪贴板权限');
    }
    return;
  }
  const text = state.clipboardText;
  if (text == null) {
    toast('当前没有可复制的内容');
    return;
  }
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(text);
    } else {
      const input = document.createElement('textarea');
      input.value = text;
      input.style.position = 'fixed';
      input.style.opacity = '0';
      document.body.appendChild(input);
      input.select();
      const copied = document.execCommand('copy');
      input.remove();
      if (!copied) throw new Error('copy unavailable');
    }
    toast('已复制到本机剪贴板');
  } catch (_) {
    toast('浏览器未授予剪贴板权限');
  }
});
$('clearClipboard').addEventListener('click', async () => {
  try {
    const res = await fetch('/api/clipboard', { method: 'DELETE' });
    if (res.ok || res.status === 404) {
      $('clipboardText').value = '';
      await refreshClipboard(true);
    } else {
      toast('清空共享剪贴板失败');
    }
  } catch (err) {
    toast('清空共享剪贴板失败：网络错误');
  }
});
$('clearCurrentClipboard').addEventListener('click', async () => {
  const id = state.clipboardSelectedId;
  if (id == null) {
    toast('当前没有共享内容');
    return;
  }
  try {
    const res = await fetch(`/api/clipboard?id=${encodeURIComponent(id)}`, { method: 'DELETE' });
    if (res.ok || res.status === 404) {
      state.clipboardSelectedId = null;
      await refreshClipboard(true);
    } else {
      toast('清空当前共享内容失败');
    }
  } catch (err) {
    toast('清空当前共享内容失败：网络错误');
  }
});
$('pickClipboardImage').addEventListener('click', () => $('clipboardImagePicker').click());
$('clipboardImagePicker').addEventListener('change', (event) => {
  const image = event.target.files && event.target.files[0];
  event.target.value = '';
  if (image) publishClipboard('image', image.type, image.name, image);
});
const imageDrop = $('clipboardImageDrop');
imageDrop.addEventListener('dragover', (event) => {
  event.preventDefault();
  imageDrop.classList.add('dragover');
});
imageDrop.addEventListener('dragleave', () => imageDrop.classList.remove('dragover'));
imageDrop.addEventListener('drop', (event) => {
  event.preventDefault();
  imageDrop.classList.remove('dragover');
  const image = [...event.dataTransfer.files].find((file) => file.type.startsWith('image/'));
  if (image) publishClipboard('image', image.type, image.name, image);
  else toast('拖入的文件不是图片');
});
document.addEventListener('paste', (event) => {
  if (!$('pane-clipboard').classList.contains('active')) return;
  const item = [...(event.clipboardData && event.clipboardData.items || [])]
    .find((entry) => entry.type.startsWith('image/'));
  const image = item && item.getAsFile();
  if (image) {
    event.preventDefault();
    publishClipboard('image', image.type, 'pasted-image', image);
  }
});

/** 上传单个分块。服务端按 offset 顺序落进 .part 文件，中断后 .part 保留。 */
function putChunk(name, size, offset, blob) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', `/api/upload?name=${encodeURIComponent(name)}&size=${size}&offset=${offset}`);
    /* XHR 不走包装 fetch，CSRF 头（和服务端口令）必须手动带。
     * 漏了就是 403，文件库上传整个不可用，前端只显示"missing csrf header"。 */
    xhr.setRequestHeader('X-Lan-Share-Request', '1');
    if (state.token) xhr.setRequestHeader('X-Lan-Share-Token', state.token);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.addEventListener('load', () => {
      if (xhr.status === 200) {
        try {
          resolve(JSON.parse(xhr.responseText));
        } catch (_) {
          resolve({ ok: true });
        }
        return;
      }
      let reason = xhr.statusText;
      try {
        reason = JSON.parse(xhr.responseText).error || reason;
      } catch (_) { /* 保持 statusText */ }
      reject(new Error(reason || `HTTP ${xhr.status}`));
    });
    xhr.addEventListener('error', () => reject(new Error('网络中断')));
    xhr.addEventListener('abort', () => reject(new Error('上传被取消')));
    xhr.send(blob);
  });
}

async function uploadOne(file) {
  const tid = makeId();
  addTransfer({
    tid,
    name: file.name,
    size: file.size,
    dir: 'up',
    peerId: null,
    peerLabel: '服务端',
    file,
    retry: () => uploadOne(file),
  });

  let offset = 0;
  try {
    /* 开传前先问一句：这个文件服务端已经收了多少字节。
     * 上次传一半断掉的话，就从那个位置接着传，不用从头再来。 */
    const st = await fetch(
      `/api/upload-status?name=${encodeURIComponent(file.name)}&size=${file.size}`,
      { cache: 'no-store' }
    ).then((r) => r.json());
    offset = Math.max(0, Math.min(Number(st.received) || 0, file.size));
  } catch (_) {
    offset = 0;
  }

  updateTransfer(tid, {
    sent: offset,
    status: offset > 0 ? `续传中（已收 ${humanSize(offset)}）` : '上传中',
  });

  try {
    if (file.size === 0) {
      await putChunk(file.name, 0, 0, file.slice(0, 0));
      updateTransfer(tid, { status: '已上传', sent: 0, done: true });
      refreshVault();
      return;
    }
    while (offset < file.size) {
      const end = Math.min(offset + UP_CHUNK, file.size);
      const res = await putChunk(file.name, file.size, offset, file.slice(offset, end));
      offset = end;
      /* 收满时服务端把 .part 重命名成正式文件，可能因为重名加了个 (1) */
      if (res && res.done && res.name && res.name !== file.name) {
        updateTransfer(tid, { status: `已保存为「${res.name}」`, sent: file.size, done: true });
        toast(`已上传「${file.name}」→「${res.name}」`);
        refreshVault();
        return;
      }
      updateTransfer(tid, { sent: offset, status: '上传中' });
    }
    updateTransfer(tid, { status: '已上传', sent: file.size, done: true });
    toast(`已上传「${file.name}」`);
    refreshVault();
  } catch (err) {
    updateTransfer(tid, {
      status: `上传失败：${err.message}（已收 ${humanSize(offset)}，可继续）`,
      failed: true,
    });
    toast(`上传「${file.name}」失败：${err.message}`);
  }
}

function uploadFiles(files) {
  for (const file of files) uploadOne(file);
}

$('pickVault').addEventListener('click', () => $('vaultPicker').click());
$('vaultPicker').addEventListener('change', (event) => {
  uploadFiles([...event.target.files]);
  event.target.value = '';
});

const dropzone = $('dropzone');
['dragenter', 'dragover'].forEach((name) => {
  dropzone.addEventListener(name, (event) => {
    event.preventDefault();
    dropzone.classList.add('dragover');
  });
});
['dragleave', 'dragend'].forEach((name) => {
  dropzone.addEventListener(name, () => dropzone.classList.remove('dragover'));
});
dropzone.addEventListener('drop', (event) => {
  event.preventDefault();
  dropzone.classList.remove('dragover');
  if (event.dataTransfer.files.length) uploadFiles([...event.dataTransfer.files]);
});

/* ------------------------------------------------------------------ 网速测试 */

/* 测速用的数据量。16MB 足够让瞬时速率稳定下来，又不至于把路由器的
 * 磁盘和内存折腾太久。 */
const SPEED_BYTES = 16 * 1024 * 1024;
const SPEED_NAME = '.speedtest-tmp';

function speedSet(id, text) {
  const el = $(id);
  if (el) el.textContent = text;
}

function speedLog(text) {
  const el = $('speedLog');
  if (el) el.textContent = text;
}

/* 服务端下载：整块拉回来，测 HTTP 吞吐。 */
function speedDownload() {
  return new Promise((resolve, reject) => {
    const t0 = performance.now();
    const xhr = new XMLHttpRequest();
    xhr.open('GET', apiUrl(`/api/download?name=${encodeURIComponent(SPEED_NAME)}`));
    xhr.responseType = 'arraybuffer';
    let bytes = 0;
    xhr.addEventListener('progress', (e) => { bytes = e.loaded; });
    xhr.addEventListener('load', () => {
      /* 404 的响应体也会触发 load，不判状态码就会把错误页当成测速结果 */
      if (xhr.status < 200 || xhr.status >= 300) {
        reject(new Error(`下载测速失败（HTTP ${xhr.status}）`));
        return;
      }
      const secs = (performance.now() - t0) / 1000;
      const got = bytes || (xhr.response ? xhr.response.byteLength : 0);
      resolve({ mbps: got / 1048576 / secs, bytes: got });
    });
    xhr.addEventListener('error', () => reject(new Error('下载测速失败')));
    xhr.addEventListener('timeout', () => reject(new Error('下载测速超时')));
    xhr.timeout = 120000;
    xhr.send();
  });
}

/* 服务端上传：整块推上去，测 HTTP 吞吐。 */
function speedUpload() {
  return new Promise((resolve, reject) => {
    const blob = new Blob([new Uint8Array(SPEED_BYTES)]);
    const t0 = performance.now();
    const xhr = new XMLHttpRequest();
    xhr.open('POST', `/api/upload?name=${encodeURIComponent(SPEED_NAME)}`
      + `&size=${SPEED_BYTES}&offset=0`);
    /* XHR 走不到包装 fetch，CSRF 头得手动补。漏了就是 403，而 load 事件照样
     * 触发，会把"秒失败"算成一个上千 MB/s 的假速度。 */
    xhr.setRequestHeader('X-Lan-Share-Request', '1');
    if (state.token) xhr.setRequestHeader('X-Lan-Share-Token', state.token);
    xhr.addEventListener('load', () => {
      if (xhr.status < 200 || xhr.status >= 300) {
        reject(new Error(`上传测速失败（HTTP ${xhr.status}）`));
        return;
      }
      const secs = (performance.now() - t0) / 1000;
      resolve({ mbps: SPEED_BYTES / 1048576 / secs });
    });
    xhr.addEventListener('error', () => reject(new Error('上传测速失败')));
    xhr.addEventListener('timeout', () => reject(new Error('上传测速超时')));
    xhr.timeout = 120000;
    xhr.send(blob);
  });
}

/* 从 getStats 里挖出选中的候选对：类型和 RTT。
 * 这两个数字直接回答"慢是不是因为没走直连"——出现 srflx/relay，或者 RTT 几十
 * 毫秒，就先别折腾发送端了，路本身就不对。host↔host + 个位数 RTT 才是内网直连。 */
async function linkDiag(pc) {
  try {
    const stats = await pc.getStats();
    const byId = new Map();
    let transport = null;
    stats.forEach((s) => {
      byId.set(s.id, s);
      if (s.type === 'transport') transport = s;
    });
    const pair = transport ? byId.get(transport.selectedCandidatePairId) : null;
    if (!pair) return { text: '候选对未知', rtt: null, kind: null };
    const local = byId.get(pair.localCandidateId) || {};
    const remote = byId.get(pair.remoteCandidateId) || {};
    const rtt = (pair.currentRoundTripTime != null)
      ? Math.round(pair.currentRoundTripTime * 1000) : null;
    const kind = `${local.candidateType || '?'}↔${remote.candidateType || '?'}`;
    return {
      text: `${kind} · RTT ${rtt != null ? rtt + 'ms' : '未知'}`
        + ` · ${(local.protocol || '').toUpperCase()}`,
      rtt, kind,
    };
  } catch (_) {
    return { text: '统计不可用', rtt: null, kind: null };
  }
}

/* 点对点：把 SPEED_BYTES 均分给几条 lane 同时推，用与真实文件传输完全相同的
 * pumpFile 通道（同样的背压、分块、并行度），所以测出来的就是实际传文件的速度。
 * 对端只计数、不弹窗、不当文件收。
 *
 * 为什么要并行着测：单条 DataChannel 只有 3.2MB/s，而同一对设备的 TCP 能跑
 * 30MB/s——瓶颈在单个 SCTP 关联里（拥塞窗口/接收窗口/消息粒度），不在链路上。
 * 只测一条的话，测出来的只是关联的上限，会误判成"网络不行"。 */
/* 在指定的一组 lane 上跑一次逐 lane 测速：握手 → 泵数据 → 等对端回执。
 * 只由网速测试按钮（speedP2p）调用；真实传输不预跑测速，直接按
 * 一次跑全部已建立的 lane；真实传输不预跑测速，也不按速率降档。
 * 速率由对端计时（speedtest-result 回执），不受发送队列吸收影响。 */
async function laneSpeedtest(lanes, totalBytes, blob) {
  const t0 = performance.now();
  /* 和真实传输用同一个切分函数，段数、边界完全一致 */
  const ranges = splitRange(0, totalBytes, lanes.length);
  const use = lanes.slice(0, ranges.length);

  /* 每条 lane 独立打招呼：发 speedtest、等对端 ready。
   * 丢包链路上这条控制消息自己就可能丢（用户 Wi-Fi 实测 2% 丢包时必中），
   * 所以每 2 秒重发一次、最多等 30 秒——不能一丢就判整个测速失败。 */
  await Promise.all(use.map((lane, i) => new Promise((resolve, reject) => {
    let tries = 0;
    const send = () => {
      tries++;
      try {
        lane.dc.send(JSON.stringify({
          t: 'speedtest', bytes: ranges[i].to - ranges[i].from, ch: lane.index,
        }));
      } catch (err) {
        clearInterval(rep); clearTimeout(to);
        reject(err); return;
      }
    };
    const rep = setInterval(send, 2000);
    const to = setTimeout(() => {
      clearInterval(rep);
      lane.dc.removeEventListener('message', onMsg);
      reject(new Error(`lane ${i} 对端没有回应测速请求（重发 ${tries} 次）`));
    }, 30000);
    const onMsg = (e) => {
      if (typeof e.data !== 'string') return;
      let m;
      try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.t === 'speedtest-ready' && Number(m.ch) === lane.index) {
        clearInterval(rep); clearTimeout(to);
        lane.dc.removeEventListener('message', onMsg);
        resolve();
      }
    };
    lane.dc.addEventListener('message', onMsg);
    send();
  })));

  /* 等每条 lane 的回执。超时给 null，不影响其他 lane 出数 */
  const echoes = use.map((lane) => new Promise((resolve) => {
    const to = setTimeout(() => resolve(null), 120000);
    const onMsg = (e) => {
      if (typeof e.data !== 'string') return;
      let m;
      try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.t === 'speedtest-result' && Number(m.ch) === lane.index) {
        clearTimeout(to);
        lane.dc.removeEventListener('message', onMsg);
        resolve(m);
      }
    };
    lane.dc.addEventListener('message', onMsg);
  }));

  try {
    await Promise.all(ranges.map((r, i) => {
      const lane = use[i];
      return pumpFile(lane.dc, blob.slice(r.from, r.to), 0, { chunk: chunkSizeFor(lane) })
        .then(() => {
          lane.dc.send(JSON.stringify({ t: 'speedtest-end', ch: lane.index }));
        });
    }));
  } catch (err) {
    /* 没能正常收尾一定要告诉对端，否则它会一直停在计数状态，
     * 把后续真实文件的分块全吞掉。 */
    for (const lane of use) {
      try { lane.dc.send(JSON.stringify({ t: 'speedtest-abort', ch: lane.index })); } catch (_) {}
    }
    throw err;
  }

  const results = await Promise.all(echoes);
  const received = results.reduce((n, r) => n + (r ? r.received : 0), 0);
  /* 并行时取最慢那条 lane 的耗时当分母，偏保守但不会虚高 */
  const slowest = results.reduce((mx, r) => Math.max(mx, r ? r.ms : 0), 0);
  return {
    received, slowest,
    secs: (performance.now() - t0) / 1000,
    perLane: results.map((r) => (r && r.ms > 0 ? (r.received / 1048576) / (r.ms / 1000) : 0)),
  };
}

/* 初始并行度。真实传输的对端回执够慢时再升档，不为决策预先发送探测数据。 */

async function speedP2p(peerId) {
  const { live } = await ensureChannel(peerId);

  /* 兜底通道没有 pc，取不到 ICE 候选对——直接说明当前走的是哪一层，
   * 这正是用户想知道的："为什么没走直连"。 */
  const diag = live[0].pc
    ? await linkDiag(live[0].pc)
    : { text: '服务器中转（WebSocket）：直连未建立，通常是 Chrome 的 mDNS '
        + '候选被网络拦掉或 UDP 不通' };
  speedLog(`链路：${diag.text}`);

  const blob = new Blob([new Uint8Array(SPEED_BYTES)]);
  const r = await laneSpeedtest(live, SPEED_BYTES, blob);
  /* 两个数字的含义不同：
   *   mbps       —— 本机把字节交给 DataChannel 的速率，含背压等待，
   *                 也就是"我这边发得多快"。它会被发送队列吸收（每条 lane
   *                 最多囤 1MB，32 条就是 32MB），链路一抖动就虚高，
   *                 只能看趋势。
   *   peerMbps   —— 对端真正收完的速率，由对端计时，这才是端到端速度。 */
  return {
    mbps: SPEED_BYTES / 1048576 / r.secs,
    peerMbps: r.slowest > 0 ? (r.received / 1048576) / (r.slowest / 1000) : 0,
    lanes: live.length,
    diag,
    echoed: r.received > 0 ? { received: r.received, ms: r.slowest } : null,
  };
}

async function runSpeedTest() {
  const btn = $('runSpeed');
  if (btn) btn.disabled = true;
  speedSet('spDown', '测试中…');
  speedSet('spUp', '测试中…');
  speedSet('spP2p', '测试中…');

  const peer = state.peers.find((p) => p.id === state.targetPeer) || state.peers[0];
  speedSet('spPeerName', peer ? `（${peer.name}）` : '（没有在线设备）');

  try {
    /* 先上传再下载：下载要用刚传上去的那个文件 */
    const up = await speedUpload();
    speedSet('spUp', `${up.mbps.toFixed(1)} MB/s`);
    const down = await speedDownload();
    speedSet('spDown', `${down.mbps.toFixed(1)} MB/s`);
    /* 清理临时文件，失败就算了，不挡着出结果 */
    fetch(`/api/files?name=${encodeURIComponent(SPEED_NAME)}`, { method: 'DELETE' })
      .then(refreshVault).catch(() => {});

    if (peer) {
      const p2p = await speedP2p(peer.id);
      /* 头条用对端确认的速率：那才是端到端真实速度。本机"交付出手"的速率
       * 会被发送队列吸收（每条 lane 最多囤 1MB，32 条就是 32MB 的吸收
       * 窗口），链路一抖动就虚高——实测发送端在 2~40MB/s 之间荡、
       * 对端却一直稳定 13MB/s，荡的就是这个数。两个都显示，但主次分明。 */
      const extra = p2p.echoed
        ? ` · 本端交付 ${p2p.mbps.toFixed(1)}`
        : ' · 对端未回执';
      speedSet('spP2p', `${p2p.peerMbps.toFixed(1)} MB/s${extra} · ${p2p.lanes} 条并行`);
      /* 单条 lane 的速率也报一下：并行没提升就说明瓶颈不在每关联的机制上，
       * 而在链路本身，继续加 lane 是徒劳。 */
      if (p2p.lanes > 1) {
        speedLog(`点对点并行：${p2p.lanes} 条 lane 对端合计 ${p2p.peerMbps.toFixed(1)} MB/s`
          + `（本端交付 ${p2p.mbps.toFixed(1)}）`);
      }
    } else {
      speedSet('spP2p', '跳过（没有在线设备）');
    }
    speedLog('完成');
  } catch (err) {
    /* 失败时不要留着"测试中…"，否则页面看起来像卡死了 */
    for (const id of ['spDown', 'spUp', 'spP2p']) {
      const el = $(id);
      if (el && el.textContent === '测试中…') speedSet(id, '失败');
    }
    speedLog(`测速中断：${err.message}`);
  } finally {
    if (btn) btn.disabled = false;
  }
}

if ($('runSpeed')) $('runSpeed').addEventListener('click', () => { runSpeedTest(); });

/* -------------------------------------------------------------------- tabs */

document.querySelectorAll('.tab').forEach((tab) => {
  tab.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t === tab));
    const name = tab.dataset.tab;
    document.querySelectorAll('.pane').forEach((pane) => {
      pane.classList.toggle('active', pane.id === `pane-${name}`);
    });
    if (name === 'vault') refreshVault();
    if (name === 'clipboard') refreshClipboard(true);
  });
});

/* 首屏先探一下服务端是否需要口令：/api/info 在未鉴权时回 401，
 * 此时不要连 WS、也不要刷数据，直接弹口令框。 */
async function boot() {
  try {
    const res = await fetch('/api/info', { cache: 'no-store' });
    if (res.status === 401) {
      showTokenModal();
      return;
    }
  } catch (_) {
    /* 服务端暂时不可达也继续：WS 会自己重连，页面至少能打开 */
  }
  connect();
  refreshVault();
  refreshClipboard();
}

renderPeers();
boot();

setInterval(() => {
  /* 页面在后台时停止轮询，回来时由 visibilitychange 立刻补一次 */
  if (document.visibilityState !== 'visible') return;
  if ($('pane-vault').classList.contains('active')) refreshVault();
}, 15000);
setInterval(() => {
  if (document.visibilityState !== 'visible') return;
  if ($('pane-clipboard').classList.contains('active')) refreshClipboard();
}, 2000);
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState !== 'visible') return;
  refreshVault();
  if ($('pane-clipboard').classList.contains('active')) refreshClipboard();
});
