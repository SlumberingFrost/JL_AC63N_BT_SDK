/*********************************************************************************************
    *   Filename        : ble_trans_cfg.c
*********************************************************************************************/
#include <string.h>
#include "includes.h"
#include "app_config.h"
#include "user_cfg.h"
#include "ble_trans.h"
#include "ble_trans_cfg.h"

#if (CONFIG_APP_LE_TRANS)
#if LE_DEBUG_PRINT_EN
#define log_info(x, ...)  printf("[BLE_TRANS_CFG]" x " ", ## __VA_ARGS__)
#define log_info_hexdump  put_buf
#else
#define log_info(...)
#define log_info_hexdump(...)
#endif

#if CONFIG_BT_GATT_SERVER_NUM
#define TRANS_IBEACON_CFG_TAG         0x5549
#define TRANS_IBEACON_CFG_VERSION     0x04
#define TRANS_IBEACON_CFG_VERSION_V3  0x03
#define TRANS_IBEACON_CFG_VERSION_V2  0x02
#define TRANS_IBEACON_CFG_VERSION_V1  0x01
#define TRANS_IBEACON_DEFAULT_TX      ((int8_t)-59)
#define TRANS_BADGE_CFG_TAG           0x4244
#define TRANS_BADGE_CFG_VERSION       0x01

uint8_t trans_badge_adv_data[10];
uint8_t trans_badge_adv_data_valid = 0;
trans_badge_cfg_t trans_badge_cfg;
trans_ibeacon_cfg_t trans_ibeacon_cfg;
uint8_t trans_uuid_ready = 0;
#endif

#if CONFIG_BT_GATT_CLIENT_NUM
#define TRANS_SCAN_FILTER_CFG_TAG         0x5346
#define TRANS_SCAN_FILTER_CFG_VERSION     0x02
#define TRANS_SCAN_FILTER_CFG_VERSION_V1  0x01
#define TRANS_SCAN_FILTER_CFG_VALID       0xA5

trans_scan_filter_cfg_t trans_scan_filter_cfg;
#endif

#if CONFIG_BT_GATT_CLIENT_NUM
static uint8_t trans_scan_filter_cfg_calc_check(const trans_scan_filter_cfg_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint8_t sum = 0;
    uint16_t i;

    if (!cfg) {
        return 0;
    }
    for (i = 0; i < (sizeof(trans_scan_filter_cfg_t) - 1); i++) {
        sum += p[i];
    }
    return sum;
}

static uint8_t trans_scan_filter_cfg_calc_check_v1(const trans_scan_filter_cfg_v1_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint8_t sum = 0;
    uint16_t i;

    if (!cfg) {
        return 0;
    }
    for (i = 0; i < (sizeof(trans_scan_filter_cfg_v1_t) - 1); i++) {
        sum += p[i];
    }
    return sum;
}

static void trans_scan_filter_cfg_runtime_reset(void)
{
    memset(&trans_scan_filter_cfg, 0, sizeof(trans_scan_filter_cfg));
    trans_scan_filter_cfg.tag = TRANS_SCAN_FILTER_CFG_TAG;
    trans_scan_filter_cfg.version = TRANS_SCAN_FILTER_CFG_VERSION;
    trans_scan_filter_cfg.len = sizeof(trans_scan_filter_cfg);
    trans_scan_filter_cfg.school_id = TCFG_TRANS_SCAN_SCHOOL_FILTER_DEFAULT_ID;
    trans_scan_filter_cfg.device_id = TCFG_TRANS_SCAN_DEVICE_FILTER_DEFAULT_ID;
    trans_scan_filter_cfg.enable = TCFG_TRANS_SCAN_SCHOOL_FILTER_DEFAULT_ENABLE;
    trans_scan_filter_cfg.valid = TRANS_SCAN_FILTER_CFG_VALID;
    trans_scan_filter_cfg.check = trans_scan_filter_cfg_calc_check(&trans_scan_filter_cfg);
}

static uint8_t trans_scan_filter_cfg_storage_is_valid(const trans_scan_filter_cfg_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->tag != TRANS_SCAN_FILTER_CFG_TAG) {
        return 0;
    }
    if (cfg->version != TRANS_SCAN_FILTER_CFG_VERSION) {
        return 0;
    }
    if (cfg->len != sizeof(trans_scan_filter_cfg_t)) {
        return 0;
    }
    if (cfg->valid != TRANS_SCAN_FILTER_CFG_VALID) {
        return 0;
    }
    if (cfg->enable > 1) {
        return 0;
    }
    if (cfg->check != trans_scan_filter_cfg_calc_check(cfg)) {
        return 0;
    }
    return 1;
}

