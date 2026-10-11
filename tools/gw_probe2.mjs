/* ============================================================
 * 靶向测试: "svc llm + 背靠背 chat" 到底会不会丢掉回复的开头?
 *
 * 思路: 让 LLM 复读一串固定字符 —— 只要开头被丢, reply 就不会以第一个字开头,
 *       这是确定性的判据 (不像问答题, LLM 措辞随机会掩盖问题)。
 *
 * 结论 (2026-xx 实测): 两种写法 3/3 vs 3/3, 无差异 —— 背靠背不会丢字。
 *       板子仍选"等 svc_ok", 理由是与 voice_session 的写法一致 + 防静默失败。
 *
 * 用法: node gw_probe2.mjs [次数]
 * ============================================================ */

import net from 'node:net';
import crypto from 'node:crypto';
import { HOST, PORT, TOKEN, describeTarget } from './gw_config.mjs';

const NEEDLE = '甲乙丙丁戊己庚辛';                       // 复读目标
const ASK = `只输出这八个字，不要标点、不要解释、不要换行：${NEEDLE}`;
const TRIALS = Number(process.argv[2] || 3);

console.log(`目标: ${describeTarget()}`);

class Ws {
  constructor() { this.buf = Buffer.alloc(0); this.h = []; }
  connect() {
    return new Promise((res, rej) => {
      const key = crypto.randomBytes(16).toString('base64');
      this.sock = net.connect(PORT, HOST, () => this.sock.write(
        `GET /?token=${TOKEN} HTTP/1.1\r\nHost: ${HOST}:${PORT}\r\nUpgrade: websocket\r\n` +
        `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
      this.sock.on('error', rej);
      let hs = false;
      this.sock.on('data', (c) => {
        this.buf = Buffer.concat([this.buf, c]);
        if (!hs) {
          const e = this.buf.indexOf('\r\n\r\n');
          if (e < 0) return;
          if (!/101/.test(this.buf.subarray(0, e).toString().split('\r\n')[0])) return rej(new Error('握手失败'));
          hs = true; this.buf = this.buf.subarray(e + 4); res();
        }
        this.drain();
      });
    });
  }
  drain() {
    for (;;) {
      if (this.buf.length < 2) return;
      const op = this.buf[0] & 0x0f;
      let len = this.buf[1] & 0x7f, off = 2;
      if (len === 126) { if (this.buf.length < 4) return; len = this.buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (this.buf.length < 10) return; len = Number(this.buf.readBigUInt64BE(2)); off = 10; }
      if (this.buf.length < off + len) return;
      const p = this.buf.subarray(off, off + len);
      this.buf = this.buf.subarray(off + len);
      if (op === 0x1) { const s = p.toString('utf8'); this.h.forEach(f => f(s)); }
      else if (op === 0x8) { this.sock.end(); return; }
    }
  }
  send(o) {
    const d = Buffer.from(JSON.stringify(o), 'utf8');
    const m = crypto.randomBytes(4);
    let hd;
    if (d.length < 126) hd = Buffer.from([0x81, 0x80 | d.length]);
    else { hd = Buffer.alloc(4); hd[0] = 0x81; hd[1] = 0x80 | 126; hd.writeUInt16BE(d.length, 2); }
    const md = Buffer.alloc(d.length);
    for (let i = 0; i < d.length; i++) md[i] = d[i] ^ m[i & 3];
    this.sock.write(Buffer.concat([hd, m, md]));
  }
  onText(f) { this.h.push(f); }
  close() { try { this.sock.end(); } catch {} }
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

/** 跑一次: mode = 'back2back' | 'wait' */
async function trial(mode, i) {
  const ws = new Ws();
  const reply = { text: null };
  let firstPartial = null, nPartial = 0;
  ws.onText((s) => {
    try {
      const o = JSON.parse(s);
      if (o.type === 'partial') { nPartial++; if (firstPartial === null) firstPartial = o.text; }
      else if (o.type === 'reply') reply.text = o.content;
    } catch {}
  });
  await ws.connect();
  await sleep(150);

  const waitSvcOk = () => new Promise((r) => {
    const t = setTimeout(() => r(false), 3000);
    const f = (s) => { try { if (JSON.parse(s).type === 'svc_ok') { clearTimeout(t); r(true); } } catch {} };
    ws.onText(f);
  });

  ws.send({ type: 'svc', service: 'llm' });
  if (mode === 'wait') { await waitSvcOk(); }
  ws.send({ type: 'chat', content: ASK });

  for (let k = 0; k < 60 && reply.text === null; k++) await sleep(250);
  ws.close();
  await sleep(200);

  const ok = reply.text !== null && reply.text.includes(NEEDLE);
  const head = reply.text === null ? '(无 reply)' : reply.text.slice(0, 12);
  return { mode, ok, head, firstPartial, nPartial, raw: reply.text };
}

console.log(`复读目标: ${NEEDLE}   每模式 ${TRIALS} 次\n`);
const stat = { back2back: { ok: 0, n: 0 }, wait: { ok: 0, n: 0 } };

for (const mode of ['wait', 'back2back']) {
  for (let i = 1; i <= TRIALS; i++) {
    const r = await trial(mode, i);
    stat[mode].n++;
    if (r.ok) stat[mode].ok++;
    console.log(`${r.ok ? '[OK]' : '[NO]'} ${mode.padEnd(10)} #${i}  partial=${String(r.nPartial).padStart(2)}条 首片="${r.firstPartial}" reply开头="${r.head}"`);
  }
  await sleep(500);
}

console.log(`\n${'='.repeat(60)}`);
for (const m of ['wait', 'back2back']) {
  console.log(`${m.padEnd(10)} 复读完整: ${stat[m].ok}/${stat[m].n}`);
}
console.log('');
process.exit(0);
