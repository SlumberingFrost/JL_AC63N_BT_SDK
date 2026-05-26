/*********************************************************************************************
    *   Filename        : ble_trans_search.c

    *   Description     : 配置搜索指定的services

    *   Author          :JM

    *   Email           : zh-jieli.com

    *   Last modifiled  : 2022-9-17 11:14

    *   Copyright:(c)JIELI  2011-2016  @ , All Rights Reserved.
*********************************************************************************************/
#include "system/app_core.h"
#include "system/includes.h"

#include "app_config.h"
#include "app_action.h"

#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "user_cfg.h"
#include "vm.h"
#include "btcontroller_modules.h"
#include "bt_common.h"
#include "3th_profile_api.h"
#include "le_common.h"
#include "rcsp_bluetooth.h"
#include "custom_cfg.h"
#include "btstack/btstack_event.h"
#include "le_client_demo.h"
#include "gatt_common/le_gatt_common.h"
#include "ble_trans.h"
#include "trans_cmd_uart.h"

#if CONFIG_APP_SPP_LE && CONFIG_BT_GATT_CLIENT_NUM

#if LE_DEBUG_PRINT_EN
#define log_info(x, ...)  printf("[TRANS-SEARCH]" x " ", ## __VA_ARGS__)
#define log_info_hexdump  put_buf

#else
#define log_info(...)
#define log_info_hexdump(...)
#endif

#define trans_scan_boot_trace(fmt, ...) printf("[SCAN-BOOT] " fmt "\r\n", ##__VA_ARGS__)

#define SET_SCAN_TYPE       SCAN_ACTIVE
#define SET_SCAN_INTERVAL   ADV_SCAN_MS(24) // unit: 0.625ms
#define SET_SCAN_WINDOW     ADV_SCAN_MS(8)  // unit: 0.625ms, <= SET_SCAN_INTERVAL
#define TRANS_SCHOOL_ID_SVC_UUID16 0xAF31
#define TRANS_SCAN_LIST_PERIOD_MS 1000
#define TRANS_SCAN_LIST_CACHE_MAX 8
#define TRANS_SCAN_LIST_TTL_MS 5000
#define TRANS_SCAN_MERGE_WINDOW_MS 1000
#define TRANS_SCAN_MERGE_SLOTS 4
#define TRANS_SCAN_EMPTY_REPORT_HYSTERESIS 2
#define TRANS_SCAN_HEALTH_CHECK_MS 1000
#define TRANS_SCAN_HEALTH_REPORT_STALL_MS 4000
#define TRANS_SCAN_HEALTH_RECOVER_COOLDOWN_MS 3000
#define TRANS_SCAN_HEALTH_RECOVER_WINDOW_MS 30000
#define TRANS_SCAN_HEALTH_RESET_MAX 3
#define TRANS_SCAN_TX_LINE_MAX 384


static int trans_client_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param);
static void trans_scan_conn_config_set(struct ctl_pair_info_t *pair_info);

typedef struct {
    uint8_t valid;
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t tx_power;
} trans_scan_ibeacon_parse_t;

typedef struct {
    uint8_t has_school_id;
    uint16_t school_id;
    uint8_t has_device_id;
    uint16_t device_id;
} trans_scan_pair_parse_t;

typedef struct {
    uint8_t valid;
    uint8_t addr_type;
    uint8_t addr[6];
    uint32_t tick_begin;
    uint32_t tick_last;
    int8_t rssi;
    uint8_t has_school_id;
    uint16_t school_id;
    uint8_t has_device_id;
    uint16_t device_id;
} trans_scan_merge_slot_t;

typedef struct {
    uint8_t valid;
    uint8_t addr_type;
    uint8_t addr[6];
    uint32_t tick_last;
    int8_t rssi;
    uint8_t has_school_id;
    uint16_t school_id;
    uint8_t has_device_id;
    uint16_t device_id;
} trans_scan_list_cache_t;

static trans_scan_merge_slot_t trans_scan_merge_slots[TRANS_SCAN_MERGE_SLOTS];
static trans_scan_list_cache_t trans_scan_list_cache[TRANS_SCAN_LIST_CACHE_MAX];
static u16 trans_scan_list_timer;
static u16 trans_scan_health_timer;
static uint8_t trans_scan_empty_frame_streak;
static uint32_t trans_scan_health_last_report_tick;
static uint32_t trans_scan_health_last_recover_tick;
static uint32_t trans_scan_health_recover_window_tick;
static uint8_t trans_scan_health_local_recover_cnt;
static uint8_t trans_scan_health_seen_report;