static uint8_t trans_scan_filter_cfg_storage_is_valid_v1(const trans_scan_filter_cfg_v1_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->tag != TRANS_SCAN_FILTER_CFG_TAG) {
        return 0;
    }
    if (cfg->version != TRANS_SCAN_FILTER_CFG_VERSION_V1) {
        return 0;
    }
    if (cfg->len != sizeof(trans_scan_filter_cfg_v1_t)) {
        return 0;
    }
    if (cfg->valid != TRANS_SCAN_FILTER_CFG_VALID) {
        return 0;
    }
    if (cfg->enable > 1) {
        return 0;
    }
    if (cfg->check != trans_scan_filter_cfg_calc_check_v1(cfg)) {
        return 0;
    }
    return 1;
}

int trans_scan_filter_cfg_save(void)
{
    int ret;

    trans_scan_filter_cfg.tag = TRANS_SCAN_FILTER_CFG_TAG;
    trans_scan_filter_cfg.version = TRANS_SCAN_FILTER_CFG_VERSION;
    trans_scan_filter_cfg.len = sizeof(trans_scan_filter_cfg);
    trans_scan_filter_cfg.valid = TRANS_SCAN_FILTER_CFG_VALID;
    trans_scan_filter_cfg.check = trans_scan_filter_cfg_calc_check(&trans_scan_filter_cfg);

    ret = syscfg_write(CFG_TRANS_SCAN_FILTER_INFO, (uint8_t *)&trans_scan_filter_cfg, sizeof(trans_scan_filter_cfg));
    if (ret != sizeof(trans_scan_filter_cfg)) {
        log_info("scan_filter_cfg save fail, ret=%d", ret);
        return -1;
    }
    return 0;
}

void trans_scan_filter_cfg_load(void)
{
    trans_scan_filter_cfg_t tmp;
    trans_scan_filter_cfg_v1_t tmp_v1;
    int ret;

    trans_scan_filter_cfg_runtime_reset();
    ret = syscfg_read(CFG_TRANS_SCAN_FILTER_INFO, (uint8_t *)&tmp, sizeof(tmp));
    if (ret == sizeof(tmp) && trans_scan_filter_cfg_storage_is_valid(&tmp)) {
        memcpy(&trans_scan_filter_cfg, &tmp, sizeof(trans_scan_filter_cfg));
        log_info("scan_filter_cfg load ok, enable=%u, school_id=0x%04x, device_id=0x%04x",
                 trans_scan_filter_cfg.enable, trans_scan_filter_cfg.school_id, trans_scan_filter_cfg.device_id);
        return;
    }

    ret = syscfg_read(CFG_TRANS_SCAN_FILTER_INFO, (uint8_t *)&tmp_v1, sizeof(tmp_v1));
    if (ret == sizeof(tmp_v1) && trans_scan_filter_cfg_storage_is_valid_v1(&tmp_v1)) {
        trans_scan_filter_cfg.school_id = tmp_v1.school_id;
        trans_scan_filter_cfg.device_id = TCFG_TRANS_SCAN_DEVICE_FILTER_DEFAULT_ID;
        trans_scan_filter_cfg.enable = tmp_v1.enable;
        trans_scan_filter_cfg.valid = tmp_v1.valid;
        trans_scan_filter_cfg.check = trans_scan_filter_cfg_calc_check(&trans_scan_filter_cfg);
        (void)trans_scan_filter_cfg_save();
        log_info("scan_filter_cfg migrate v1->v2, enable=%u, school_id=0x%04x, device_id=0x%04x",
                 trans_scan_filter_cfg.enable, trans_scan_filter_cfg.school_id, trans_scan_filter_cfg.device_id);
        return;
    }

    log_info("scan_filter_cfg load miss/invalid, ret=%d, use_default enable=%u, school_id=0x%04x, device_id=0x%04x",
             ret, trans_scan_filter_cfg.enable, trans_scan_filter_cfg.school_id, trans_scan_filter_cfg.device_id);
}
#endif

