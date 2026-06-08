// binary representation
// attribute size in bytes (16), flags(16), handle (16), uuid (16/128), value(...)

#ifndef _BLE_TRANS_H
#define _BLE_TRANS_H

#include <stdint.h>
#include "app_config.h"
#include "gatt_common/le_gatt_common.h"

#ifndef CONFIG_APP_LE_TRANS
#define CONFIG_APP_LE_TRANS CONFIG_APP_SPP_LE
#endif

#ifndef TCFG_TRANS_INTERCOM_MODE_ENABLE
#define TCFG_TRANS_INTERCOM_MODE_ENABLE      0
#endif
#ifndef TCFG_TRANS_UUID_AUTO_DERIVE_FROM_MAC
#define TCFG_TRANS_UUID_AUTO_DERIVE_FROM_MAC 1
#endif
#ifndef TCFG_TRANS_UUID_REQUIRE_READY
#define TCFG_TRANS_UUID_REQUIRE_READY        1
#endif
#ifndef TCFG_TRANS_SCAN_SCHOOL_FILTER_DEFAULT_ENABLE
#define TCFG_TRANS_SCAN_SCHOOL_FILTER_DEFAULT_ENABLE 1
#endif
#ifndef TCFG_TRANS_SCAN_SCHOOL_FILTER_DEFAULT_ID
#define TCFG_TRANS_SCAN_SCHOOL_FILTER_DEFAULT_ID     0x0000
#endif
#ifndef TCFG_TRANS_SCAN_DEVICE_FILTER_DEFAULT_ID
#define TCFG_TRANS_SCAN_DEVICE_FILTER_DEFAULT_ID     0x0000
#endif

#define TRANS_UUID_PROFILE_PAIR  0
#define TRANS_UUID_PROFILE_GROUP 1

extern const gatt_client_cfg_t trans_client_init_cfg;
void trans_client_init(void);
void trans_client_exit(void);
int trans_client_search_remote_profile(u16 conn_handle);
int trans_client_search_remote_stop(u16 conn_handle);

void trans_ios_services_init(void);
void trans_ios_services_exit(void);
int trans_badge_payload_set(const uint8_t *payload, uint8_t len);
uint8_t trans_badge_payload_is_valid(void);
int trans_badge_payload_get(uint8_t *payload, uint8_t len);
int trans_badge_lock_get(uint8_t *lock, uint8_t *ready);
int trans_badge_lock_set(uint8_t lock);
uint8_t trans_badge_is_locked(void);
int trans_uuid_info_get(uint8_t *uuid, uint16_t *major, uint16_t *minor, int8_t *tx_power,
                        uint8_t *profile, uint16_t *group_id, uint16_t *school_id, uint16_t *device_id,
                        uint8_t *lock, uint8_t *ready);
int trans_uuid_info_set_ex(const uint8_t *uuid, uint16_t major, uint16_t minor, int8_t tx_power,
                           uint8_t profile, uint16_t group_id, uint16_t school_id, uint16_t device_id,
                           uint8_t apply_adv_now);
int trans_uuid_info_set(const uint8_t *uuid, uint16_t major, uint16_t minor, int8_t tx_power,
                        uint8_t profile, uint16_t group_id, uint16_t school_id, uint16_t device_id);
int trans_uuid_lock_set(uint8_t lock);
uint8_t trans_uuid_is_ready(void);
uint8_t trans_uuid_is_locked(void);
int trans_scan_filter_get(uint16_t *school_id, uint16_t *device_id, uint8_t *enable);
int trans_scan_filter_set(uint16_t school_id, uint16_t device_id, uint8_t enable);
int trans_broadcast_restart_now(void);
void trans_set_params_stop_broadcast_timer_restart(void);
void trans_set_params_stop_broadcast_timer_cancel(void);
#endif