#define TRANS_SCAN_LIST_TTL_TICKS             msecs_to_jiffies(TRANS_SCAN_LIST_TTL_MS)
#define TRANS_SCAN_MERGE_WINDOW_TICKS         msecs_to_jiffies(TRANS_SCAN_MERGE_WINDOW_MS)
#define TRANS_SCAN_HEALTH_REPORT_STALL_TICKS  msecs_to_jiffies(TRANS_SCAN_HEALTH_REPORT_STALL_MS)
#define TRANS_SCAN_RECOVER_COOLDOWN_TICKS     msecs_to_jiffies(TRANS_SCAN_HEALTH_RECOVER_COOLDOWN_MS)
#define TRANS_SCAN_RECOVER_WINDOW_TICKS       msecs_to_jiffies(TRANS_SCAN_HEALTH_RECOVER_WINDOW_MS)

static uint32_t trans_scan_now_tick(void)
{
    return (uint32_t)jiffies;
}

static uint8_t trans_scan_tick_expired(uint32_t now_tick, uint32_t start_tick, uint32_t delta_tick)
{
    uint32_t expire_tick;

    if (!delta_tick) {
        return 1;
    }
    expire_tick = start_tick + delta_tick;
    if (now_tick == expire_tick) {
        return 1;
    }
    return time_after(now_tick, expire_tick) ? 1 : 0;
}

static uint8_t trans_scan_school_id_filter_match(uint16_t school_id)
{
    uint16_t expected_school_id = 0;
    uint8_t enable = 0;

    if (trans_scan_filter_get(&expected_school_id, NULL, &enable)) {
        return 1;
    }
    if (!enable) {
        return 1;
    }
    return school_id == expected_school_id;
}

static uint8_t trans_scan_pair_filter_match(const trans_scan_pair_parse_t *pair_info)
{
    if (!pair_info || !pair_info->has_school_id) {
        return 0;
    }
    return trans_scan_school_id_filter_match(pair_info->school_id);
}

static void trans_scan_hex_encode(const uint8_t *src, uint8_t src_len, char *dst, uint16_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    uint16_t i;

    if (!src || !dst || dst_len < ((uint16_t)src_len * 2 + 1)) {
        if (dst && dst_len) {
            dst[0] = '\0';
        }
        return;
    }

    for (i = 0; i < src_len; i++) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex[src[i] & 0x0F];
    }
    dst[src_len * 2] = '\0';
}

static uint8_t trans_adv_get_ibeacon(const uint8_t *data, uint8_t len, trans_scan_ibeacon_parse_t *out)
{
    uint8_t i = 0;

    if (!data || !out) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    while (i + 1 < len) {
        uint8_t ad_len = data[i];
        if (ad_len == 0) {
            break;
        }
        if (ad_len < 1 || (uint8_t)(i + ad_len) >= len) {
            break;
        }

        if (data[i + 1] == HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA && ad_len >= 26) {
            const uint8_t *p = &data[i + 2];
            if (p[0] == 0x4C && p[1] == 0x00 && p[2] == 0x02 && p[3] == 0x15) {
                memcpy(out->uuid, &p[4], sizeof(out->uuid));
                out->major = (uint16_t)(((uint16_t)p[20] << 8) | p[21]);
                out->minor = (uint16_t)(((uint16_t)p[22] << 8) | p[23]);
                out->tx_power = (int8_t)p[24];
                out->valid = 1;
                return 1;
            }
        }

        i += (ad_len + 1);
    }

    return 0;
}

static uint8_t trans_adv_get_school_and_device_id(const uint8_t *data, uint8_t len, trans_scan_pair_parse_t *out)
{
    uint8_t i = 0;

    if (!data || !out) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    while (i + 1 < len) {
        uint8_t ad_len = data[i];
        if (ad_len == 0) {
            break;
        }
        if (ad_len < 1 || (uint8_t)(i + ad_len) >= len) {
            break;
        }

        if (data[i + 1] == HCI_EIR_DATATYPE_SERVICE_DATA && ad_len >= 5) {
            const uint8_t *payload = &data[i + 2];
            uint8_t payload_len = ad_len - 1;

            if (payload_len >= 4) {
                uint16_t uuid16 = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
                if (uuid16 == TRANS_SCHOOL_ID_SVC_UUID16) {
                    out->has_school_id = 1;
                    out->school_id = (uint16_t)(((uint16_t)payload[2] << 8) | payload[3]);
                    if (payload_len >= 6) {
                        out->has_device_id = 1;
                        out->device_id = (uint16_t)(((uint16_t)payload[4] << 8) | payload[5]);
                    }
                    return 1;
                }
            }
        }

        i += (ad_len + 1);
    }

    return 0;
}

static void trans_log_scan_adv_report(const adv_report_t *report_data)
{
    (void)report_data;
}

