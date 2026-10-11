/* ============================================================
 * 探针 3: 验证 start / 音频 / end 这条 ASR 链 —— 重点是【网关认不认 end】
 *
 * 注意: 本探针发的是【噪声】, 讯飞识别不出内容, 所以拿不到 final 属正常。
 *       要判定 end 是否管用, 请用 gw_probe4.mjs (它发真实语音)。
 *
 * 用法: node gw_probe3.mjs
 * ============================================================ */

import net from 'node:net';
import crypto from 'node:crypto';
import { HOST, PORT, TOKEN, describeTarget } from './gw_config.mjs';

const t0 = Date.now();
const log = (...a) => console.log(`[${String(Date.now() - t0).padStart(6)}ms]`, ...a);
console.log(`目标: ${describeTarget()}\n`);

class Ws {
  constructor() { this.buf = Buffer.alloc(0); this.h = []; }
  connect() {
    return new Promise((res, rej) => {
      const key = crypto.randomBytes(16).toString('base64');
      this.sock = net.connect(PORT, HOST, () => this.sock.write(
        `GET /?token=${TOKEN} HTTP/1.1\r\nHost: ${HOST}:${PORT}\r\nUpgrade: websocket\r\n` +
        `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
      this.sock.on('error', rej);
      this.sock.on('close', () => log('  连接被对端关闭'));
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
      else if (op === 0x8) { log('  收到 close 帧'); this.sock.end(); return; }
    }
  }
  frame(payload, opcode) {
    const d = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'utf8');
    const m = crypto.randomBytes(4);
    let hd;
    if (d.length < 126) hd = Buffer.from([0x80 | opcode, 0x80 | d.length]);
    else if (d.length < 65536) { hd = Buffer.alloc(4); hd[0] = 0x80 | opcode; hd[1] = 0x80 | 126; hd.writeUInt16BE(d.length, 2); }
    else { hd = Buffer.alloc(10); hd[0] = 0x80 | opcode; hd[1] = 0x80 | 127; hd.writeBigUInt64BE(BigInt(d.length), 2); }
    const md = Buffer.alloc(d.length);
    for (let i = 0; i < d.length; i++) md[i] = d[i] ^ m[i & 3];
    this.sock.write(Buffer.concat([hd, m, md]));
  }
  sendJson(o) { log('  ->', JSON.stringify(o)); this.frame(JSON.stringify(o), 0x1); }
  sendBin(b)  { this.frame(b, 0x2); }
  onText(f) { this.h.push(f); }
  close() { try { this.sock.end(); } catch {} }
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

const ws = new Ws();
let gotFinal = false, gotAsr = 0, gotError = null;

ws.onText((s) => {
  let brief = s;
  try {
    const o = JSON.parse(s);
    if (o.type === 'asr')          { gotAsr++;  brief = `asr "${o.text}"`; }
    else if (o.type === 'final')   { gotFinal = true; brief = `** final "${o.text}"`; }
    else if (o.type === 'error')   { gotError = o; brief = `** error ${JSON.stringify(o)}`; }
    else                           { brief = JSON.stringify(o).slice(0, 120); }
  } catch {}
  log('  <-', brief);
});

await ws.connect();
await sleep(200);

ws.sendJson({ type: 'svc', service: 'text' });
await new Promise((r) => {
  const t = setTimeout(r, 3000);
  ws.onText((s) => { try { if (JSON.parse(s).type === 'svc_ok') { clearTimeout(t); r(); } } catch {} });
});
await sleep(200);

ws.sendJson({ type: 'start' });
await sleep(300);

/* 发 ~1.2 秒低幅噪声 (1280 字节/40ms, 与板子 I2S 一致) */
const FRAME = 1280, N = 30;
log(`  发送 ${N} 帧音频 (${N * 40}ms, 低幅噪声)...`);
for (let i = 0; i < N; i++) {
  const buf = Buffer.alloc(FRAME);
  for (let k = 0; k < FRAME; k += 2) buf.writeInt16LE(((Math.random() * 2 - 1) * 800) | 0, k);
  ws.sendBin(buf);
  await sleep(40);
}
log('  音频发完');

await sleep(200);
log('  发 end');
ws.sendJson({ type: 'end' });

log('  观察 15 秒...');
await sleep(15000);

console.log(`\n${'='.repeat(64)}`);
console.log(`asr 条数: ${gotAsr}   收到 final: ${gotFinal ? '是' : '否'}   error: ${gotError ? JSON.stringify(gotError) : '无'}`);
console.log('注: 噪声识别不出内容, 没有 final 属正常 —— 请用 gw_probe4.mjs 做真实语音对照。');
console.log('');
ws.close();
await sleep(300);
process.exit(0);
