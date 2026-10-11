/* ============================================================
 * 探针 4: 【对照实验】发真实语音, 看 end 到底管不管用
 *
 *   A 组: start -> 语音 -> end       -> final 多久来?
 *   B 组: start -> 语音 -> (不发 end) -> 网关多久才被迫断开?
 *
 *   A 组拿到 final 且 B 组要等很久 -> end 有用, 板子该发出来
 *   A 组也拿不到 final            -> 网关不认这个 end, 协议要改
 *
 * 语音来源: tools/speech_test.wav (Windows TTS 生成, 16kHz/单声道/16bit, 与板子 I2S 一致)
 * 用法: node gw_probe4.mjs
 * ============================================================ */

import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { HOST, PORT, TOKEN, describeTarget } from './gw_config.mjs';

const WAV = path.join(process.cwd(), 'tools', 'speech_test.wav');
const FRAME = 1280;          // 40ms @16k/16bit/mono —— 和板子 I2S_MIC_FRAME_BYTES 一致

console.log(`目标: ${describeTarget()}`);

/* ---- 读 WAV, 按 chunk 找 data ---- */
function loadPcm(file) {
  const b = fs.readFileSync(file);
  let p = 12;
  while (p < b.length - 8) {
    const id = b.toString('ascii', p, p + 4);
    const sz = b.readInt32LE(p + 4);
    if (id === 'data') return b.subarray(p + 8, p + 8 + sz);
    p += 8 + sz + (sz % 2);
  }
  throw new Error('WAV 里找不到 data chunk');
}
const PCM = loadPcm(WAV);
console.log(`音频: ${PCM.length} 字节 = ${(PCM.length / 32000).toFixed(2)} 秒\n`);

