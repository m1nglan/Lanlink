/* ============================================================
 * 网关 WebSocket 探针 (纯 Node, 不依赖 npm 包)
 *
 * 目的: 在烧板子之前, 先确认下行链路到底通不通。
 *   ① text 服务下直接发 chat          -> 会被拒吗?
 *   ② svc llm 紧跟 chat (不等 svc_ok) -> 核心: 回复来不来?
 *   ③ svc llm -> 等 svc_ok -> chat    -> 对照组
 *   ④ 完整板子时序 (含 llm -> text 切回)
 *
 * 用法: node gw_probe.mjs ["要问的话"]
 * ============================================================ */

import net from 'node:net';
import crypto from 'node:crypto';
import { HOST, PORT, TOKEN, describeTarget } from './gw_config.mjs';

const CHAT = process.argv[2] || '你好，用一句话回答：1+1等于几？';
/* 转发目标服务 —— 默认和板子的 ASK_SERVICE 保持一致 (ui_bridge.cpp) */
const SERVICE = process.argv[3] || 'openclaw';
/* ★ openclaw(明岚) 调工具时会几秒~几十秒没有输出 (PROTOCOL.md:112), 实测最长 ~26 秒,
 *   所以等待窗口要足够长, 否则会把"慢"误判成"没回"。 */
const REPLY_WAIT_MS = Number(process.argv[4] || 40000);

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
          log('WS 已连接');
          resolve();
        }
        this.drain();
      });
    });
  }

  /** 解析所有完整帧 (服务端到客户端不掩码) */
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
      else if (opcode === 0x9) { this.sendFrame(payload, 0xa); }   // ping 回 pong
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

  send(obj) {
    const s = JSON.stringify(obj);
    log('  -> 发送', s.length > 120 ? s.slice(0, 120) + '...' : s);
    this.sendFrame(s);
  }
  onText(h) { this.handlers.push(h); }
  close() { try { this.sock.end(); } catch {} }
}

/* ---------- 测试用例 ---------- */
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function runCase(title, body) {
  console.log(`\n${'='.repeat(72)}\n> ${title}\n${'='.repeat(72)}`);
  const ws = new Ws();
  const got = [];
  ws.onText((s) => {
    let brief = s;
    try {
      const o = JSON.parse(s);
      if (o.type === 'partial') brief = `partial "${o.text}"`;
      else if (o.type === 'reply') brief = `** reply "${(o.content || '').slice(0, 80)}"`;
      else brief = JSON.stringify(o).slice(0, 140);
    } catch {}
    got.push(brief);
    log('  <- 收到', brief);
  });
  await ws.connect();
  await sleep(200);
  try { await body(ws, got); } finally { ws.close(); }
  await sleep(300);

  const ok = got.some(g => g.includes('reply'));
  console.log(`\n  结论: ${ok ? '[OK] 拿到 reply' : '[NO] 没有 reply'}   收到 ${got.length} 条`);
  return { title, ok, got };
}

const results = [];

/* 1 不切服务直接发 chat -- 该被拒 (证明"必须先切服务") */
results.push(await runCase('1 [对照] 不切服务, 直接发 chat (当前在 text 服务)', async (ws) => {
  ws.send({ type: 'chat', content: CHAT });
  await sleep(3000);
}));

/* 2 核心: svc llm 紧跟 chat, 中间不等 svc_ok */
results.push(await runCase('2 [核心] svc llm + 立刻 chat (背靠背, 不等 svc_ok)', async (ws) => {
  ws.send({ type: 'svc', service: SERVICE });
  ws.send({ type: 'chat', content: CHAT });
  await sleep(REPLY_WAIT_MS);
}));

/* 3 对照: 等 svc_ok 再发 chat */
results.push(await runCase('3 [对照] svc llm -> 等 svc_ok -> 再 chat', async (ws) => {
  ws.send({ type: 'svc', service: SERVICE });
  await new Promise((res) => {
    const t = setTimeout(res, 3000);
    const h = (s) => { try { if (JSON.parse(s).type === 'svc_ok') { clearTimeout(t); res(); } } catch {} };
    ws.onText(h);
  });
  log('  (svc_ok 已到, 现在发 chat)');
  ws.send({ type: 'chat', content: CHAT });
  await sleep(REPLY_WAIT_MS);
}));

/* 4 完整模拟板子的时序: text -> llm+chat -> 切回 text -> 再来一轮 */
results.push(await runCase('4 [板子时序] text -> llm+chat -> 切回 text -> 再问一次', async (ws, got) => {
  const waitSvcOk = (svc) => new Promise((res) => {
    const t = setTimeout(() => res(false), 3000);
    const h = (s) => {
      try {
        const o = JSON.parse(s);
        if (o.type === 'svc_ok' && o.service === svc) { clearTimeout(t); res(true); }
      } catch {}
    };
    ws.onText(h);
  });

  ws.send({ type: 'svc', service: 'text' });
  log('  等 svc_ok(text)...', await waitSvcOk('text'));
  await sleep(200);

  ws.send({ type: 'svc', service: SERVICE });
  log('  等 svc_ok(llm)...', await waitSvcOk('llm'));
  ws.send({ type: 'chat', content: '第一问：1+1=?' });
  await sleep(REPLY_WAIT_MS);

  ws.send({ type: 'svc', service: 'text' });
  log('  等 svc_ok(text) 第二次...', await waitSvcOk('text'));
  await sleep(200);
  ws.send({ type: 'svc', service: SERVICE });
  log(`  等 svc_ok(${SERVICE})...`, await waitSvcOk(SERVICE));
  ws.send({ type: 'chat', content: '第二问：2+2=?' });
  await sleep(REPLY_WAIT_MS);
}));

/* ---------- 汇总 ---------- */
console.log(`\n${'='.repeat(72)}\n汇总\n${'='.repeat(72)}`);
for (const r of results) {
  console.log(`${r.ok ? '[OK]' : '[NO]'}  ${r.title}`);
  console.log(`      收到 ${r.got.length} 条: ${r.got.slice(0, 6).join(' | ') || '(无)'}`);
}
console.log('');
process.exit(0);
