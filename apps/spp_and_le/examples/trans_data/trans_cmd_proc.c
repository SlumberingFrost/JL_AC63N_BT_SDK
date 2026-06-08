#include "system/includes.h"
#include "app_config.h"
#include "ble_trans.h"
#include "trans_cmd_uart.h"
#include "trans_cmd_proc.h"

#if CONFIG_APP_SPP_LE

#define LOG_TAG_CONST       TRANS_CMD_UART
#define LOG_TAG             "[TRANS_CMD_UART]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"

static uint8_t trans_cmd_proc_inited;

typedef struct {
    char token_text[64];
    char key_text[24];
    char value_text[40];
} trans_cmd_uuid_parse_scratch_t;

/*
 * 串口命令按行串行处理，这里把 UUID 整包解析临时缓冲挪到静态区，
 * 避免每次命令分发都在栈上压一组大数组，直接拉高链接期 stack warning。
 */
static trans_cmd_uuid_parse_scratch_t trans_cmd_uuid_parse_scratch;
static char trans_cmd_line_scratch[256];

#define TRANS_CMD_SET_UUID_TRACE_BANNER   "TRACE:SETUUID:PROBE_V2\r\n"

typedef struct {
    uint16_t school_id;
    uint16_t device_id;
    uint8_t has_school_id;
    uint8_t has_device_id;
} trans_cmd_set_params_t;

typedef struct {
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    uint16_t school_id;
    uint16_t device_id;
    int8_t tx_power;
    uint8_t has_tx;
    uint8_t has_school_id;
    uint8_t has_device_id;
} trans_cmd_set_uuid_t;

static const char *trans_cmd_skip_line_prefix(const char *text);
static int trans_cmd_handle_uuid_set(const char *line);
static int trans_cmd_handle_json_set_uuid(const char *line);

static char trans_cmd_ascii_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        c -= ('a' - 'A');
    }
    return c;
}

