#!/usr/bin/env node
/* 从 web_assets.h 里解出被嵌入的前端文件，落盘到指定目录，方便做跨版本比对。
 * 用法： node tools/dump-embedded-assets.js <web_assets.h> <输出目录>
 */
const fs = require('fs');
const path = require('path');

const src = process.argv[2];
const outDir = process.argv[3];
if (!src || !outDir) {
  console.error('用法: node tools/dump-embedded-assets.js <web_assets.h> <输出目录>');
  process.exit(2);
}

const text = fs.readFileSync(src, 'utf8');
const re = /static const unsigned char\s+(ls_web_[a-z0-9_]+)\[\]\s*=\s*\{([\s\S]*?)\n\};/g;
let m;
let count = 0;
fs.mkdirSync(outDir, { recursive: true });
while ((m = re.exec(text)) !== null) {
  const name = m[1];
  const body = m[2];
  const bytes = [];
  const bre = /0x([0-9a-fA-F]{2})/g;
  let b;
  while ((b = bre.exec(body)) !== null) bytes.push(parseInt(b[1], 16));
  const buf = Buffer.from(bytes);
  // ls_web_index_html -> index.html
  let file = name.replace(/^ls_web_/, '');
  file = file.replace(/_([a-z])/g, (_, c) => c.toUpperCase());
  file = file.replace(/_([a-z]+)$/, (_, s) => '.' + s);
  file = file.replace(/_([a-z]+)_/g, (_, s) => '.' + s + '_');
  file = file.replace(/Html$/, '.html').replace(/Js$/, '.js').replace(/Css$/, '.css');
  file = file.replace(/_$/g, '');
  const p = path.join(outDir, file);
  fs.writeFileSync(p, buf);
  console.log(`${file}\t${buf.length}\t${p}`);
  count++;
}
if (count === 0) {
  console.error('没有解析到任何嵌入资产，正则可能过时了');
  process.exit(1);
}