static void trans_scan_list_cache_reset(void)
{
    memset(trans_scan_merge_slots, 0, sizeof(trans_scan_merge_slots));
    memset(trans_scan_list_cache, 0, sizeof(trans_scan_list_cache));
    trans_scan_empty_frame_streak = 0;
}

static void trans_scan_send_empty_result(void)
{
    (void)trans_cmd_uart_send_text("{\"cmd\":\"scanResult\",\"data\":[]}\r\n");
}

static int trans_scan_list_cache_find(uint8_t addr_type, const uint8_t *addr)
{
    uint8_t i;

    if (!addr) {
        return -1;
    }
    for (i = 0; i < TRANS_SCAN_LIST_CACHE_MAX; i++) {
        if (trans_scan_list_cache[i].valid &&
            trans_scan_list_cache[i].addr_type == addr_type &&
            !memcmp(trans_scan_list_cache[i].addr, addr, 6)) {
            return i;
        }
    }
    return -1;
}

static uint16_t trans_scan_list_cache_get_active_count(uint32_t now_tick)
{
    uint8_t i;
    uint16_t active_cnt = 0;

    for (i = 0; i < TRANS_SCAN_LIST_CACHE_MAX; i++) {
        trans_scan_list_cache_t *entry = &trans_scan_list_cache[i];

        if (!entry->valid) {
            continue;
        }
        if (trans_scan_tick_expired(now_tick, entry->tick_last, TRANS_SCAN_LIST_TTL_TICKS)) {
            memset(entry, 0, sizeof(*entry));
            continue;
        }
        active_cnt++;
    }
    return active_cnt;
}

static int trans_scan_list_cache_pick(void)
{
    uint8_t i;
    int oldest = 0;

    for (i = 0; i < TRANS_SCAN_LIST_CACHE_MAX; i++) {
        if (!trans_scan_list_cache[i].valid) {
            return i;
        }
    }

    for (i = 1; i < TRANS_SCAN_LIST_CACHE_MAX; i++) {
        if (time_before(trans_scan_list_cache[i].tick_last, trans_scan_list_cache[oldest].tick_last)) {
            oldest = i;
        }
    }
    return oldest;
}

static void trans_scan_list_cache_update_from_merge(const trans_scan_merge_slot_t *slot)
{
    int idx;
    uint32_t now_tick;
    trans_scan_list_cache_t *entry;

    if (!slot || !slot->valid || !slot->has_school_id) {
        return;
    }
    now_tick = trans_scan_now_tick();
    idx = trans_scan_list_cache_find(slot->addr_type, slot->addr);
    if (idx < 0) {
        idx = trans_scan_list_cache_pick();
        memset(&trans_scan_list_cache[idx], 0, sizeof(trans_scan_list_cache[idx]));
        trans_scan_list_cache[idx].valid = 1;
        trans_scan_list_cache[idx].addr_type = slot->addr_type;
        memcpy(trans_scan_list_cache[idx].addr, slot->addr, 6);
    }

    entry = &trans_scan_list_cache[idx];
    entry->tick_last = now_tick;
    entry->rssi = slot->rssi;
    entry->has_school_id = slot->has_school_id;
    entry->school_id = slot->school_id;
    entry->has_device_id = slot->has_device_id;
    entry->device_id = slot->device_id;
}

static void trans_scan_list_send_frame(void)
{
    uint8_t i;
    uint16_t item_cnt = 0;
    uint16_t off = 0;
    uint32_t now_tick = trans_scan_now_tick();
    char msg[TRANS_SCAN_TX_LINE_MAX];

    (void)trans_scan_list_cache_get_active_count(now_tick);
    off = (uint16_t)snprintf(msg, sizeof(msg), "{\"cmd\":\"scanResult\",\"data\":[");
    if (off >= sizeof(msg)) {
        return;
    }

    for (i = 0; i < TRANS_SCAN_LIST_CACHE_MAX; i++) {
        trans_scan_list_cache_t *entry = &trans_scan_list_cache[i];
        int device_id_num;
        int len;

        if (!entry->valid) {
            continue;
        }
        if (!entry->has_school_id) {
            continue;
        }
        if (!trans_scan_school_id_filter_match(entry->school_id)) {
            continue;
        }
        device_id_num = entry->has_device_id ? (int)entry->device_id : -1;
        len = snprintf(msg + off, sizeof(msg) - off,
                       "%s{\"schoolId\":%u,\"deviceId\":%d}",
                       item_cnt ? "," : "",
                       entry->school_id,
                       device_id_num);
        if (len < 0 || len >= (int)(sizeof(msg) - off)) {
            break;
        }
        off += (uint16_t)len;
        item_cnt++;
    }

    if (!item_cnt) {
        if (trans_scan_empty_frame_streak < 0xFF) {
            trans_scan_empty_frame_streak++;
        }
        if (trans_scan_empty_frame_streak < TRANS_SCAN_EMPTY_REPORT_HYSTERESIS) {
            return;
        }
        trans_scan_send_empty_result();
        return;
    }

    trans_scan_empty_frame_streak = 0;
    if (snprintf(msg + off, sizeof(msg) - off, "]}\r\n") >= (int)(sizeof(msg) - off)) {
        return;
    }
    (void)trans_cmd_uart_send_text(msg);
}

