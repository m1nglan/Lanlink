#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统一网关测试: type=llm(上下文) + type=openclaw(转发)。"""
import asyncio
import json

import websockets

URI = "ws://127.0.0.1:18888/?token=lanlink2026&type="


async def test_llm():
    print("=== type=llm (DeepSeek, 上下文测试) ===")
    async with websockets.connect(URI + "llm", max_size=None) as ws:
        await ws.send(json.dumps({"type": "chat", "content": "我叫小明，记住这个名字"}))
        r1 = json.loads(await ws.recv())
        print("R1:", r1.get("content", "")[:80])
        await ws.send(json.dumps({"type": "chat", "content": "我叫什么？"}))
        r2 = json.loads(await ws.recv())
        print("R2:", r2.get("content", "")[:80])
        await ws.send(json.dumps({"type": "clear"}))
        r3 = json.loads(await ws.recv())
        print("R3(clear):", r3.get("content", "")[:40])
        await ws.send(json.dumps({"type": "chat", "content": "我叫什么？"}))
        r4 = json.loads(await ws.recv())
        print("R4(清空后):", r4.get("content", "")[:80])


async def test_openclaw():
    print("=== type=openclaw (明岚转发) ===")
    async with websockets.connect(URI + "openclaw", max_size=None) as ws:
        await ws.send(json.dumps({"type": "chat", "content": "在吗？一句话"}))
        r = json.loads(await asyncio.wait_for(ws.recv(), timeout=120))
        print("R:", r.get("content", "")[:100])


async def main():
    await test_llm()
    await test_openclaw()
    print("=== 全部完成 ===")


asyncio.run(main())
