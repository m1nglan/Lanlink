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
#define WS_STALE_TIMEOUT_MS   (130000)     /*!< 超时未收到任何数据 → 判死连接, 重连。
                                            ★ 阈值须大于服务器的 120s 协议 ping
                                            (LLM 工具调用空窗可达几十秒, 不能误判) */

/* 消息处理器: 收到 TEXT 帧且 type 匹配时调用
 * payload: null 终止的完整 JSON 字符串(WS 已补 \0); len: 原始长度; ctx: set_handler 传入 */
typedef void (*ws_msg_handler_t)(const char *payload, int len, void *ctx);

/* 服务器确认服务切换完成的回调 —— 驱动不认识业务, 由业务注册 (同 set_handler 套路)。
 * service: 本地刚请求切换到的服务名。
 * ★ 回调跑在 websocket_task 且**持着 client->lock** → 里面只能做非阻塞操作。 */
typedef void (*ws_svc_ok_cb_t)(const char *service, void *ctx);

/*!
 * 统一服务网关 WebSocket 连接驱动 (单例, 一条长连接, 应用层按 type 分发)
 *
 *   ws://HOST:PORT/?token=SERVER_TOKEN;  用 {"type":"svc","service":...} 在
 *   text / llm / openclaw / usage / weather / echo 间切换。
 * ★ init / deinit / 重连 **由 ws_keeper 独占负责** (ws_keeper.cpp:69), 业务方不要自己调。
 *   业务只做三件事:
 *       WS::get().set_handler("asr", my_asr_cb, NULL);       // 注册下行类型
 *       WS::get().set_svc_ok_callback(cb, NULL);             // 切服务确认
 *       WS::get().send_text("{\"type\":\"start\"}", 1000);   // 上行
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

    /*! 注册消息处理器: 收到 TEXT 帧且 JSON 里 type == type_key 时调用 cb(payload,len,ctx)
     *  ★ V2: 所有下行类型都走这一张表 —— asr / final / partial / reply / error ...
     *    (V1 因 partial 被 text 和 llm 共用而另有一套"按 m_service 分流"的
     *     set_partial_handler, V2 里一维 type 就能区分 → 已删除。) */
    void set_handler(const char *type_key, ws_msg_handler_t cb, void *ctx);

    /*! 注册 svc_ok 回调 —— 收到服务器"服务切换完成"确认时调用。
     *  voice_session 要靠它才知道"服务器真的切好了", 才敢发 start。 */
    void set_svc_ok_callback(ws_svc_ok_cb_t cb, void *ctx);

    /*! 记录当前服务 (V2 里仅用于诊断/未来扩展, 不再参与消息路由) */
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

    /* svc_ok 回调: 服务器确认切换完成 → 通知在等它的人 (voice_session) */
    ws_svc_ok_cb_t m_svc_ok_cb  = NULL;
    void          *m_svc_ok_ctx = NULL;

    esp_websocket_client_handle_t m_ws = NULL;
    volatile bool m_connected = false;
    uint32_t m_last_rx_ms = 0;   /*!< 最后收到数据的时间(ms,esp_timer) */
    const char *m_service = "text";  /*!< 当前服务(text/llm/openclaw/echo) */

    /* 接收累积缓冲(处理 websocket 粘包/分帧): 段属性 EXT_RAM_BSS_ATTR 只允许
     * 静态存储期变量, 故声明为 static 成员。定义在 ws.cpp(WS 是单例, 仅一份)。 */
    static const int RX_BUF_SIZE = 8192;
    static char m_rx_buf[RX_BUF_SIZE];
    int m_rx_len = 0;
};
