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

typedef struct {
    uint16_t school_id;
    uint16_t device_id;
    uint8_t has_school_id;
    uint8_t has_device_id;
} trans_cmd_set_params_t;

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

static int trans_cmd_json_parse_set_params(const char *json, trans_cmd_set_params_t *out)
{
    const char *p;
    const char *expect = "setParams";

    if (!json || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

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
    if (!trans_cmd_json_parse_u16_dec_field(json, "schoolId", &out->school_id)) {
        out->has_school_id = 1;
    }
    if (!trans_cmd_json_parse_u16_dec_field(json, "deviceId", &out->device_id)) {
        out->has_device_id = 1;
    }
    if (!out->has_school_id || !out->has_device_id) {
        return -1;
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

static int trans_cmd_parse_lock_value(const char *text, uint8_t *lock_val)
{
    uint16_t tmp = 0;

    if (!lock_val || trans_cmd_parse_u16_dec(text, &tmp) || tmp > 1) {
        return -1;
    }
    *lock_val = (uint8_t)tmp;
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
     * SCHOOLID 保持十进制，DEVICEID 保持 4 位大写十六进制，兼容现有主控与产测口径。
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
        trans_cmd_send_hex_u16_4(device_id) ||
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

static int trans_cmd_handle_json_set_params(const char *line)
{
    trans_cmd_set_params_t cfg;
    uint8_t uuid[16];
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t school_id = 0;
    uint16_t device_id = 0;
    int8_t tx_power = (int8_t)-59;
    uint8_t ready = 0;
    int ret;

    ret = trans_cmd_json_parse_set_params(line, &cfg);
    if (ret == -2) {
        return -1;
    }
    if (ret) {
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
}

static int trans_cmd_proc_handle_line(const uint8_t *line, uint16_t len)
{
    const char *text = (const char *)line;

    if (!line || !len) {
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
    if (text[0] == '{') {
        return trans_cmd_handle_json_set_params(text);
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
    log_info("proc ready: mode/uuid/badge/lock/setParams");
}

#endif
