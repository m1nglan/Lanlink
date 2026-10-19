# LanLink Gateway

LanLink 的统一服务网关：**一条 WebSocket 长连接 + 应用层服务切换**，为 ESP32 板端提供语音听写、大模型对话、服务器监控、天气等能力。

## 特性

- **单连接多服务**：连一次，用 `{"type":"svc","service":"text"}` 切换，不用重连
- **全流式**：语音识别（讯飞 IAT）、LLM 回复（DeepSeek）、明岚转发（OpenClaw）都是流式输出
- **协议自包含**：`API.md` 有完整消息定义
- **轻量**：纯 Python 标准库 + `websockets`，无框架依赖

## 服务列表

| service | 说明 |
| --- | --- |
| `text` | 语音听写（讯飞 IAT，动态修正 + 全量句输出）|
| `llm` | DeepSeek 对话（带上下文）|
| `openclaw` | 转发给 OpenClaw（明岚本人）|
| `usage` | 服务器监控（CPU / 内存 / 磁盘 / 网速，2s 一跳）|
| `weather` | 和风天气（当前 + 预报）|
| `balance` | DeepSeek 账户余额（按需查询，不定时推）|
| `echo` | 调试回显 |

## 快速开始

```bash
# 1. 依赖
pip3 install websockets

# 2. 配置（复制示例，填自己的 key）
cp config.example.json config.json
#    openclaw.token      OpenClaw Gateway 的 token
#    llm.api_key         DeepSeek API Key
#    qweather.host/key   和风天气专属 API host 和 key

# 3. 启动
./start.sh
#    或：python3 server.py

# 4. 验证
python3 tests/test_services.py
```

## 连接方式

```
ws://<server-ip>:18888/?token=<your-token>
```

token 在 `config.json` 的 `token` 字段中配置。

## 目录结构

```
gateway/
├── server.py             # 主程序：连接管理 + 各服务实现
├── xfyun_iat.py          # 讯飞语音听写客户端（流式 + 动态修正）
├── API.md                # 完整协议文档
├── config.example.json   # 配置模板（复制成 config.json 填 key）
├── start.sh              # 启动脚本
├── board_example/        # 板端 C++ 客户端示例
└── tests/                # 各服务的连通性测试脚本
```

## 协议要点

完整定义见 [`API.md`](./API.md)，几个关键点：

- **心跳**：`{"type":"ping"}` → `{"type":"pong"}`
- **语音识别**：`start` → 发 PCM 帧 → `end`；中间持续收到 `{"type":"asr","text":"全量句"}`，结束后收 `final`
- **异常收尾**：静音超时等情况下会发 `{"type":"abroad","text": null 或 文本}`，与正常 `final` 区分
- **用量/天气/余额**：切到对应服务时主动推一次；之后可发 `{"type":"get"}` 主动刷新

## License

个人项目，仅供学习参考。