static void trans_scan_health_reset_runtime(void)
{
    uint32_t now_tick = trans_scan_now_tick();

    trans_scan_health_last_report_tick = now_tick;
    trans_scan_health_last_recover_tick = now_tick;
    trans_scan_health_recover_window_tick = now_tick;
    trans_scan_health_local_recover_cnt = 0;
    trans_scan_health_seen_report = 0;
}

static void trans_scan_health_local_recover(uint8_t reason, uint32_t now_tick)
{
    if (!trans_scan_tick_expired(now_tick, trans_scan_health_last_recover_tick, TRANS_SCAN_RECOVER_COOLDOWN_TICKS)) {
        return;
    }
    if (trans_scan_tick_expired(now_tick, trans_scan_health_recover_window_tick, TRANS_SCAN_RECOVER_WINDOW_TICKS)) {
        trans_scan_health_recover_window_tick = now_tick;
        trans_scan_health_local_recover_cnt = 0;
    }

    trans_scan_health_last_recover_tick = now_tick;
    if (trans_scan_health_local_recover_cnt < 0xFF) {
        trans_scan_health_local_recover_cnt++;
    }
    log_info("scan_health: recover reason=%u count=%u", reason, trans_scan_health_local_recover_cnt);

    trans_scan_list_cache_reset();
    ble_gatt_client_scan_enable(0);
    mdelay(20);
    ble_gatt_client_scan_enable(1);

    trans_scan_health_last_report_tick = now_tick;
    trans_scan_health_seen_report = 0;
    if (trans_scan_health_local_recover_cnt >= TRANS_SCAN_HEALTH_RESET_MAX) {
        trans_scan_health_recover_window_tick = now_tick;
        trans_scan_health_local_recover_cnt = 0;
    }
}

static void trans_scan_health_check(void)
{
    uint32_t now_tick = trans_scan_now_tick();
    uint16_t active_cnt;

    active_cnt = trans_scan_list_cache_get_active_count(now_tick);
    if (!trans_scan_health_seen_report && !active_cnt) {
        return;
    }
    if (trans_scan_tick_expired(now_tick, trans_scan_health_last_report_tick, TRANS_SCAN_HEALTH_REPORT_STALL_TICKS)) {
        trans_scan_health_local_recover(active_cnt ? 2 : 1, now_tick);
    }
}

static void trans_scan_health_timer_cb(void *priv)
{
    (void)priv;
    trans_scan_health_check();
}

static void trans_scan_health_timer_start(void)
{
    trans_scan_health_reset_runtime();
    if (!trans_scan_health_timer) {
        trans_scan_health_timer = sys_timer_add(NULL, trans_scan_health_timer_cb, TRANS_SCAN_HEALTH_CHECK_MS);
    }
}

static void trans_scan_health_timer_stop(void)
{
    if (trans_scan_health_timer) {
        sys_timer_del(trans_scan_health_timer);
        trans_scan_health_timer = 0;
    }
    trans_scan_health_local_recover_cnt = 0;
    trans_scan_health_seen_report = 0;
}

static void trans_scan_merge_slot_clear(trans_scan_merge_slot_t *slot)
{
    if (!slot) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
}

static int trans_scan_merge_find_slot(const adv_report_t *report)
{
    uint8_t i;

    if (!report) {
        return -1;
    }
    for (i = 0; i < TRANS_SCAN_MERGE_SLOTS; i++) {
        if (trans_scan_merge_slots[i].valid &&
            trans_scan_merge_slots[i].addr_type == report->address_type &&
            !memcmp(trans_scan_merge_slots[i].addr, report->address, 6)) {
            return i;
        }
    }
    return -1;
}

static void trans_scan_merge_report_and_clear(trans_scan_merge_slot_t *slot)
{
    if (!slot || !slot->valid) {
        return;
    }
    if (slot->has_school_id && trans_scan_school_id_filter_match(slot->school_id)) {
        trans_scan_list_cache_update_from_merge(slot);
    }
    trans_scan_merge_slot_clear(slot);
}