#if CONFIG_BT_GATT_SERVER_NUM
static uint8_t trans_uuid_cfg_calc_check(const trans_ibeacon_cfg_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint8_t sum = 0;
    uint16_t i;

    if (!cfg) {
        return 0;
    }
    for (i = 0; i < (sizeof(trans_ibeacon_cfg_t) - 1); i++) {
        sum += p[i];
    }
    return sum;
}

static uint8_t trans_uuid_cfg_calc_check_v1(const trans_ibeacon_cfg_v1_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint8_t sum = 0;
    uint16_t i;

    if (!cfg) {
        return 0;
    }
    for (i = 0; i < (sizeof(trans_ibeacon_cfg_v1_t) - 1); i++) {
        sum += p[i];
    }
    return sum;
}

static uint8_t trans_uuid_cfg_calc_check_v2(const trans_ibeacon_cfg_v2_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint8_t sum = 0;
    uint16_t i;

    if (!cfg) {
        return 0;
    }
    for (i = 0; i < (sizeof(trans_ibeacon_cfg_v2_t) - 1); i++) {
        sum += p[i];
    }
    return sum;
}

static uint8_t trans_uuid_cfg_calc_check_v3(const trans_ibeacon_cfg_v3_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint8_t sum = 0;
    uint16_t i;

    if (!cfg) {
        return 0;
    }
    for (i = 0; i < (sizeof(trans_ibeacon_cfg_v3_t) - 1); i++) {
        sum += p[i];
    }
    return sum;
}

static void trans_uuid_cfg_runtime_reset(void)
{
    memset(&trans_ibeacon_cfg, 0, sizeof(trans_ibeacon_cfg));
    trans_ibeacon_cfg.tag = TRANS_IBEACON_CFG_TAG;
    trans_ibeacon_cfg.version = TRANS_IBEACON_CFG_VERSION;
    trans_ibeacon_cfg.len = sizeof(trans_ibeacon_cfg);
    trans_ibeacon_cfg.tx_power = TRANS_IBEACON_DEFAULT_TX;
    trans_ibeacon_cfg.profile = TRANS_UUID_PROFILE_PAIR;
    trans_ibeacon_cfg.group_id = 0x0000;
    trans_ibeacon_cfg.school_id = 0x0000;
    trans_ibeacon_cfg.device_id = 0x0000;
    trans_ibeacon_cfg.lock = 0;
    trans_ibeacon_cfg.valid = 0;
    trans_ibeacon_cfg.check = trans_uuid_cfg_calc_check(&trans_ibeacon_cfg);
    trans_uuid_ready = 0;
}

static void trans_uuid_cfg_runtime_default(void)
{
#if TCFG_TRANS_UUID_AUTO_DERIVE_FROM_MAC
    const uint8_t *mac = bt_get_mac_addr();
    static const uint8_t prefix[10] = {'A', 'W', '3', '1', 'N', 'B', 'L', 'E', 'U', 'U'};

    if (mac) {
        memcpy(trans_ibeacon_cfg.uuid, prefix, sizeof(prefix));
        memcpy(&trans_ibeacon_cfg.uuid[10], mac, 6);
        trans_ibeacon_cfg.major = ((uint16_t)mac[4] << 8) | mac[5];
        trans_ibeacon_cfg.minor = ((uint16_t)mac[2] << 8) | mac[3];
        trans_ibeacon_cfg.tx_power = TRANS_IBEACON_DEFAULT_TX;
        trans_ibeacon_cfg.profile = TCFG_TRANS_INTERCOM_MODE_ENABLE ? TRANS_UUID_PROFILE_GROUP : TRANS_UUID_PROFILE_PAIR;
        trans_ibeacon_cfg.group_id = 0x0000;
        trans_ibeacon_cfg.school_id = 0x0000;
        trans_ibeacon_cfg.device_id = 0x0000;
        trans_ibeacon_cfg.valid = TRANS_IBEACON_CFG_VALID;
        trans_uuid_ready = 1;
        trans_ibeacon_cfg.check = trans_uuid_cfg_calc_check(&trans_ibeacon_cfg);
    }
#endif
}

