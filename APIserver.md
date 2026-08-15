# LanLink 统一服务网关 接口文档（V1.1 应用层切换）

> 服务器: 39.104.84.177 · 端口: 18888 · 2026-08-15 更新
> **一条长连接 + 应用层切换**：板子只需一个固定 URL

---

## 1. 统一入口（固定一个URL）

```
ws://39.104.84.177:18888/?token=***
```

连接建立后，用切换消息在三个服务间切换：

```json
{"type":"svc","service":"text"}       → {"type":"svc_ok","service":"text"}
{"type":"svc","service":"llm"}        → {"type":"svc_ok","service":"llm"}
{"type":"svc","service":"openclaw"}   → {"type":"svc_ok","service":"openclaw"}
```

| service | 功能 |
|---|---|
| `text` | 语音听写（讯飞, 流式） |
| `llm` | 大模型对话（DeepSeek, 网关维护上下文） |
| `openclaw` | 明岚对话（转发 OpenClaw, 接 main 会话） |

切换时自动清理上一服务状态（语音会话/对话历史）。默认服务 = text。

---

## 2. type=text 语音听写

```
板子 → {"type":"start"}           开始（触发器）
板子 → [PCM二进制帧] 任意段数      音频（1280B/帧=40ms）
网关 → {"type":"partial","text"}  流式中间结果
板子 → {"type":"end"}             结束（触发器）
网关 → {"type":"final","text"}    最终结果
```

- start 前发音频会被丢弃；单轮最长55秒；final后可立即再 start

## 3. type=llm / type=openclaw 对话

```
板子 → {"type":"chat","content":"记作业：数学第3页"}
网关 → {"type":"reply","content":"好的，已记下"}
板子 → {"type":"clear"}           清空上下文（llm）
```

- llm: DeepSeek，网关维护最近20条消息+system提示词
- openclaw: 转发明岚（openclaw/default），会话=main（主子微信可见）

## 4. 通用

- 心跳: `{"type":"ping"}` → `{"type":"pong"}`
- 错误: `{"type":"error","code":N,"message":"..."}`（1=讯飞 2=llm/openclaw 3=超55s 4=未知服务）

---

## 5. 部署

| 项 | 值 |
|---|---|
| 代码 | `/home/m1nglan/lanlink-gateway/` |
| 密钥 | `config.json`（chmod 600, **勿提交 git**） |
| 启动 | `bash start.sh` |
| 测试 | `test_switch.py`（一条连接三服务切换） |

## 6. 测试记录（2026-08-15）

一条连接: text识别 → 切llm对话(上下文记忆) → 切回text再识别 ✅