static int trans_scan_merge_pick_slot(void)
{
    uint8_t i;
    int oldest = 0;

    for (i = 0; i < TRANS_SCAN_MERGE_SLOTS; i++) {
        if (!trans_scan_merge_slots[i].valid) {
            return i;
        }
    }
    for (i = 1; i < TRANS_SCAN_MERGE_SLOTS; i++) {
        if (time_before(trans_scan_merge_slots[i].tick_begin, trans_scan_merge_slots[oldest].tick_begin)) {
            oldest = i;
        }
    }
    trans_scan_merge_report_and_clear(&trans_scan_merge_slots[oldest]);
    return oldest;
}

static void trans_scan_merge_update_slot(trans_scan_merge_slot_t *slot, const adv_report_t *report,
                                         const trans_scan_pair_parse_t *pair_info)
{
    if (!slot || !report) {
        return;
    }
    slot->tick_last = trans_scan_now_tick();
    slot->rssi = report->rssi;
    if (!pair_info) {
        return;
    }
    if (pair_info->has_school_id) {
        slot->has_school_id = 1;
        slot->school_id = pair_info->school_id;
    }
    if (pair_info->has_device_id) {
        slot->has_device_id = 1;
        slot->device_id = pair_info->device_id;
    }
}

static void trans_scan_merge_flush_timeout(uint32_t now_tick)
{
    uint8_t i;

    for (i = 0; i < TRANS_SCAN_MERGE_SLOTS; i++) {
        trans_scan_merge_slot_t *slot = &trans_scan_merge_slots[i];

        if (!slot->valid) {
            continue;
        }
        if (trans_scan_tick_expired(now_tick, slot->tick_begin, TRANS_SCAN_MERGE_WINDOW_TICKS)) {
            trans_scan_merge_report_and_clear(slot);
            continue;
        }
        if (trans_scan_tick_expired(now_tick, slot->tick_last, TRANS_SCAN_MERGE_WINDOW_TICKS * 2)) {
            trans_scan_merge_slot_clear(slot);
        }
    }
}

static void trans_scan_handle_adv_report(const adv_report_t *report_data)
{
    trans_scan_pair_parse_t pair_info;
    uint32_t now_tick;
    int idx;

    if (!report_data || !report_data->length) {
        return;
    }

    now_tick = trans_scan_now_tick();
    trans_scan_health_last_report_tick = now_tick;
    trans_scan_health_seen_report = 1;
    trans_scan_merge_flush_timeout(now_tick);

    memset(&pair_info, 0, sizeof(pair_info));
    if (!trans_adv_get_school_and_device_id(report_data->data, report_data->length, &pair_info)) {
        idx = trans_scan_merge_find_slot(report_data);
        if (idx >= 0) {
            trans_scan_merge_update_slot(&trans_scan_merge_slots[idx], report_data, NULL);
        }
        return;
    }
    if (!pair_info.has_school_id || !trans_scan_pair_filter_match(&pair_info)) {
        return;
    }

    idx = trans_scan_merge_find_slot(report_data);
    if (idx >= 0 && trans_scan_tick_expired(now_tick, trans_scan_merge_slots[idx].tick_begin, TRANS_SCAN_MERGE_WINDOW_TICKS)) {
        trans_scan_merge_report_and_clear(&trans_scan_merge_slots[idx]);
        idx = -1;
    }
    if (idx < 0) {
        idx = trans_scan_merge_pick_slot();
        memset(&trans_scan_merge_slots[idx], 0, sizeof(trans_scan_merge_slots[idx]));
        trans_scan_merge_slots[idx].valid = 1;
        trans_scan_merge_slots[idx].addr_type = report_data->address_type;
        memcpy(trans_scan_merge_slots[idx].addr, report_data->address, 6);
        trans_scan_merge_slots[idx].tick_begin = now_tick;
        trans_scan_merge_slots[idx].tick_last = now_tick;
    }
    trans_scan_merge_update_slot(&trans_scan_merge_slots[idx], report_data, &pair_info);
}

static void trans_scan_list_timer_cb(void *priv)
{
    uint32_t now_tick;

    (void)priv;
    now_tick = trans_scan_now_tick();
    trans_scan_merge_flush_timeout(now_tick);
    trans_scan_list_send_frame();
}

const gatt_client_cfg_t trans_client_init_cfg = {
    .event_packet_handler = trans_client_event_packet_handler,
};
//---------------------------------------------------------------------------
#define UUID_HANDLE_MAX   16
typedef struct {
    u16 uuid16;
    /* u16 uuid128[16]; */
    u16 value_handle;
    u16 properties;
} match_uuid_t;
static match_uuid_t uuid_handle_table[UUID_HANDLE_MAX];
static u16 match_uuid_cnt;

