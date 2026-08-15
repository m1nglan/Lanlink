# LanLink 统一服务网关 接口文档（V1.4）

> 服务器: 39.104.84.177 · 端口: 18888 · 2026-08-15 更新
> 一条长连接 + 应用层切换: text(语音听写) / llm(大模型) / openclaw(明岚) / echo(调试)

---

## 1. 统一入口（固定一个URL）

```
ws://39.104.84.177:18888/?token=***
```

切换服务：
```json
{"type":"svc","service":"text"}       → {"type":"svc_ok","service":"text"}
{"type":"svc","service":"llm"}        → {"type":"svc_ok","service":"llm"}
{"type":"svc","service":"openclaw"}   → {"type":"svc_ok","service":"openclaw"}
{"type":"svc","service":"echo"}       → {"type":"svc_ok","service":"echo"}
```

- 切换自动清理上一服务状态；默认服务 text
- 心跳: `{"type":"ping"}` → `{"type":"pong"}`（任何模式有效）

---

## 2. type=text 语音听写

```
板子 → {"type":"start"}             开始（触发器）
板子 → [PCM二进制帧] 任意段数       音频（16kHz/16bit/mono, 1280B/帧≈40ms）
网关 → {"type":"partial","text":"新字"}   增量（只发新增的字, 板子 buffer += text）
板子 → {"type":"end"}               结束（触发器）
网关 → {"type":"final","text":"完整"}     最终（整体替换, 修正所有错字）
```

- start 前发音频会被丢弃；单轮最长55秒
- **增量规则**：partial 只含新增后缀；讯飞修正时服务器不发中间消息，**final 统一修正**
- **final = 本轮完成**

---

## 3. type=llm / type=openclaw 对话

```
板子 → {"type":"chat","content":"记作业：数学第3页"}
网关 → {"type":"partial","text":"新字"}   流式增量（多条, 追加显示）
网关 → {"type":"reply","content":"完整"}   完整回复（★ 最后一条 = 本轮完成）
板子 → {"type":"clear"}              清空上下文（llm 生效）
```

- **llm**: DeepSeek（deepseek-chat），网关维护最近20条消息+system提示词
- **openclaw**: 转发明岚（openclaw/default），SSE 流式，**接入 main 会话（主子微信可见）**
- **工具调用**：明岚调工具期间无输出（partial 空窗，可能几秒~几十秒），工具完继续流式——**板子无需感知，等 reply 即可**
- **reply 永远是最后一条**，收到即本轮完成，用完整内容覆盖刷新显示

---

## 4. type=echo 调试

接受什么回什么：文本帧回 `{"type":"echo","data":"..."}`，二进制帧原样回显。

---

## 5. 错误

```json
{"type":"error","code":1,"message":"讯飞错误"}
{"type":"error","code":2,"message":"llm/openclaw 调用失败"}
{"type":"error","code":3,"message":"音频超过55s"}
{"type":"error","code":4,"message":"未知服务"}
```

---

## 6. 连接保活

- 服务器 120s 协议层 ping 检测死连接（正常板子自动回 pong，不会误踢）
- 板子建议 20~40s 发一次 `{"type":"ping"}` 应用层保活
- 板子断电/断网：服务器 60s 内判定死亡并清理

---

## 7. 部署

| 项 | 值 |
|---|---|
| 代码 | `/home/m1nglan/lanlink-gateway/` |
| 密钥 | `config.json`（chmod 600, 勿提交 git） |
| 启动 | `bash start.sh` |
| 日志 | `gateway.log` |
| 板端示例 | `board_example/`（gateway_client + handle_text） |

## 8. 消息一览

| 方向 | type | 含义 | 处理 |
|---|---|---|---|
| 上行 | svc | 切换服务 | - |
| 上行 | start/end | 语音轮次触发 | - |
| 上行 | chat/clear | 对话/清空 | - |
| 上行 | ping | 心跳 | - |
| 下行 | svc_ok | 切换确认 | - |
| 下行 | partial | 增量字(text)/流式字(对话) | 追加显示 |
| 下行 | final | 语音最终文本 | 整体替换 = 完成 |
| 下行 | reply | 对话完整回复 | 整体替换 = 完成 |
| 下行 | pong | 心跳应答 | - |
| 下行 | error | 错误 | 处理提示 |