static uint8_t trans_uuid_cfg_storage_is_valid(const trans_ibeacon_cfg_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->tag != TRANS_IBEACON_CFG_TAG) {
        return 0;
    }
    if (cfg->version != TRANS_IBEACON_CFG_VERSION) {
        return 0;
    }
    if (cfg->len != sizeof(trans_ibeacon_cfg_t)) {
        return 0;
    }
    if ((cfg->profile != TRANS_UUID_PROFILE_PAIR) && (cfg->profile != TRANS_UUID_PROFILE_GROUP)) {
        return 0;
    }
    if (cfg->check != trans_uuid_cfg_calc_check(cfg)) {
        return 0;
    }
    return 1;
}

static uint8_t trans_uuid_cfg_storage_is_valid_v1(const trans_ibeacon_cfg_v1_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->tag != TRANS_IBEACON_CFG_TAG) {
        return 0;
    }
    if (cfg->version != TRANS_IBEACON_CFG_VERSION_V1) {
        return 0;
    }
    if (cfg->len != sizeof(trans_ibeacon_cfg_v1_t)) {
        return 0;
    }
    if (cfg->check != trans_uuid_cfg_calc_check_v1(cfg)) {
        return 0;
    }
    return 1;
}

static uint8_t trans_uuid_cfg_storage_is_valid_v2(const trans_ibeacon_cfg_v2_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->tag != TRANS_IBEACON_CFG_TAG) {
        return 0;
    }
    if (cfg->version != TRANS_IBEACON_CFG_VERSION_V2) {
        return 0;
    }
    if (cfg->len != sizeof(trans_ibeacon_cfg_v2_t)) {
        return 0;
    }
    if (cfg->check != trans_uuid_cfg_calc_check_v2(cfg)) {
        return 0;
    }
    return 1;
}

static uint8_t trans_uuid_cfg_storage_is_valid_v3(const trans_ibeacon_cfg_v3_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->tag != TRANS_IBEACON_CFG_TAG) {
        return 0;
    }
    if (cfg->version != TRANS_IBEACON_CFG_VERSION_V3) {
        return 0;
    }
    if (cfg->len != sizeof(trans_ibeacon_cfg_v3_t)) {
        return 0;
    }
    if (cfg->check != trans_uuid_cfg_calc_check_v3(cfg)) {
        return 0;
    }
    return 1;
}

int trans_uuid_cfg_save(void)
{
    int ret;

    trans_ibeacon_cfg.tag = TRANS_IBEACON_CFG_TAG;
    trans_ibeacon_cfg.version = TRANS_IBEACON_CFG_VERSION;
    trans_ibeacon_cfg.len = sizeof(trans_ibeacon_cfg);
    trans_ibeacon_cfg.check = trans_uuid_cfg_calc_check(&trans_ibeacon_cfg);

    ret = syscfg_write(CFG_TRANS_UUID_INFO, (uint8_t *)&trans_ibeacon_cfg, sizeof(trans_ibeacon_cfg));
    if (ret != sizeof(trans_ibeacon_cfg)) {
        log_info("uuid_cfg save fail, ret=%d", ret);
        return -1;
    }
    return 0;
}