static u16 trans_client_conn_handle;
static u16 trans_client_read_handle;
static u16 trans_client_write_handle;
static u16 trans_client_notify_handle;

/*
 * scan-only 阶段不需要做 GATT 读写测试。
 * 保持为 0，避免后续误连上设备后自动触发测试流量干扰扫描链路定位。
 */
#define TRANS_TEST_READ_SEND            0 //测试发数
//---------------------------------------------------------------------------
//指定搜索uuid
//指定搜索uuid
static const target_uuid_t  jl_trans_search_uuid_table[] = {

    {
        .services_uuid16 = 0x1800,         //Generic Access
        .characteristic_uuid16 = 0x2a00,   //Device Name
        .opt_type = ATT_PROPERTY_READ,
        .read_long_enable = 1,
    },

    {
        .services_uuid16 = 0xae30,
        .characteristic_uuid16 = 0xae01,
        .opt_type = ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
    },

    {
        .services_uuid16 = 0xae30,
        .characteristic_uuid16 = 0xae02,
        .opt_type = ATT_PROPERTY_NOTIFY,
    },

#if CONFIG_BT_SM_SUPPORT_ENABLE /*ios need pair*/
    {
        .services_uuid16 = 0x180f,         //Battery
        .characteristic_uuid16 = 0x2a19,   //Battery Level
        .opt_type = ATT_PROPERTY_NOTIFY | ATT_PROPERTY_READ,
    },

    {
        .services_uuid16 = 0x1805,         //Current Time
        .characteristic_uuid16 = 0x2a2b,   //Current Time
        .opt_type = ATT_PROPERTY_NOTIFY | ATT_PROPERTY_READ,
    },

    {
        .services_uuid16 = 0x1805,         //Current Time
        .characteristic_uuid16 = 0x2a0f,   //Local Time Information
        .opt_type = ATT_PROPERTY_READ,
    },

    {
        .services_uuid16 = 0x180a,         //Device Information
        .characteristic_uuid16 = 0x2a29,   //Manufacturer Name String
        .opt_type = ATT_PROPERTY_READ,
    },

    {
        .services_uuid16 = 0x180a,         //Device Information
        .characteristic_uuid16 = 0x2a24,   //Model Number String
        .opt_type = ATT_PROPERTY_READ,
    },
#endif

    //for uuid128,sample
    //	PRIMARY_SERVICE, 0000F530-1212-EFDE-1523-785FEABCD123
    //	CHARACTERISTIC,  0000F531-1212-EFDE-1523-785FEABCD123, NOTIFY,
    //	CHARACTERISTIC,  0000F532-1212-EFDE-1523-785FEABCD123, WRITE_WITHOUT_RESPONSE | DYNAMIC,
    /*
    	{
    		.services_uuid16 = 0,
    		.services_uuid128 =       {0x00,0x00,0xF5,0x30 ,0x12,0x12 ,0xEF, 0xDE ,0x15,0x23 ,0x78,0x5F,0xEA ,0xBC,0xD1,0x23} ,
    		.characteristic_uuid16 = 0,
    		.characteristic_uuid128 = {0x00,0x00,0xF5,0x31 ,0x12,0x12 ,0xEF, 0xDE ,0x15,0x23 ,0x78,0x5F,0xEA ,0xBC,0xD1,0x23},
    		.opt_type = ATT_PROPERTY_NOTIFY,
    	},

    	{
    		.services_uuid16 = 0,
    		.services_uuid128 =       {0x00,0x00,0xF5,0x30 ,0x12,0x12 ,0xEF, 0xDE ,0x15,0x23 ,0x78,0x5F,0xEA ,0xBC,0xD1,0x23} ,
    		.characteristic_uuid16 = 0,
    		.characteristic_uuid128 = {0x00,0x00,0xF5,0x32 ,0x12,0x12 ,0xEF, 0xDE ,0x15,0x23 ,0x78,0x5F,0xEA ,0xBC,0xD1,0x23},
    		.opt_type = ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
    	},
    */

};

//测试write数据操作
static void trans_client_test_read(void)
{
#if TRANS_TEST_READ_SEND
    int ret = 0;
    u16 tmp_handle = trans_client_conn_handle;

    if (tmp_handle && trans_client_read_handle) {
        ret = ble_comm_att_send_data(tmp_handle, trans_client_read_handle, 0, 0, ATT_OP_READ);
        log_info("test_read:%04x, %04x, %d ", tmp_handle, trans_client_read_handle, ret);
    }

    static int write_count;
    if (tmp_handle && trans_client_write_handle) {
        ret = ble_comm_att_send_data(tmp_handle, trans_client_write_handle, &write_count, 4, ATT_OP_WRITE_WITHOUT_RESPOND);
        log_info("test_write:%04x, %04x, %d ", tmp_handle, trans_client_write_handle, ret);
        if (!ret) {
            write_count++;
        }
    } else {
        write_count = 0;
    }

#endif
}