class Ws {
  constructor(tag) { this.tag = tag; this.buf = Buffer.alloc(0); this.h = []; this.closedAt = null;
                     this.log = () => {};   /* 由 run() 赋成带时间戳的 L */ }
  connect() {
    return new Promise((res, rej) => {
      const key = crypto.randomBytes(16).toString('base64');
      this.sock = net.connect(PORT, HOST, () => this.sock.write(
        `GET /?token=${TOKEN} HTTP/1.1\r\nHost: ${HOST}:${PORT}\r\nUpgrade: websocket\r\n` +
        `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
      this.sock.on('error', rej);
      this.sock.on('close', () => { if (this.closedAt === null) this.closedAt = Date.now(); });
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
  /* ★★ 发送即打印 —— 见 run() 里 ws.log = L 那段说明。
   *   这里绝不能再出现"只 L(...) 不 frame(...)"的写法。 */
  json(o) { const s = JSON.stringify(o); this.log('  -> ' + s); this.frame(s, 0x1); }
  /* 音频帧有 109 条, 逐条打会刷屏 —— 保持静默, 由调用方汇总打一行 */
  bin(b)  { this.frame(b, 0x2); }
  onText(f) { this.h.push(f); }
  close() { try { this.sock.end(); } catch {} }
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function run(sendEnd) {
  const name = sendEnd ? 'A 组 (发 end)' : 'B 组 (不发 end)';
  console.log(`${'='.repeat(68)}\n> ${name}\n${'='.repeat(68)}`);
  const t0 = Date.now();
  const L = (...a) => console.log(`[${String(Date.now() - t0).padStart(6)}ms]`, ...a);

  const ws = new Ws(name);
  /* ★★ 让【发送动作自己打日志】, 而不是在外面手写一行 L('-> ...')。
   *
   * 为什么必须这样: 这个脚本曾经踩着一个大坑 —— 分支里写了
   *     L('  -> {"type":"end"}   ** 关键动作');
   * 却【漏了真正的 ws.json({type:'end'})】。于是日志上看起来"发了 end",
   * 实际上一条都没发, A/B 两组跑的是同一件事, 却被当成"end 无效"的铁证,
   * 白查了半天网关。日志和动作分成两处写, 就一定会有一天对不上。
   * 现在发送=打印, 两者不可能再分家。 */
  ws.log = L;

  const ev = { asr: [], final: null, error: null, endSentAt: null, finalAt: null };
  ws.onText((s) => {
    let o; try { o = JSON.parse(s); } catch { return; }
    if (o.type === 'asr')        { ev.asr.push(o.text); L(`  <- asr "${o.text}"`); }
    else if (o.type === 'final') { if (!ev.final) { ev.final = o.text; ev.finalAt = Date.now(); }
                                   L(`  <- *** final "${o.text}"  (距 end ${ev.endSentAt ? Date.now() - ev.endSentAt : '-'}ms)`); }
    else if (o.type === 'error') { ev.error = o; L(`  <- ** error ${JSON.stringify(o)}`); }
    else if (o.type !== 'svc_ok') L(`  <- ${JSON.stringify(o).slice(0, 110)}`);
  });

  await ws.connect();
  await sleep(200);

  /* svc 的发出由 ws.json 自己打日志; svc_ok 的到达在这里记 (含往返耗时) */
  ws.json({ type: 'svc', service: 'text' });
  {
    const t0svc = Date.now();
    let gotSvcOk = false;
    await new Promise((r) => {
      const t = setTimeout(r, 3000);
      ws.onText((s) => {
        try {
          const o = JSON.parse(s);
          if (o.type === 'svc_ok') {
            gotSvcOk = true;
            L(`  <- svc_ok("${o.service}")   (往返 ${Date.now() - t0svc}ms)`);
            clearTimeout(t); r();
          }
        } catch {}
      });
    });
    if (!gotSvcOk) L('  ⚠️ 3 秒内没等到 svc_ok —— 后面的 start 可能是白发');
  }
  await sleep(200);

  ws.json({ type: 'start' });
  await sleep(300);

  const n = Math.floor(PCM.length / FRAME);
  L(`  发送 ${n} 帧语音 (${(n * 40 / 1000).toFixed(1)} 秒)...`);
  for (let i = 0; i < n; i++) { ws.bin(PCM.subarray(i * FRAME, (i + 1) * FRAME)); await sleep(40); }
  L('  语音发完');
  await sleep(200);

  if (sendEnd) {
    ev.endSentAt = Date.now();
    /* ★★★ 这一行曾经【漏掉过】—— 只写了上面的日志没写它,
     *   导致 5 次实验 A 组压根没发 end, 却被当成"end 无效"的铁证。
     *   现在 ws.json() 自己会打日志, 日志和动作不可能再分家。 */
    ws.json({ type: 'end' });
  } else {
    L('  (故意不发 end)');
  }

  const waitMs = sendEnd ? 15000 : 70000;
  L(`  观察 ${waitMs / 1000} 秒...`);
  const deadline = Date.now() + waitMs;
  while (Date.now() < deadline && !ev.final && ws.closedAt === null) await sleep(300);

  if (ws.closedAt !== null) L(`  连接在 ${ws.closedAt - t0}ms 被对端关闭`);
  ws.close();
  await sleep(300);

  console.log(`\n  小结: asr=${ev.asr.length}条   final=${ev.final !== null ? '有' : '无'}`
            + `   end->final=${ev.finalAt && ev.endSentAt ? (ev.finalAt - ev.endSentAt) + 'ms' : '-'}`
            + `   连接关闭=${ws.closedAt ? (ws.closedAt - t0) + 'ms' : '未关'}\n`);
  return ev;
}

const A = await run(true);
await sleep(1000);
const B = await run(false);

console.log(`${'='.repeat(68)}\n结论\n${'='.repeat(68)}`);
console.log(`A 组 (发 end)  : final ${A.final !== null ? '有' : '无'}`
          + (A.endSentAt && A.finalAt ? `   end->final = ${A.finalAt - A.endSentAt}ms` : ''));
console.log(`B 组 (不发 end): final ${B.final !== null ? '有' : '无'}`);
console.log('');
if (A.final !== null) {
  console.log('-> 网关【认得】{"type":"end"}: 发出去就正常下发 final。');
  console.log('   所以板子那边的问题是【end 没发成功】, 不是协议格式。');
  console.log('   下一步: 看板子日志里 rtasr 打的 "-> end (service=..., connected=...)" 那行。');
} else {
  console.log('-> 网关收到 end 也没有 final。可能是格式不对, 或语音没被正确识别。');
  console.log(`   (A 组收到了 ${A.asr.length} 条 asr —— 若为 0, 说明语音根本没进识别, 本次实验无效)`);
}
console.log('');
process.exit(0);
