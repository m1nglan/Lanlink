#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
lanlink-gateway server.py — LanLink 统一服务网关 V1.0

统一入口: ws://<server>:18888/?token=***&type=<service>
  type=text      语音听写（PCM -> 讯飞 -> 文本流式回传）
  type=llm       大模型对话（DeepSeek, 网关自维护上下文）
  type=openclaw  明岚对话（转发 OpenClaw chatCompletions, 接入 main 会话）

对话类(type=llm/openclaw)协议:
  上行: {"type":"chat","content":"..."}   发消息
        {"type":"clear"}                   清空上下文
  下行: {"type":"reply","content":"..."}  完整回复
        {"type":"partial","text":"..."}   流式增量(openclaw 支持)
        {"type":"error","code":N,"message":"..."}

运行: python3 server.py
"""

import asyncio
import gzip
import json
import os
import logging
import subprocess
import time
import urllib.request

import websockets

from xfyun_iat import XfyunIAT

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("gateway")

CONFIG = {
    "host": "0.0.0.0",
    "port": 18888,
    "token": "lanlink2026",            # 板子连接鉴权 token
    "default_service": "echo",         # 默认服务(临时echo便于调试, 调完改回text)
    "xfyun": {
        "app_id": "c943edd0",
        "api_key": "fadda09de753c118a20be29a0a7f25d6",
        "api_secret": "Y2I2MWI4MmE4MzkzOTYxMWUzZTU1NDdk",
        "wpgs": True,   # True=边说边出字(可能有早期噪音) False=结束才出(更准)
    },
    "llm": {
        "base_url": "https://api.deepseek.com/v1",
        "api_key": "",  # 会被 openclaw.json 覆盖
        "model": "deepseek-chat",
        "max_history": 20,             # 最多保留的历史消息条数
        "system_prompt": "你是 LanLink 的智能助手，运行在一个带屏幕和麦克风的便携终端上。回答简短、自然、友好，像朋友聊天（通常 1~3 句，适合语音播报和小屏幕显示）。不要使用 Markdown 格式（不要加粗、代码块、列表、表格），也不要使用 emoji 表情；可以用简单的颜文字如 ^_^ 或 (￣▽￣)。",
    },
    "vision": {
        "model": "deepseek-v4-flash-vision-exp",   # 多模态实验模型(图片输入)
    },
    "openclaw": {
        "base_url": "http://127.0.0.1:18789/v1",
        "token": None,                  # 自动从 openclaw.json 读取
        "model": "openclaw/default",
        "session_key": "agent:main:main",   # 接入 main 会话(主子微信可见)
    },
    "qweather": {
        "host": "mk49vkv76j.re.qweatherapi.com",   # 和风专属 API host
        "key": "36ff58cb4a424d0bbdb181cbcc836c93",  # 会被 config.json 覆盖
        "location": "101080109",      # 呼和浩特 (lat 40.81515, lon 111.66216)
    },
}

# 天气 icon code → weather_kind 归类 (1晴 2夜晴 3多云 4阴 5雨 6雪 7雷雨 8风)
def icon_to_kind(icon):
    try:
        code = int(icon)
    except (TypeError, ValueError):
        return 3
    if code == 100:
        return 1        # 白天晴
    if code == 150:
        return 2        # 夜晚晴
    if 101 <= code <= 103 or 151 <= code <= 153:
        return 3        # 多云
    if code == 104:
        return 4        # 阴天
    if code in (302, 303, 304):
        return 7        # 雷雨(雷阵雨/强雷暴/伴冰雹)
    if 200 <= code <= 213:
        return 8        # 风(有风/狂风/飓风/龙卷风/热带风暴)
    if 300 <= code <= 399:
        return 5        # 雨(排除雷暴)
    if 400 <= code <= 499:
        return 6        # 雪(含雨夹雪/阵雪)
    return 3            # 999未知等 → 兜底多云

# 音频帧大小: 40ms @16kHz/16bit/mono = 1280B
FRAME_BYTES = 1280
MAX_SESSION_SECONDS = 55


# ============ 读取真实密钥 ============
CONFIG_FILE = "/home/m1nglan/lanlink-gateway/config.json"


def load_keys():
    """从服务器上的 config.json 读取密钥(容器内没有 openclaw.json)。"""
    try:
        with open(CONFIG_FILE) as f:
            cfg = json.load(f)
        oc = cfg.get("openclaw", {})
        llm = cfg.get("llm", {})
        qw = cfg.get("qweather", {})
        CONFIG["openclaw"]["base_url"] = oc.get("base_url", CONFIG["openclaw"]["base_url"])
        CONFIG["openclaw"]["token"] = oc.get("token", CONFIG["openclaw"]["token"])
        CONFIG["openclaw"]["session_key"] = oc.get("session_key", CONFIG["openclaw"]["session_key"])
        if llm.get("api_key"):
            CONFIG["llm"]["api_key"] = llm["api_key"]
            CONFIG["llm"]["base_url"] = llm.get("base_url", CONFIG["llm"]["base_url"])
        if qw.get("key"):
            CONFIG["qweather"].update(qw)
        log.info("config.json 加载完成: openclaw=%s deepseek=%s qweather=%s",
                 (CONFIG["openclaw"]["token"] or "")[:8] + "...",
                 CONFIG["llm"]["api_key"][:8] + "...",
                 CONFIG["qweather"]["key"][:8] + "...")
    except Exception as e:
        log.warning("读取 config.json 失败: %s (请用 gen_config.py 生成)", e)


# ============ 语音听写 (type=text) ============

async def run_text_session(ws, audio_queue):
    """一轮识别: audio_queue -> 讯飞 -> 全量回传。
    asr:   中间结果，每次都是【从头到现在的完整句】(板子整体替换)
    final: 最终结果，完整句(板子整体替换，修正错字)
    """
    results_q = asyncio.Queue()

    async def send_result(text, is_final=False):
        await results_q.put((text, is_final, "final"))

    iat = XfyunIAT(CONFIG["xfyun"]["app_id"],
                   CONFIG["xfyun"]["api_key"],
                   CONFIG["xfyun"]["api_secret"],
                   wpgs=CONFIG["xfyun"].get("wpgs", True))

    async def iat_worker():
        try:
            frames = [0]
            SILENCE = b"\x00" * 1280   # 40ms 静音@16k/16bit/mono（保活用）

            async def gen():
                while True:
                    try:
                        item = await asyncio.wait_for(audio_queue.get(), timeout=0.02)  # 队列空→快速产静音帧保持 40ms 节奏
                    except asyncio.TimeoutError:
                        # ★ 保活：队列空时补静音帧，防止讯飞"30秒未收到音频"断连
                        yield SILENCE
                        continue
                    if item is None:
                        break
                    frames[0] += 1
                    yield item

            log.info("讯飞会话开始")
            final_sentence, abnormal = await iat.transcribe(gen(), on_text=send_result)
            log.info("讯飞会话结束, 转发帧数: %d", frames[0])
            if abnormal:
                # 异常收尾(静默超时等) → 通知板子：有字带字，没字带 null
                await results_q.put((final_sentence or "", True, "abroad"))
            else:
                # 正常收尾 → 完整句
                await results_q.put((final_sentence or "", True, "final"))
        except Exception as e:
            log.error("iat error: %s", e)
            await results_q.put((f"__ERROR__:{e}", True, "final"))

    task = asyncio.create_task(iat_worker())
    try:
        while True:
            text, is_final, kind = await results_q.get()
            if text.startswith("__ERROR__:"):
                await ws.send(json.dumps({"type": "error", "code": 1,
                                          "message": text[len("__ERROR__:"):]}, ensure_ascii=False))
                break
            if text or is_final:   # final/abroad 即使内容为空也必须发（abroad 要带 null）
                if is_final:
                    if kind == "abroad":
                        # 异常收尾(静音超时等)：有字带字，没字带 null
                        log.info("下行 abroad: %s", text if text else "(null)")
                        await ws.send(json.dumps({"type": "abroad", "text": text if text else None},
                                                 ensure_ascii=False))
                    else:
                        # 最终: 完整句，板子整体替换
                        log.info("下行 final: %s", text)
                        await ws.send(json.dumps({"type": "final", "text": text},
                                                 ensure_ascii=False))
                else:
                    # 中间: 全量完整句（不限流，讯飞自身约 3~8 条/秒）
                    log.info("下行 asr: %s", text)
                    await ws.send(json.dumps({"type": "asr", "text": text},
                                             ensure_ascii=False))
            if is_final:
                break
    finally:
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass


async def handle_text(ws):
    """type=text: 长连接多轮语音听写。"""
    audio_queue = None
    session_task = None
    total_bytes = 0
    buf = b""
    session_active = False
    try:
        while True:
            msg = await ws.recv()
            if isinstance(msg, bytes):
                if not session_active:
                    log.warning("收到音频 %dB 但没有 start, 丢弃", len(msg))
                    continue
                buf += msg
                total_bytes += len(msg)
                if total_bytes > 16000 * 2 * MAX_SESSION_SECONDS:
                    await ws.send(json.dumps({"type": "error", "code": 3,
                                              "message": "本轮音频超过 55s 上限"}, ensure_ascii=False))
                    if audio_queue:
                        await audio_queue.put(None)
                    audio_queue = None
                    session_active = False
                    total_bytes = 0
                    buf = b""
                    continue
                while len(buf) >= FRAME_BYTES:
                    if audio_queue:
                        await audio_queue.put(buf[:FRAME_BYTES])
                    buf = buf[FRAME_BYTES:]
                continue
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                continue
            mtype = data.get("type")
            if mtype == "start":
                if session_active:
                    if audio_queue:
                        await audio_queue.put(None)
                    audio_queue = None
                    if session_task:
                        session_task.cancel()
                    session_active = False
                    total_bytes = 0
                    buf = b""
                audio_queue = asyncio.Queue()
                session_task = asyncio.create_task(run_text_session(ws, audio_queue))
                session_active = True
                total_bytes = 0
                buf = b""
            elif mtype == "end":
                if session_active:
                    if buf and audio_queue:
                        await audio_queue.put(buf)
                    buf = b""
                    if audio_queue:
                        log.info("收到 end：队列剩余 %d 帧待转发(%.2fs音频)", audio_queue.qsize(), audio_queue.qsize()*0.04)
                        await audio_queue.put(None)
                    audio_queue = None
                    session_active = False
                    total_bytes = 0
            elif mtype == "ping":
                await ws.send(json.dumps({"type": "pong"}))
    except websockets.exceptions.ConnectionClosed:
        log.info("text client disconnected")
    finally:
        if audio_queue:
            try:
                await audio_queue.put(None)
            except Exception:
                pass
        if session_task:
            session_task.cancel()


# ============ 对话类 (type=llm / type=openclaw) ============

async def call_openai(base_url, api_key, path, body, timeout=60):
    """调用 OpenAI 兼容接口, 返回 (status, json)。"""
    req = urllib.request.Request(
        f"{base_url}{path}",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {api_key}"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8"))


async def handle_chat(ws, backend: str):
    """type=llm/openclaw: 对话服务, 网关维护上下文。"""
    history = []
    system_prompt = CONFIG["llm"]["system_prompt"]
    try:
        while True:
            msg = await ws.recv()
            if isinstance(msg, bytes):
                log.warning("对话模式收到二进制帧, 忽略")
                continue
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                continue
            mtype = data.get("type")

            if mtype == "ping":
                await ws.send(json.dumps({"type": "pong"}))
                continue

            if mtype == "clear":
                history = []
                await ws.send(json.dumps({"type": "reply", "content": "上下文已清空"}))
                continue

            if mtype != "chat":
                continue

            content = (data.get("content") or "").strip()
            if not content:
                continue

            # 维护上下文
            if not history and system_prompt:
                history.append({"role": "system", "content": system_prompt})
            history.append({"role": "user", "content": content})
            if len(history) > CONFIG["llm"]["max_history"]:
                history = [history[0]] + history[-(CONFIG["llm"]["max_history"] - 1):]

            try:
                if backend == "openclaw":
                    body = {
                        "model": CONFIG["openclaw"]["model"],
                        "messages": [{"role": "user", "content": content}],
                    }
                    req = urllib.request.Request(
                        f"{CONFIG['openclaw']['base_url']}/chat/completions",
                        data=json.dumps(body).encode("utf-8"),
                        headers={
                            "Content-Type": "application/json",
                            "Authorization": f"Bearer {CONFIG['openclaw']['token']}",
                            "x-openclaw-session-key": CONFIG["openclaw"]["session_key"],
                        },
                        method="POST",
                    )
                    with urllib.request.urlopen(req, timeout=90) as resp:
                        result = json.loads(resp.read().decode("utf-8"))
                    reply = result["choices"][0]["message"]["content"].strip()
                else:
                    # llm: DeepSeek, 带完整上下文
                    _, result = await call_openai(
                        CONFIG["llm"]["base_url"], CONFIG["llm"]["api_key"],
                        "/chat/completions",
                        {"model": CONFIG["llm"]["model"], "messages": history})
                    reply = result["choices"][0]["message"]["content"].strip()

                history.append({"role": "assistant", "content": reply})
                await ws.send(json.dumps({"type": "reply", "content": reply},
                                         ensure_ascii=False))
            except Exception as e:
                log.error("%s 调用失败: %s", backend, e)
                await ws.send(json.dumps({"type": "error", "code": 2,
                                          "message": f"{backend} 调用失败: {e}"},
                                         ensure_ascii=False))
    except websockets.exceptions.ConnectionClosed:
        log.info("%s client disconnected", backend)


# ============ 服务器指标采集 (service=usage) ============

def sh(cmd):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=5)
        return r.stdout.strip()
    except Exception:
        return ''

_net_last = None
_net_last_t = 0.0

def net_speed():
    """返回 (rx_kbs, tx_kbs)，基于 /proc/net/dev 两次采样差值, 单位 KB/s。"""
    global _net_last, _net_last_t
    now = time.time()
    rx = tx = 0
    try:
        with open('/proc/net/dev') as f:
            for line in f:
                if ':' not in line:
                    continue
                iface, data = line.split(':')
                iface = iface.strip()
                if iface == 'lo':
                    continue
                parts = data.split()
                rx += int(parts[0])
                tx += int(parts[8])
    except Exception:
        return 0, 0
    if _net_last is not None and _net_last_t > 0:
        dt = now - _net_last_t
        if dt > 0:
            out = (max(0, int((rx - _net_last[0]) / dt / 1024)),
                   max(0, int((tx - _net_last[1]) / dt / 1024)))
            _net_last = (rx, tx)
            _net_last_t = now
            return out
    _net_last = (rx, tx)
    _net_last_t = now
    return 0, 0


def collect_usage():
    try:
        cpu_total = os.cpu_count() or 1
    except Exception:
        cpu_total = 1
    """2s一跳的服务器指标, 与独立 ws_server.py 格式一致。"""
    c = sh("top -bn1 | grep 'Cpu(s)' | awk '{print int($2+$4)}'")
    m = sh("free -m | awk 'NR==2{print $3,$2,$NF}'").split()
    mu = int(m[0]) if len(m) > 0 else 0
    mt = int(m[1]) if len(m) > 1 else 0
    ma = int(m[2]) if len(m) > 2 else 0
    mp = round((mt - ma) / mt * 100) if mt > 0 else 0
    d = sh("df / | awk 'NR==2{print $3,$2}'").split()
    du = int(d[0]) if len(d) > 0 else 0
    dt = int(d[1]) if len(d) > 1 else 0
    dp = round(du / dt * 100) if dt > 0 else 0
    rx, tx = net_speed()
    ram_used = f"{mu/1024:.1f}G/{round(mt/1024)}G" if mt > 0 else "0G/0G"
    disk_used = f"{du/1024/1024:.1f}G/{dt/1024/1024:.1f}G" if dt > 0 else "0G/0G"
    return {
        "type": "usage",
        "usage": True,
        "t": int(time.time()),
        "cpu": int(c) if c else 0,
        "cpu_str": f"{(int(c) if c else 0) * cpu_total / 100:.1f}/{cpu_total} Core",
        "ram_used": ram_used,
        "ram_pct": int(mp),
        "disk_used": disk_used,
        "disk_pct": int(dp),
        "rx": rx,
        "tx": tx,
    }


# ============ DeepSeek 余额 (service=balance) ============

def fetch_balance():
    """查询 DeepSeek 账户余额（按需调用，不做定时轮询）。"""
    key = CONFIG["llm"]["api_key"]
    req = urllib.request.Request(
        "https://api.deepseek.com/user/balance",
        headers={"Authorization": f"Bearer {key}", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        d = json.loads(r.read().decode("utf-8"))

    infos = d.get("balance_infos") or []
    info = infos[0] if infos else {}
    total = info.get("total_balance", "0")

    def f2(x):
        try:
            return round(float(x), 2)
        except Exception:
            return 0.0

    return {
        "type": "balance",
        "balance": True,
        "total": f2(total),
        "t": int(time.time()),
    }


# ============ 和风天气 (service=weather) ============

def qw_fetch_json(url):
    """GET + 自动解压 gzip。"""
    req = urllib.request.Request(url, headers={"Accept-Encoding": "gzip"})
    with urllib.request.urlopen(req, timeout=10) as r:
        data = r.read()
        if r.headers.get("Content-Encoding") == "gzip":
            data = gzip.decompress(data)
        return json.loads(data)


_qw_cache = {"t": 0.0, "data": None}


def fetch_weather():
    """拉实时天气+每日天气, 返回板子用的5字段; 60s 缓存省 QPS。"""
    global _qw_cache
    if _qw_cache["data"] and time.time() - _qw_cache["t"] < 60:
        return _qw_cache["data"]
    qw = CONFIG["qweather"]
    base = f"https://{qw['host']}/v7/weather"
    loc = qw["location"]
    key = qw["key"]
    now = qw_fetch_json(f"{base}/now?location={loc}&key={key}")
    daily = qw_fetch_json(f"{base}/3d?location={loc}&key={key}")
    if now.get("code") != "200" or daily.get("code") != "200":
        raise RuntimeError(f"qweather code: now={now.get('code')} daily={daily.get('code')}")
    n = now["now"]
    d0 = daily["daily"][0]
    out = {
        "type": "weather",
        "weather": True,
        "text": n["text"],            # 当前天气(中文)
        "temp": n["temp"],            # 当前温度
        "temp_max": d0["tempMax"],    # 当日最高
        "temp_min": d0["tempMin"],    # 当日最低
        "kind": icon_to_kind(n["icon"]),   # 天气类型 1~8
    }
    _qw_cache = {"t": time.time(), "data": out}
    return out


# ============ 统一入口路由（应用层切换服务） ============

async def handle_conn(ws):
    """一条连接: 用 {"type":"svc","service":"xxx"} 在 text/llm/openclaw 间切换。"""
    current = CONFIG.get("default_service", "text")
    import time as _time
    last_ping_log = 0.0
    # 各服务状态
    audio_queue = None
    session_task = None
    usage_task = None
    total_bytes = 0
    buf = b""
    session_active = False
    history = []
    system_prompt = CONFIG["llm"]["system_prompt"]

    def reset_text():
        nonlocal audio_queue, session_task, total_bytes, buf, session_active
        if audio_queue:
            try:
                asyncio.get_running_loop().create_task(audio_queue.put(None))
            except Exception:
                pass
        audio_queue = None
        if session_task:
            session_task.cancel()
        session_task = None
        session_active = False
        total_bytes = 0
        buf = b""

    def stop_usage():
        nonlocal usage_task
        if usage_task:
            usage_task.cancel()
            usage_task = None

    async def start_usage():
        nonlocal usage_task
        stop_usage()
        async def pusher():
            try:
                while True:
                    data = await asyncio.to_thread(collect_usage)
                    try:
                        await ws.send(json.dumps(data, ensure_ascii=False))
                    except Exception:
                        return
                    await asyncio.sleep(2)
            except asyncio.CancelledError:
                pass
        usage_task = asyncio.create_task(pusher())

    async def push_weather():
        try:
            data = await asyncio.to_thread(fetch_weather)
            await ws.send(json.dumps(data, ensure_ascii=False))
        except Exception as e:
            log.error("weather 获取失败: %s", e)
            await ws.send(json.dumps({"type": "error", "code": 2,
                                      "message": f"weather 获取失败: {e}"},
                                     ensure_ascii=False))

    async def push_balance():
        try:
            data = await asyncio.to_thread(fetch_balance)
            await ws.send(json.dumps(data, ensure_ascii=False))
        except Exception as e:
            log.error("balance 获取失败: %s", e)
            await ws.send(json.dumps({"type": "error", "code": 5,
                                      "message": f"balance 获取失败: {e}"},
                                     ensure_ascii=False))

    async def run_chat(content):
        nonlocal history
        if not history and system_prompt:
            history.append({"role": "system", "content": system_prompt})
        history.append({"role": "user", "content": content})
        if len(history) > CONFIG["llm"]["max_history"]:
            history = [history[0]] + history[-(CONFIG["llm"]["max_history"] - 1):]
        try:
            if current == "openclaw":
                body = {"model": CONFIG["openclaw"]["model"],
                        "messages": [{"role": "user", "content": content}],
                        "stream": True}   # SSE 流式, 边说边出
                req = urllib.request.Request(
                    f"{CONFIG['openclaw']['base_url']}/chat/completions",
                    data=json.dumps(body).encode("utf-8"),
                    headers={
                        "Content-Type": "application/json",
                        "Authorization": f"Bearer {CONFIG['openclaw']['token']}",
                        "x-openclaw-session-key": CONFIG["openclaw"]["session_key"],
                    },
                    method="POST",
                )
                resp = urllib.request.urlopen(req, timeout=120)
                reply = ""
                sse_buf = b""   # 不完整的 SSE 行缓冲(半个 UTF-8 字节时等待拼接)
                for raw_chunk in resp:   # 流式读
                    sse_buf += raw_chunk
                    # 按行切分
                    while b"\n" in sse_buf:
                        line_b, sse_buf = sse_buf.split(b"\n", 1)
                        line_b = line_b.strip()
                        if not line_b.startswith(b"data:"):
                            continue
                        payload_b = line_b[5:].strip()
                        if payload_b == b"[DONE]":
                            break
                        try:
                            chunk = json.loads(payload_b)   # bytes 严格 UTF-8 解码
                        except (json.JSONDecodeError, UnicodeDecodeError):
                            continue   # 半个字符行, 丢弃(下一行会带上完整内容)
                        choices = chunk.get("choices") or []
                        if not choices:
                            continue
                        delta = (choices[0].get("delta") or {}).get("content") or ""
                        if delta:
                            reply += delta
                            await ws.send(json.dumps({"type": "partial", "text": delta},
                                                     ensure_ascii=False))
                    else:
                        continue
                    break
            else:
                # llm: DeepSeek 流式(SSE), 带完整上下文
                body = {"model": CONFIG["llm"]["model"], "messages": history,
                        "stream": True}
                req = urllib.request.Request(
                    f"{CONFIG['llm']['base_url']}/chat/completions",
                    data=json.dumps(body).encode("utf-8"),
                    headers={"Content-Type": "application/json",
                             "Authorization": f"Bearer {CONFIG['llm']['api_key']}"},
                    method="POST",
                )
                resp = urllib.request.urlopen(req, timeout=120)
                reply = ""
                sse_buf = b""
                for raw_chunk in resp:
                    sse_buf += raw_chunk
                    while b"\n" in sse_buf:
                        line_b, sse_buf = sse_buf.split(b"\n", 1)
                        line_b = line_b.strip()
                        if not line_b.startswith(b"data:"):
                            continue
                        payload_b = line_b[5:].strip()
                        if payload_b == b"[DONE]":
                            break
                        try:
                            chunk = json.loads(payload_b)
                        except (json.JSONDecodeError, UnicodeDecodeError):
                            continue
                        choices = chunk.get("choices") or []
                        if not choices:
                            continue
                        delta = (choices[0].get("delta") or {}).get("content") or ""
                        if delta:
                            reply += delta
                            await ws.send(json.dumps({"type": "partial", "text": delta},
                                                     ensure_ascii=False))
                    else:
                        continue
                    break
            history.append({"role": "assistant", "content": reply})
            await ws.send(json.dumps({"type": "reply", "content": reply},
                                     ensure_ascii=False))
        except Exception as e:
            log.error("%s 调用失败: %s", current, e)
            await ws.send(json.dumps({"type": "error", "code": 2,
                                      "message": f"{current} 调用失败: {e}"},
                                     ensure_ascii=False))

    try:
        while True:
            msg = await ws.recv()

            # ---- 音频帧: 仅 text 模式有效 ----
            if isinstance(msg, bytes):
                if current == "echo":
                    await ws.send(msg)   # echo 调试: 二进制原样回显
                    continue
                if current != "text":
                    log.warning("当前模式=%s, 忽略音频帧", current)
                    continue
                if not session_active:
                    log.warning("收到音频但没有 start, 丢弃")
                    continue
                buf += msg
                total_bytes += len(msg)
                if total_bytes % (FRAME_BYTES * 25) == 0:  # 每25帧(1秒)打一条
                    log.info("收到音频: %d 帧 / %d 字节", total_bytes // FRAME_BYTES, total_bytes)
                if total_bytes > 16000 * 2 * MAX_SESSION_SECONDS:
                    await ws.send(json.dumps({"type": "error", "code": 3,
                                              "message": "本轮音频超过 55s 上限"},
                                             ensure_ascii=False))
                    reset_text()
                    continue
                while len(buf) >= FRAME_BYTES:
                    if audio_queue:
                        await audio_queue.put(buf[:FRAME_BYTES])
                    buf = buf[FRAME_BYTES:]
                continue

            # ---- 文本帧 ----
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                log.warning("非法JSON: %s", str(msg)[:80])
                continue
            mtype = data.get("type")
            log.info("收到消息: %s", mtype)

            if mtype == "ping":
                now = _time.time()
                if now - last_ping_log > 30:
                    log.info("ping from client")
                    last_ping_log = now
                await ws.send(json.dumps({"type": "pong"}))

            elif mtype == "svc":
                svc = data.get("service")
                if svc not in ("text", "llm", "openclaw", "echo", "vision",
                               "usage", "weather", "balance"):
                    await ws.send(json.dumps({"type": "error", "code": 4,
                                              "message": f"未知服务: {svc}"},
                                             ensure_ascii=False))
                    continue
                if svc != current:
                    reset_text()
                    stop_usage()
                    history = []
                    current = svc
                    log.info("切换到服务: %s", svc)
                    await ws.send(json.dumps({"type": "svc_ok", "service": current}))
                    if current == "usage":
                        await start_usage()
                    elif current == "weather":
                        await push_weather()
                    elif current == "balance":
                        await push_balance()
                else:
                    await ws.send(json.dumps({"type": "svc_ok", "service": current}))

            elif mtype == "get" and current == "weather":
                # 板子主动申请刷新天气
                await push_weather()

            elif mtype == "get" and current == "balance":
                # 板子主动申请刷新余额
                await push_balance()

            elif current == "echo":
                # 调试模式: 接受什么回什么
                if isinstance(msg, bytes):
                    await ws.send(msg)
                else:
                    await ws.send(json.dumps({"type": "echo", "data": msg},
                                             ensure_ascii=False))

            elif mtype == "start":
                log.info("收到 start (current=%s)", current)
                if current != "text":
                    continue
                reset_text()
                audio_queue = asyncio.Queue()
                session_task = asyncio.create_task(run_text_session(ws, audio_queue))
                session_active = True
                total_bytes = 0
                buf = b""

            elif mtype == "end":
                log.info("收到 end (current=%s active=%s)", current, session_active)
                # 只认「有没有活跃会话」：不再依赖 current（svc 未发时 current 可能不是 text）
                if not session_active:
                    continue
                if buf and audio_queue:
                    await audio_queue.put(buf)
                buf = b""
                if audio_queue:
                    await audio_queue.put(None)
                audio_queue = None
                session_active = False
                total_bytes = 0

            elif mtype == "clear":
                history = []
                await ws.send(json.dumps({"type": "reply", "content": "上下文已清空"}))

            elif mtype == "image" and current == "vision":
                # 图片识别: base64 data URL 或 外部URL
                img = (data.get("base64") or data.get("url") or "").strip()
                prompt = (data.get("prompt") or "请详细描述这张图片的内容").strip()
                if not img:
                    await ws.send(json.dumps({"type": "error", "code": 4,
                                              "message": "缺少 base64 或 url"},
                                             ensure_ascii=False))
                    continue
                try:
                    body = {
                        "model": CONFIG["vision"]["model"],
                        "messages": [{"role": "user", "content": [
                            {"type": "text", "text": prompt},
                            {"type": "image_url", "image_url": {"url": img}},
                        ]}],
                    }
                    _, result = await call_openai(
                        CONFIG["llm"]["base_url"], CONFIG["llm"]["api_key"],
                        "/chat/completions", body)
                    reply = result["choices"][0]["message"]["content"].strip()
                    await ws.send(json.dumps({"type": "reply", "content": reply},
                                             ensure_ascii=False))
                except Exception as e:
                    log.error("vision 调用失败: %s", e)
                    await ws.send(json.dumps({"type": "error", "code": 2,
                                              "message": f"vision 失败: {e}"},
                                             ensure_ascii=False))

            elif mtype == "chat":
                content = (data.get("content") or "").strip()
                if content and current in ("llm", "openclaw"):
                    await run_chat(content)

    except websockets.exceptions.ConnectionClosed:
        log.info("client disconnected")
    finally:
        reset_text()
        stop_usage()


async def main():
    async def handler(ws):
        # 解析 /?token=xxx (不再用 type 分流, 统一入口 + 应用层切换)
        params = {}
        try:
            query = ws.request.path.split("?", 1)[1]
            params = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)
        except Exception:
            pass
        if params.get("token") != CONFIG["token"]:
            await ws.close(code=4001, reason="bad token")
            return
        await handle_conn(ws)

    async with websockets.serve(handler, CONFIG["host"], CONFIG["port"],
                                max_size=None,
                                ping_interval=120,   # 120s ping一次(检测死连接)
                                ping_timeout=60):    # 60s 无 pong 判定死亡并断开
        log.info("lanlink-gateway V1.3 启动(半开检测): ws://%s:%d/?token=***",
                 CONFIG["host"], CONFIG["port"])
        await asyncio.Future()


if __name__ == "__main__":
    load_keys()
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