void trans_uuid_cfg_load(void)
{
    trans_ibeacon_cfg_t tmp;
    trans_ibeacon_cfg_v3_t tmp_v3;
    trans_ibeacon_cfg_v2_t tmp_v2;
    trans_ibeacon_cfg_v1_t tmp_v1;
    int ret;

    trans_uuid_cfg_runtime_reset();
    ret = syscfg_read(CFG_TRANS_UUID_INFO, (uint8_t *)&tmp, sizeof(tmp));
    if (ret == sizeof(tmp) && trans_uuid_cfg_storage_is_valid(&tmp)) {
        memcpy(&trans_ibeacon_cfg, &tmp, sizeof(trans_ibeacon_cfg));
        trans_uuid_ready = (trans_ibeacon_cfg.valid == TRANS_IBEACON_CFG_VALID) ? 1 : 0;
        log_info("uuid_cfg load ok, ready=%u, lock=%u, profile=%u,group_id=0x%04x,school_id=0x%04x,device_id=0x%04x",
                 trans_uuid_ready, trans_ibeacon_cfg.lock, trans_ibeacon_cfg.profile, trans_ibeacon_cfg.group_id,
                 trans_ibeacon_cfg.school_id, trans_ibeacon_cfg.device_id);
        return;
    }

    ret = syscfg_read(CFG_TRANS_UUID_INFO, (uint8_t *)&tmp_v3, sizeof(tmp_v3));
    if (ret == sizeof(tmp_v3) && trans_uuid_cfg_storage_is_valid_v3(&tmp_v3)) {
        memcpy(trans_ibeacon_cfg.uuid, tmp_v3.uuid, sizeof(trans_ibeacon_cfg.uuid));
        trans_ibeacon_cfg.major = tmp_v3.major;
        trans_ibeacon_cfg.minor = tmp_v3.minor;
        trans_ibeacon_cfg.tx_power = tmp_v3.tx_power;
        trans_ibeacon_cfg.profile = TRANS_UUID_PROFILE_PAIR;
        trans_ibeacon_cfg.group_id = 0x0000;
        trans_ibeacon_cfg.school_id = tmp_v3.school_id;
        trans_ibeacon_cfg.device_id = tmp_v3.device_id;
        trans_ibeacon_cfg.lock = tmp_v3.lock;
        trans_ibeacon_cfg.valid = tmp_v3.valid;
        trans_uuid_ready = (trans_ibeacon_cfg.valid == TRANS_IBEACON_CFG_VALID) ? 1 : 0;
        trans_ibeacon_cfg.check = trans_uuid_cfg_calc_check(&trans_ibeacon_cfg);
        (void)trans_uuid_cfg_save();
        log_info("uuid_cfg migrate v3->v4, ready=%u, profile=%u,group_id=0x0000,school_id=0x%04x,device_id=0x%04x",
                 trans_uuid_ready, trans_ibeacon_cfg.profile, trans_ibeacon_cfg.school_id, trans_ibeacon_cfg.device_id);
        return;
    }

    ret = syscfg_read(CFG_TRANS_UUID_INFO, (uint8_t *)&tmp_v2, sizeof(tmp_v2));
    if (ret == sizeof(tmp_v2) && trans_uuid_cfg_storage_is_valid_v2(&tmp_v2)) {
        memcpy(trans_ibeacon_cfg.uuid, tmp_v2.uuid, sizeof(trans_ibeacon_cfg.uuid));
        trans_ibeacon_cfg.major = tmp_v2.major;
        trans_ibeacon_cfg.minor = tmp_v2.minor;
        trans_ibeacon_cfg.tx_power = tmp_v2.tx_power;
        trans_ibeacon_cfg.profile = TRANS_UUID_PROFILE_PAIR;
        trans_ibeacon_cfg.group_id = 0x0000;
        trans_ibeacon_cfg.school_id = tmp_v2.school_id;
        trans_ibeacon_cfg.device_id = 0x0000;
        trans_ibeacon_cfg.lock = tmp_v2.lock;
        trans_ibeacon_cfg.valid = tmp_v2.valid;
        trans_uuid_ready = (trans_ibeacon_cfg.valid == TRANS_IBEACON_CFG_VALID) ? 1 : 0;
        trans_ibeacon_cfg.check = trans_uuid_cfg_calc_check(&trans_ibeacon_cfg);
        (void)trans_uuid_cfg_save();
        log_info("uuid_cfg migrate v2->v4, ready=%u, profile=%u,group_id=0x0000,school_id=0x%04x,device_id=0x0000",
                 trans_uuid_ready, trans_ibeacon_cfg.profile, trans_ibeacon_cfg.school_id);
        return;
    }

    ret = syscfg_read(CFG_TRANS_UUID_INFO, (uint8_t *)&tmp_v1, sizeof(tmp_v1));
    if (ret == sizeof(tmp_v1) && trans_uuid_cfg_storage_is_valid_v1(&tmp_v1)) {
        memcpy(trans_ibeacon_cfg.uuid, tmp_v1.uuid, sizeof(trans_ibeacon_cfg.uuid));
        trans_ibeacon_cfg.major = tmp_v1.major;
        trans_ibeacon_cfg.minor = tmp_v1.minor;
        trans_ibeacon_cfg.tx_power = tmp_v1.tx_power;
        trans_ibeacon_cfg.profile = TRANS_UUID_PROFILE_PAIR;
        trans_ibeacon_cfg.group_id = 0x0000;
        trans_ibeacon_cfg.school_id = 0x0000;
        trans_ibeacon_cfg.device_id = 0x0000;
        trans_ibeacon_cfg.lock = tmp_v1.lock;
        trans_ibeacon_cfg.valid = tmp_v1.valid;
        trans_uuid_ready = (trans_ibeacon_cfg.valid == TRANS_IBEACON_CFG_VALID) ? 1 : 0;
        trans_ibeacon_cfg.check = trans_uuid_cfg_calc_check(&trans_ibeacon_cfg);
        (void)trans_uuid_cfg_save();
        log_info("uuid_cfg migrate v1->v4, ready=%u, profile=%u,group_id=0x0000,school_id=0x0000,device_id=0x0000",
                 trans_uuid_ready, trans_ibeacon_cfg.profile);
        return;
    }

    log_info("uuid_cfg load miss/invalid, ret=%d", ret);
    trans_uuid_cfg_runtime_default();
    if (trans_uuid_ready) {
        (void)trans_uuid_cfg_save();
    }
}