static int trans_cmd_hex_char_to_val(char c)
{
    c = trans_cmd_ascii_upper(c);
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int trans_cmd_parse_hex_bytes(const char *text, uint8_t *out, uint8_t out_len)
{
    uint8_t i;

    if (!text || !out) {
        return -1;
    }

    for (i = 0; i < out_len; i++) {
        int high = trans_cmd_hex_char_to_val(text[i * 2]);
        int low = trans_cmd_hex_char_to_val(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return -1;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }

    if (text[out_len * 2] != '\0') {
        return -1;
    }
    return 0;
}

static int trans_cmd_parse_u16_hex_exact(const char *text, uint8_t digits, uint16_t *value)
{
    uint16_t parsed = 0;
    uint8_t i;

    if (!text || !value || !digits || digits > 4) {
        return -1;
    }

    for (i = 0; i < digits; i++) {
        int n = trans_cmd_hex_char_to_val(text[i]);
        if (n < 0) {
            return -1;
        }
        parsed = (uint16_t)((parsed << 4) | (uint16_t)n);
    }

    if (text[digits] != '\0') {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int trans_cmd_parse_i8_hex_exact(const char *text, uint8_t digits, int8_t *value)
{
    uint16_t tmp = 0;

    if (!value || trans_cmd_parse_u16_hex_exact(text, digits, &tmp)) {
        return -1;
    }
    if (tmp > 0xFF) {
        return -1;
    }
    *value = (int8_t)(uint8_t)tmp;
    return 0;
}

static int trans_cmd_parse_u16_dec(const char *text, uint16_t *value)
{
    uint32_t parsed = 0;
    const char *p = text;
    const char *end;

    if (!p || !value || !*p) {
        return -1;
    }

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (!*p) {
        return -1;
    }

    end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    while (p < end) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        parsed = parsed * 10u + (uint32_t)(*p - '0');
        if (parsed > 0xFFFFu) {
            return -1;
        }
        p++;
    }

    *value = (uint16_t)parsed;
    return 0;
}

static int trans_cmd_parse_u16_auto(const char *text, uint16_t *value)
{
    const char *p = text;
    uint16_t parsed = 0;
    uint8_t hex_mode = 0;
    uint8_t has_alpha = 0;
    uint8_t digit_cnt = 0;
    const char *end;

    if (!p || !value || !*p) {
        return -1;
    }

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (!*p) {
        return -1;
    }

    end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        hex_mode = 1;
        p += 2;
    } else {
        const char *scan = p;
        while (scan < end) {
            if (((*scan >= 'a' && *scan <= 'f') || (*scan >= 'A' && *scan <= 'F'))) {
                has_alpha = 1;
                break;
            }
            scan++;
        }
        hex_mode = has_alpha ? 1 : 0;
    }

    if (!hex_mode) {
        return trans_cmd_parse_u16_dec(p, value);
    }

    while (p < end) {
        int n = trans_cmd_hex_char_to_val(*p);
        if (n < 0) {
            return -1;
        }
        if (digit_cnt >= 4) {
            return -1;
        }
        parsed = (uint16_t)((parsed << 4) | (uint16_t)n);
        digit_cnt++;
        p++;
    }
    if (!digit_cnt) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int trans_cmd_send_bytes_checked(const uint8_t *buf, uint16_t len)
{
    if (!buf || !len) {
        return -1;
    }
    return trans_cmd_uart_send_bytes(buf, len);
}

static int trans_cmd_send_literal(const char *text)
{
    if (!text) {
        return -1;
    }
    return trans_cmd_uart_send_text(text);
}

static void trans_cmd_set_uuid_trace(const char *stage)
{
    if (!stage || !stage[0]) {
        return;
    }
    (void)trans_cmd_uart_send_text("TRACE:SETUUID:");
    (void)trans_cmd_uart_send_text(stage);
    (void)trans_cmd_uart_send_text("\r\n");
}

static char *trans_cmd_find_cmd_anchor(char *text)
{
    char *p;

    if (!text || !text[0]) {
        return text;
    }
    if (!strncmp(text, "CMD:", 4) || text[0] == '{') {
        return text;
    }

    p = strstr(text, "CMD:");
    if (p) {
        return p;
    }
    p = strstr(text, "{\"cmd\"");
    if (p) {
        return p;
    }
    p = strstr(text, "{\\\"cmd\\\"");
    if (p) {
        return p;
    }
    p = strchr(text, '{');
    if (p) {
        return p;
    }
    return text;
}

static void trans_cmd_unescape_simple_json(char *text)
{
    char *src;
    char *dst;

    if (!text || text[0] != '{' || !strstr(text, "\\\"")) {
        return;
    }

    src = text;
    dst = text;
    while (*src) {
        if (src[0] == '\\' && src[1] == '\"') {
            src++;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static const char *trans_cmd_find_uuid_fallback_anchor(char *text)
{
    char *p;

    if (!text || !text[0]) {
        return NULL;
    }
    if (!strncmp(text, "CMD:UUID=", 9) || !strncmp(text, "CMD:UUID?", 9) || text[0] == '{') {
        return text;
    }

    p = strstr(text, "CMD:UUID=");
    if (p) {
        return p;
    }
    p = strstr(text, "CMD:UUID?");
    if (p) {
        return p;
    }
    p = strstr(text, "{\"cmd\"");
    if (p && strstr(p, "setUuid")) {
        return p;
    }
    p = strstr(text, "{\\\"cmd\\\"");
    if (p && strstr(p, "setUuid")) {
        return p;
    }
    p = strchr(text, '{');
    if (p && strstr(p, "setUuid")) {
        return p;
    }
    return NULL;
}

static uint8_t trans_cmd_line_contains_token(const uint8_t *line, uint16_t len, const char *token)
{
    uint16_t token_len;
    uint16_t i;

    if (!line || !len || !token || !token[0]) {
        return 0;
    }
    token_len = (uint16_t)strlen(token);
    if (!token_len || token_len > len) {
        return 0;
    }
    for (i = 0; i + token_len <= len; i++) {
        if (!memcmp(line + i, token, token_len)) {
            return 1;
        }
    }
    return 0;
}

static int trans_cmd_try_uuid_fallback(const uint8_t *line, uint16_t len)
{
    char local[256];
    uint16_t copy_len;
    char *text;
    int text_len;

    if (!line || !len) {
        return -1;
    }

    copy_len = len;
    if (copy_len >= sizeof(local)) {
        copy_len = sizeof(local) - 1;
    }
    memcpy(local, line, copy_len);
    local[copy_len] = '\0';

    text = (char *)trans_cmd_skip_line_prefix(local);
    if (!text || !text[0]) {
        return -1;
    }
    text = (char *)trans_cmd_find_uuid_fallback_anchor(text);
    if (!text || !text[0]) {
        return -1;
    }
    text = (char *)trans_cmd_skip_line_prefix(text);
    if (!text || !text[0]) {
        return -1;
    }

    text_len = (int)strlen(text);
    while (text_len > 0) {
        uint8_t tail = (uint8_t)text[text_len - 1];
        if (tail == ' ' || tail == '\t' || tail == '\r' || tail == '\n' || tail < 0x20 || tail == 0x7F) {
            text[--text_len] = '\0';
            continue;
        }
        break;
    }

    trans_cmd_unescape_simple_json(text);
    if (!strncmp(text, "CMD:UUID=", 9)) {
        trans_cmd_set_uuid_trace("FALLBACK_CMD_UUID");
        return trans_cmd_handle_uuid_set(text);
    }
    if (text[0] == '{' && strstr(text, "\"setUuid\"")) {
        trans_cmd_set_uuid_trace("FALLBACK_JSON_UUID");
        if (!trans_cmd_json_cmd_match(text, "setUuid")) {
            return trans_cmd_handle_json_set_uuid(text);
        }
    }
    return -1;
}

static const char *trans_cmd_prepare_line(const uint8_t *line, uint16_t len)
{
    uint16_t copy_len;
    char *text;
    int text_len;

    if (!line || !len) {
        return NULL;
    }

    copy_len = len;
    if (copy_len >= sizeof(trans_cmd_line_scratch)) {
        copy_len = sizeof(trans_cmd_line_scratch) - 1;
    }
    memcpy(trans_cmd_line_scratch, line, copy_len);
    trans_cmd_line_scratch[copy_len] = '\0';

    text = (char *)trans_cmd_skip_line_prefix(trans_cmd_line_scratch);
    if (!text || !text[0]) {
        return NULL;
    }

    text = trans_cmd_find_cmd_anchor(text);
    text = (char *)trans_cmd_skip_line_prefix(text);
    if (!text || !text[0]) {
        return NULL;
    }

    text_len = (int)strlen(text);
    while (text_len > 0) {
        uint8_t tail = (uint8_t)text[text_len - 1];
        if (tail == ' ' || tail == '\t' || tail == '\r' || tail == '\n' || tail < 0x20 || tail == 0x7F) {
            text[--text_len] = '\0';
            continue;
        }
        break;
    }

    trans_cmd_unescape_simple_json(text);
    return text;
}

static int trans_cmd_send_dec_u16(uint16_t value)
{
    char buf[5];
    uint8_t len = 0;
    uint16_t tmp = value;
    uint8_t i = 0;

    do {
        buf[len++] = (char)('0' + (tmp % 10u));
        tmp /= 10u;
    } while (tmp && len < sizeof(buf));

    while (i < (len / 2)) {
        char t = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = t;
        i++;
    }
    return trans_cmd_send_bytes_checked((const uint8_t *)buf, len);
}

static int trans_cmd_send_hex_u8(uint8_t value)
{
    static const char hex_tab[] = "0123456789ABCDEF";
    char buf[2];

    buf[0] = hex_tab[(value >> 4) & 0x0F];
    buf[1] = hex_tab[value & 0x0F];
    return trans_cmd_send_bytes_checked((const uint8_t *)buf, sizeof(buf));
}

static int trans_cmd_send_hex_u16_4(uint16_t value)
{
    static const char hex_tab[] = "0123456789ABCDEF";
    char buf[4];

    buf[0] = hex_tab[(value >> 12) & 0x0F];
    buf[1] = hex_tab[(value >> 8) & 0x0F];
    buf[2] = hex_tab[(value >> 4) & 0x0F];
    buf[3] = hex_tab[value & 0x0F];
    return trans_cmd_send_bytes_checked((const uint8_t *)buf, sizeof(buf));
}

static int trans_cmd_send_hex_buf(const uint8_t *src, uint8_t src_len)
{
    uint8_t i;

    if (!src) {
        return -1;
    }
    for (i = 0; i < src_len; i++) {
        if (trans_cmd_send_hex_u8(src[i])) {
            return -1;
        }
    }
    return 0;
}

static int trans_cmd_copy_range_trim(const char *start, const char *end, char *dst, uint16_t dst_size)
{
    uint16_t len;

    if (!start || !end || !dst || dst_size < 2 || end < start) {
        return -1;
    }
    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    len = (uint16_t)(end - start);
    if (!len || len >= dst_size) {
        return -1;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
    return 0;
}

static uint8_t trans_cmd_key_equal(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return 0;
    }
    while (*lhs && *rhs) {
        if (trans_cmd_ascii_upper(*lhs) != trans_cmd_ascii_upper(*rhs)) {
            return 0;
        }
        lhs++;
        rhs++;
    }
    return (*lhs == '\0' && *rhs == '\0') ? 1 : 0;
}

static const char *trans_cmd_skip_line_prefix(const char *text)
{
    const uint8_t *p = (const uint8_t *)text;
    uint8_t skip_more = 1;

    if (!p) {
        return NULL;
    }

    while (skip_more) {
        skip_more = 0;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
            skip_more = 1;
        }

        /* UTF-8 BOM */
        if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
            p += 3;
            skip_more = 1;
            continue;
        }

        /* Zero-width space / ZWNJ / ZWJ */
        if (p[0] == 0xE2 && p[1] == 0x80 &&
            (p[2] == 0x8B || p[2] == 0x8C || p[2] == 0x8D)) {
            p += 3;
            skip_more = 1;
            continue;
        }

        /* Non-breaking space */
        if (p[0] == 0xC2 && p[1] == 0xA0) {
            p += 2;
            skip_more = 1;
            continue;
        }

        /* Full-width space */
        if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) {
            p += 3;
            skip_more = 1;
            continue;
        }
    }
    return (const char *)p;
}

static const char *trans_cmd_json_find_field_value(const char *json, const char *key)
{
    size_t key_len;
    const char *p;

    if (!json || !key) {
        return NULL;
    }
    key_len = strlen(key);
    p = json;

    while ((p = strchr(p, '\"')) != NULL) {
        p++;
        if (!strncmp(p, key, key_len) && p[key_len] == '\"') {
            p += key_len + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p != ':') {
                return NULL;
            }
            p++;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            return p;
        }
    }
    return NULL;
}

static int trans_cmd_json_parse_u16_dec_field(const char *json, const char *key, uint16_t *value)
{
    const char *p;
    uint32_t parsed = 0;
    uint8_t digit_cnt = 0;

    if (!json || !key || !value) {
        return -1;
    }

    p = trans_cmd_json_find_field_value(json, key);
    if (!p) {
        return -1;
    }
    while (*p >= '0' && *p <= '9') {
        parsed = parsed * 10u + (uint32_t)(*p - '0');
        if (parsed > 0xFFFFu) {
            return -1;
        }
        digit_cnt++;
        p++;
    }
    if (!digit_cnt) {
        return -1;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ',' && *p != '}' && *p != '\0') {
        return -1;
    }
    *value = (uint16_t)parsed;
    return 0;
}

static int trans_cmd_json_parse_i16_dec_field(const char *json, const char *key, int16_t *value)
{
    const char *p;
    int32_t parsed = 0;
    uint8_t digit_cnt = 0;
    uint8_t negative = 0;

    if (!json || !key || !value) {
        return -1;
    }

    p = trans_cmd_json_find_field_value(json, key);
    if (!p) {
        return -1;
    }
    if (*p == '-') {
        negative = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        parsed = parsed * 10 + (int32_t)(*p - '0');
        if ((!negative && parsed > 32767) || (negative && parsed > 32768)) {
            return -1;
        }
        digit_cnt++;
        p++;
    }
    if (!digit_cnt) {
        return -1;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ',' && *p != '}' && *p != '\0') {
        return -1;
    }
    *value = negative ? (int16_t)(-parsed) : (int16_t)parsed;
    return 0;
}

static int trans_cmd_json_parse_string_field(const char *json, const char *key, char *out, uint16_t out_size)
{
    const char *p;
    const char *end;
    uint16_t len;

    if (!json || !key || !out || out_size < 2) {
        return -1;
    }

    p = trans_cmd_json_find_field_value(json, key);
    if (!p || *p != '\"') {
        return -1;
    }
    p++;
    end = strchr(p, '\"');
    if (!end) {
        return -1;
    }
    len = (uint16_t)(end - p);
    if (!len || len >= out_size) {
        return -1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int trans_cmd_json_cmd_match(const char *json, const char *expect)
{
    const char *p;

    if (!json || !expect) {
        return -1;
    }
    p = trans_cmd_json_find_field_value(json, "cmd");
    if (!p || *p != '\"') {
        return -1;
    }
    p++;
    while (*expect && *p == *expect) {
        p++;
        expect++;
    }
    if (*expect || *p != '\"') {
        return -2;
    }
    return 0;
}

static int trans_cmd_json_parse_set_params(const char *json, trans_cmd_set_params_t *out)
{
    if (!json || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (trans_cmd_json_cmd_match(json, "setParams")) {
        return -2;
    }
    if (!trans_cmd_json_parse_u16_dec_field(json, "schoolId", &out->school_id)) {
        out->has_school_id = 1;
    }
    if (!trans_cmd_json_parse_u16_dec_field(json, "deviceId", &out->device_id)) {
        out->has_device_id = 1;
    }
    if (!out->has_school_id && !out->has_device_id) {
        return -1;
    }
    return 0;
}

static int trans_cmd_json_parse_set_uuid(const char *json, trans_cmd_set_uuid_t *out)
{
    char uuid_hex[40];
    int16_t tx_val = 0;

    if (!json || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (trans_cmd_json_cmd_match(json, "setUuid")) {
        return -2;
    }
    if (trans_cmd_json_parse_string_field(json, "uuid", uuid_hex, sizeof(uuid_hex))) {
        return -1;
    }
    if ((int)strlen(uuid_hex) != 32) {
        return -3;
    }
    if (trans_cmd_parse_hex_bytes(uuid_hex, out->uuid, sizeof(out->uuid))) {
        return -3;
    }
    if (trans_cmd_json_parse_u16_dec_field(json, "major", &out->major)) {
        return -1;
    }
    if (trans_cmd_json_parse_u16_dec_field(json, "minor", &out->minor)) {
        return -1;
    }

    out->tx_power = (int8_t)-59;
    if (trans_cmd_json_find_field_value(json, "tx")) {
        if (trans_cmd_json_parse_i16_dec_field(json, "tx", &tx_val)) {
            return -1;
        }
        if (tx_val < -128 || tx_val > 127) {
            return -1;
        }
        out->tx_power = (int8_t)tx_val;
        out->has_tx = 1;
    }
    if (!trans_cmd_json_parse_u16_dec_field(json, "schoolId", &out->school_id)) {
        out->has_school_id = 1;
    }
    if (!trans_cmd_json_parse_u16_dec_field(json, "deviceId", &out->device_id)) {
        out->has_device_id = 1;
    }
    return 0;
}

static int trans_cmd_json_send_set_params_result(uint8_t ret_code, const char *code,
                                                 uint16_t school_id, uint16_t device_id, uint8_t with_cfg)
{
    if (trans_cmd_send_literal("{\"cmd\":\"setParamsResult\",\"retCode\":")) {
        return -1;
    }
    if (trans_cmd_send_dec_u16(ret_code)) {
        return -1;
    }
    if (with_cfg) {
        if (trans_cmd_send_literal(",\"schoolId\":")) {
            return -1;
        }
        if (trans_cmd_send_dec_u16(school_id)) {
            return -1;
        }
        if (trans_cmd_send_literal(",\"deviceId\":")) {
            return -1;
        }
        if (trans_cmd_send_dec_u16(device_id)) {
            return -1;
        }
        return trans_cmd_send_literal("}\r\n");
    }
    if (code && code[0]) {
        if (trans_cmd_send_literal(",\"code\":\"")) {
            return -1;
        }
        if (trans_cmd_send_literal(code)) {
            return -1;
        }
        return trans_cmd_send_literal("\"}\r\n");
    }
    return trans_cmd_send_literal("}\r\n");
}

static int trans_cmd_json_send_scan_filter_result(const char *cmd, uint8_t ret_code, const char *code,
                                                  uint16_t school_id, uint16_t device_id, uint8_t enable, uint8_t with_cfg)
{
    const char *op = "result";

    if (cmd) {
        if (strstr(cmd, "Get")) {
            op = "getResult";
        } else if (strstr(cmd, "Set")) {
            op = "setResult";
        }
    }

    if (trans_cmd_send_literal("{\"cmd\":\"")) {
        return -1;
    }
    if (trans_cmd_send_literal(op)) {
        return -1;
    }
    if (trans_cmd_send_literal("\",\"retCode\":")) {
        return -1;
    }
    if (trans_cmd_send_dec_u16(ret_code)) {
        return -1;
    }
    if (with_cfg) {
        if (trans_cmd_send_literal(",\"schoolId\":")) {
            return -1;
        }
        if (trans_cmd_send_dec_u16(school_id)) {
            return -1;
        }
        if (trans_cmd_send_literal(",\"deviceId\":")) {
            return -1;
        }
        if (trans_cmd_send_dec_u16(device_id)) {
            return -1;
        }
        if (trans_cmd_send_literal(",\"enable\":")) {
            return -1;
        }
        if (trans_cmd_send_dec_u16(enable ? 1 : 0)) {
            return -1;
        }
        return trans_cmd_send_literal("}\r\n");
    }
    if (code && code[0]) {
        if (trans_cmd_send_literal(",\"code\":\"")) {
            return -1;
        }
        if (trans_cmd_send_literal(code)) {
            return -1;
        }
        return trans_cmd_send_literal("\"}\r\n");
    }
    return trans_cmd_send_literal("}\r\n");
}

static int trans_cmd_json_send_set_uuid_result(uint8_t ret_code, const char *code,
                                               const uint8_t *uuid, uint16_t major, uint16_t minor,
                                               int8_t tx_power, uint16_t school_id, uint16_t device_id,
                                               uint8_t with_cfg)
{
    if (trans_cmd_send_literal("{\"cmd\":\"setUuidResult\",\"retCode\":")) {
        return -1;
    }
    if (trans_cmd_send_dec_u16(ret_code)) {
        return -1;
    }
    if (with_cfg) {
        if (!uuid) {
            return -1;
        }
        if (trans_cmd_send_literal(",\"uuid\":\"") ||
            trans_cmd_send_hex_buf(uuid, 16) ||
            trans_cmd_send_literal("\",\"major\":") ||
            trans_cmd_send_dec_u16(major) ||
            trans_cmd_send_literal(",\"minor\":") ||
            trans_cmd_send_dec_u16(minor) ||
            trans_cmd_send_literal(",\"tx\":")) {
            return -1;
        }
        if (tx_power < 0) {
            if (trans_cmd_send_literal("-")) {
                return -1;
            }
            if (trans_cmd_send_dec_u16((uint16_t)(-(int16_t)tx_power))) {
                return -1;
            }
        } else if (trans_cmd_send_dec_u16((uint16_t)tx_power)) {
            return -1;
        }
        if (trans_cmd_send_literal(",\"majorHex\":\"") ||
            trans_cmd_send_hex_u16_4(major) ||
            trans_cmd_send_literal("\",\"minorHex\":\"") ||
            trans_cmd_send_hex_u16_4(minor) ||
            trans_cmd_send_literal("\",\"txHex\":\"") ||
            trans_cmd_send_hex_u8((uint8_t)tx_power) ||
            trans_cmd_send_literal("\",\"schoolId\":") ||
            trans_cmd_send_dec_u16(school_id) ||
            trans_cmd_send_literal(",\"deviceId\":") ||
            trans_cmd_send_dec_u16(device_id)) {
            return -1;
        }
        return trans_cmd_send_literal("}\r\n");
    }
    if (code && code[0]) {
        if (trans_cmd_send_literal(",\"code\":\"") ||
            trans_cmd_send_literal(code) ||
            trans_cmd_send_literal("\"}\r\n")) {
            return -1;
        }
        return 0;
    }
    return trans_cmd_send_literal("}\r\n");
}

static int trans_cmd_parse_lock_value(const char *text, uint8_t *lock_val)
{
    uint16_t tmp = 0;

    if (!lock_val || trans_cmd_parse_u16_dec(text, &tmp) || tmp > 1) {
        return -1;
    }
    *lock_val = (uint8_t)tmp;
    return 0;
}

static void trans_cmd_trim_ascii_blank(char *text)
{
    char *start;
    int len;

    if (!text) {
        return;
    }
    start = text;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    len = (int)strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }
}

static uint8_t trans_cmd_str_ieq(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return 0;
    }
    while (*lhs && *rhs) {
        if (trans_cmd_ascii_upper(*lhs) != trans_cmd_ascii_upper(*rhs)) {
            return 0;
        }
        lhs++;
        rhs++;
    }
    return (*lhs == '\0' && *rhs == '\0') ? 1 : 0;
}

static int trans_cmd_parse_scan_filter_enable(const char *text, uint8_t *enable)
{
    if (!text || !enable) {
        return -1;
    }
    if (!strcmp(text, "1") || trans_cmd_str_ieq(text, "ON")) {
        *enable = 1;
        return 0;
    }
    if (!strcmp(text, "0") || trans_cmd_str_ieq(text, "OFF")) {
        *enable = 0;
        return 0;
    }
    return -1;
}

static int trans_cmd_parse_scan_filter_tail(const char *tail,
                                            uint16_t cur_school_id,
                                            uint16_t cur_device_id,
                                            uint8_t cur_enable,
                                            uint16_t *out_school_id,
                                            uint16_t *out_device_id,
                                            uint8_t *out_enable)
{
    char kv_buf[80];
    uint16_t school_id = cur_school_id;
    uint16_t device_id = cur_device_id;
    uint8_t enable = cur_enable;
    uint8_t has_school = 0;
    uint8_t has_device = 0;
    uint8_t has_enable = 0;
    int tail_len;
    char *token;

    if (!tail || !out_school_id || !out_device_id || !out_enable) {
        return -1;
    }
    if (!trans_cmd_parse_scan_filter_enable(tail, &enable)) {
        *out_school_id = school_id;
        *out_device_id = device_id;
        *out_enable = enable;
        return 0;
    }

    tail_len = (int)strlen(tail);
    if (tail_len <= 0 || tail_len >= (int)sizeof(kv_buf)) {
        return -1;
    }
    memcpy(kv_buf, tail, tail_len + 1);

    token = kv_buf;
    while (token && *token) {
        char *next = strchr(token, ',');
        char *eq;
        char *key;
        char *val;
        uint16_t parsed_u16 = 0;
        uint8_t parsed_enable = 0;

        if (next) {
            *next = '\0';
            next++;
        }
        trans_cmd_trim_ascii_blank(token);
        if (token[0] == '\0') {
            return -1;
        }

        eq = strchr(token, '=');
        if (!eq) {
            return -1;
        }
        *eq = '\0';
        key = token;
        val = eq + 1;
        trans_cmd_trim_ascii_blank(key);
        trans_cmd_trim_ascii_blank(val);
        if (key[0] == '\0' || val[0] == '\0') {
            return -1;
        }

        if (trans_cmd_str_ieq(key, "SCHOOLID") || trans_cmd_str_ieq(key, "SCHOOL_ID")) {
            if (has_school || trans_cmd_parse_u16_auto(val, &parsed_u16)) {
                return -1;
            }
            school_id = parsed_u16;
            has_school = 1;
        } else if (trans_cmd_str_ieq(key, "DEVICEID") || trans_cmd_str_ieq(key, "DEVICE_ID")) {
            if (has_device || trans_cmd_parse_u16_auto(val, &parsed_u16)) {
                return -1;
            }
            device_id = parsed_u16;
            has_device = 1;
        } else if (trans_cmd_str_ieq(key, "ENABLE")) {
            if (has_enable || trans_cmd_parse_scan_filter_enable(val, &parsed_enable)) {
                return -1;
            }
            enable = parsed_enable;
            has_enable = 1;
        } else {
            return -1;
        }

        token = next;
    }

    if (!has_school && !has_device && !has_enable) {
        return -1;
    }
    *out_school_id = school_id;
    *out_device_id = device_id;
    *out_enable = enable;
    return 0;
}

static int trans_cmd_parse_badge_hex_10(const char *text, uint8_t *payload)
{
    char badge_hex[21];
    const char *p = text;
    const char *end;
    uint16_t len = 0;

    if (!text || !payload) {
        return -1;
    }

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    len = (uint16_t)(end - p);
    if (len != 20) {
        return -2;
    }

    memcpy(badge_hex, p, len);
    badge_hex[len] = '\0';
    if (trans_cmd_parse_hex_bytes(badge_hex, payload, 10)) {
        return -1;
    }
    return 0;
}

static int trans_cmd_adv_reconfig_ok_resp(const char *label)
{
    if (trans_cmd_send_literal("ACK:")) {
        return -1;
    }
    if (trans_cmd_send_literal(label)) {
        return -1;
    }
    return trans_cmd_send_literal(",OK,ADV=ON\r\n");
}

static int trans_cmd_handle_mode_query(void)
{
    return trans_cmd_uart_send_text("MODE:ADV\r\n");
}

static int trans_cmd_handle_uuid_query(void)
{
    uint8_t uuid[16];
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    int8_t tx_power = (int8_t)-59;
    uint8_t lock = 0;
    uint8_t ready = 0;

    if (trans_uuid_info_get(uuid, &major, &minor, &tx_power, NULL, NULL,
                            &school_id, &device_id, &lock, &ready)) {
        return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
    }
    if (!ready) {
        return trans_cmd_uart_send_text("ERR:UUID_UNSET\r\n");
    }

    /*
     * 当前产品固定为普通广播 PROFILE=PAIR。
     * SCHOOLID/DEVICEID 统一按十进制回显，和 setParams JSON number 口径保持一致，
     * 避免主控侧对同一字段同时维护十进制/十六进制两套解析分支。
     */
    if (trans_cmd_send_literal("UUID:HEX=")) {
        return -1;
    }
    if (trans_cmd_send_hex_buf(uuid, sizeof(uuid))) {
        return -1;
    }
    if (trans_cmd_send_literal(",MAJOR=") ||
        trans_cmd_send_hex_u16_4(major) ||
        trans_cmd_send_literal(",MINOR=") ||
        trans_cmd_send_hex_u16_4(minor) ||
        trans_cmd_send_literal(",TX=") ||
        trans_cmd_send_hex_u8((uint8_t)tx_power) ||
        trans_cmd_send_literal(",PROFILE=PAIR,SCHOOLID=") ||
        trans_cmd_send_dec_u16(school_id) ||
        trans_cmd_send_literal(",DEVICEID=") ||
        trans_cmd_send_dec_u16(device_id) ||
        trans_cmd_send_literal(",LOCK=") ||
        trans_cmd_send_dec_u16(lock ? 1 : 0)) {
        return -1;
    }
    return trans_cmd_send_literal("\r\n");
}

static int trans_cmd_handle_uuid_lock_query(void)
{
    uint8_t lock = 0;
    uint8_t ready = 0;

    if (trans_uuid_info_get(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &lock, &ready)) {
        return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
    }
    if (!ready) {
        return trans_cmd_uart_send_text("ERR:UUID_UNSET\r\n");
    }

    if (trans_cmd_send_literal("UUID:LOCK=") ||
        trans_cmd_send_dec_u16(lock ? 1 : 0)) {
        return -1;
    }
    return trans_cmd_send_literal("\r\n");
}

static int trans_cmd_handle_uuid_lock_set(const char *value_text)
{
    uint8_t lock_val = 0;
    int ret;

    if (trans_cmd_parse_lock_value(value_text, &lock_val)) {
        return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
    }

    ret = trans_uuid_lock_set(lock_val);
    if (ret == 0) {
        if (trans_cmd_send_literal("ACK:UUID:LOCK=") ||
            trans_cmd_send_dec_u16(lock_val)) {
            return -1;
        }
        return trans_cmd_send_literal("\r\n");
    }
    if (ret == -5) {
        return trans_cmd_uart_send_text("ERR:UUID_UNSET\r\n");
    }
    if (ret == -4) {
        return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
    }
    if (ret == -3) {
        return trans_cmd_uart_send_text("ERR:UUID_SAVE\r\n");
    }
    return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
}

static int trans_cmd_handle_uuid_set(const char *line)
{
    char *token_text = trans_cmd_uuid_parse_scratch.token_text;
    char *key_text = trans_cmd_uuid_parse_scratch.key_text;
    char *value_text = trans_cmd_uuid_parse_scratch.value_text;
    uint8_t uuid[16];
    uint8_t ready = 0;
    uint8_t have_major = 0;
    uint8_t have_minor = 0;
    uint8_t have_tx = 0;
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint16_t tmp_u16 = 0;
    int8_t tx_power = 0;
    const char *cursor;
    const char *token_start;
    const char *token_end;
    char *eq;
    int ret;

    if (!line || strncmp(line, "CMD:UUID=", 9)) {
        return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
    }

    ret = trans_uuid_info_get(NULL, &major, &minor, &tx_power, NULL, NULL, &school_id, &device_id, NULL, &ready);
    if (ret == -4) {
        return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
    }
    if (ret) {
        return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
    }

    cursor = line + 9;
    token_start = cursor;
    while (*cursor && *cursor != ',') {
        cursor++;
    }
    token_end = cursor;
    if (trans_cmd_copy_range_trim(token_start, token_end, token_text, sizeof(trans_cmd_uuid_parse_scratch.token_text)) ||
        trans_cmd_parse_hex_bytes(token_text, uuid, sizeof(uuid))) {
        return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
    }

    while (*cursor == ',') {
        cursor++;
        token_start = cursor;
        while (*cursor && *cursor != ',') {
            cursor++;
        }
        token_end = cursor;
        if (trans_cmd_copy_range_trim(token_start, token_end, token_text, sizeof(trans_cmd_uuid_parse_scratch.token_text))) {
            return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
        }

        eq = strchr(token_text, '=');
        if (!eq) {
            return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
        }
        if (trans_cmd_copy_range_trim(token_text, eq, key_text, sizeof(trans_cmd_uuid_parse_scratch.key_text)) ||
            trans_cmd_copy_range_trim(eq + 1, token_text + strlen(token_text), value_text, sizeof(trans_cmd_uuid_parse_scratch.value_text))) {
            return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
        }

        if (trans_cmd_key_equal(key_text, "MAJOR")) {
            if (trans_cmd_parse_u16_hex_exact(value_text, 4, &major)) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            have_major = 1;
            continue;
        }
        if (trans_cmd_key_equal(key_text, "MINOR")) {
            if (trans_cmd_parse_u16_hex_exact(value_text, 4, &minor)) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            have_minor = 1;
            continue;
        }
        if (trans_cmd_key_equal(key_text, "TX")) {
            if (trans_cmd_parse_i8_hex_exact(value_text, 2, &tx_power)) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            have_tx = 1;
            continue;
        }
        if (trans_cmd_key_equal(key_text, "SCHOOLID") || trans_cmd_key_equal(key_text, "SCHOOL_ID")) {
            if (trans_cmd_parse_u16_auto(value_text, &school_id)) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            continue;
        }
        if (trans_cmd_key_equal(key_text, "DEVICEID") || trans_cmd_key_equal(key_text, "DEVICE_ID")) {
            if (trans_cmd_parse_u16_auto(value_text, &device_id)) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            continue;
        }
        if (trans_cmd_key_equal(key_text, "PROFILE")) {
            if (!trans_cmd_key_equal(value_text, "PAIR")) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            continue;
        }
        if (trans_cmd_key_equal(key_text, "GROUPID") || trans_cmd_key_equal(key_text, "GROUP_ID")) {
            if (trans_cmd_parse_u16_auto(value_text, &tmp_u16) || tmp_u16 != 0) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            continue;
        }
        if (trans_cmd_key_equal(key_text, "LOCK")) {
            uint8_t lock_val = 0;
            if (trans_cmd_parse_lock_value(value_text, &lock_val)) {
                return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
            }
            continue;
        }

        return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
    }

    if ((!have_major || !have_minor || !have_tx) && !ready) {
        return trans_cmd_uart_send_text("ERR:UUID_UNSET\r\n");
    }

    ret = trans_uuid_info_set(uuid, major, minor, tx_power, TRANS_UUID_PROFILE_PAIR, 0, school_id, device_id);
    if (ret == -2) {
        return trans_cmd_uart_send_text("ERR:UUID_LOCKED\r\n");
    }
    if (ret == -4) {
        return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
    }
    if (ret == -3) {
        return trans_cmd_uart_send_text("ERR:UUID_SAVE\r\n");
    }
    if (ret) {
        return trans_cmd_uart_send_text("ERR:UUID_PARAM\r\n");
    }

    return trans_cmd_adv_reconfig_ok_resp("UUID");
}

static int trans_cmd_handle_badge_query(void)
{
    uint8_t badge_payload[10];
    int get_ret;

    get_ret = trans_badge_payload_get(badge_payload, sizeof(badge_payload));
    if (get_ret > 0) {
        if (trans_cmd_send_literal("BADGE:HEX=") ||
            trans_cmd_send_hex_buf(badge_payload, sizeof(badge_payload))) {
            return -1;
        }
        return trans_cmd_send_literal("\r\n");
    }
    if (get_ret == -2) {
        return trans_cmd_uart_send_text("ERR:BADGE_UNSET\r\n");
    }
    return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
}

static int trans_cmd_handle_badge_set(const char *value_text)
{
    uint8_t badge_payload[10];
    int parse_ret;
    int set_ret;

    /*
     * BADGE 固定 10 字节透传载荷，长度必须锁死在 20 个 HEX 字符，
     * 这样 RSP 侧的空间预算才是稳定可控的。
     */
    parse_ret = trans_cmd_parse_badge_hex_10(value_text, badge_payload);
    if (parse_ret == -2) {
        return trans_cmd_uart_send_text("ERR:BADGE_LEN\r\n");
    }
    if (parse_ret) {
        return trans_cmd_uart_send_text("ERR:BADGE_FORMAT\r\n");
    }

    set_ret = trans_badge_payload_set(badge_payload, sizeof(badge_payload));
    if (set_ret == 0) {
        return trans_cmd_adv_reconfig_ok_resp("BADGE");
    }
    if (set_ret == -2) {
        return trans_cmd_uart_send_text("ERR:BADGE_LOCKED\r\n");
    }
    if (set_ret == -5) {
        return trans_cmd_uart_send_text("ERR:BADGE_SAVE\r\n");
    }
    if (set_ret == -4) {
        return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
    }
    return trans_cmd_uart_send_text("ERR:BADGE_SET\r\n");
}

static int trans_cmd_handle_badge_lock_query(void)
{
    uint8_t lock = 0;
    uint8_t ready = 0;

    if (trans_badge_lock_get(&lock, &ready) == -4) {
        return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
    }
    if (!ready) {
        return trans_cmd_uart_send_text("ERR:BADGE_UNSET\r\n");
    }

    if (trans_cmd_send_literal("BADGE:LOCK=") ||
        trans_cmd_send_dec_u16(lock ? 1 : 0)) {
        return -1;
    }
    return trans_cmd_send_literal("\r\n");
}

static int trans_cmd_handle_badge_lock_set(const char *value_text)
{
    uint8_t lock_val = 0;
    int ret;

    if (trans_cmd_parse_lock_value(value_text, &lock_val)) {
        return trans_cmd_uart_send_text("ERR:BADGE_PARAM\r\n");
    }

    ret = trans_badge_lock_set(lock_val);
    if (ret == 0) {
        if (trans_cmd_send_literal("ACK:BADGE:LOCK=") ||
            trans_cmd_send_dec_u16(lock_val)) {
            return -1;
        }
        return trans_cmd_send_literal("\r\n");
    }
    if (ret == -5) {
        return trans_cmd_uart_send_text("ERR:BADGE_UNSET\r\n");
    }
    if (ret == -4) {
        return trans_cmd_uart_send_text("ERR:ADV_NOT_SUPPORTED\r\n");
    }
    if (ret == -3) {
        return trans_cmd_uart_send_text("ERR:BADGE_SAVE\r\n");
    }
    return trans_cmd_uart_send_text("ERR:BADGE_PARAM\r\n");
}

static int trans_cmd_handle_scan_filter_query(void)
{
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint8_t enable = 0;
    int ret;

    ret = trans_scan_filter_get(&school_id, &device_id, &enable);
    if (ret == 0) {
        return trans_cmd_json_send_scan_filter_result("scanFilterGet", 0, NULL, school_id, device_id, enable, 1);
    }
    if (ret == -2) {
        return trans_cmd_json_send_scan_filter_result("scanFilterGet", 1, "SCAN_NOT_SUPPORTED", 0, 0, 0, 0);
    }
    return trans_cmd_json_send_scan_filter_result("scanFilterGet", 1, "SCAN_FILTER_GET", 0, 0, 0, 0);
}

static int trans_cmd_handle_scan_school_id_set(const char *value_text)
{
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint8_t enable = 0;
    int ret;

    if (trans_cmd_parse_u16_auto(value_text, &school_id)) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "PARAM", 0, 0, 0, 0);
    }

    ret = trans_scan_filter_get(NULL, &device_id, &enable);
    if (ret == -2) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_NOT_SUPPORTED", 0, 0, 0, 0);
    }
    if (ret) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_FILTER_GET", 0, 0, 0, 0);
    }

    ret = trans_scan_filter_set(school_id, device_id, enable);
    if (ret == 0) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 0, NULL, school_id, device_id, enable, 1);
    }
    if (ret == -2) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_NOT_SUPPORTED", 0, 0, 0, 0);
    }
    if (ret == -3) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SAVE_FAIL", 0, 0, 0, 0);
    }
    return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "PARAM", 0, 0, 0, 0);
}

