#ifndef __BLE_TRANS_CFG_H__
#define __BLE_TRANS_CFG_H__

#include "includes.h"
#include "app_config.h"

#if CONFIG_BT_GATT_SERVER_NUM
#define TRANS_IBEACON_CFG_VALID 0xA5
#define TRANS_BADGE_CFG_VALID   0xA5

typedef struct {
    uint16_t tag;
    uint8_t version;
    uint8_t len;
    uint8_t payload[10];
    uint8_t lock;
    uint8_t valid;
    uint8_t check;
} _GNU_PACKED_ trans_badge_cfg_t;

typedef struct {
    uint16_t tag;
    uint8_t version;
    uint8_t len;
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t tx_power;
    uint8_t profile;
    uint16_t group_id;
    uint16_t school_id;
    uint16_t device_id;
    uint8_t lock;
    uint8_t valid;
    uint8_t check;
} _GNU_PACKED_ trans_ibeacon_cfg_t;

typedef struct {
    uint16_t tag;
    uint8_t version;
    uint8_t len;
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t tx_power;
    uint8_t lock;
    uint8_t valid;
    uint8_t check;
} _GNU_PACKED_ trans_ibeacon_cfg_v1_t;

typedef struct {
    uint16_t tag;
    uint8_t version;
    uint8_t len;
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t tx_power;
    uint16_t school_id;
    uint8_t lock;
    uint8_t valid;
    uint8_t check;
} _GNU_PACKED_ trans_ibeacon_cfg_v2_t;

typedef struct {
    uint16_t tag;
    uint8_t version;
    uint8_t len;
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t tx_power;
    uint16_t school_id;
    uint16_t device_id;
    uint8_t lock;
    uint8_t valid;
    uint8_t check;
} _GNU_PACKED_ trans_ibeacon_cfg_v3_t;

extern uint8_t trans_badge_adv_data[10];
extern uint8_t trans_badge_adv_data_valid;
extern trans_badge_cfg_t trans_badge_cfg;
extern trans_ibeacon_cfg_t trans_ibeacon_cfg;
extern uint8_t trans_uuid_ready;

int trans_badge_cfg_save(void);
void trans_badge_cfg_load(void);
int trans_uuid_cfg_save(void);
void trans_uuid_cfg_load(void);
#endif

#if CONFIG_BT_GATT_CLIENT_NUM
typedef struct {
    uint16_t tag;
    uint8_t version;
    uint8_t len;
    uint16_t school_id;
    uint16_t device_id;
    uint8_t enable;
    uint8_t valid;
    uint8_t check;
} _GNU_PACKED_ trans_scan_filter_cfg_t;

typedef struct {
    uint16_t tag;
    uint8_t version;
    uint8_t len;
    uint16_t school_id;
    uint8_t enable;
    uint8_t valid;
    uint8_t check;
} _GNU_PACKED_ trans_scan_filter_cfg_v1_t;

extern trans_scan_filter_cfg_t trans_scan_filter_cfg;

int trans_scan_filter_cfg_save(void);
void trans_scan_filter_cfg_load(void);
#endif

#endif
