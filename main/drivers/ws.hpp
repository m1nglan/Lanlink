#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_websocket_client.h"

/* ============ 统一服务网关(长连接) 参数 ============ */
#define WS_HOST               "39.104.84.177"
#define WS_PORT               (18888)
#define WS_PATH               "/"
#define WS_CONNECT_TIMEOUT_MS (15000)      /*!< 连接超时 */
#define WS_SEND_TIMEOUT_MS    (1000)       /*!< 发送超时 */
#define WS_PING_INTERVAL_SEC  (20)         /*!< 板子主动 PING 保活间隔 */
#define WS_STALE_TIMEOUT_MS   (45000)      /*!< 超过该时长未收到任何数据→判定死连接,重连 */

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

    esp_websocket_client_handle_t m_ws = NULL;
    volatile bool m_connected = false;
    uint32_t m_last_rx_ms = 0;   /*!< 最后收到数据的时间(ms,esp_timer) */

    /* 接收累积缓冲: 处理 websocket 粘包/分帧 */
    static const int RX_BUF_SIZE = 4096;
    char m_rx_buf[RX_BUF_SIZE];
    int m_rx_len = 0;
};