//配置扫描匹配连接的设备，已经连上后搜索匹配的profile uuid
static const gatt_search_cfg_t trans_client_search_config = {
    .match_devices = NULL,
    .match_devices_count = 0,
    .match_rssi_enable = 0,

    .search_uuid_group = jl_trans_search_uuid_table,
    .search_uuid_count = (sizeof(jl_trans_search_uuid_table) / sizeof(target_uuid_t)),
    .auto_enable_ccc = 1,
};

/*handle 转换 对应的uuid*/
static u16 __handle_to_uuid(u16 value_handle)
{
    u8 i;
    match_uuid_t *t;
    for (i = 0; i < match_uuid_cnt; i++) {
        t = &uuid_handle_table[i];
        if (t->value_handle == value_handle) {
            return t->uuid16;
        }
    }
    return 0xFFFF;
}

/*解析对应handle的数据内容*/
static void __read_info_handle(u16 value_handle, u8 *data, u16 len)
{
    u16 uuid16 = __handle_to_uuid(value_handle);
    switch (uuid16) {
    case 0x2a00:
        log_info("0x2a00: Device Name: %s", data);
        break;

    case 0x2a19:
        log_info("0x2a19: Battery Level: %d", data[0]);
        break;

    case 0x2a2b:
        log_info("0x2a2b: Current Time: %d-%d-%d %d:%d:%d week:%d",
                 (data[1] << 8) | data[0], data[2], data[3],
                 data[4], data[5], data[6], data[7]);
        break;

    case 0x2a0f:
        char utc = data[0];
        log_info("0x2a0f: Local Time Information: UTC:%d", utc / 4);
        break;

    case 0x2a29:
        log_info("0x2a29: Manufacturer Name String: %s", data);
        break;

    case 0x2a24:
        log_info("0x2a24: Model Number String: %s", data);
        break;

    default:
        break;
    }
}

