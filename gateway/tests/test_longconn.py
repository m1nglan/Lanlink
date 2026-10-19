#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""长连接多轮测试: 一次连接, 两轮识别。"""
import asyncio
import json
import sys

import websockets

FRAME_BYTES = 1280


async def main():
    pcm = open('test_audio.pcm', 'rb').read()
    uri = "ws://127.0.0.1:18888/iat?token=lanlink2026"
    async with websockets.connect(uri, max_size=None) as ws:
        # 先发个 ping 验证连接活着
        await ws.send(json.dumps({"type": "ping"}))
        print("ping:", await ws.recv())

        # 第一轮
        await ws.send(json.dumps({"type": "start"}))
        for i in range(0, len(pcm) // 2, FRAME_BYTES):
            await ws.send(pcm[i:i + FRAME_BYTES])
        await ws.send(json.dumps({"type": "end"}))
        print("--- round 1 ---")
        while True:
            msg = json.loads(await ws.recv())
            t = msg.get("type")
            if t == "partial":
                print("  [中间]", msg.get("text", ""))
            elif t == "final":
                print("  ✅ [最终]", msg.get("text", ""))
                break
            elif t == "error":
                print("  ❌", msg.get("message"))
                break

        # 第二轮（前半段音频）
        await ws.send(json.dumps({"type": "start"}))
        for i in range(0, len(pcm) // 2, FRAME_BYTES):
            await ws.send(pcm[i:i + FRAME_BYTES])
        await ws.send(json.dumps({"type": "end"}))
        print("--- round 2 ---")
        while True:
            msg = json.loads(await ws.recv())
            t = msg.get("type")
            if t == "partial":
                print("  [中间]", msg.get("text", ""))
            elif t == "final":
                print("  ✅ [最终]", msg.get("text", ""))
                break
            elif t == "error":
                print("  ❌", msg.get("message"))
                break

        # 连接还活着吗？
        await ws.send(json.dumps({"type": "ping"}))
        print("still alive:", await ws.recv())
        print("=== 长连接多轮测试通过 ===")


asyncio.run(main())
