#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xfyun_iat.py — 讯飞语音听写（流式版）WebSocket 客户端
文档: https://www.xfyun.cn/doc/asr/voicedictation/API.html

供 lanlink-gateway 使用: 接收 PCM 音频流, 转发讯飞, 回调返回识别文本。
"""

import asyncio
import base64
import datetime
import hashlib
import hmac
import json
from urllib.parse import urlencode

import websockets

# 讯飞接口地址（中英文）
IAT_URL = "wss://iat-api.xfyun.cn/v2/iat"

RAW_FRAME_LOG = False   # True: 打印讯飞原始帧(rg/pgs/ws)，排查用


def build_auth_url(api_key: str, api_secret: str, host: str = "iat-api.xfyun.cn",
                   path: str = "/v2/iat") -> str:
    """按讯飞 hmac-sha256 签名规范生成带鉴权参数的 wss URL。"""
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%a, %d %b %Y %H:%M:%S GMT")

    # signature_origin = host + "\n" + date + "\n" + "GET {path} HTTP/1.1"
    signature_origin = f"host: {host}\ndate: {date}\nGET {path} HTTP/1.1"
    signature_sha = hmac.new(api_secret.encode("utf-8"),
                             signature_origin.encode("utf-8"),
                             hashlib.sha256).digest()
    signature = base64.b64encode(signature_sha).decode("utf-8")

    authorization_origin = (
        f'api_key="{api_key}", algorithm="hmac-sha256", '
        f'headers="host date request-line", signature="{signature}"'
    )
    authorization = base64.b64encode(authorization_origin.encode("utf-8")).decode("utf-8")

    query = urlencode({
        "host": host,
        "date": date,
        "authorization": authorization,
    })
    return f"wss://{host}{path}?{query}"


def build_start_frame(app_id: str, language: str = "zh_cn",
                      domain: str = "iat", accent: str = "mandarin",
                      wpgs: bool = True) -> str:
    """构造首帧 JSON。wpgs=True: 开启动态修正/流式结果返回（边说边出字，可能带早期噪音）
    wpgs=False: 停顿/结束时才返回结果（更干净准确）"""
    business = {
        "language": language,   # zh_cn: 中文
        "domain": domain,       # iat: 日常用语
        "accent": accent,       # mandarin: 普通话
    }
    if wpgs:
        business["dwa"] = "wpgs"   # 流式结果返回(动态修正)
    frame = {
        "common": {"app_id": app_id},
        "business": business,
        "data": {
            "status": 0,            # 0: 首帧
            "format": "audio/L16;rate=16000",
            "encoding": "raw",
            "audio": "",
        },
    }
    return json.dumps(frame)


def build_end_frame() -> str:
    """构造结束帧 JSON（data.status=2）。"""
    frame = {"data": {"status": 2, "format": "audio/L16;rate=16000",
                      "encoding": "raw", "audio": ""}}
    return json.dumps(frame)


def parse_result(data: dict) -> str:
    """解析讯飞返回 JSON, 拼出文本。"""
    if data.get("code") != 0:
        raise RuntimeError(f"讯飞错误 code={data.get('code')} msg={data.get('message')}")
    text = ""
    result = data.get("data", {}).get("result", {})
    for ws in result.get("ws", []):
        for cw in ws.get("cw", []):
            text += cw.get("w", "")
    return text


class XfyunIAT:
    """讯飞语音听写客户端: 流式转发 PCM -> 识别文本回调。"""

    def __init__(self, app_id: str, api_key: str, api_secret: str, wpgs: bool = True):
        self.app_id = app_id
        self.api_key = api_key
        self.api_secret = api_secret
        self.wpgs = wpgs

    async def transcribe(self, audio_frames, on_text=None):
        """
        转发音频帧到讯飞, 返回最终完整文本。
        audio_frames: 可迭代的 bytes 块 (16kHz/16bit/mono PCM)
        on_text: 回调 on_text(text, is_final)
                 - 中间结果: text 恒为【从头到现在的完整句】(全量)
                 - 最终结果: is_final=True, text 也是完整句
        """
        url = build_auth_url(self.api_key, self.api_secret)
        words = []          # ★ 整句词数组(动态修正 rpl 的落点)
        sent_end = [False]  # 是否已发送结束帧(用于判断"整轮真结束")
        abnormal = [False]  # 是否异常收尾(静默超时/连接被关等 → 上层发 abroad)
        async with websockets.connect(url, max_size=None) as ws:
            # 首帧
            await ws.send(build_start_frame(self.app_id, wpgs=self.wpgs))

            # 接收循环（并行! 否则讯飞的中间结果会被堵在缓冲区, end 后才读出）
            async def recv_loop():
                while True:
                    try:
                        msg = await asyncio.wait_for(ws.recv(), timeout=30)
                    except asyncio.TimeoutError:
                        print("[IAT-DBG] 讯飞接收超时", flush=True)
                        break
                    except websockets.exceptions.ConnectionClosed as e:
                        print(f"[IAT-DBG] 讯飞连接已关闭(code={getattr(e, 'code', '?')}): {getattr(e, 'reason', '')}", flush=True)
                        break
                    except Exception as e:
                        print(f"[IAT-DBG] recv_loop 异常: {type(e).__name__}: {e}", flush=True)
                        break
                    data = json.loads(msg)
                    if data.get("code") != 0:
                        # 不再 raise（避免异常逃逸）—— 讯飞报错（如 10165 invalid handle、超时）就正常收尾
                        print(f"[IAT-DBG] 讯飞错误 code={data.get('code')} msg={data.get('message')}（收尾）", flush=True)
                        break
                    status = data.get("data", {}).get("status", -1)
                    result = data.get("data", {}).get("result", {})
                    ws_list = result.get("ws", [])
                    new_words = [cw.get("w", "")
                                 for item in ws_list
                                 for cw in item.get("cw", [])]
                    rg = result.get("rg")
                    pgs = result.get("pgs")
                    if RAW_FRAME_LOG:
                        print(f"[IAT-RAW] rg={rg} pgs={pgs} rst={result.get('rst')} ws={new_words}", flush=True)

                    # ---- 词数组维护: rpl 必须应用到句尾(原实现只覆盖 rg 范围 → 旧词残留) ----
                    if pgs == "rpl" and isinstance(rg, list) and len(rg) == 2:
                        # 讯飞语义: ws 是【从 start 到句尾的最新全量】
                        # 所以必须替换到数组末尾，否则早期 apd 的旧词会残留(反复加量 bug)
                        start = max(0, int(rg[0]) - 1)
                        words[start:] = new_words
                        print(f"[IAT-DBG] rpl 替换 words[{start}:] <- {len(new_words)}词 (rg={rg})", flush=True)
                    else:
                        # apd(追加) 或 无 pgs 的普通帧
                        words.extend(new_words)

                    sentence = "".join(words)
                    # ★ 无条件打印: 讯飞每返回一条就输出当前拼接结果(与是否下发板子无关)
                    print(f"[讯飞返回] {sentence}", flush=True)

                    if status == 2:
                        if sent_end[0]:
                            # ★ 咱已发过结束帧 → 这条 status==2 是整轮最后的响应 → 立即收尾
                            print("[IAT-DBG] 讯飞确认整轮结束，立即收尾", flush=True)
                            return
                        # 否则只是「本段识别结束」信号（长音频会分段），继续等
                        print("[IAT-DBG] 讯飞报告段结束(status=2)，继续等待后续内容", flush=True)
                        continue
                    if sentence and on_text:
                        print(f"[IAT-DBG] 全量: {sentence}", flush=True)
                        res = on_text(sentence, False)
                        if asyncio.iscoroutine(res):
                            await res

            recv_task = asyncio.create_task(recv_loop())

            # 音频帧（JSON + base64，40ms 间隔）
            async for frame in audio_frames:
                if not frame:
                    continue
                payload = {
                    "data": {
                        "status": 1,
                        "format": "audio/L16;rate=16000",
                        "encoding": "raw",
                        "audio": base64.b64encode(frame).decode("utf-8"),
                    }
                }
                try:
                    await ws.send(json.dumps(payload))
                except websockets.exceptions.ConnectionClosed as e:
                    print(f"[IAT-DBG] 讯飞连接已关闭(可能静默超时)，优雅收尾: {getattr(e, 'code', '?')}", flush=True)
                    abnormal[0] = True
                    break
                # 讯飞建议 40ms/帧；用 25ms 发送以便追上积压（缩短 end→final 延迟）
                await asyncio.sleep(0.025)

            # 结束帧（标记：此后收到的 status==2 即整轮结束）
            sent_end[0] = True
            try:
                await ws.send(build_end_frame())
            except websockets.exceptions.ConnectionClosed:
                print("[IAT-DBG] 发结束帧时连接已关(静默超时)，跳过", flush=True)

            # ★ 内容在音频阶段已全部拿到，结束帧只是通知讯飞收尾。
            #   短等 0.3s 让尾部结果到达，然后立即取消接收（不等它的 30s 超时）
            await asyncio.sleep(0.3)
            if not recv_task.done():
                recv_task.cancel()
            try:
                await recv_task
            except asyncio.CancelledError:
                pass
            except Exception as e:
                print(f"[IAT-DBG] recv_task 收尾异常(已忽略): {type(e).__name__}: {e}", flush=True)
        return "".join(words), abnormal[0]
