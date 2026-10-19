/* =====================================================================
 * 网关协议版 handle_text —— 替换旧版 RTASR 解析
 * 下行格式:
 *   {"type":"partial","text":"新字"}      增量, 追加到 buffer
 *   {"type":"revise","text":"完整文本"}   修正, 整体替换中间显示
 *   {"type":"final","text":"完整文本"}    最终, 整体替换
 *   {"type":"svc_ok",...} / {"type":"pong"} / {"type":"error",...}
 * ===================================================================== */

void RtAsr::handle_text(const char *payload, int len)
{
    (void)len;

    /* 提取 "type":"xxx" */
    const char *type = NULL;
    const char *p_type = strstr(payload, "\"type\":\"");
    if (p_type != NULL) {
        type = p_type + 8;  /* 跳到值 */
    }

    /* 提取 "text":"xxx" 的值(处理 \\u 转义和 \" 转义) */
    char text[1024] = {0};
    size_t o = 0;
    const char *p = strstr(payload, "\"text\":\"");
    if (p != NULL) {
        p += 8;
        while (*p && o + 1 < sizeof(text)) {
            if (*p == '\\' && p[1] == '"') break;       /* 转义引号 = 值结束 */
            if (*p == '\\' && p[1] == '\\') { text[o++] = '\\'; p += 2; continue; }
            if (*p == '"') break;                        /* 普通引号 = 值结束 */
            text[o++] = *p++;
        }
        text[o] = '\0';
    }

    if (type == NULL) {
        ESP_LOGI(TAG, "收到未知消息: %.100s", payload);
        return;
    }

    /* 用 type 前几个字符快速判断 */
    if (strncmp(type, "partial", 7) == 0) {
        if (text[0] != '\0') {
            ESP_LOGI(TAG, "[中间+] %s", text);
            if (m_cb != NULL) m_cb(text, false, m_cb_ctx);   /* is_final=false */
        }
    } else if (strncmp(type, "revise", 6) == 0) {
        ESP_LOGI(TAG, "[修正] %s", text);
        if (m_cb != NULL) m_cb(text, false, m_cb_ctx);       /* 中间态, 板子整体替换 */
    } else if (strncmp(type, "final", 5) == 0) {
        ESP_LOGI(TAG, "[最终] %s", text);
        if (m_cb != NULL) m_cb(text, true, m_cb_ctx);        /* is_final=true */
    } else if (strncmp(type, "svc_ok", 6) == 0) {
        ESP_LOGI(TAG, "服务切换成功: %s", text);
    } else if (strncmp(type, "pong", 4) == 0) {
        ESP_LOGD(TAG, "pong");
    } else if (strncmp(type, "error", 5) == 0) {
        ESP_LOGE(TAG, "网关错误: %s", text);
    } else {
        ESP_LOGI(TAG, "其他消息: %.100s", payload);
    }
}
