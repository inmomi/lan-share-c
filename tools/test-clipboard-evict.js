#!/usr/bin/env node
'use strict';
/* 剪贴板历史"写不下的时候淘汰最老一条"的行为测试（ENOSPC 路径）。
 *
 * 走的是 clipboard_write_retry 里那个分支：write() 返回 ENOSPC 时不能直接
 * 把上传打成 500，而要删掉最老的一条历史腾出空间、再从断点续写。
 * 这条路径靠正常文件系统测不出来——必须真的把盘写满。CI 里用 1 MiB 的
 * tmpfs 制造空间不足（见 .github/workflows/build.yml）。
 *
 * 用法：
 *   node tools/test-clipboard-evict.js --bin ./lan-share --dir /mnt/clipsmall --port 8123
 *
 * 需要 --dir 所在文件系统总容量明显小于 items × item-bytes，否则测不到淘汰。
 */

const http = require('http');
const fs = require('fs');
const { spawn } = require('child_process');

const argv = process.argv.slice(2);
function arg(name, def) {
    const i = argv.indexOf(`--${name}`);
    return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
}

const BIN = arg('bin', './lan-share');
const DIR = arg('dir', '/tmp/lanshare-clip-evict');
const PORT = Number(arg('port', '8123'));
const ITEM_BYTES = Number(arg('item-bytes', String(320 * 1024)));
const ITEMS = Number(arg('items', '10'));
const WAIT_MS = Number(arg('wait-ms', '20000'));

let passed = 0;
let failed = 0;