static int trans_cmd_handle_scan_device_id_set(const char *value_text)
{
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint8_t enable = 0;
    int ret;

    if (trans_cmd_parse_u16_auto(value_text, &device_id)) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "PARAM", 0, 0, 0, 0);
    }

    ret = trans_scan_filter_get(&school_id, NULL, &enable);
    if (ret == -2) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_NOT_SUPPORTED", 0, 0, 0, 0);
    }
    if (ret) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_FILTER_GET", 0, 0, 0, 0);
    }

    ret = trans_scan_filter_set(school_id, device_id, enable);
    if (ret == 0) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 0, NULL, school_id, device_id, enable, 1);
    }
    if (ret == -2) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_NOT_SUPPORTED", 0, 0, 0, 0);
    }
    if (ret == -3) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SAVE_FAIL", 0, 0, 0, 0);
    }
    return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "PARAM", 0, 0, 0, 0);
}

static int trans_cmd_handle_scan_filter_set(const char *value_text)
{
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint8_t enable = 0;
    int ret;

    ret = trans_scan_filter_get(&school_id, &device_id, &enable);
    if (ret == -2) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_NOT_SUPPORTED", 0, 0, 0, 0);
    }
    if (ret) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_FILTER_GET", 0, 0, 0, 0);
    }
    if (trans_cmd_parse_scan_filter_tail(value_text, school_id, device_id, enable,
                                         &school_id, &device_id, &enable)) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "PARAM", 0, 0, 0, 0);
    }

    ret = trans_scan_filter_set(school_id, device_id, enable);
    if (ret == 0) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 0, NULL, school_id, device_id, enable, 1);
    }
    if (ret == -2) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SCAN_NOT_SUPPORTED", 0, 0, 0, 0);
    }
    if (ret == -3) {
        return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "SAVE_FAIL", 0, 0, 0, 0);
    }
    return trans_cmd_json_send_scan_filter_result("scanFilterSet", 1, "PARAM", 0, 0, 0, 0);
}

