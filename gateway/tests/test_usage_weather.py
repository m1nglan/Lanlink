#!/usr/bin/env python3
"""严谨版测试: usage/weather 合并进网关后的全链路"""
import asyncio, json, websockets

async def recv_n(ws, n, timeout=4):
    """收 n 条消息, 每条带序号"""
    out = []
    for i in range(n):
        try:
            m = await asyncio.wait_for(ws.recv(), timeout)
            out.append(json.loads(m))
        except asyncio.TimeoutError:
            out.append(None)
    return out

async def t():
    async with websockets.connect('ws://127.0.0.1:18888/?token=lanlink2026') as ws:
        # 1. 切换 usage
        await ws.send(json.dumps({"type": "svc", "service": "usage"}))
        msgs = await recv_n(ws, 2)   # svc_ok + 第一条 usage
        print("[usage切换]", [m.get("type") if m else None for m in msgs])
        assert msgs[0]["type"] == "svc_ok" and msgs[0]["service"] == "usage"
        assert msgs[1]["type"] == "usage" and msgs[1]["usage"] is True
        u1 = msgs[1]
        # 2. 等 2 秒收第二条, 验证 2s 一跳
        msgs = await recv_n(ws, 1, timeout=5)
        u2 = msgs[0]
        print(f"[usage连续] t1={u1['t']} t2={u2['t']} 间隔={u2['t']-u1['t']}s")
        assert u2["type"] == "usage" and u2["t"] - u1["t"] >= 1
        print(f"[usage字段] cpu={u1['cpu']} ram={u1['ram_used']} {u1['ram_pct']}% "
              f"disk={u1['disk_used']} {u1['disk_pct']}% rx={u1['rx']} tx={u1['tx']}")

        # 3. 切换 weather (自动推一次)
        await ws.send(json.dumps({"type": "svc", "service": "weather"}))
        msgs = await recv_n(ws, 2, timeout=6)   # svc_ok + weather(自动)
        print("[weather切换]", [m.get("type") if m else None for m in msgs])
        assert msgs[0]["type"] == "svc_ok" and msgs[0]["service"] == "weather"
        assert msgs[1]["type"] == "weather" and msgs[1]["weather"] is True
        w = msgs[1]
        print(f"[weather自动] {w['text']} {w['temp']}℃ 最高{w['temp_max']} 最低{w['temp_min']} kind={w['kind']}")
        assert w["kind"] in range(1, 9)

        # 4. get 主动刷新
        await ws.send(json.dumps({"type": "get"}))
        msgs = await recv_n(ws, 1, timeout=6)
        w2 = msgs[0]
        print(f"[weather get] {w2['text']} {w2['temp']}℃ kind={w2['kind']}")
        assert w2["type"] == "weather"

        # 5. 心跳
        await ws.send(json.dumps({"type": "ping"}))
        msgs = await recv_n(ws, 1, timeout=4)
        print("[ping]", msgs[0])
        assert msgs[0]["type"] == "pong"

        # 6. 切回 text
        await ws.send(json.dumps({"type": "svc", "service": "text"}))
        msgs = await recv_n(ws, 1, timeout=4)
        print("[切回text]", msgs[0])
        assert msgs[0]["type"] == "svc_ok" and msgs[0]["service"] == "text"

        print("\n=== 全部测试通过 ===")

asyncio.run(t())
