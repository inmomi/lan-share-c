#!/usr/bin/env node
'use strict';
/*
 * 把 public/ 下的前端文件转成 C 头文件，编进二进制。
 *
 * 为什么必须有这一步：
 *   路由器上通常只 scp 一个二进制过去，前端目录不在旁边。这时打开页面
 *   只会得到 404 Not Found（/api/info 却是好的），看起来像程序坏了。
 *   把前端编进去之后，拷一个文件就能跑，不再依赖外部目录。
 *
 * 磁盘优先：serve_static 先找 -w 指定的目录，找不到才回退到内嵌资源。
 * 所以改 public/ 里的文件不需要重新编译，直接刷新页面即可。
 *
 * 用法：
 *   node tools/embed-web.js                       # 默认 public/ -> web_assets.h
 *   node tools/embed-web.js <前端目录> <输出头文件>
 */

const fs = require('fs');
const path = require('path');

const pubDir = path.resolve(process.argv[2] || path.join(__dirname, '..', 'public'));
const outFile = path.resolve(process.argv[3] || path.join(__dirname, '..', 'web_assets.h'));

/* route -> 磁盘文件名。 "/" 与 "/index.html" 指向同一个文件。 */
const ASSETS = [
    { file: 'index.html', routes: ['/', '/index.html'] },
    { file: 'app.js', routes: ['/app.js'] },
    { file: 'style.css', routes: ['/style.css'] },
];

function cVarName(fileName) {
    return 'ls_web_' + fileName.replace(/[^A-Za-z0-9]/g, '_');
}

/* 逐字节写成十六进制数组。
 * 不用字符串字面量是因为前端里有中文（UTF-8 多字节）、引号、反斜杠和换行，
 * 转义起来很容易出错，而且某些编译器对超长字符串字面量有限制。 */
function bytesToC(buf) {
    const lines = [];
    for (let i = 0; i < buf.length; i += 16) {
        const slice = buf.subarray(i, Math.min(i + 16, buf.length));
        lines.push('    ' + [...slice].map((b) => `0x${b.toString(16).padStart(2, '0')}`).join(', '));
    }
    return lines.join(',\n');
}

const chunks = [];
const entries = [];

for (const asset of ASSETS) {
    const full = path.join(pubDir, asset.file);
    if (!fs.existsSync(full)) {
        console.error(`缺少前端文件: ${full}`);
        process.exit(1);
    }
    const buf = fs.readFileSync(full);
    const varName = cVarName(asset.file);
    chunks.push(`static const unsigned char ${varName}[] = {\n${bytesToC(buf)}\n};`);
    for (const route of asset.routes) {
        entries.push(`    { "${route}", ${varName}, sizeof ${varName} },`);
    }
    console.log(`  ${asset.file}  ${buf.length} 字节 -> ${varName} (${asset.routes.join(', ')})`);
}

const out = `/* 自动生成，请勿手工编辑。
 * 由 tools/embed-web.js 从 public/ 生成，执行 make 会重新生成。
 *
 * 前端被编进二进制，这样只把可执行文件拷到路由器上也能直接打开页面，
 * 不会因为没有 public/ 目录而返回 404 Not Found。
 * 磁盘上的 public/ 优先级更高，改前端无需重新编译。
 */
#ifndef LAN_SHARE_WEB_ASSETS_H
#define LAN_SHARE_WEB_ASSETS_H

#include <stddef.h>

typedef struct {
    const char *route;
    const unsigned char *data;
    size_t len;
} ls_web_asset;

${chunks.join('\n\n')}

static const ls_web_asset ls_web_assets[] = {
${entries.join('\n')}
};

#define LS_WEB_ASSET_COUNT (sizeof ls_web_assets / sizeof ls_web_assets[0])

#endif /* LAN_SHARE_WEB_ASSETS_H */
`;

fs.writeFileSync(outFile, out);
console.log(`已生成 ${path.relative(process.cwd(), outFile)} (${out.length} 字节)`);