static int trans_cmd_handle_school_id_set(const char *value_text)
{
    uint8_t uuid[16];
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint16_t new_school_id = 0;
    int8_t tx_power = (int8_t)-59;
    uint8_t ready = 0;
    int ret;

    if (trans_cmd_parse_u16_auto(value_text, &new_school_id)) {
        return trans_cmd_uart_send_text("ERR:SCHOOL_ID_PARAM\r\n");
    }
    ret = trans_uuid_info_get(uuid, &major, &minor, &tx_power, NULL, NULL, &school_id, &device_id, NULL, &ready);
    if (ret || !ready) {
        return trans_cmd_uart_send_text("ERR:UUID_UNSET\r\n");
    }
    if (new_school_id == school_id) {
        return trans_cmd_adv_reconfig_ok_resp("SCHOOL_ID");
    }

    ret = trans_uuid_info_set(uuid, major, minor, tx_power, TRANS_UUID_PROFILE_PAIR, 0, new_school_id, device_id);
    if (ret == -2) {
        return trans_cmd_uart_send_text("ERR:UUID_LOCKED\r\n");
    }
    if (ret) {
        return trans_cmd_uart_send_text("ERR:SCHOOL_ID_PARAM\r\n");
    }

    return trans_cmd_adv_reconfig_ok_resp("SCHOOL_ID");
}

