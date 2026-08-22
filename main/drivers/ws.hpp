#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_attr.h"
#include "esp_websocket_client.h"

/* ============ 统一服务网关(长连接) 参数 ============ */
#define WS_HOST               "39.104.84.177"
#define WS_PORT               (18888)
#define WS_PATH               "/"
#define WS_CONNECT_TIMEOUT_MS (15000)      /*!< 连接超时 */
#define WS_SEND_TIMEOUT_MS    (1000)       /*!< 发送超时 */
#define WS_PING_INTERVAL_SEC  (20)         /*!< 板子主动 PING 保活间隔 */
#define WS_STALE_TIMEOUT_MS   (130000)     /*!< 超过该时长未收到任何数据→判定死连接,重连。
                                            OpenClaw 工具调用空窗可达几十秒,阈值须大于服务器120s ping,
                                            避免 LLM 空窗期误判断连。 */

/* 消息处理器: 收到 TEXT 帧且 type 匹配时调用
 * payload: null 终止的完整 JSON 字符串(WS 已补 \0); len: 原始长度; ctx: set_handler 传入 */
typedef void (*ws_msg_handler_t)(const char *payload, int len, void *ctx);

/*!
 * 统一服务网关 WebSocket 连接驱动(单例,一条长连接,应用层按 type 分发)
 *
 * 服务器: ws://HOST:PORT/?token=SERVER_TOKEN
 * 消息通过 {"type":"svc","service":"..."} 在 text/llm/openclaw 间切换。
 *
 * 使用示例:
 *   WS::get().init();
 *   WS::get().set_handler("partial", my_partial_cb, NULL);
 *   WS::get().send_text("{\"type\":\"start\"}", 1000);
 */
class WS {
public:
    /* 单例访问 */
    static WS& get(void);

    /*! 创建+注册事件+启动+等待握手 */
    esp_err_t init(void);
    /*! 断开并释放 */
    void deinit(void);
    bool is_connected(void) const { return m_connected; }

    /*! 连接是否"死"(超过 WS_STALE_TIMEOUT_MS 未收到任何数据)。未连接也返回 true。 */
    bool is_stale(void) const;

    /*! 注册消息处理器: 收到 TEXT 帧且 JSON 里 type == type_key 时调用 cb(payload,len,ctx) */
    void set_handler(const char *type_key, ws_msg_handler_t cb, void *ctx);

    /*! 注册 partial 处理器(按服务区分): "text"=语音增量, "llm"=对话流式 */
    void set_partial_handler(const char *service, ws_msg_handler_t cb, void *ctx);

    /*! 记录当前服务(用于区分 partial 属于语音还是对话流式) */
    void set_service(const char *service) { m_service = service; }
    const char *get_service(void) const { return m_service; }

    /*! 发送文本帧 */
    esp_err_t send_text(const char *text, uint32_t timeout_ms);
    /*! 发送二进制帧 */
    esp_err_t send_bin(const uint8_t *data, size_t len, uint32_t timeout_ms);
    /*! 发送应用层保活 ping {"type":"ping"} */
    esp_err_t send_ping(uint32_t timeout_ms);

private:
    WS() = default;
    ~WS() = default;
    WS(const WS&) = delete;
    WS& operator=(const WS&) = delete;

    /* 事件回调(静态) */
    static void ws_event_handler(void *handler_args, esp_event_base_t base,
                                 int32_t event_id, void *event_data);
    void handle_data(const char *payload, int len);   /* 累积缓冲 + 切分完整 JSON */
    void dispatch_msg(const char *msg, int len);      /* 解析单条 JSON 并按 type 分发 */

    /* 处理器表: type_key 精确匹配 */
    static const int MAX_HANDLERS = 4;
    const char *m_type_keys[MAX_HANDLERS] = {};
    ws_msg_handler_t m_handlers[MAX_HANDLERS] = {};
    void *m_ctxs[MAX_HANDLERS] = {};
    int m_handler_count = 0;

    /* partial 处理器(按服务): "text"→RtAsr, "llm"/"openclaw"→Llm */
    ws_msg_handler_t m_partial_text_cb = NULL;
    void *m_partial_text_ctx = NULL;
    ws_msg_handler_t m_partial_chat_cb = NULL;
    void *m_partial_chat_ctx = NULL;

    esp_websocket_client_handle_t m_ws = NULL;
    volatile bool m_connected = false;
    uint32_t m_last_rx_ms = 0;   /*!< 最后收到数据的时间(ms,esp_timer) */
    const char *m_service = "text";  /*!< 当前服务(text/llm/openclaw/echo) */

    /* 接收累积缓冲: 处理 websocket 粘包/分帧。
     * 段属性(EXT_RAM_BSS_ATTR)只允许静态存储期变量,故声明为 static 成员,
     * 存储定义在 ws.cpp(WS 是单例,仅一份)。8KB 防粘包数据超长溢出。 */
    static const int RX_BUF_SIZE = 8192;
    static char m_rx_buf[RX_BUF_SIZE];
    int m_rx_len = 0;
};
