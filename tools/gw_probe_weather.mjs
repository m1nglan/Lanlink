/* ============================================================
 * 天气服务探针 —— 确认板子能不能【不切服务】拿到天气
 *
 * 要回答的核心问题:
 *   板子现在的 svc 状态是【全局独占】的 (voice_task 切 text / ask_router 切 openclaw)。
 *   如果天气每 5 分钟要切一次 svc=weather, 就会和语音/对话抢。
 *   所以先测清楚:
 *
 *   ① 默认 (text 服务) 下直接发 {"type":"get"} → 会不会回 weather?
 *      ★ 如果会 → 板子完全不用切服务, 最简单 ✓
 *   ② 切到 weather 后, 网关会不会【自动推送】一次 weather?
 *   ③ weather 服务下发 {"type":"get"} → 会不会刷新?
 *   ④ 切回 text 后, {"type":"get"} 还有效吗?
 *
 * 用法: node gw_probe_weather.mjs
 * ============================================================ */

import net from 'node:net';
import crypto from 'node:crypto';
import { HOST, PORT, TOKEN, describeTarget } from './gw_config.mjs';

const t0 = Date.now();
const log = (...a) => console.log(`[${String(Date.now() - t0).padStart(5)}ms]`, ...a);
console.log(`目标: ${describeTarget()}\n`);

/* ---------- 极简 WebSocket 客户端 ---------- */
class Ws {
  constructor() { this.buf = Buffer.alloc(0); this.handlers = []; this.open = false; }

