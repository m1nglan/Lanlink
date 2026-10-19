#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""echo 调试模式测试"""
import asyncio
import json
import websockets


async def main():
    async with websockets.connect('ws://127.0.0.1:18888/?token=lanlink2026', max_size=None) as ws:
        await ws.send(json.dumps({"type": "svc", "service": "echo"}))
        print("svc:", json.loads(await ws.recv()))
        await ws.send(json.dumps({"type": "test", "hello": "world"}))
        print("文本回显:", await ws.recv())
        await ws.send(b"ABC123")
        r = await ws.recv()
        print("二进制回显:", r if isinstance(r, bytes) else str(r))
        print("=== echo 测试完成 ===")


asyncio.run(main())