function check(name, cond, extra) {
    if (cond) {
        passed++;
        console.log(`PASS  ${name}${extra ? ' :: ' + extra : ''}`);
    } else {
        failed++;
        console.log(`FAIL  ${name}${extra ? ' :: ' + extra : ''}`);
    }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function req(method, path, body, headers) {
    return new Promise((resolve, reject) => {
        const h = Object.assign({}, headers || {});
        if (body) h['Content-Length'] = String(body.length);
        const r = http.request({ host: '127.0.0.1', port: PORT, method, path, headers: h }, (res) => {
            const chunks = [];
            res.on('data', (c) => chunks.push(c));
            res.on('end', () => resolve({
                status: res.statusCode,
                headers: res.headers,
                body: Buffer.concat(chunks),
            }));
        });
        r.on('error', reject);
        if (body) r.write(body);
        r.end();
    });
}

async function jsonReq(method, path, body, headers) {
    const res = await req(method, path, body, headers);
    let parsed = null;
    try {
        parsed = JSON.parse(res.body.toString('utf8'));
    } catch (_) {
        parsed = null;
    }
    return { status: res.status, json: parsed, body: res.body };
}

/* 目标文件系统剩余空间（字节）。用来确认"确实写不下"，也用来校验
 * 历史总量没有超过物理容量——那说明淘汰算错了。 */
function fsCapacity(dir) {
    try {
        const st = fs.statfsSync(dir);
        return Number(st.bsize) * Number(st.bavail);
    } catch (_) {
        return 0;
    }
}

function makePayload(i, size) {
    const buf = Buffer.alloc(size);
    /* 每条内容不同且可校验：首尾各放一个标记，其余填确定的伪随机字节 */
    buf.write(`HEAD:${i}`, 0, 'utf8');
    buf.write(`TAIL:${i}`, size - 8, 'utf8');
    for (let k = 16; k < size - 16; k++) buf[k] = (k * 31 + i * 7) & 0xff;
    return buf;
}

async function waitReady(child) {
    const deadline = Date.now() + WAIT_MS;
    while (Date.now() < deadline) {
        if (child.exitCode !== null) return false;
        try {
            await req('GET', '/');
            return true;
        } catch (_) {
            await sleep(150);
        }
    }
    return false;
}

(async () => {
    fs.mkdirSync(DIR, { recursive: true });
    const capBytes = fsCapacity(DIR);
    const need = ITEM_BYTES * ITEMS;
    console.log(`目标目录 ${DIR} 可用 ${(capBytes / 1024).toFixed(0)} KiB，`
        + `计划写入 ${ITEMS} × ${(ITEM_BYTES / 1024).toFixed(0)} KiB = ${(need / 1024).toFixed(0)} KiB`);
    check('测试盘的容量确实小于要写入的总量（否则测不到淘汰）',
        capBytes > 0 && capBytes < need, `cap=${capBytes} need=${need}`);

    const child = spawn(BIN, ['-p', String(PORT), '-d', DIR, '-w', './public'], {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    let serverLog = '';
    child.stdout.on('data', (d) => { serverLog += d.toString(); });
    child.stderr.on('data', (d) => { serverLog += d.toString(); });

    const stop = () => {
        if (child.exitCode === null) {
            child.kill('SIGTERM');
            /* 同步等一下，让 fd 关掉、临时文件清掉，再交给下一步（不能 await，
             * 这里在 finally 里）。Atomics.wait 比 sleep 子进程干净。 */
            Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 200);
        }
    };

    try {
        const ready = await waitReady(child);
        check('服务在测试盘上启动成功', ready, ready ? '' : serverLog.slice(-300));
        if (!ready) throw new Error('服务未就绪');

        /* 先把上一轮可能残留的历史清干净，保证容量判断是干净的 */
        await jsonReq('DELETE', '/api/clipboard');

        const posted = [];
        for (let i = 0; i < ITEMS; i++) {
            const payload = makePayload(i, ITEM_BYTES);
            const res = await jsonReq('POST', `/api/clipboard?kind=text&name=e${i}.txt`, payload, {
                'Content-Type': 'text/plain; charset=utf-8',
            });
            posted.push({ i, payload, res });
        }

        const rejected = posted.filter((p) => p.res.status !== 200);
        check('盘写满时上传仍然成功（淘汰旧记录而不是 500）',
            rejected.length === 0,
            rejected.length
                ? `第 ${rejected[0].i} 条 -> ${rejected[0].res.status} ${rejected[0].res.body.toString('utf8').slice(0, 120)}`
                : `${posted.length} 条全部 200`);

        const meta = await jsonReq('GET', '/api/clipboard');
        const items = (meta.json && meta.json.items) || [];
        check('历史条数少于写入次数（确实发生了淘汰）',
            items.length > 0 && items.length < ITEMS, `items=${items.length} posted=${ITEMS}`);
        check('历史总量没有超过物理容量',
            items.reduce((s, it) => s + Number(it.size || 0), 0) <= capBytes,
            `total=${items.reduce((s, it) => s + Number(it.size || 0), 0)} cap=${capBytes}`);
        check('最新一条就是最后写入的那条',
            !!items[0] && items[0].name === `e${ITEMS - 1}.txt`,
            items[0] ? items[0].name : 'empty');

        /* 淘汰之后被复用的槽位必须装着完整的新内容：这是最容易出错的地方
         * ——只留元数据、数据被截断，接口照样 200，但用户拿到的是坏文件。 */
        const last = posted[posted.length - 1];
        const latest = await req('GET', '/api/clipboard/data');
        check('最新一条逐字节完整（没被 ENOSPC 截断）',
            latest.status === 200 && latest.body.length === ITEM_BYTES
            && latest.body.equals(last.payload),
            `status=${latest.status} len=${latest.body.length}`);
        const lastId = latest.status === 200 && last.res.json ? last.res.json.id : -1;
        const byId = lastId >= 0 ? await req('GET', `/api/clipboard/data?id=${lastId}`) : null;
        check('按 id 取到的最新一条同样完整',
            !!byId && byId.status === 200 && byId.body.equals(last.payload),
            byId ? `status=${byId.status} len=${byId.body.length}` : 'no id');

        /* 最老的一条必须真的没了：它的槽位已经给了新内容 */
        const first = posted[0];
        const firstId = first.res.json ? first.res.json.id : -1;
        const firstData = firstId >= 0 ? await req('GET', `/api/clipboard/data?id=${firstId}`) : null;
        const stillOriginal = !!firstData && firstData.status === 200
            && firstData.body.equals(first.payload);
        check('最老的一条已被淘汰（内容不再是它）', !stillOriginal,
            firstData ? `status=${firstData.status} len=${firstData.body.length}` : 'no id');

        /* 服务端不能崩：淘汰过程动过多条记录 */
        check('淘汰之后服务仍在运行', child.exitCode === null,
            child.exitCode === null ? '' : `exit=${child.exitCode}`);
    } catch (err) {
        failed++;
        console.log(`FAIL  未捕获异常 :: ${err.message}`);
        console.log(serverLog.slice(-800));
    } finally {
        stop();
    }

    console.log(`\n===== 通过 ${passed} / 失败 ${failed} =====`);
    process.exit(failed === 0 ? 0 : 1);
})();
