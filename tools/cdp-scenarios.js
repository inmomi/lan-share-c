'use strict';

/* 全场景 CDP 测试：两个真浏览器 + 三个服务端实例，把用户能点到的路径全走一遍。
 *
 * 与 p2p-e2e.js 的分工：那个盯"点对点吞吐 / 降级 / 丢包"的物理层，这个盯"功能与
 * 边界"——空文件、Unicode 名、拖拽、拒绝、并发、双向、剪贴板、文件库、测速、
 * 口令、配额、目录穿越，以及全程有没有 JS 异常。
 *
 * 用法：
 *   CHROME_PATH=/path/to/chrome node tools/cdp-scenarios.js
 *   node tools/cdp-scenarios.js --server ./lan-share --keep   # 失败时保留现场
 */

const { spawn } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { launchBrowser } = require('./cdp');

const ROOT = path.resolve(__dirname, '..');
const SERVER = arg('server', path.join(ROOT, 'lan-share'));
const WEB = path.join(ROOT, 'public');
const KEEP = arg('keep', null) === true;
const ONLY = arg('only', null);

const P1 = 8484;            // 普通实例
const P2 = 8485;            // 口令实例
const P3 = 8486;            // 配额实例（1 MiB）
const TOKEN = 'scn-token-42';

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'cdp-scn-'));
const log = (...a) => fs.writeSync(1, a.join(' ') + '\n');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let pass = 0;
let fail = 0;
const failures = [];
const consoleErrors = [];

function arg(name, dflt) {
  const i = process.argv.indexOf(`--${name}`);
  if (i < 0) return dflt === undefined ? null : dflt;
  const v = process.argv[i + 1];
  return v && !v.startsWith('--') ? v : true;
}

function check(name, ok, detail) {
  if (ok) { pass++; log(`  PASS  ${name}`); }
  else { fail++; failures.push(name + (detail ? ` :: ${detail}` : '')); log(`  FAIL  ${name}${detail ? ' :: ' + detail : ''}`); }
}

/** 页内表达式轮询求值，直到返回真值。 */
async function waitFor(page, expr, ms, label) {
  const deadline = Date.now() + (ms || 20000);
  let last;
  while (Date.now() < deadline) {
    try { last = await page.evaluate(expr, 15000); } catch (e) { last = e.message; }
    if (last) return last;
    await sleep(150);
  }
  throw new Error(`等待超时：${label || expr.slice(0, 60)}（最后值 ${JSON.stringify(last)}）`);
}

function sha256(buf) {
  return crypto.createHash('sha256').update(buf).digest('hex');
}

/** 与页内生成器完全一致的字节模式，用来算期望哈希。 */
function patternBuffer(size) {
  const buf = Buffer.alloc(size);
  for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
  return buf;
}

function httpReq(opts, body) {
  /* 调用方两种写法都支持：第二参数，或 opts.body（很多人会顺手写进对象） */
  const payload = body !== undefined ? body : opts.body;
  const options = Object.assign({ agent: false }, opts);
  delete options.body;
  if (payload && options.headers && !options.headers['Content-Length']
    && !options.headers['Transfer-Encoding']) {
    options.headers['Content-Length'] = payload.length;
  }
  return new Promise((resolve, reject) => {
    const req = http.request(options, (res) => {
      const chunks = [];
      res.on('data', (d) => chunks.push(d));
      res.on('end', () => resolve({
        status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks),
      }));
    });
    req.on('error', (e) => reject(new Error(`${e.code || ''} ${e.message} [${opts.method || 'GET'} ${opts.path}]`)));
    if (payload) req.write(payload);
    req.end();
  });
}

async function waitServer(url, ms) {
  const deadline = Date.now() + (ms || 15000);
  for (;;) {
    try {
      const r = await httpReq({ hostname: '127.0.0.1', port: new URL(url).port, path: '/api/info' });
      if (r.status < 500) return;
    } catch (_) { /* 还没起来 */ }
    if (Date.now() > deadline) throw new Error(`服务端 ${url} 没起来`);
    await sleep(200);
  }
}

/** 页面级错误采集：未捕获异常 + console.error。 */
async function hookErrors(page, tag, ctx) {
  await page.send('Runtime.enable').catch(() => {});
  page.on('Runtime.exceptionThrown', (p) => {
    const d = p.exceptionDetails || {};
    const text = (d.exception && d.exception.description) || d.text || 'unknown';
    ctx.push(`[${tag}] 未捕获异常: ${String(text).split('\n')[0]}`);
  });
  page.on('Runtime.consoleAPICalled', (p) => {
    if (p.type !== 'error') return;
    const text = (p.args || []).map((a) => a.value != null ? a.value
      : (a.description || a.type)).join(' ');
    ctx.push(`[${tag}] console.error: ${text}`);
  });
}