static int trans_cmd_handle_device_id_set(const char *value_text)
{
    uint8_t uuid[16];
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint16_t new_device_id = 0;
    int8_t tx_power = (int8_t)-59;
    uint8_t ready = 0;
    int ret;

    if (trans_cmd_parse_u16_auto(value_text, &new_device_id)) {
        return trans_cmd_uart_send_text("ERR:DEVICE_ID_PARAM\r\n");
    }
    ret = trans_uuid_info_get(uuid, &major, &minor, &tx_power, NULL, NULL, &school_id, &device_id, NULL, &ready);
    if (ret || !ready) {
        return trans_cmd_uart_send_text("ERR:UUID_UNSET\r\n");
    }
    if (new_device_id == device_id) {
        return trans_cmd_adv_reconfig_ok_resp("DEVICE_ID");
    }

    ret = trans_uuid_info_set(uuid, major, minor, tx_power, TRANS_UUID_PROFILE_PAIR, 0, school_id, new_device_id);
    if (ret == -2) {
        return trans_cmd_uart_send_text("ERR:UUID_LOCKED\r\n");
    }
    if (ret) {
        return trans_cmd_uart_send_text("ERR:DEVICE_ID_PARAM\r\n");
    }

    return trans_cmd_adv_reconfig_ok_resp("DEVICE_ID");
}