#if !TCFG_TRANS_INTERCOM_MODE_ENABLE
static uint8_t trans_badge_cfg_calc_check(const trans_badge_cfg_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint8_t sum = 0;
    uint16_t i;

    if (!cfg) {
        return 0;
    }
    for (i = 0; i < (sizeof(trans_badge_cfg_t) - 1); i++) {
        sum += p[i];
    }
    return sum;
}

static void trans_badge_cfg_runtime_reset(void)
{
    memset(&trans_badge_cfg, 0, sizeof(trans_badge_cfg));
    trans_badge_cfg.tag = TRANS_BADGE_CFG_TAG;
    trans_badge_cfg.version = TRANS_BADGE_CFG_VERSION;
    trans_badge_cfg.len = sizeof(trans_badge_cfg);
    trans_badge_cfg.lock = 0;
    trans_badge_cfg.valid = 0;
    trans_badge_cfg.check = trans_badge_cfg_calc_check(&trans_badge_cfg);
    memset(trans_badge_adv_data, 0, sizeof(trans_badge_adv_data));
    trans_badge_adv_data_valid = 0;
}

static uint8_t trans_badge_cfg_storage_is_valid(const trans_badge_cfg_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    if (cfg->tag != TRANS_BADGE_CFG_TAG) {
        return 0;
    }
    if (cfg->version != TRANS_BADGE_CFG_VERSION) {
        return 0;
    }
    if (cfg->len != sizeof(trans_badge_cfg_t)) {
        return 0;
    }
    if (cfg->check != trans_badge_cfg_calc_check(cfg)) {
        return 0;
    }
    return 1;
}

int trans_badge_cfg_save(void)
{
    int ret;

    trans_badge_cfg.tag = TRANS_BADGE_CFG_TAG;
    trans_badge_cfg.version = TRANS_BADGE_CFG_VERSION;
    trans_badge_cfg.len = sizeof(trans_badge_cfg);
    trans_badge_cfg.check = trans_badge_cfg_calc_check(&trans_badge_cfg);

    ret = syscfg_write(CFG_TRANS_BADGE_INFO, (uint8_t *)&trans_badge_cfg, sizeof(trans_badge_cfg));
    if (ret != sizeof(trans_badge_cfg)) {
        log_info("badge_cfg save fail, ret=%d", ret);
        return -1;
    }
    return 0;
}

void trans_badge_cfg_load(void)
{
    trans_badge_cfg_t tmp;
    int ret;

    trans_badge_cfg_runtime_reset();
    ret = syscfg_read(CFG_TRANS_BADGE_INFO, (uint8_t *)&tmp, sizeof(tmp));
    if (ret == sizeof(tmp) && trans_badge_cfg_storage_is_valid(&tmp)) {
        memcpy(&trans_badge_cfg, &tmp, sizeof(trans_badge_cfg));
        if (trans_badge_cfg.valid == TRANS_BADGE_CFG_VALID) {
            memcpy(trans_badge_adv_data, trans_badge_cfg.payload, sizeof(trans_badge_adv_data));
            trans_badge_adv_data_valid = 1;
        }
        log_info("badge_cfg load ok, valid=%u, lock=%u", trans_badge_adv_data_valid, trans_badge_cfg.lock);
        return;
    }
    log_info("badge_cfg load miss/invalid, ret=%d", ret);
}
#else
int trans_badge_cfg_save(void)
{
    return 0;
}

void trans_badge_cfg_load(void)
{
    memset(&trans_badge_cfg, 0, sizeof(trans_badge_cfg));
    memset(trans_badge_adv_data, 0, sizeof(trans_badge_adv_data));
    trans_badge_adv_data_valid = 0;
}
#endif
#endif

#endif