/** B 侧：自动接受接收确认框（可临时关掉以测"拒绝"）。 */
async function hookReceiver(page) {
  const r = await page.evaluate(`(function(){
    window.__scn = { auto: true };
    const modal = document.getElementById('modal');
    const yes = document.getElementById('modalYes');
    if (!modal || !yes) return 'no-modal';
    window.__scnObs = new MutationObserver(() => {
      if (!modal.classList.contains('hidden') && window.__scn.auto) yes.click();
    });
    window.__scnObs.observe(modal, { attributes: true, attributeFilter: ['class'] });
    return 'ok';
  })()`);
  if (r !== 'ok') throw new Error(`接收端弹窗挂钩失败: ${r}`);
}

/** 等某次传输进入终态；返回记录快照。 */
async function waitTransfer(page, pred, ms, label) {
  const deadline = Date.now() + (ms || 60000);
  let last = null;
  while (Date.now() < deadline) {
    last = await page.evaluate(`(function(){
      const out = [];
      for (const r of state.transfers.values()) {
        out.push({
          name: r.file ? r.file.name : '',
          dir: r.dir, peerLabel: r.peerLabel || '', status: r.status,
          sent: r.sent, acked: r.acked,
          size: r.size, done: !!r.done, failed: !!r.failed,
          speed: typeof r.speed === 'number' ? r.speed : null,
        });
      }
      return out;
    })()`);
    const hit = (last || []).find(pred);
    if (hit && (hit.done || hit.failed)) return hit;
    await sleep(150);
  }
  throw new Error(`传输未在期限内结束：${label || ''}（最后 ${JSON.stringify((last || []).slice(-2))}）`);
}

async function downloadOf(page, dir, name, ms) {
  const deadline = Date.now() + (ms || 60000);
  while (Date.now() < deadline) {
    const files = fs.existsSync(dir) ? fs.readdirSync(dir) : [];
    const hit = files.find((f) => f === name) || files.find((f) => f.startsWith(name));
    if (hit) {
      const full = path.join(dir, hit);
      const st = fs.statSync(full);
      if (st.size > 0 || hit === name) return { file: full, name: hit, size: st.size };
    }
    await sleep(200);
  }
  return null;
}

function clearDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
  for (const f of fs.readdirSync(dir)) fs.unlinkSync(path.join(dir, f));
}

