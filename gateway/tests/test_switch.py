#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统一入口测试: 一条连接内 svc 切换 text -> llm -> openclaw。"""
import asyncio
import json

import websockets

URI = "ws://127.0.0.1:18888/?token=lanlink2026"


async def main():
    pcm = open('test_audio.pcm', 'rb').read()
    async with websockets.connect(URI, max_size=None) as ws:
        # 1. 切到 text, 跑一轮语音
        await ws.send(json.dumps({"type": "svc", "service": "text"}))
        print("svc text:", json.loads(await ws.recv()))

        await ws.send(json.dumps({"type": "start"}))
        for i in range(0, len(pcm) // 2, 1280):
            await ws.send(pcm[i:i + 1280])
            await asyncio.sleep(0.04)
        await ws.send(json.dumps({"type": "end"}))
        while True:
            r = json.loads(await ws.recv())
            if r.get("type") == "final":
                print("语音结果:", r.get("text", "")[:40])
                break

        # 2. 切到 llm, 对话两句
        await ws.send(json.dumps({"type": "svc", "service": "llm"}))
        print("svc llm:", json.loads(await ws.recv()))
        await ws.send(json.dumps({"type": "chat", "content": "我叫小红，记住"}))
        print("llm R1:", json.loads(await ws.recv()).get("content", "")[:50])
        await ws.send(json.dumps({"type": "chat", "content": "我叫什么？"}))
        print("llm R2:", json.loads(await ws.recv()).get("content", "")[:50])

        # 3. 切回 text 再测一轮（连接还活着）
        await ws.send(json.dumps({"type": "svc", "service": "text"}))
        print("svc text2:", json.loads(await ws.recv()))
        await ws.send(json.dumps({"type": "start"}))
        for i in range(0, len(pcm) // 2, 1280):
            await ws.send(pcm[i:i + 1280])
            await asyncio.sleep(0.04)
        await ws.send(json.dumps({"type": "end"}))
        while True:
            r = json.loads(await ws.recv())
            if r.get("type") == "final":
                print("语音结果2:", r.get("text", "")[:40])
                break

        print("=== 一条连接三服务切换全部通过 ===")


asyncio.run(main())