static int trans_cmd_handle_json_set_uuid(const char *line)
{
    trans_cmd_set_uuid_t cfg;
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    int8_t tx_power = (int8_t)-59;
    uint8_t ready = 0;
    int ret;

    trans_cmd_set_uuid_trace("HANDLER_ENTER");
    ret = trans_cmd_json_parse_set_uuid(line, &cfg);
    if (ret == -2) {
        trans_cmd_set_uuid_trace("PARSE_CMD_MISMATCH");
        return -1;
    }
    if (ret == -3) {
        trans_cmd_set_uuid_trace("PARSE_UUID_FORMAT");
        return trans_cmd_json_send_set_uuid_result(1, "UUID_FORMAT", NULL, 0, 0, 0, 0, 0, 0);
    }
    if (ret) {
        trans_cmd_set_uuid_trace("PARSE_PARAM_FAIL");
        return trans_cmd_json_send_set_uuid_result(1, "UUID_PARAM", NULL, 0, 0, 0, 0, 0, 0);
    }
    trans_cmd_set_uuid_trace("PARSE_OK");

    ret = trans_uuid_info_get(NULL, NULL, NULL, &tx_power, NULL, NULL,
                              &school_id, &device_id, NULL, &ready);
    if (ret == -4) {
        trans_cmd_set_uuid_trace("GET_ADV_NOT_SUPPORTED");
        return trans_cmd_json_send_set_uuid_result(1, "ADV_NOT_SUPPORTED", NULL, 0, 0, 0, 0, 0, 0);
    }
    if (ret) {
        trans_cmd_set_uuid_trace("GET_CFG_FAIL");
        return trans_cmd_json_send_set_uuid_result(1, "UUID_PARAM", NULL, 0, 0, 0, 0, 0, 0);
    }

    if (!cfg.has_tx && ready) {
        cfg.tx_power = tx_power;
    }
    if (!cfg.has_school_id && ready) {
        cfg.school_id = school_id;
    }
    if (!cfg.has_device_id && ready) {
        cfg.device_id = device_id;
    }

    trans_cmd_set_uuid_trace("WRITE_BEGIN");
    ret = trans_uuid_info_set(cfg.uuid, cfg.major, cfg.minor, cfg.tx_power,
                              TRANS_UUID_PROFILE_PAIR, 0, cfg.school_id, cfg.device_id);
    if (ret == -2) {
        trans_cmd_set_uuid_trace("WRITE_LOCKED");
        return trans_cmd_json_send_set_uuid_result(1, "UUID_LOCKED", NULL, 0, 0, 0, 0, 0, 0);
    }
    if (ret == -4) {
        trans_cmd_set_uuid_trace("WRITE_ADV_NOT_SUPPORTED");
        return trans_cmd_json_send_set_uuid_result(1, "ADV_NOT_SUPPORTED", NULL, 0, 0, 0, 0, 0, 0);
    }
    if (ret == -3) {
        trans_cmd_set_uuid_trace("WRITE_SAVE_FAIL");
        return trans_cmd_json_send_set_uuid_result(1, "UUID_SAVE", NULL, 0, 0, 0, 0, 0, 0);
    }
    if (ret) {
        trans_cmd_set_uuid_trace("WRITE_FAIL");
        return trans_cmd_json_send_set_uuid_result(1, "UUID_PARAM", NULL, 0, 0, 0, 0, 0, 0);
    }

    trans_cmd_set_uuid_trace("WRITE_OK");
    return trans_cmd_json_send_set_uuid_result(0, NULL, cfg.uuid, cfg.major, cfg.minor,
                                               cfg.tx_power, cfg.school_id, cfg.device_id, 1);
}