(async () => {
  const servers = [];
  const browsers = [];
  let A = null, B = null, C = null;

  const startServer = (port, dir, extra) => {
    fs.mkdirSync(dir, { recursive: true });
    const p = spawn(SERVER, ['-p', String(port), '-d', dir, '-w', WEB, ...(extra || []),
      ...(process.env.SCN_VERBOSE ? ['-v'] : [])],
      { stdio: ['ignore', 'ignore', 'pipe'] });
    p.stderr.on('data', (d) => fs.writeSync(2, '[srv] ' + d));
    servers.push(p);
    return p;
  };

  try {
    startServer(P1, path.join(TMP, 'v1'));
    startServer(P2, path.join(TMP, 'v2'), ['--token', TOKEN]);
    startServer(P3, path.join(TMP, 'v3'), ['--quota', '1']);
    await waitServer(`http://127.0.0.1:${P1}/`);
    await waitServer(`http://127.0.0.1:${P2}/login.html`);
    await waitServer(`http://127.0.0.1:${P3}/`);

    const dlA = path.join(TMP, 'dl-a');
    const dlB = path.join(TMP, 'dl-b');
    clearDir(dlA); clearDir(dlB);

    A = await launchBrowser({ port: 9356, userDataDir: path.join(TMP, 'prof-a') });
    B = await launchBrowser({ port: 9357, userDataDir: path.join(TMP, 'prof-b') });
    await hookErrors(A, 'A', consoleErrors);
    await hookErrors(B, 'B', consoleErrors);

    await A.navigate(`http://127.0.0.1:${P1}/`);
    await B.navigate(`http://127.0.0.1:${P1}/`);
    await waitFor(A, 'typeof state === "object" && !!state.myId', 20000, 'A 就绪');
    await waitFor(B, 'typeof state === "object" && !!state.myId', 20000, 'B 就绪');
    await A.enableDownloads(dlA);
    await B.enableDownloads(dlB);
    await hookReceiver(A);
    await hookReceiver(B);

    const idB = await B.evaluate('state.myId');
    const idA = await A.evaluate('state.myId');
    await waitFor(A, 'state.peers.length > 0 && state.peers[0].id !== state.myId', 20000, 'A 看到 B');
    await waitFor(B, 'state.peers.length > 0', 20000, 'B 看到 A');

    /* 把 B 的大文件保存接口换成"只校验不落盘"的桩，用来验证流式写入的
     * 偏移连续性；小于 32MB 的文件走内存路径，不受影响。 */
    await B.evaluate(`(function(){
      Object.defineProperty(window, 'showSaveFilePicker', {
        configurable: true,
        value: async ({ suggestedName }) => ({
          createWritable: async () => {
            const spans = []; let bytes = 0; let bad = 0; let size = 0;
            return {
              async write(cmd) {
                const d = cmd.data instanceof ArrayBuffer ? new Uint8Array(cmd.data)
                  : new Uint8Array(cmd.data.buffer, cmd.data.byteOffset, cmd.data.byteLength);
                for (let i = 0; i < d.length; i++) {
                  const at = cmd.position + i;
                  if (d[i] !== ((at * 7 + (at >>> 3)) & 0xFF)) bad++;
                }
                spans.push([cmd.position, cmd.position + d.byteLength]);
                bytes += d.byteLength;
              },
              async truncate(s) { size = s; },
              async close() {
                /* 多 lane 并发写，position 天然乱序，只能验"覆盖连续、不重不漏" */
                spans.sort((a, b) => a[0] - b[0]);
                let end = 0; let contiguous = true;
                for (const r of spans) {
                  if (r[0] !== end) contiguous = false;
                  end = Math.max(end, r[1]);
                }
                window.__stream = { name: suggestedName, bytes, bad, contiguous, end, size };
              },
              async abort() {},
            };
          },
        }),
      });
      return true;
    })()`);

    const steps = [];
    const step = (name, fn) => steps.push([name, fn]);

    /* ------------------------------------------------------------ 1. 基座 */
    step('页面与设备', async () => {
      const tabs = await A.evaluate('[...document.querySelectorAll(".tab")].map(t => t.textContent)');
      check('四个标签页齐全', Array.isArray(tabs) && tabs.length === 4, JSON.stringify(tabs));
      const badge = await A.evaluate('document.getElementById("peerCount").textContent');
      check('在线设备计数为 1', /1\s*台/.test(badge), badge);
      const nameShown = await A.evaluate('document.getElementById("myName").value');
      check('本机名称非空', typeof nameShown === 'string' && nameShown.length > 0, String(nameShown));
      const cards = await A.evaluate('document.querySelectorAll("#peerGrid .peer-card, #peerGrid > *").length');
      check('设备卡片已渲染', cards >= 1, String(cards));
    });

    /* -------------------------------------------------- 2. 小文件（内存路径） */
    step('小文件传输', async () => {
      const name = 'small-1mb.bin';
      const size = 1 * 1024 * 1024;
      clearDir(dlB);
      await A.evaluate(`(function(){
        const size = ${size};
        const buf = new Uint8Array(size);
        for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
        enqueueSend(${JSON.stringify(idB)}, new File([buf], ${JSON.stringify(name)}));
        return true;
      })()`);
      const up = await waitTransfer(A, (r) => r.dir === 'up' && r.name === name, 60000, name);
      check('小文件状态为已送达', up.done && !up.failed, JSON.stringify(up));
      check('小文件 acked 等于文件大小', up.acked === size, `${up.acked} vs ${size}`);
      check('小文件速率有效（非 NaN/Infinity）', up.speed === null || Number.isFinite(up.speed), String(up.speed));
      const got = await downloadOf(B, dlB, name, 30000);
      check('接收端落盘存在', !!got, got ? got.name : '（无文件）');
      if (got) {
        const hash = sha256(fs.readFileSync(got.file));
        check('小文件内容逐字节一致', hash === sha256(patternBuffer(size)), got.name);
      }
      const down = await waitTransfer(B, (r) => r.dir === 'down' && r.size === size, 10000, name + '（接收端）');
      check('接收端记录为已保存', down.done && !down.failed, JSON.stringify(down));
    });

    /* -------------------------------------------------------- 3. 空文件 */
    step('空文件', async () => {
      const name = 'empty-0b.txt';
      clearDir(dlB);
      await A.evaluate(`enqueueSend(${JSON.stringify(idB)}, new File([], ${JSON.stringify(name)}))`);
      let up = null;
      try {
        up = await waitTransfer(A, (r) => r.dir === 'up' && r.name === name, 15000, name);
      } catch (e) {
        const snap = await A.evaluate(`(function(){
          const out = [];
          for (const r of state.transfers.values()) if (r.file && r.file.name === ${JSON.stringify(name)})
            out.push({ status: r.status, done: !!r.done, failed: !!r.failed, sent: r.sent, acked: r.acked });
          return out;
        })()`);
        check('空文件能传完（不卡死）', false, JSON.stringify(snap));
        return;
      }
      check('空文件能传完（不卡死）', up.done && !up.failed, JSON.stringify(up));
      const down = await waitTransfer(B, (r) => r.dir === 'down' && r.size === 0, 10000, '空文件接收端');
      check('空文件在接收端标记已保存', down.done && !down.failed, JSON.stringify(down));
    });

    /* ------------------------------------------------ 4. Unicode / 特殊名 */
    step('特殊文件名', async () => {
      const name = '测试 #1 & 空格+v2.txt';
      const body = 'hello 中文 😀 #&?%';
      clearDir(dlB);
      await A.evaluate(`(function(){
        const text = ${JSON.stringify(body)};
        const file = new File([new TextEncoder().encode(text)], ${JSON.stringify(name)}, { type: 'text/plain' });
        enqueueSend(${JSON.stringify(idB)}, file);
        return true;
      })()`);
      const up = await waitTransfer(A, (r) => r.dir === 'up' && r.name === name, 30000, name);
      check('特殊名文件送达', up.done && !up.failed, JSON.stringify(up));
      const got = await downloadOf(B, dlB, name, 20000);
      check('特殊名文件落盘名字正确', !!got && got.name === name, got ? got.name : '（无）');
      if (got) {
        check('特殊名文件内容一致',
          fs.readFileSync(got.file).toString('utf8') === body, fs.readFileSync(got.file).toString('utf8'));
      }
    });

    /* -------------------------------------------------------- 5. 拖拽路径 */
    step('拖拽发送', async () => {
      const name = 'drop-2mb.bin';
      const size = 2 * 1024 * 1024;
      clearDir(dlB);
      const r = await A.evaluate(`(function(){
        const card = document.querySelector('#peerGrid > *');
        if (!card) return 'no-card';
        const size = ${size};
        const buf = new Uint8Array(size);
        for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
        const dt = new DataTransfer();
        dt.items.add(new File([buf], ${JSON.stringify(name)}));
        card.dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer: dt }));
        return 'ok';
      })()`);
      check('拖拽事件被接受', r === 'ok', String(r));
      const up = await waitTransfer(A, (x) => x.dir === 'up' && x.name === name, 60000, name);
      check('拖拽文件送达', up.done && !up.failed, JSON.stringify(up));
      const got = await downloadOf(B, dlB, name, 30000);
      check('拖拽文件内容一致', !!got && sha256(fs.readFileSync(got.file)) === sha256(patternBuffer(size)),
        got ? got.name : '（无文件）');
    });

    /* ------------------------------------------------ 6. 大文件流式写入 */
    step('大文件流式落盘', async () => {
      const name = 'stream-40mb.bin';
      const size = 40 * 1024 * 1024;
      await A.evaluate(`(function(){
        const size = ${size};
        const buf = new Uint8Array(size);
        for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
        window.__big = new File([buf], ${JSON.stringify(name)});
        enqueueSend(${JSON.stringify(idB)}, window.__big);
        return true;
      })()`, 60000);
      const up = await waitTransfer(A, (r) => r.dir === 'up' && r.name === name, 180000, name);
      check('40MB 文件送达', up.done && !up.failed, JSON.stringify(up));
      const st = await B.evaluate('window.__stream || null');
      check('流式写入被调用', !!st, JSON.stringify(st));
      if (st) {
        check('流式写入字节数正确', st.bytes === size, `${st.bytes} vs ${size}`);
        check('流式写入偏移线性无空洞', st.contiguous && st.bad === 0, `bad=${st.bad}`);
      }
      await A.evaluate('delete window.__big');
    });

    /* ------------------------------------------------------ 7. 同一对端并发 */
    step('并发两个文件', async () => {
      const n1 = 'conc-a.bin';
      const n2 = 'conc-b.bin';
      const s1 = 8 * 1024 * 1024;
      const s2 = 4 * 1024 * 1024;
      clearDir(dlB);
      await A.evaluate(`(function(){
        const mk = (size, name) => {
          const buf = new Uint8Array(size);
          for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
          return new File([buf], name);
        };
        const id = ${JSON.stringify(idB)};
        enqueueSend(id, mk(${s1}, ${JSON.stringify(n1)}));
        enqueueSend(id, mk(${s2}, ${JSON.stringify(n2)}));
        return true;
      })()`);
      const up1 = await waitTransfer(A, (r) => r.dir === 'up' && r.name === n1, 120000, n1);
      const up2 = await waitTransfer(A, (r) => r.dir === 'up' && r.name === n2, 120000, n2);
      check('并发文件 1 送达', up1.done && !up1.failed, JSON.stringify(up1));
      check('并发文件 2 送达', up2.done && !up2.failed, JSON.stringify(up2));
      const g1 = await downloadOf(B, dlB, n1, 20000);
      const g2 = await downloadOf(B, dlB, n2, 20000);
      check('并发文件 1 内容一致', !!g1 && sha256(fs.readFileSync(g1.file)) === sha256(patternBuffer(s1)),
        g1 ? g1.name : '（无）');
      check('并发文件 2 内容一致', !!g2 && sha256(fs.readFileSync(g2.file)) === sha256(patternBuffer(s2)),
        g2 ? g2.name : '（无）');
    });

    /* ---------------------------------------------------------- 8. 双向并发 */
    step('双向同时传输', async () => {
      const size = 6 * 1024 * 1024;
      clearDir(dlA); clearDir(dlB);
      await A.evaluate(`(function(){
        const size = ${size}; const buf = new Uint8Array(size);
        for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
        enqueueSend(${JSON.stringify(idB)}, new File([buf], 'dir-a2b.bin'));
        return true;
      })()`);
      await B.evaluate(`(function(){
        const size = ${size}; const buf = new Uint8Array(size);
        for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
        enqueueSend(${JSON.stringify(idA)}, new File([buf], 'dir-b2a.bin'));
        return true;
      })()`);
      const a2b = await waitTransfer(A, (r) => r.dir === 'up' && r.name === 'dir-a2b.bin', 120000, 'a2b');
      const b2a = await waitTransfer(B, (r) => r.dir === 'up' && r.name === 'dir-b2a.bin', 120000, 'b2a');
      check('A→B 与 B→A 同时完成', a2b.done && b2a.done, `${a2b.status} / ${b2a.status}`);
      const gb = await downloadOf(B, dlB, 'dir-a2b.bin', 20000);
      const ga = await downloadOf(A, dlA, 'dir-b2a.bin', 20000);
      check('双向传输内容各自一致',
        !!gb && !!ga
        && sha256(fs.readFileSync(gb.file)) === sha256(patternBuffer(size))
        && sha256(fs.readFileSync(ga.file)) === sha256(patternBuffer(size)),
        `${gb && gb.name} / ${ga && ga.name}`);
    });

    /* ------------------------------------------------------------ 9. 拒绝接收 */
    step('拒绝接收', async () => {
      const name = 'reject-me.bin';
      await B.evaluate('window.__scn.auto = false');
      await A.evaluate(`(function(){
        const buf = new Uint8Array(1024 * 1024);
        enqueueSend(${JSON.stringify(idB)}, new File([buf], ${JSON.stringify(name)}));
        return true;
      })()`);
      await waitFor(B, `!document.getElementById('modal').classList.contains('hidden')`, 20000, '弹窗出现');
      await B.evaluate(`document.getElementById('modalNo').click()`);
      await B.evaluate('window.__scn.auto = true');
      const up = await waitTransfer(A, (r) => r.dir === 'up' && r.name === name, 20000, name);
      check('被拒绝时发送端标记失败', up.failed === true, JSON.stringify(up));
      check('拒绝原因可读', /拒绝/.test(up.status || ''), String(up.status));
      const stuckDown = await B.evaluate(`(function(){
        for (const r of state.transfers.values()) if (r.dir === 'down' && r.name === ${JSON.stringify(name)}) return r.status;
        return null;
      })()`);
      check('拒绝后接收端没有残留「接收中」', stuckDown === null || !/接收中/.test(stuckDown), String(stuckDown));
    });

    /* ---------------------------------------------------------- 10. 剪贴板 */
    step('共享剪贴板', async () => {
      const text = '场景测试文字 ' + Date.now();
      await A.evaluate(`(function(){
        document.querySelector('.tab[data-tab="clipboard"]').click();
        document.getElementById('clipboardText').value = ${JSON.stringify(text)};
        document.getElementById('publishClipboardText').click();
        return true;
      })()`);
      const okText = await waitFor(B, `(function(){
        return fetch('/api/clipboard').then(r => r.json()).then(j => {
          const items = j.items || [];
          return items.length && items[0].kind === 'text' ? 'yes' : null;
        }).catch(() => null);
      })()`, 20000, 'B 收到剪贴板文字');
      check('剪贴板文字已发布', okText === 'yes');
      await B.evaluate(`(function(){
        document.querySelector('.tab[data-tab="clipboard"]').click();
        if (typeof refreshClipboard === 'function') refreshClipboard();
        return true;
      })()`);
      const preview = await waitFor(B, `(function(){
        const el = document.getElementById('clipboardPreview');
        return el && el.textContent.includes(${JSON.stringify(text)}) ? el.textContent.trim() : null;
      })()`, 20000, 'B 预览显示文字');
      check('B 侧预览显示同一段文字', !!preview, String(preview).slice(0, 40));

      /* 图片：页内 canvas 造一张 8x8 PNG 后走发布接口 */
      const imgOk = await A.evaluate(`(async function(){
        const c = document.createElement('canvas');
        c.width = 8; c.height = 8;
        const ctx = c.getContext('2d');
        ctx.fillStyle = '#3366ff'; ctx.fillRect(0, 0, 8, 8);
        const blob = await new Promise((res) => c.toBlob(res, 'image/png'));
        return await publishClipboard('image', 'image/png', 'pixel.png', blob) ? 'ok' : 'fail';
      })()`, 30000);
      check('剪贴板图片已发布', imgOk === 'ok', String(imgOk));
      const hasImg = await waitFor(B, `(function(){
        return fetch('/api/clipboard').then(r => r.json()).then(j => {
          const it = (j.items || [])[0];
          return it && it.kind === 'image' ? it.name : null;
        }).catch(() => null);
      })()`, 20000, 'B 收到图片');
      check('B 侧能取到图片条目', !!hasImg, String(hasImg));
      const hist = await A.evaluate(`fetch('/api/clipboard').then(r => r.json()).then(j => (j.items || []).length)`);
      check('历史记录包含两条', hist >= 2, String(hist));
      /* 空内容：页面不该提交，服务端单独也拒 */
      await A.evaluate(`(function(){
        document.getElementById('clipboardText').value = '   ';
        document.getElementById('publishClipboardText').click();
        return true;
      })()`);
      await sleep(800);
      const hist2 = await A.evaluate(`fetch('/api/clipboard').then(r => r.json()).then(j => (j.items || []).length)`);
      check('纯空白文字不会被发布', hist2 === hist, `${hist} -> ${hist2}`);
      const emptyPost = await httpReq({
        hostname: '127.0.0.1', port: P1, path: '/api/clipboard?kind=text&mime=text/plain',
        method: 'POST',
        headers: { 'X-Lan-Share-Request': '1', 'Content-Type': 'text/plain', 'Content-Length': 0 },
      });
      check('服务端拒绝空剪贴板内容（400）', emptyPost.status === 400, String(emptyPost.status));
      /* 超限：16 MiB 上限之外的内容必须被拒，且页面不崩 */
      const over = await A.evaluate(`(async function(){
        const blob = new Blob([new Uint8Array(17 * 1024 * 1024)], { type: 'image/png' });
        try { const ok = await publishClipboard('image', 'image/png', 'huge.png', blob); return ok ? 'accepted' : 'rejected'; }
        catch (e) { return 'threw:' + e.message; }
      })()`, 60000);
      check('超过 16MiB 的剪贴板内容被拒绝', over === 'rejected', String(over));
      const alive = await A.evaluate('typeof refreshClipboard === "function" && !!document.getElementById("clipboardHistory")');
      check('超限之后页面仍可用', alive === true);
    });

    /* -------------------------------------------------------- 11. 文件库 */
    step('服务端文件库', async () => {
      const name = 'vault-8mb.bin';
      const size = 8 * 1024 * 1024;
      const body = patternBuffer(size);
      await A.evaluate(`(function(){
        document.querySelector('.tab[data-tab="vault"]').click();
        const size = ${size};
        const buf = new Uint8Array(size);
        for (let i = 0; i < size; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
        const dt = new DataTransfer();
        dt.items.add(new File([buf], ${JSON.stringify(name)}));
        const input = document.getElementById('vaultPicker');
        input.files = dt.files;
        input.dispatchEvent(new Event('change'));
        return true;
      })()`, 60000);
      const up = await waitTransfer(A, (r) => r.dir === 'up' && r.name === name && r.peerLabel === '服务端',
        120000, 'vault upload');
      check('文件库上传完成', up.done && !up.failed, JSON.stringify(up));
      const listed = await waitFor(A, `(function(){
        return fetch('/api/files').then(r => r.json()).then(j =>
          (j.files || []).some(f => f.name === ${JSON.stringify(name)}) ? 'yes' : null);
      })()`, 20000, '文件出现在列表');
      check('文件库列表包含该文件', listed === 'yes');
      const dl = await httpReq({
        hostname: '127.0.0.1', port: P1, path: `/api/download?name=${encodeURIComponent(name)}`,
      });
      check('文件库下载状态 200', dl.status === 200, String(dl.status));
      check('文件库下载内容一致', sha256(dl.body) === sha256(body), `${dl.body.length} vs ${size}`);
      const range = await httpReq({
        hostname: '127.0.0.1', port: P1,
        path: `/api/download?name=${encodeURIComponent(name)}`,
        headers: { Range: 'bytes=0-1023' },
      });
      check('Range 请求返回 206', range.status === 206, String(range.status));
      check('Range 请求返回 1024 字节', range.body.length === 1024, String(range.body.length));
      const bad = await httpReq({
        hostname: '127.0.0.1', port: P1, path: '/api/download?name=' + encodeURIComponent('../lan_share.c'),
      });
      check('目录穿越被拒（非 200）', bad.status !== 200, String(bad.status));
      const del = await httpReq({
        hostname: '127.0.0.1', port: P1,
        path: `/api/files?name=${encodeURIComponent(name)}`,
        method: 'DELETE', headers: { 'X-Lan-Share-Request': '1' },
      });
      check('删除文件返回 200', del.status === 200, String(del.status));

      /* 分块上传 + 断点续传：先传一半，再问 upload-status，再从断点续传 */
      const rn = 'resume-2mb.bin';
      const half = 1 * 1024 * 1024;
      const total = 2 * 1024 * 1024;
      const rbody = patternBuffer(total);   /* 必须是恰好 total 字节，别拿 8MB 的 body 切片 */
      const p1 = await httpReq({
        hostname: '127.0.0.1', port: P1,
        path: `/api/upload?name=${encodeURIComponent(rn)}&size=${total}&offset=0`,
        method: 'POST',
        headers: { 'X-Lan-Share-Request': '1', 'Content-Length': half },
        body: rbody.subarray(0, half),
      });
      const st = await httpReq({
        hostname: '127.0.0.1', port: P1,
        path: `/api/upload-status?name=${encodeURIComponent(rn)}&size=${total}`,
      });
      let stJson = {};
      try { stJson = JSON.parse(st.body.toString()); } catch (_) { stJson = {}; }
      check('半成品上传记录已收字节', Number(stJson.received) === half, JSON.stringify(stJson));
      const p2 = await httpReq({
        hostname: '127.0.0.1', port: P1,
        path: `/api/upload?name=${encodeURIComponent(rn)}&size=${total}&offset=${half}`,
        method: 'POST',
        headers: { 'X-Lan-Share-Request': '1', 'Content-Length': total - half },
        body: rbody.subarray(half),
      });
      let p2Json = {};
      try { p2Json = JSON.parse(p2.body.toString()); } catch (_) {}
      check('半成品分块请求返回 200', p1.status === 200, String(p1.status));
      check('续传剩下的字节后完成', p2Json.done === true, `${p1.status}/${p2.status} ${JSON.stringify(p2Json)}`);
    });

    /* ---------------------------------------------------------- 12. 网速测试 */
    step('网速测试', async () => {
      const r = await A.evaluate(`(async function(){
        document.querySelector('.tab[data-tab="speed"]').click();
        await runSpeedTest();
        return {
          up: document.getElementById('spUp').textContent,
          down: document.getElementById('spDown').textContent,
          p2p: document.getElementById('spP2p').textContent,
        };
      })()`, 180000);
      const num = (s) => {
        const m = /([\d.]+)\s*MB\/s/.exec(s || '');
        return m ? Number(m[1]) : NaN;
      };
      check('服务端上传测速有数值', Number.isFinite(num(r.up)) && num(r.up) > 0, r.up);
      check('服务端下载测速有数值', Number.isFinite(num(r.down)) && num(r.down) > 0, r.down);
      check('点对点测速有数值', Number.isFinite(num(r.p2p)) && num(r.p2p) > 0, r.p2p);
    });

    /* ------------------------------------------------------ 13. 断开与重连 */
    step('断开重连', async () => {
      await B.evaluate('ws.close()');
      const gone = await waitFor(A, 'state.peers.length === 0', 20000, 'A 发现 B 离线').then(() => true).catch(() => false);
      check('对端断开后列表清空', gone);
      const back = await waitFor(A, 'state.peers.length > 0', 30000, 'B 重连').then(() => true).catch(() => false);
      check('对端自动重连后重新出现', back);
      await waitFor(A, 'state.peers.length > 0', 5000, '重连稳定');
      const idB2 = await B.evaluate('state.myId');
      const name = 'after-reconnect.bin';
      clearDir(dlB);
      await A.evaluate(`(function(){
        const buf = new Uint8Array(512 * 1024);
        for (let i = 0; i < buf.length; i++) buf[i] = (i * 7 + (i >>> 3)) & 0xFF;
        enqueueSend(${JSON.stringify(idB2)}, new File([buf], ${JSON.stringify(name)}));
        return true;
      })()`);
      const up = await waitTransfer(A, (r) => r.dir === 'up' && r.name === name, 90000, name);
      check('重连之后仍能传文件', up.done && !up.failed, JSON.stringify(up));
    });

    /* ------------------------------------------------------------ 14. 口令 */
    step('访问口令', async () => {
      C = await launchBrowser({ port: 9358, userDataDir: path.join(TMP, 'prof-c') });
      await hookErrors(C, 'C', consoleErrors);
      await C.navigate(`http://127.0.0.1:${P2}/`);
      const title = await C.evaluate('document.title');
      check('未认证访问进入登录页', /口令/.test(title), title);
      const noAuth = await httpReq({ hostname: '127.0.0.1', port: P2, path: '/app.js' });
      check('未认证拿不到 app.js', noAuth.status !== 200, String(noAuth.status));
      const wrong = await httpReq({
        hostname: '127.0.0.1', port: P2, path: '/api/login?token=nope',
        method: 'POST', headers: { 'X-Lan-Share-Request': '1' },
      });
      check('错误口令返回 401', wrong.status === 401, String(wrong.status));
      const right = await httpReq({
        hostname: '127.0.0.1', port: P2, path: `/api/login?token=${encodeURIComponent(TOKEN)}`,
        method: 'POST', headers: { 'X-Lan-Share-Request': '1' },
      });
      check('正确口令返回 200', right.status === 200, String(right.status));
      const cookie = (right.headers['set-cookie'] || []).join(';');
      check('登录下发 HttpOnly Cookie', /ls_auth=/.test(cookie) && /HttpOnly/i.test(cookie), cookie.slice(0, 60));
      const authJs = await httpReq({
        hostname: '127.0.0.1', port: P2, path: '/app.js',
        headers: { Cookie: (cookie.match(/ls_auth=[^;]+/) || [''])[0] },
      });
      check('带 Cookie 可访问 app.js', authJs.status === 200, String(authJs.status));
      const noCsrf = await httpReq({
        hostname: '127.0.0.1', port: P2, path: `/api/login?token=${encodeURIComponent(TOKEN)}`, method: 'POST',
      });
      check('缺 X-Lan-Share-Request 的写请求被拒', noCsrf.status !== 200, String(noCsrf.status));
    });

    /* -------------------------------------------------------- 15. 协议边界 */
    step('协议边界', async () => {
      /* 不带 Content-Length 时 curl / Node 会走 chunked；服务端不解析 chunked
       * 请求体，必须明确拒绝，绝不能"建个 0 字节文件还回 200"。 */
      const chunked = await httpReq({
        hostname: '127.0.0.1', port: P1, path: '/api/upload?name=chunked.bin',
        method: 'POST',
        headers: { 'X-Lan-Share-Request': '1', 'Transfer-Encoding': 'chunked' },
      }, Buffer.from('hello-chunked-body'));
      check('chunked 请求体被明确拒绝（411）', chunked.status === 411, String(chunked.status));
      const empty = await httpReq({
        hostname: '127.0.0.1', port: P1, path: '/api/files',
      });
      check('被拒后没有留下 0 字节垃圾文件',
        !JSON.parse(empty.body.toString() || '{}').files
          ?.some((f) => f.name === 'chunked.bin'), empty.body.toString().slice(0, 120));
      const noCsrf = await httpReq({
        hostname: '127.0.0.1', port: P1, path: '/api/files?name=x',
        method: 'DELETE',
      });
      check('缺 CSRF 头的写请求返回 403', noCsrf.status === 403, String(noCsrf.status));
      const badRange = await httpReq({
        hostname: '127.0.0.1', port: P1, path: '/api/download?name=nope.bin',
        headers: { Range: 'bytes=999999-' },
      });
      check('不存在的文件 Range 请求返回 404', badRange.status === 404, String(badRange.status));
    });

    /* ------------------------------------------------------------ 16. 配额 */
    step('配额限制', async () => {
      const big = Buffer.alloc(2 * 1024 * 1024, 1);
      const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
      const r = await httpReq({
        hostname: '127.0.0.1', port: P3, path: '/api/upload?name=quota-over.bin',
        method: 'POST', agent,
        headers: { 'X-Lan-Share-Request': '1', 'Content-Length': big.length },
        body: big,
      });
      check('超过配额的整块上传返回 507', r.status === 507, `${r.status} ${r.body.toString().slice(0, 80)}`);
      /* 关键回归：被拒请求的 2MB 请求体没被消费，若服务端不停在原地排水，
       * 同一连接上的下一个请求就会被这些字节顶掉（表现为 socket hang up）。 */
      const small = await httpReq({
        hostname: '127.0.0.1', port: P3, path: '/api/upload?name=quota-ok.bin',
        method: 'POST', agent,
        headers: { 'X-Lan-Share-Request': '1', 'Content-Length': 256 * 1024 },
        body: Buffer.alloc(256 * 1024, 2),
      });
      check('配额内的上传正常返回 200', small.status === 200, String(small.status));
      check('507 之后同一 keep-alive 连接仍可用', small.status === 200 && r.status === 507,
        `${r.status}/${small.status}`);
      agent.destroy();
    });

    for (const [name, fn] of steps) {
      if (ONLY && !name.includes(ONLY)) continue;
      log(`\n--- ${name}`);
      try { await fn(); } catch (e) {
        check(`${name}（未抛出异常）`, false, e.message);
      }
    }

    log('\n===== 会话内 JS 错误 =====');
    if (!consoleErrors.length) log('  无');
    else for (const e of consoleErrors.slice(0, 20)) log('  ' + e);
    check('全程无未捕获 JS 异常', consoleErrors.filter((e) => e.includes('未捕获异常')).length === 0,
      consoleErrors.filter((e) => e.includes('未捕获异常')).join(' | '));
  } finally {
    for (const b of [A, B, C]) { try { if (b) await b.close(); } catch (_) {} }
    for (const s of servers) { try { s.kill('SIGKILL'); } catch (_) {} }
    if (KEEP) log(`\n现场保留在 ${TMP}`);
    else {
      /* 服务端刚被 SIGKILL，落盘线程可能还攥着句柄：删不掉不是测试失败 */
      for (let i = 0; i < 3; i++) {
        try { fs.rmSync(TMP, { recursive: true, force: true }); break; }
        catch (_) { await sleep(200); }
      }
    }
  }

  log(`\n===== 通过 ${pass} / 失败 ${fail} =====`);
  if (failures.length) {
    log('失败项：');
    for (const f of failures) log('  - ' + f);
  }
  process.exit(fail ? 1 : 0);
})().catch((e) => {
  log('FATAL ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
