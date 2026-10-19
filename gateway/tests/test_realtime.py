#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""边发边收测试: 发送音频的同时实时打印收到的消息, 确认 partial 到达时机。"""
import asyncio
import json
import time

import websockets

FRAME_BYTES = 1280


async def main():
    pcm = open('test_audio.pcm', 'rb').read()
    uri = "ws://127.0.0.1:18888/iat?token=lanlink2026"
    async with websockets.connect(uri, max_size=None) as ws:
        await ws.send(json.dumps({"type": "start"}))
        t0 = time.time()

        async def recv_loop():
            while True:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=60)
                except Exception:
                    return
                dt = time.time() - t0
                data = json.loads(msg)
                print(f"[+{dt:6.2f}s] 收: {data.get('type')}: {data.get('text','')[:30]}")

        recv_task = asyncio.create_task(recv_loop())

        # 发送: 模拟板子推流 (40ms/帧)
        for i in range(0, len(pcm), FRAME_BYTES):
            await ws.send(pcm[i:i + FRAME_BYTES])
            if i % (FRAME_BYTES * 10) == 0:
                print(f"[+{time.time()-t0:6.2f}s] 发: {i//FRAME_BYTES} 帧")
            await asyncio.sleep(0.04)

        print(f"[+{time.time()-t0:6.2f}s] 全部音频发完, 发 end")
        await ws.send(json.dumps({"type": "end"}))
        await asyncio.sleep(5)
        recv_task.cancel()
        print("=== done ===")


asyncio.run(main())