static int trans_cmd_handle_json_set_params(const char *line)
{
    trans_cmd_set_params_t cfg;
#if TCFG_AC6323A_PRODUCT_MODE_IS_SCAN
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    uint8_t enable = 0;
#endif
#if !TCFG_AC6323A_PRODUCT_MODE_IS_SCAN
    uint8_t uuid[16];
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    int8_t tx_power = (int8_t)-59;
    uint8_t ready = 0;
#endif
    int ret;

    ret = trans_cmd_json_parse_set_params(line, &cfg);
    if (ret == -2) {
        return -1;
    }
    if (ret) {
        return trans_cmd_json_send_set_params_result(1, "PARAM", 0, 0, 0);
    }

#if TCFG_AC6323A_PRODUCT_MODE_IS_SCAN
    /*
     * 扫描模式下，setParams 复用为过滤条件写入口：
     * 主控下发 schoolId/deviceId 后，直接更新 scanFilter，
     * 与 AW31N 当前“主扫改过滤”的联调口径保持一致。
     */
    ret = trans_scan_filter_get(&school_id, &device_id, &enable);
    if (ret == -2) {
        return trans_cmd_json_send_set_params_result(1, "SCAN_NOT_SUPPORTED", 0, 0, 0);
    }
    if (ret) {
        return trans_cmd_json_send_set_params_result(1, "SCAN_FILTER_GET", 0, 0, 0);
    }
    if (cfg.has_school_id) {
        school_id = cfg.school_id;
    }
    if (cfg.has_device_id) {
        device_id = cfg.device_id;
    }
    ret = trans_scan_filter_set(school_id, device_id, 1);
    if (ret == -2) {
        return trans_cmd_json_send_set_params_result(1, "SCAN_NOT_SUPPORTED", 0, 0, 0);
    }
    if (ret) {
        return trans_cmd_json_send_set_params_result(1, "SAVE_FAIL", 0, 0, 0);
    }
    return trans_cmd_json_send_set_params_result(0, NULL, school_id, device_id, 1);
#else
    if (!cfg.has_school_id || !cfg.has_device_id) {
        return trans_cmd_json_send_set_params_result(1, "PARAM", 0, 0, 0);
    }
    ret = trans_uuid_info_get(uuid, &major, &minor, &tx_power, NULL, NULL, &school_id, &device_id, NULL, &ready);
    if (ret || !ready) {
        return trans_cmd_json_send_set_params_result(1, "UUID_UNSET", 0, 0, 0);
    }
    if (cfg.school_id == school_id && cfg.device_id == device_id) {
        if (trans_broadcast_restart_now()) {
            return trans_cmd_json_send_set_params_result(1, "ADV_FAIL", 0, 0, 0);
        }
        ret = trans_cmd_json_send_set_params_result(0, NULL, cfg.school_id, cfg.device_id, 1);
        trans_set_params_stop_broadcast_timer_restart();
        return ret;
    }

    /*
     * setParams 只改 schoolId/deviceId，沿用现有 UUID/Major/Minor/Tx。
     * 这样主控量产时不必重复下发整包 UUID，即可立即刷新业务归属字段。
     */
    ret = trans_uuid_info_set(uuid, major, minor, tx_power, TRANS_UUID_PROFILE_PAIR, 0, cfg.school_id, cfg.device_id);
    if (ret == -2) {
        return trans_cmd_json_send_set_params_result(1, "UUID_LOCKED", 0, 0, 0);
    }
    if (ret) {
        return trans_cmd_json_send_set_params_result(1, "SAVE_FAIL", 0, 0, 0);
    }

    ret = trans_cmd_json_send_set_params_result(0, NULL, cfg.school_id, cfg.device_id, 1);
    trans_set_params_stop_broadcast_timer_restart();
    return ret;
#endif
}