//-------------------------------------------------------------------------------------
//处理gatt 回调的事件，hci & gatt
static int trans_client_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param)
{
    /* log_info("event: %02x,size= %d\n",event,size); */
    switch (event) {
    case GATT_COMM_EVENT_GATT_DATA_REPORT: {
        att_data_report_t *report_data = (void *)packet;
        switch (report_data->packet_type) {
        case GATT_EVENT_NOTIFICATION://notify
            log_info("notify_report:");
            break;

        case GATT_EVENT_INDICATION://indicate
            log_info("indicate_report:");
            break;

        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT://read
            log_info("read_report:");
            __read_info_handle(report_data->value_handle, report_data->blob, report_data->blob_length);
            break;

        case GATT_EVENT_LONG_CHARACTERISTIC_VALUE_QUERY_RESULT://read long
            log_info("read_long_report:");
            __read_info_handle(report_data->value_handle, report_data->blob, report_data->blob_length);
            break;

        case GATT_EVENT_QUERY_COMPLETE:
            /*ATT读写操作等结果返回,对应spec的att error*/
            log_info("QUERY_COMPLETE:att_error: %02x\n", report_data->blob[0]);
            break;

        default:
            log_info("unknow_report:");
            break;
        }

        log_info("conn_handle=%04x,value_handle= %04x,size=%d\n", report_data->conn_handle, report_data->value_handle, report_data->blob_length);
        put_buf(report_data->blob, report_data->blob_length);
    }
    break;

    case GATT_COMM_EVENT_GATT_SEARCH_MATCH_UUID: {
        opt_handle_t *opt_hdl = packet;
        log_info("match:server_uuid= %04x,charactc_uuid= %04x,value_handle= %04x\n", \
                 opt_hdl->search_uuid->services_uuid16, opt_hdl->search_uuid->characteristic_uuid16, opt_hdl->value_handle);

        if (match_uuid_cnt < UUID_HANDLE_MAX) {
            uuid_handle_table[match_uuid_cnt].uuid16 = opt_hdl->search_uuid->characteristic_uuid16;
            uuid_handle_table[match_uuid_cnt].properties = opt_hdl->search_uuid->opt_type;
            uuid_handle_table[match_uuid_cnt].value_handle = opt_hdl->value_handle;
            match_uuid_cnt++;
        }

#if TRANS_TEST_READ_SEND
        //for test
#if CONFIG_BT_SM_SUPPORT_ENABLE
        if (opt_hdl->search_uuid->characteristic_uuid16 == 0x2a2b) {
#else
        if (opt_hdl->search_uuid->characteristic_uuid16 == 0x2a00) {
#endif
            trans_client_read_handle = opt_hdl->value_handle;
            log_info("get read_handle= %04x\n", trans_client_read_handle);
        }

        if (opt_hdl->search_uuid->characteristic_uuid16 == 0xae01) {
            trans_client_write_handle = opt_hdl->value_handle;
            log_info("get write_handle= %04x\n", trans_client_write_handle);
        }

        if (opt_hdl->search_uuid->characteristic_uuid16 == 0xae02) {
            trans_client_notify_handle = opt_hdl->value_handle;
            log_info("get notify_handle= %04x\n", trans_client_notify_handle);
        }

#endif
    }
    break;

    case GATT_COMM_EVENT_GATT_SEARCH_DESCRIPTOR_RESULT: {
        log_info("GATT_COMM_EVENT_GATT_SEARCH_DESCRIPTOR_RESULT");
        charact_descriptor_t *result_descriptor = ext_param;
    }
    break;

    case GATT_COMM_EVENT_GATT_SEARCH_PROFILE_COMPLETE:
        log_info("GATT_SEARCH_PROFILE_COMPLETE:%04x\n", little_endian_read_16(packet, 0));
        break;

    case GATT_COMM_EVENT_SCAN_ADV_REPORT: {
        adv_report_t *report_data = (adv_report_t *)packet;
        trans_log_scan_adv_report(report_data);
        trans_scan_handle_adv_report(report_data);
    }
    break;

    case GATT_COMM_EVENT_CLIENT_STATE:
        log_info("client_state: handle=%02x,%02x\n", little_endian_read_16(packet, 1), packet[0]);
        break;

    default:
        break;
    }
    return 0;
}

/*启动搜索服务*/
int trans_client_search_remote_profile(u16 conn_handle)
{
#if TRANS_CLIENT_SEARCH_PROFILE_ENABLE
    log_info("%s", __FUNCTION__);
    ble_gatt_just_search_profile_start(conn_handle);
    trans_client_conn_handle = conn_handle;
    match_uuid_cnt = 0;
#endif
    return 0;
}

/*停止搜索服务*/
int trans_client_search_remote_stop(u16 conn_handle)
{
#if TRANS_CLIENT_SEARCH_PROFILE_ENABLE
    log_info("%s", __FUNCTION__);
    if (conn_handle == trans_client_conn_handle) {
        ble_gatt_just_search_profile_stop(conn_handle);
        trans_client_conn_handle = 0;
    }
#endif
    return 0;
}


/*scan-only 骨架：上电即扫，但不自动建连，也不做业务解析*/
static const scan_conn_cfg_t trans_client_scan_cfg = {
    .scan_auto_do = 1,
    .creat_auto_do = 0,
    .scan_type = SET_SCAN_TYPE,
    .scan_filter = 0,
    .scan_interval = SET_SCAN_INTERVAL,
    .scan_window = SET_SCAN_WINDOW,
};

//trans client 初始化
void trans_client_init(void)
{
    trans_scan_boot_trace("trans_client_init: enter");
    log_info("%s", __FUNCTION__);
    trans_scan_boot_trace("trans_client_init: set_scan_config begin");
    ble_gatt_client_set_scan_config(&trans_client_scan_cfg);
    trans_scan_boot_trace("trans_client_init: set_scan_config done");
    trans_scan_boot_trace("trans_client_init: set_search_config begin");
    ble_gatt_client_set_search_config(&trans_client_search_config);
    trans_scan_boot_trace("trans_client_init: set_search_config done");
    log_info("scan-only skeleton enabled");
    trans_scan_boot_trace("trans_client_init: exit");
    trans_scan_list_cache_reset();
    trans_scan_health_timer_start();
    if (!trans_scan_list_timer) {
        trans_scan_list_timer = sys_timer_add(NULL, trans_scan_list_timer_cb, TRANS_SCAN_LIST_PERIOD_MS);
    }

#if TRANS_CLIENT_SEARCH_PROFILE_ENABLE
#if TRANS_TEST_READ_SEND
    sys_timer_add(0, trans_client_test_read, 500);
#endif
#endif
}

//trans client exit
void trans_client_exit(void)
{
    if (trans_scan_list_timer) {
        sys_timer_del(trans_scan_list_timer);
        trans_scan_list_timer = 0;
    }
    trans_scan_health_timer_stop();
    trans_scan_list_cache_reset();

#if TRANS_CLIENT_SEARCH_PROFILE_ENABLE
    log_info("%s", __FUNCTION__);
#endif
}

#endif
