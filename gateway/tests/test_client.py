#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_client.py — 网关测试客户端（V0.2 长连接协议版）
用法: python3 test_client.py <音频文件.wav|.pcm> [--host HOST] [--port PORT] [--token TOKEN]

流程: 连接 -> start -> 音频帧 -> end -> 收 final -> 可再来一轮
"""

import argparse
import asyncio
import json
import wave

import websockets

FRAME_BYTES = 1280  # 40ms @16kHz/16bit/mono


def load_pcm(path: str) -> bytes:
    if path.endswith(".wav"):
        with wave.open(path, "rb") as w:
            assert w.getframerate() == 16000, "需要 16kHz"
            assert w.getsampwidth() == 2, "需要 16bit"
            assert w.getnchannels() == 1, "需要单声道"
            return w.readframes(w.getnframes())
    with open(path, "rb") as f:
        return f.read()


async def run_one_round(ws, pcm: bytes, label: str):
    """一轮: start -> 音频 -> end -> 等 final"""
    await ws.send(json.dumps({"type": "start"}))
    for i in range(0, len(pcm), FRAME_BYTES):
        await ws.send(pcm[i:i + FRAME_BYTES])
        await asyncio.sleep(0.04)   # 40ms 帧间隔
    await ws.send(json.dumps({"type": "end"}))

    print(f"[{label}] 识别结果:")
    while True:
        msg = await asyncio.wait_for(ws.recv(), timeout=30)
        data = json.loads(msg)
        t = data.get("type")
        if t == "partial":
            print(f"  [中间] {data.get('text','')}")
        elif t == "final":
            print(f"  ✅ [最终] {data.get('text','')}")
            return
        elif t == "error":
            print(f"  ❌ [错误] {data.get('message','')}")
            return


async def run(uri: str, pcm: bytes, token: str):
    async with websockets.connect(f"{uri}/iat?token={token}", max_size=None) as ws:
        # 心跳确认连接
        await ws.send(json.dumps({"type": "ping"}))
        pong = json.loads(await ws.recv())
        print("连接正常:", pong.get("type"))

        # 一轮
        await run_one_round(ws, pcm, "第1轮")

        # 第二轮（连接保持验证）
        await run_one_round(ws, pcm, "第2轮")

        # 连接仍存活
        await ws.send(json.dumps({"type": "ping"}))
        print("连接仍存活:", json.loads(await ws.recv()).get("type"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio", help="音频文件 (wav/pcm, 16kHz/16bit/mono)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", default=18888)
    ap.add_argument("--token", default="lanlink2026")
    args = ap.parse_args()

    pcm = load_pcm(args.audio)
    print(f"音频: {args.audio} ({len(pcm)} bytes, {len(pcm)/32000:.1f}s)")
    uri = f"ws://{args.host}:{args.port}"
    asyncio.run(run(uri, pcm, args.token))


if __name__ == "__main__":
    main()