static int trans_cmd_proc_handle_line(const uint8_t *line, uint16_t len)
{
    const char *text = trans_cmd_prepare_line(line, len);

    if (!line || !len || !text || !text[0]) {
        return -1;
    }

    if (!strcmp(text, "CMD:MODE?")) {
        return trans_cmd_handle_mode_query();
    }
    if (!strcmp(text, "CMD:UUID?")) {
        return trans_cmd_handle_uuid_query();
    }
    if (!strcmp(text, "CMD:UUID:LOCK?")) {
        return trans_cmd_handle_uuid_lock_query();
    }
    if (!strncmp(text, "CMD:UUID=", 9)) {
        return trans_cmd_handle_uuid_set(text);
    }
    if (!strncmp(text, "CMD:UUID:LOCK=", 14)) {
        return trans_cmd_handle_uuid_lock_set(text + 14);
    }
    if (!strncmp(text, "CMD:SCHOOL_ID=", 14)) {
        return trans_cmd_handle_school_id_set(text + 14);
    }
    if (!strncmp(text, "CMD:DEVICE_ID=", 14)) {
        return trans_cmd_handle_device_id_set(text + 14);
    }
    if (!strcmp(text, "CMD:BADGE?")) {
        return trans_cmd_handle_badge_query();
    }
    if (!strcmp(text, "CMD:BADGE:LOCK?")) {
        return trans_cmd_handle_badge_lock_query();
    }
    if (!strncmp(text, "CMD:BADGE=", 10)) {
        return trans_cmd_handle_badge_set(text + 10);
    }
    if (!strncmp(text, "CMD:BADGE:LOCK=", 15)) {
        return trans_cmd_handle_badge_lock_set(text + 15);
    }
    if (!strcmp(text, "CMD:SCAN:FILTER?") ||
        !strcmp(text, "CMD:SCAN:SCHOOLID?") ||
        !strcmp(text, "CMD:SCAN:DEVICEID?")) {
        return trans_cmd_handle_scan_filter_query();
    }
    if (!strncmp(text, "CMD:SCAN:SCHOOLID=", 18)) {
        return trans_cmd_handle_scan_school_id_set(text + 18);
    }
    if (!strncmp(text, "CMD:SCAN:DEVICEID=", 18)) {
        return trans_cmd_handle_scan_device_id_set(text + 18);
    }
    if (!strncmp(text, "CMD:SCAN:FILTER=", 16)) {
        return trans_cmd_handle_scan_filter_set(text + 16);
    }
    if (text[0] == '{') {
        if (strstr(text, "\"setUuid\"") || strstr(text, "\\\"setUuid\\\"")) {
            trans_cmd_set_uuid_trace("JSON_SEEN");
            if (strstr(text, "\\\"cmd\\\"")) {
                trans_cmd_set_uuid_trace("ESCAPED_INPUT");
            }
        }
        if (!trans_cmd_json_cmd_match(text, "setUuid")) {
            trans_cmd_set_uuid_trace("DISPATCH_HIT");
            return trans_cmd_handle_json_set_uuid(text);
        }
        if (strstr(text, "\"setUuid\"") || strstr(text, "\\\"setUuid\\\"")) {
            trans_cmd_set_uuid_trace("DISPATCH_MISS");
        }
        if (!trans_cmd_json_cmd_match(text, "setParams")) {
            return trans_cmd_handle_json_set_params(text);
        }
        return -1;
    }
    if (len && (trans_cmd_line_contains_token(line, len, "CMD:UUID=") ||
                trans_cmd_line_contains_token(line, len, "setUuid"))) {
        return trans_cmd_try_uuid_fallback(line, len);
    }
    return -1;
}

void trans_cmd_proc_init(void)
{
    if (trans_cmd_proc_inited) {
        return;
    }

    trans_cmd_uart_register_line_handler(trans_cmd_proc_handle_line);
    trans_cmd_proc_inited = 1;
    (void)trans_cmd_uart_send_text(TRANS_CMD_SET_UUID_TRACE_BANNER);
    log_info("proc ready: mode/uuid/badge/lock/setParams/setUuid/scanFilter");
}

#endif
