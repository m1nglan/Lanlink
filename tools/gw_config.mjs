/* ============================================================
 * 网关连接参数 —— 全部从【板子源码】里读, 不在这里硬编码
 *
 *   main/apikey.h        → SERVER_TOKEN   (本文件已 gitignore, 不入库)
 *   main/drivers/ws.hpp  → WS_HOST / WS_PORT
 *
 * ★ 这样 token 永远不会出现在 tools/ 里被误提交。
 *   改网关地址/token 只需改板子那两个文件, 探针自动跟着变。
 *
 * 用法: import { HOST, PORT, TOKEN } from './gw_config.mjs';
 * ============================================================ */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');

/** 从 C 头文件里取一个 #define 的字符串或数字值 */
function readDefine(file, name) {
  if (!fs.existsSync(file)) return null;
  const src = fs.readFileSync(file, 'utf8');
  // 匹配: #define NAME   "value"   或   #define NAME   (12345)   或  #define NAME 12345
  const re = new RegExp(`^\\s*#define\\s+${name}\\s+\\(?\\s*"?([^"\\s)]+)"?\\s*\\)?`, 'm');
  const m = src.match(re);
  return m ? m[1] : null;
}

const APIKEY = path.join(ROOT, 'main', 'apikey.h');
const WSHPP  = path.join(ROOT, 'main', 'drivers', 'ws.hpp');

const TOKEN = readDefine(APIKEY, 'SERVER_TOKEN');
const HOST  = readDefine(WSHPP,  'WS_HOST');
const PORT  = Number(readDefine(WSHPP, 'WS_PORT'));

if (!TOKEN) {
  console.error(`\n❌ 读不到 SERVER_TOKEN —— 检查 ${APIKEY}\n`
              + `   (该文件已 gitignore, 从版本库 clone 出来时是缺失的, 需要手工补)\n`);
  process.exit(1);
}
if (!HOST || !Number.isFinite(PORT)) {
  console.error(`\n❌ 读不到 WS_HOST / WS_PORT —— 检查 ${WSHPP}\n`);
  process.exit(1);
}

export { HOST, PORT, TOKEN };
export const WS_URL = `ws://${HOST}:${PORT}/?token=${TOKEN}`;

/** 打印一行连接信息 (不含完整 token, 免得日志泄露) */
export function describeTarget() {
  const masked = TOKEN.length > 4 ? TOKEN.slice(0, 4) + '***' : '***';
  return `${HOST}:${PORT}  token=${masked}  (读自 main/apikey.h + ws.hpp)`;
}