  connect() {
    return new Promise((resolve, reject) => {
      const key = crypto.randomBytes(16).toString('base64');
      this.sock = net.connect(PORT, HOST, () => {
        this.sock.write(
          `GET /?token=${TOKEN} HTTP/1.1\r\n` +
          `Host: ${HOST}:${PORT}\r\n` +
          `Upgrade: websocket\r\nConnection: Upgrade\r\n` +
          `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
      });
      this.sock.on('error', reject);
      let handshook = false;
      this.sock.on('data', (chunk) => {
        this.buf = Buffer.concat([this.buf, chunk]);
        if (!handshook) {
          const end = this.buf.indexOf('\r\n\r\n');
          if (end < 0) return;
          const head = this.buf.subarray(0, end).toString();
          if (!/101/.test(head.split('\r\n')[0])) { reject(new Error('握手失败: ' + head.split('\r\n')[0])); return; }
          handshook = true; this.open = true;
          this.buf = this.buf.subarray(end + 4);
          log('WS 已连接 (新连接, 服务状态应为默认 text)');
          resolve();
        }
        this.drain();
      });
    });
  }

  drain() {
    for (;;) {
      if (this.buf.length < 2) return;
      const opcode = this.buf[0] & 0x0f;
      let len = this.buf[1] & 0x7f, off = 2;
      if (len === 126) { if (this.buf.length < 4) return; len = this.buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (this.buf.length < 10) return; len = Number(this.buf.readBigUInt64BE(2)); off = 10; }
      if (this.buf.length < off + len) return;
      const payload = this.buf.subarray(off, off + len);
      this.buf = this.buf.subarray(off + len);

      if (opcode === 0x1) { const s = payload.toString('utf8'); this.handlers.forEach(h => h(s)); }
      else if (opcode === 0x8) { log('服务端关闭连接'); this.sock.end(); return; }
      else if (opcode === 0x9) { this.sendFrame(payload, 0xa); }
    }
  }

  sendFrame(payload, opcode = 0x1) {
    const data = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'utf8');
    const mask = crypto.randomBytes(4);
    let head;
    if (data.length < 126)      head = Buffer.from([0x80 | opcode, 0x80 | data.length]);
    else if (data.length < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | opcode; head[1] = 0x80 | 126; head.writeUInt16BE(data.length, 2); }
    else { head = Buffer.alloc(10); head[0] = 0x80 | opcode; head[1] = 0x80 | 127; head.writeBigUInt64BE(BigInt(data.length), 2); }
    const masked = Buffer.alloc(data.length);
    for (let i = 0; i < data.length; i++) masked[i] = data[i] ^ mask[i & 3];
    this.sock.write(Buffer.concat([head, mask, masked]));
  }

  /* ★ 先 log 再发 —— 保证日志和实际动作一致 (上一版探针在这栽过: 只打不发) */
  send(obj) {
    const s = JSON.stringify(obj);
    log('  -> 发送', s);
    this.sendFrame(s);
  }

  onText(h) { this.handlers.push(h); }
  close() { try { this.sock.end(); } catch {} }
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

/* ---------- 记录所有下行, 特别标出 weather ---------- */
function attachLogger(ws, sink) {
  ws.onText((s) => {
    let o = null;
    try { o = JSON.parse(s); } catch {}

    if (o && o.type === 'weather') {
      /* ★ 完整打印 —— 要拿到真实的 kind / text / temp 来验证板子的映射 */
      log('  <- 【WEATHER】', JSON.stringify(o));
      sink.weather.push(o);
    } else if (o && o.type === 'svc_ok') {
      log('  <- svc_ok', o.service);
      sink.svcOk.push(o.service);
    } else if (o && o.type === 'error') {
      log('  <- error', JSON.stringify(o));
      sink.errors.push(o);
    } else {
      log('  <- 其它', s.length > 160 ? s.slice(0, 160) + '...' : s);
    }
    sink.all.push(s);
  });
}

async function runCase(title, body) {
  console.log(`\n${'='.repeat(74)}\n> ${title}\n${'='.repeat(74)}`);
  const ws = new Ws();
  const sink = { weather: [], svcOk: [], errors: [], all: [] };
  attachLogger(ws, sink);
  await ws.connect();
  await sleep(300);
  try {
    await body(ws, sink);
  } finally {
    ws.close();
  }
  await sleep(300);

  console.log(`\n  小结: weather ${sink.weather.length} 条 · svc_ok ${sink.svcOk.length} 条 · error ${sink.errors.length} 条`);
  if (sink.weather.length > 0) {
    const w = sink.weather[sink.weather.length - 1];
    console.log(`  最新天气: text="${w.text}" temp="${w.temp}" kind=${w.kind} max=${w.temp_max} min=${w.temp_min}`);
  }
  return { title, weather: sink.weather, svcOk: sink.svcOk, errors: sink.errors, sink };
}

const results = [];

/* ---------------------------------------------------------------
 * ① 【最关键】默认服务 (text) 下直接发 get
 * --------------------------------------------------------------- */
results.push(await runCase('① 【最关键】不切服务 (默认 text) → 直接发 {"type":"get"}', async (ws) => {
  ws.send({ type: 'get' });
  await sleep(5000);
}));

/* ---------------------------------------------------------------
 * ② 切到 weather → 看是否自动推送一次
 * --------------------------------------------------------------- */
results.push(await runCase('② svc weather → 等 svc_ok → 看网关是否自动推一次 weather', async (ws, sink) => {
  ws.send({ type: 'svc', service: 'weather' });
  /* 等 svc_ok 最多 3 秒 */
  for (let i = 0; i < 30 && sink.svcOk.length === 0; i++) await sleep(100);
  log('  svc_ok 到了, 再等 4 秒看自动推送');
  await sleep(4000);
}));

/* ---------------------------------------------------------------
 * ③ weather 服务下主动 get
 * --------------------------------------------------------------- */
results.push(await runCase('③ weather 服务下 → 发 {"type":"get"} (测主动刷新)', async (ws, sink) => {
  ws.send({ type: 'svc', service: 'weather' });
  for (let i = 0; i < 30 && sink.svcOk.length === 0; i++) await sleep(100);
  await sleep(1500);                       /* 先让自动推送落地, 便于区分 */
  const before = sink.weather.length;
  log(`  (自动推送已有 ${before} 条, 现在主动 get)`);
  ws.send({ type: 'get' });
  await sleep(4000);
  log(`  get 之后 weather 条数: ${sink.weather.length} (之前 ${before})`);
}));

/* ---------------------------------------------------------------
 * ④ 切回 text 后再 get —— 决定板子能不能"切完立刻切回"
 * --------------------------------------------------------------- */
results.push(await runCase('④ svc weather → svc text (切回) → 再发 get', async (ws, sink) => {
  ws.send({ type: 'svc', service: 'weather' });
  for (let i = 0; i < 30 && sink.svcOk.length === 0; i++) await sleep(100);
  await sleep(2000);
  const afterWeather = sink.weather.length;
  ws.send({ type: 'svc', service: 'text' });
  for (let i = 0; i < 30 && sink.svcOk.length < 2; i++) await sleep(100);
  await sleep(1000);
  log(`  切回 text 了 (weather 已有 ${afterWeather} 条), 再发 get`);
  /* ★ 记下切回 text 时的条数 —— 否则会把 weather 服务下的自动推送误算成本次成功 */
  sink.baselineAtSwitchBack = afterWeather;
  ws.send({ type: 'get' });
  await sleep(4000);
  log(`  切回 text 后 get 拿到 weather: ${sink.weather.length - afterWeather} 条`);
}));

/* ---------------------------------------------------------------
 * 汇总
 * --------------------------------------------------------------- */
console.log(`\n${'='.repeat(74)}\n汇总\n${'='.repeat(74)}`);
for (const r of results) {
  const got = r.weather.length;
  console.log(`  ${got > 0 ? '[YES]' : '[NO ]'} weather×${String(got).padEnd(2)}  svc_ok×${r.svcOk.length}  │ ${r.title}`);
}

const c1 = results[0].weather.length > 0;
const c2 = results[1].weather.length > 0;
const c3 = results[2].weather.length > results[1].weather.length;  /* 粗略 */
/* ★ ④ 必须用"切回 text 之后新增的条数", 不能直接用总数 (里面含 weather 服务下的自动推送) */
const baseline4 = results[3].sink.baselineAtSwitchBack ?? 0;
const c4 = results[3].weather.length > baseline4;

console.log(`\n  ★ 结论:`);
console.log(`     ① text 服务下 get 有效?        ${c1 ? 'YES ✓ 板子不用切服务, 最省事' : 'NO  ✗ 必须切到 weather'}`);
console.log(`     ② 切 weather 后自动推送?        ${c2 ? 'YES ✓' : 'NO  ✗ 必须自己发 get'}`);
console.log(`     ③ weather 下 get 能刷新?        ${c3 ? 'YES ✓' : 'NO  (看上面条数对比)'}`);
console.log(`     ④ 切回 text 后 get 还有效?      ${c4 ? 'YES ✓' : 'NO  ✗ 只有 weather 服务下才行'}`);
console.log(`\n  建议:`);
if (c1) {
  console.log(`     → 【首选】天气轮询完全不碰 svc: 直接 {"type":"get"}, 不会和语音/对话抢 ✓`);
} else {
  console.log(`     → 必须切 svc=weather: 需要"空闲才轮询 + 拿完立刻切回 text"`);
  console.log(`       且要处理与 voice_task / ask_router 的 svc 抢占`);
}

process.exit(0);
