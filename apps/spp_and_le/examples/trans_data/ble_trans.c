/*********************************************************************************************
    *   Filename        : .c

    *   Description     :

    *   Author          : JM

    *   Email           : zh-jieli.com

    *   Last modifiled  : 2017-01-17 11:14

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
#include "JL_rcsp_api.h"
#include "custom_cfg.h"
#include "btstack/btstack_event.h"
#include "gatt_common/le_gatt_common.h"
#include "ble_trans.h"
#include "ble_trans_cfg.h"
#include "ble_trans_profile.h"
#include "trans_cmd_uart.h"

#if CONFIG_APP_SPP_LE

#if LE_DEBUG_PRINT_EN
#define log_info(x, ...)  printf("[BLE_TRANS]" x " ", ## __VA_ARGS__)
#define log_info_hexdump  put_buf

#else
#define log_info(...)
#define log_info_hexdump(...)
#endif

#define trans_boot_trace(fmt, ...) printf("[SCAN-BOOT] " fmt "\r\n", ##__VA_ARGS__)

#if CONFIG_APP_SPP_LE
#ifndef CONFIG_APP_LE_TRANS
#define CONFIG_APP_LE_TRANS CONFIG_APP_SPP_LE
#endif
#endif

#define TRANS_SCHOOL_ID_SVC_UUID16           0xAF31

/*
 打开流控使能后,确定使能接口 att_server_flow_enable 被调用
 然后使用过程 通过接口 att_server_flow_hold 来控制流控开关
 注意:流控只能控制对方使用带响应READ/WRITE等命令方式
 例如:ATT_WRITE_REQUEST = 0x12
 */
#define ATT_DATA_RECIEVT_FLOW      0//流控功能使能


//测试NRF连接,工具不会主动发起交换流程,需要手动操作; 但设备可配置主动发起MTU长度交换请求
#define ATT_MTU_REQUEST_ENALBE     0    /*配置1,就是设备端主动发起交换*/

//检测对方的系统类型，ios or 非ios
#define ATT_CHECK_REMOTE_REQUEST_ENALBE     0    /*配置1,就是设备端主动检查*/

//ATT发送的包长,    note: 23 <=need >= MTU
#define ATT_LOCAL_MTU_SIZE        (512) /*一般是主机发起交换,如果主机没有发起,设备端也可以主动发起(ATT_MTU_REQUEST_ENALBE set 1)*/

//ATT缓存的buffer支持缓存数据包个数
#define ATT_PACKET_NUMS_MAX       (2)

//ATT缓存的buffer大小,  note: need >= 23,可修改
#define ATT_SEND_CBUF_SIZE        (ATT_PACKET_NUMS_MAX * (ATT_PACKET_HEAD_SIZE + ATT_LOCAL_MTU_SIZE))

// 广播周期 (unit:0.625ms)
#define ADV_INTERVAL_MIN          (160 * 5)//
#define TRANS_SET_PARAMS_BROADCAST_WINDOW_MS 10000

#define TEST_TRANS_CHANNEL_DATA      0 /*测试记录收发数据速度*/
#define TEST_TRANS_NOTIFY_HANDLE     ATT_CHARACTERISTIC_ae02_01_VALUE_HANDLE /*主动发送hanlde,为空则不测试发数*/
#if CONFIG_BLE_HIGH_SPEED
#define TEST_TRANS_TIMER_MS          5
#else
#define TEST_TRANS_TIMER_MS          500
#endif
#define TEST_PAYLOAD_LEN            (244)/*发送配PDU长度是251的包*/

static u32 trans_recieve_test_count;
static u32 trans_send_test_count;

//---------------
//连接参数更新请求设置
//是否使能参数请求更新,0--disable, 1--enable
static uint8_t trans_connection_update_enable = 1; ///0--disable, 1--enable

//请求的参数数组表,排队方式请求;哪组对方接受就用那组
static const struct conn_update_param_t trans_connection_param_table[] = {

#if CONFIG_BLE_HIGH_SPEED
    {6, 12,  10, 400},// ios fast
#endif

    {16, 24, 10, 600},//11
    {12, 28, 10, 600},//3.7
    {8,  20, 10, 600},
};

//共可用的参数组数
#define CONN_PARAM_TABLE_CNT      (sizeof(trans_connection_param_table)/sizeof(struct conn_update_param_t))

#define EIR_TAG_STRING   0xd6, 0x05, 0x08, 0x00, 'J', 'L', 'A', 'I', 'S', 'D','K'
static const char user_tag_string[] = {EIR_TAG_STRING};

//定义的产品信息,for test
#define  PNP_VID_SOURCE   0x02
#define  PNP_VID          0x05ac //0x05d6
#define  PNP_PID          0x022C //
#define  PNP_PID_VERSION  0x011b //1.1.11

static const u8 trans_PNP_ID[] = {PNP_VID_SOURCE, PNP_VID & 0xFF, PNP_VID >> 8, PNP_PID & 0xFF, PNP_PID >> 8, PNP_PID_VERSION & 0xFF, PNP_PID_VERSION >> 8};

static u8  trans_test_read_write_buf[4];
static u16 trans_con_handle;
#if CONFIG_BT_GATT_SERVER_NUM
static u8  trans_adv_data[ADV_RSP_PACKET_MAX];//max is 31
static u8  trans_scan_rsp_data[ADV_RSP_PACKET_MAX];//max is 31
static adv_cfg_t trans_server_adv_config;
#endif
static int trans_set_params_stop_adv_timer;

#define TRANS_IBEACON_CFG_VALID 0xA5
//-------------------------------------------------------------------------------------
static uint16_t trans_att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static int trans_att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static int trans_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param);
extern void uart_db_regiest_recieve_callback(void *rx_cb);
//-------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------
//输入passkey 加密
#define PASSKEY_ENABLE                     0

static const sm_cfg_t trans_sm_init_config = {
    .slave_security_auto_req = 0,
    .slave_set_wait_security = 0,

#if PASSKEY_ENABLE
    .io_capabilities = IO_CAPABILITY_DISPLAY_ONLY,
#else
    .io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
#endif

    .authentication_req_flags = SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION,
    .min_key_size = 7,
    .max_key_size = 16,
    .sm_cb_packet_handler = NULL,
};

const gatt_server_cfg_t trans_server_init_cfg = {
    .att_read_cb = &trans_att_read_callback,
    .att_write_cb = &trans_att_write_callback,
    .event_packet_handler = &trans_event_packet_handler,
};

static gatt_ctrl_t trans_gatt_control_block = {
    //public
    .mtu_size = ATT_LOCAL_MTU_SIZE,
    .cbuffer_size = ATT_SEND_CBUF_SIZE,
    .multi_dev_flag	= 0,

    //config
#if CONFIG_BT_GATT_SERVER_NUM
    .server_config = &trans_server_init_cfg,
#else
    .server_config = NULL,
#endif

#if CONFIG_BT_GATT_CLIENT_NUM
    .client_config = &trans_client_init_cfg,
#else
    .client_config = NULL,
#endif

#if CONFIG_BT_SM_SUPPORT_ENABLE
    .sm_config = &trans_sm_init_config,
#else
    .sm_config = NULL,
#endif
    //cbk,event handle
    .hci_cb_packet_handler = NULL,
};


#define TEST_AUDIO_DATA_UPLOAD       0//测试文件上传

#if TEST_AUDIO_DATA_UPLOAD
static const u8 test_audio_data_file[1024] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9
};

/*************************************************************************************************/
/*!
 *  \brief      测试上传文件
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
#define AUDIO_ONE_PACKET_LEN  128
static void trans_test_send_audio_data(int init_flag)
{
    static u32 send_pt = 0;
    static u32 start_flag = 0;

    if (!trans_con_handle) {
        return;
    }

    if (init_flag) {
        log_info("audio send init\n");
        send_pt = 0;
        start_flag = 1;
    }

    if (!start_flag) {
        return;
    }

    u32 file_size = sizeof(test_audio_data_file);
    u8 *file_ptr = test_audio_data_file;

    if (send_pt >= file_size) {
        log_info("audio send Complete\n");
        start_flag = 0;
        return;
    }

    u32 send_len = file_size - send_pt;
    if (send_len > AUDIO_ONE_PACKET_LEN) {
        send_len = AUDIO_ONE_PACKET_LEN;
    }

    while (1) {
        if (ble_comm_cbuffer_vaild_len(trans_con_handle) > send_len) {
            log_info("audio send %08x\n", send_pt);
            if (ble_comm_att_send_data(trans_con_handle, ATT_CHARACTERISTIC_ae3c_01_VALUE_HANDLE, &file_ptr[send_pt], send_len, ATT_OP_AUTO_READ_CCC)) {
                log_info("audio send fail!\n");
                break;
            } else {
                send_pt += send_len;
            }
        } else {
            break;
        }
    }
}

#endif

/*************************************************************************************************/
/*!
 *  \brief      串口接收转发到BLE
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void trans_uart_rx_to_ble(u8 *packet, u32 size)
{
    if (trans_con_handle && ble_comm_att_check_send(trans_con_handle, size) &&
        ble_gatt_server_characteristic_ccc_get(trans_con_handle, ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE)) {
        ble_comm_att_send_data(trans_con_handle, ATT_CHARACTERISTIC_ae02_01_VALUE_HANDLE, packet, size, ATT_OP_AUTO_READ_CCC);
    } else {
        log_info("drop uart data!!!\n");
    }
}

/*************************************************************************************************/
/*!
 *  \brief      发送请求连接参数表
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void trans_send_connetion_updata_deal(u16 conn_handle)
{
    if (trans_connection_update_enable) {
        if (0 == ble_gatt_server_connetion_update_request(conn_handle, trans_connection_param_table, CONN_PARAM_TABLE_CNT)) {
            trans_connection_update_enable = 0;
        }
    }
}

/*************************************************************************************************/
/*!
 *  \brief      回连状态，使能所有profile
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note      配对绑定的方式，主机回连不是在使能server的通知开关，需要自己打开
 */
/*************************************************************************************************/
static void trans_resume_all_ccc_enable(u16 conn_handle, u8 update_request)
{
    log_info("resume_all_ccc_enable\n");

#if RCSP_BTMATE_EN
    ble_gatt_server_characteristic_ccc_set(conn_handle, ATT_CHARACTERISTIC_ae02_02_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
#endif
    ble_gatt_server_characteristic_ccc_set(conn_handle, ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(conn_handle, ATT_CHARACTERISTIC_ae04_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(conn_handle, ATT_CHARACTERISTIC_ae05_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_INDICATE);
    ble_gatt_server_characteristic_ccc_set(conn_handle, ATT_CHARACTERISTIC_ae3c_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);

    if (update_request) {
        trans_send_connetion_updata_deal(conn_handle);
    }
}

/*************************************************************************************************/
/*!
 *  \brief      反馈检查对方的操作系统
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note 参考识别手机系统
 */
/*************************************************************************************************/
static void trans_check_remote_result(u16 con_handle, remote_type_e remote_type)
{
    char *str;
    if (REMOTE_TYPE_IOS == remote_type) {
        str = "is";
    } else {
        str = "not";
    }

    log_info("trans_check %02x:remote_type= %02x, %s ios", con_handle, remote_type, str);
}

/*************************************************************************************************/
/*!
 *  \brief      处理gatt 返回的事件（hci && gatt）
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static int trans_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param)
{
    /* log_info("event: %02x,size= %d\n",event,size); */

    switch (event) {

    case GATT_COMM_EVENT_CAN_SEND_NOW:
#if TEST_AUDIO_DATA_UPLOAD
        trans_test_send_audio_data(0);
#endif
        break;

    case GATT_COMM_EVENT_SERVER_INDICATION_COMPLETE:
        log_info("INDICATION_COMPLETE:con_handle= %04x,att_handle= %04x\n", \
                 little_endian_read_16(packet, 0), little_endian_read_16(packet, 2));
        break;


    case GATT_COMM_EVENT_CONNECTION_COMPLETE:
        trans_con_handle = little_endian_read_16(packet, 0);
        trans_connection_update_enable = 1;

        log_info("connection_handle:%04x\n", little_endian_read_16(packet, 0));
        log_info("connection_handle:%04x, rssi= %d\n", trans_con_handle, ble_vendor_get_peer_rssi(trans_con_handle));
        log_info("peer_address_info:");
        put_buf(&ext_param[7], 7);

        log_info("con_interval = %d\n", little_endian_read_16(ext_param, 14 + 0));
        log_info("con_latency = %d\n", little_endian_read_16(ext_param, 14 + 2));
        log_info("cnn_timeout = %d\n", little_endian_read_16(ext_param, 14 + 4));

#if ATT_MTU_REQUEST_ENALBE
        att_server_set_exchange_mtu(trans_con_handle);/*主动请求MTU长度交换*/
#endif

#if TCFG_UART0_RX_PORT != NO_CONFIG_PORT
        /*
         * 产品级约束：
         * 当前项目把 UART0(PA1/PA2) 固定复用为主控配置串口。
         * 因此 BLE 建链后不能再把同一条 UART0 RX hook 改绑成“串口数据直通 BLE”，
         * 否则会抢走 trans_cmd_uart 的控制入口。
         */
#endif

#if CONFIG_BT_GATT_CLIENT_NUM
#if CONFIG_BT_SM_SUPPORT_ENABLE == 0
        trans_client_search_remote_profile(trans_con_handle);
#endif
#endif

#if ATT_CHECK_REMOTE_REQUEST_ENALBE
        att_server_set_check_remote(trans_con_handle, trans_check_remote_result);
#endif
        break;

    case GATT_COMM_EVENT_DISCONNECT_COMPLETE:
        log_info("disconnect_handle:%04x,reason= %02x\n", little_endian_read_16(packet, 0), packet[2]);
        if (trans_con_handle == little_endian_read_16(packet, 0)) {
#if CONFIG_BT_GATT_CLIENT_NUM
            trans_client_search_remote_stop(trans_con_handle);
#endif
            trans_con_handle = 0;
        }
        break;

    case GATT_COMM_EVENT_ENCRYPTION_CHANGE:
        log_info("ENCRYPTION_CHANGE:handle=%04x,state=%d,process =%d", little_endian_read_16(packet, 0), packet[2], packet[3]);
        if (packet[3] == LINK_ENCRYPTION_RECONNECT) {
            trans_resume_all_ccc_enable(little_endian_read_16(packet, 0), 1);
        }

#if CONFIG_BT_GATT_CLIENT_NUM
        trans_client_search_remote_profile(trans_con_handle);
#endif
        break;

    case GATT_COMM_EVENT_CONNECTION_UPDATE_COMPLETE:
        log_info("conn_param update_complete:%04x\n", little_endian_read_16(packet, 0));
        log_info("update_interval = %d\n", little_endian_read_16(ext_param, 6 + 0));
        log_info("update_latency = %d\n", little_endian_read_16(ext_param, 6 + 2));
        log_info("update_timeout = %d\n", little_endian_read_16(ext_param, 6 + 4));
        break;

    case GATT_COMM_EVENT_CONNECTION_UPDATE_REQUEST_RESULT:
        break;

    case GATT_COMM_EVENT_MTU_EXCHANGE_COMPLETE:
        log_info("con_handle= %02x, ATT MTU = %u\n", little_endian_read_16(packet, 0), little_endian_read_16(packet, 2));
        break;

    case GATT_COMM_EVENT_SERVER_STATE:
        break;

    case GATT_COMM_EVENT_SM_PASSKEY_INPUT: {
        u32 *key = little_endian_read_32(packet, 2);
        *key = 888888;
        r_printf("input_key:%6u\n", *key);
    }
    break;

    default:
        break;
    }
    return 0;
}


/*************************************************************************************************/
/*!
 *  \brief      处理client 读操作
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note      profile的读属性uuid 有配置 DYNAMIC 关键字，就有read_callback 回调
 */
/*************************************************************************************************/
// ATT Client Read Callback for Dynamic Data
// - if buffer == NULL, don't copy data, just return size of value
// - if buffer != NULL, copy data and return number bytes copied
// @param con_handle of hci le connection
// @param attribute_handle to be read
// @param offset defines start of attribute value
// @param buffer
// @param buffer_size
static uint16_t trans_att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    uint16_t  att_value_len = 0;
    uint16_t handle = att_handle;

    log_info("read_callback,conn_handle =%04x, handle=%04x,buffer=%08x\n", connection_handle, handle, (u32)buffer);

    switch (handle) {
    case ATT_CHARACTERISTIC_2a00_01_VALUE_HANDLE: {
        char *gap_name = ble_comm_get_gap_name();
        att_value_len = strlen(gap_name);

        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }

        if (buffer) {
            memcpy(buffer, &gap_name[offset], buffer_size);
            att_value_len = buffer_size;
            log_info("\n------read gap_name: %s\n", gap_name);
        }
    }
    break;

    case ATT_CHARACTERISTIC_ae10_01_VALUE_HANDLE:
        att_value_len = sizeof(trans_test_read_write_buf);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }

        if (buffer) {
            memcpy(buffer, &trans_test_read_write_buf[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_ae04_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_ae05_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_ae3c_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a05_01_CLIENT_CONFIGURATION_HANDLE:
        if (buffer) {
            buffer[0] = ble_gatt_server_characteristic_ccc_get(connection_handle, handle);
            buffer[1] = 0;
        }
        att_value_len = 2;
        break;

    case ATT_CHARACTERISTIC_2a50_01_VALUE_HANDLE:
        log_info("read PnP_ID\n");
        att_value_len = sizeof(trans_PNP_ID);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &trans_PNP_ID[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    default:
        break;
    }

    log_info("att_value_len= %d\n", att_value_len);
    return att_value_len;
}


/*************************************************************************************************/
/*!
 *  \brief      处理client write操作
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note      profile的写属性uuid 有配置 DYNAMIC 关键字，就有write_callback 回调
 */
/*************************************************************************************************/
// ATT Client Write Callback for Dynamic Data
// @param con_handle of hci le connection
// @param attribute_handle to be written
// @param transaction - ATT_TRANSACTION_MODE_NONE for regular writes, ATT_TRANSACTION_MODE_ACTIVE for prepared writes and ATT_TRANSACTION_MODE_EXECUTE
// @param offset into the value - used for queued writes and long attributes
// @param buffer
// @param buffer_size
// @param signature used for signed write commmands
// @returns 0 if write was ok, ATT_ERROR_PREPARE_QUEUE_FULL if no space in queue, ATT_ERROR_INVALID_OFFSET if offset is larger than max buffer

static int trans_att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    int result = 0;
    u16 tmp16;

    u16 handle = att_handle;

#if !TEST_TRANS_CHANNEL_DATA
    log_info("write_callback,conn_handle =%04x, handle =%04x,size =%d\n", connection_handle, handle, buffer_size);
#endif

    switch (handle) {

    case ATT_CHARACTERISTIC_2a00_01_VALUE_HANDLE:
        break;

    case ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_ae04_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_ae05_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_ae3c_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a05_01_CLIENT_CONFIGURATION_HANDLE:
        trans_send_connetion_updata_deal(connection_handle);
        log_info("\n------write ccc:%04x,%02x\n", handle, buffer[0]);
        ble_gatt_server_characteristic_ccc_set(connection_handle, handle, buffer[0]);
        break;

    case ATT_CHARACTERISTIC_ae10_01_VALUE_HANDLE:
        tmp16 = sizeof(trans_test_read_write_buf);
        if ((offset >= tmp16) || (offset + buffer_size) > tmp16) {
            break;
        }
        memcpy(&trans_test_read_write_buf[offset], buffer, buffer_size);
        log_info("\n-ae10_rx(%d):", buffer_size);
        put_buf(buffer, buffer_size);
        break;

    case ATT_CHARACTERISTIC_ae01_01_VALUE_HANDLE:

#if TEST_TRANS_CHANNEL_DATA
        /* putchar('R'); */
        trans_recieve_test_count += buffer_size;
        break;
#endif

        log_info("\n-ae01_rx(%d):", buffer_size);
        put_buf(buffer, buffer_size);

        //收发测试，自动发送收到的数据;for test
        if (ble_comm_att_check_send(connection_handle, buffer_size) &&
            ble_gatt_server_characteristic_ccc_get(trans_con_handle, ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE)) {
            log_info("-loop send1\n");
            ble_comm_att_send_data(connection_handle, ATT_CHARACTERISTIC_ae02_01_VALUE_HANDLE, buffer, buffer_size, ATT_OP_AUTO_READ_CCC);
        }
        break;

    case ATT_CHARACTERISTIC_ae03_01_VALUE_HANDLE:
        log_info("\n-ae03_rx(%d):", buffer_size);
        put_buf(buffer, buffer_size);

        //收发测试，自动发送收到的数据;for test
        if (ble_comm_att_check_send(connection_handle, buffer_size) && \
            ble_gatt_server_characteristic_ccc_get(trans_con_handle, ATT_CHARACTERISTIC_ae05_01_CLIENT_CONFIGURATION_HANDLE)) {
            log_info("-loop send2\n");
            ble_comm_att_send_data(connection_handle, ATT_CHARACTERISTIC_ae05_01_VALUE_HANDLE, buffer, buffer_size, ATT_OP_AUTO_READ_CCC);
        }
        break;

#if RCSP_BTMATE_EN
    case ATT_CHARACTERISTIC_ae02_02_CLIENT_CONFIGURATION_HANDLE:
        ble_op_latency_skip(connection_handle, 0xffff); //
        ble_gatt_server_set_update_send(connection_handle, ATT_CHARACTERISTIC_ae02_02_VALUE_HANDLE, ATT_OP_AUTO_READ_CCC);
#endif
        /* trans_send_connetion_updata_deal(connection_handle); */
        log_info("------write ccc:%04x,%02x\n", handle, buffer[0]);
        ble_gatt_server_characteristic_ccc_set(connection_handle, handle, buffer[0]);
        break;

#if RCSP_BTMATE_EN
    case ATT_CHARACTERISTIC_ae01_02_VALUE_HANDLE:
        log_info("rcsp_read:%x\n", buffer_size);
        ble_gatt_server_receive_update_data(NULL, buffer, buffer_size);
        break;
#endif

    case ATT_CHARACTERISTIC_ae3b_01_VALUE_HANDLE:
        log_info("\n-ae3b_rx(%d):", buffer_size);
        put_buf(buffer, buffer_size);

#if TEST_AUDIO_DATA_UPLOAD
        if (0 == memcmp(buffer, "start", 5)) {
            trans_test_send_audio_data(1);
        }
#endif
        break;

    default:
        break;
    }
    return 0;
}


/*************************************************************************************************/
/*!
 *  \brief      计算当前offset还能容纳的AD payload长度（不含Length+Type两个字节）
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static uint8_t trans_adv_calc_payload_room(uint16_t offset)
{
    if ((offset + 2) > ADV_RSP_PACKET_MAX) {
        return 0;
    }
    return (uint8_t)(ADV_RSP_PACKET_MAX - offset - 2);
}

/*************************************************************************************************/
/*!
 *  \brief      以“写前检查”方式追加EIR整型字段
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static int trans_adv_eir_append_val(uint8_t *buf, uint16_t *offset, uint8_t data_type, uint32_t val, uint8_t val_size)
{
    uint16_t cur_offset;
    uint16_t need_len;

    if (!buf || !offset) {
        return -1;
    }
    cur_offset = *offset;
    need_len = (uint16_t)val_size + 2;
    if ((cur_offset + need_len) > ADV_RSP_PACKET_MAX) {
        return -1;
    }
    *offset = cur_offset + make_eir_packet_val(&buf[cur_offset], cur_offset, data_type, val, val_size);
    return 0;
}

/*************************************************************************************************/
/*!
 *  \brief      以“写前检查”方式追加EIR数据字段
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static int trans_adv_eir_append_data(uint8_t *buf, uint16_t *offset, uint8_t data_type, const uint8_t *data, uint8_t data_len)
{
    uint16_t cur_offset;
    uint16_t need_len;

    if (!buf || !offset || !data) {
        return -1;
    }
    cur_offset = *offset;
    need_len = (uint16_t)data_len + 2;
    if ((cur_offset + need_len) > ADV_RSP_PACKET_MAX) {
        return -1;
    }
    *offset = cur_offset + make_eir_packet_data(&buf[cur_offset], cur_offset, data_type, (void *)data, data_len);
    return 0;
}

/*************************************************************************************************/
/*!
 *  \brief      组织adv包数据，放入buff
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
#if CONFIG_BT_GATT_SERVER_NUM
static int trans_make_set_adv_data(void)
{
    uint16_t offset = 0;
    uint8_t *buf = trans_adv_data;
    uint8_t ibeacon[25];

    memset(buf, 0, sizeof(trans_adv_data));

#if DOUBLE_BT_SAME_MAC
    if (trans_adv_eir_append_val(buf, &offset, HCI_EIR_DATATYPE_FLAGS,
                                 FLAGS_GENERAL_DISCOVERABLE_MODE | FLAGS_LE_AND_EDR_SAME_CONTROLLER, 1)) {
        return -1;

#else
    if (trans_adv_eir_append_val(buf, &offset, HCI_EIR_DATATYPE_FLAGS,
                                 FLAGS_GENERAL_DISCOVERABLE_MODE | FLAGS_EDR_NOT_SUPPORTED, 1)) {
        return -1;
    }
#endif

    if (trans_uuid_ready) {
        /*
         * 产品协议约束：
         * ADV 保持标准 iBeacon 主体，避免把 school/device/badge 再塞回主广播，
         * 这样扫描侧可以稳定解析 UUID/Major/Minor/TxPower，不受后续业务字段扩展影响。
         */
        ibeacon[0] = 0x4C;
        ibeacon[1] = 0x00;
        ibeacon[2] = 0x02;
        ibeacon[3] = 0x15;
        memcpy(&ibeacon[4], trans_ibeacon_cfg.uuid, 16);
        ibeacon[20] = (uint8_t)((trans_ibeacon_cfg.major >> 8) & 0xFF);
        ibeacon[21] = (uint8_t)(trans_ibeacon_cfg.major & 0xFF);
        ibeacon[22] = (uint8_t)((trans_ibeacon_cfg.minor >> 8) & 0xFF);
        ibeacon[23] = (uint8_t)(trans_ibeacon_cfg.minor & 0xFF);
        ibeacon[24] = (uint8_t)trans_ibeacon_cfg.tx_power;
        if (trans_adv_eir_append_data(buf, &offset, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA,
                                      ibeacon, sizeof(ibeacon))) {
            return -1;
        }
    }

    if (offset > ADV_RSP_PACKET_MAX) {
        puts("***trans_adv_data overflow!!!!!!\n");
        return -1;
    }
    trans_server_adv_config.adv_data_len = (uint8_t)offset;
    trans_server_adv_config.adv_data = trans_adv_data;
    return 0;
}

/*************************************************************************************************/
/*!
 *  \brief      组织rsp包数据，放入buff
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static int trans_make_set_rsp_data(void)
{
    uint16_t offset = 0;
    uint8_t *buf = trans_scan_rsp_data;
    uint8_t school_data[6];
    uint8_t vaild_len;
    const char *gap_name = ble_comm_get_gap_name();
    uint8_t name_len = gap_name ? strlen(gap_name) : 0;

    memset(buf, 0, sizeof(trans_scan_rsp_data));

    /*
     * 产品协议约束：
     * RSP 里的优先级固定为 BADGE -> SCHOOL_ID/DEVICE_ID -> GAP Name。
     * 当 31 字节空间不足时，业务字段必须优先保留，名称最后截断。
     */
    if (trans_badge_adv_data_valid) {
        vaild_len = trans_adv_calc_payload_room(offset);
        if (sizeof(trans_badge_adv_data) <= vaild_len) {
            if (trans_adv_eir_append_data(buf, &offset, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA,
                                          trans_badge_adv_data, sizeof(trans_badge_adv_data))) {
                return -1;
            }
        }
    }

    if (trans_uuid_ready) {
        school_data[0] = (uint8_t)(TRANS_SCHOOL_ID_SVC_UUID16 & 0xFF);
        school_data[1] = (uint8_t)((TRANS_SCHOOL_ID_SVC_UUID16 >> 8) & 0xFF);
        school_data[2] = (uint8_t)((trans_ibeacon_cfg.school_id >> 8) & 0xFF);
        school_data[3] = (uint8_t)(trans_ibeacon_cfg.school_id & 0xFF);
        school_data[4] = (uint8_t)((trans_ibeacon_cfg.device_id >> 8) & 0xFF);
        school_data[5] = (uint8_t)(trans_ibeacon_cfg.device_id & 0xFF);
        vaild_len = trans_adv_calc_payload_room(offset);
        if (sizeof(school_data) <= vaild_len) {
            if (trans_adv_eir_append_data(buf, &offset, HCI_EIR_DATATYPE_SERVICE_DATA,
                                          school_data, sizeof(school_data))) {
                return -1;
            }
        }
    }

    if (name_len) {
        vaild_len = trans_adv_calc_payload_room(offset);
        if (name_len > vaild_len) {
            name_len = vaild_len;
        }
        if (name_len) {
            if (trans_adv_eir_append_data(buf, &offset, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME,
                                          (const uint8_t *)gap_name, name_len)) {
                return -1;
            }
        }
    }

    if (offset > ADV_RSP_PACKET_MAX) {
        puts("***rsp_data overflow!!!!!!\n");
        return -1;
    }

    trans_server_adv_config.rsp_data_len = (uint8_t)offset;
    trans_server_adv_config.rsp_data = trans_scan_rsp_data;
    return 0;
}

/*************************************************************************************************/
/*!
 *  \brief      配置广播参数
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note      开广播前配置都有效
 */
/*************************************************************************************************/
static int trans_adv_config_set(void)
{
    int ret = 0;
    ret |= trans_make_set_adv_data();
    ret |= trans_make_set_rsp_data();

    trans_server_adv_config.adv_interval = ADV_INTERVAL_MIN;
    trans_server_adv_config.adv_auto_do = trans_uuid_ready ? 1 : 0;
    trans_server_adv_config.adv_type = ADV_IND;
    trans_server_adv_config.adv_channel = ADV_CHANNEL_ALL;
    memset(trans_server_adv_config.direct_address_info, 0, 7);

    if (ret) {
        log_info("adv_setup_init fail!!!\n");
        return -1;
    }
    ble_gatt_server_set_adv_config(&trans_server_adv_config);
    return 0;
}

/*************************************************************************************************/
/*!
 *  \brief      server init初始化
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void trans_server_init(void)
{
    ble_gatt_server_set_profile(trans_profile_data, sizeof(trans_profile_data));
    trans_uuid_cfg_load();
    trans_badge_cfg_load();
    (void)trans_adv_config_set();
}
#endif

/*************************************************************************************************/
/*!
 *  \brief      断开连接
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void trans_disconnect(void)
{
    log_info("%s", __FUNCTION__);
    if (trans_con_handle) {
        ble_comm_disconnect(trans_con_handle);
    }
}


/*************************************************************************************************/
/*!
 *  \brief      协议栈初始化前调用
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/

void bt_ble_before_start_init(void)
{
    trans_boot_trace("bt_ble_before_start_init: enter");
    log_info("%s", __FUNCTION__);
    ble_comm_init(&trans_gatt_control_block);
    trans_boot_trace("bt_ble_before_start_init: ble_comm_init done");
}


/*************************************************************************************************/
/*!
 *  \brief
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void trans_test_send_data(void)
{
#if TEST_TRANS_CHANNEL_DATA
    static u32 count = 0;
    static u32 send_index;

    int i, ret = 0;
    int send_len = TEST_PAYLOAD_LEN;
    u32 time_index_max = 1000 / TEST_TRANS_TIMER_MS;

    if (!trans_con_handle) {
        return;
    }

    send_index++;

#if TEST_TRANS_NOTIFY_HANDLE
    do {
        if (ble_comm_att_check_send(trans_con_handle, send_len) && ble_gatt_server_characteristic_ccc_get(trans_con_handle, TEST_TRANS_NOTIFY_HANDLE + 1)) {
            count++;
            ret = ble_comm_att_send_data(trans_con_handle, TEST_TRANS_NOTIFY_HANDLE, &count, send_len, ATT_OP_AUTO_READ_CCC);
            if (!ret) {
                /* putchar('T'); */
                trans_send_test_count += send_len;
            }
        } else {
            break;
        }
    } while (ret == 0);
#endif

    if (send_index >= time_index_max) {
        if (trans_send_test_count) {
            log_info(">>>>>> send_rate= %d byte/s\n", trans_send_test_count);
        }
        send_index = 0;
        trans_send_test_count = 0;

        if (trans_recieve_test_count) {
            log_info("<<<<<<< recieve_rate= %d byte/s\n", trans_recieve_test_count);
            trans_recieve_test_count = 0;
        }
    }
#endif
}
/*************************************************************************************************/
/*!
 *  \brief      控制应答对方READ/WRITE行为的响应包RESPONSE的回复
 *
 *  \param      [in]流控使能 en: 1-停止收数 or 0-继续收数
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
int ble_trans_flow_enable(u8 en)
{
    int ret = -1;

#if ATT_DATA_RECIEVT_FLOW
    if (trans_con_handle) {
        att_server_flow_hold(trans_con_handle, en);
        ret = 0;
        log_info("ble_trans_flow_enable:%d\n", en);
    }
#endif

    return ret;
}

//for test
static void timer_trans_flow_test(void)
{
    static u8 sw = 0;
    if (trans_con_handle) {
        sw = !sw;
        ble_trans_flow_enable(sw);
    }
}

/*************************************************************************************************/
/*!
 *  \brief      模块初始化
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void bt_ble_init(void)
{
    trans_boot_trace("bt_ble_init: enter");
    ble_comm_set_config_name(bt_get_local_name(), 0);
    trans_con_handle = 0;

#if CONFIG_BT_GATT_SERVER_NUM
    trans_boot_trace("bt_ble_init: trans_server_init begin");
    trans_server_init();
    trans_boot_trace("bt_ble_init: trans_server_init done");
#endif

#if CONFIG_BT_GATT_CLIENT_NUM
    trans_boot_trace("bt_ble_init: trans_scan_filter_cfg_load begin");
    trans_scan_filter_cfg_load();
    trans_boot_trace("bt_ble_init: trans_scan_filter_cfg_load done");
    trans_boot_trace("bt_ble_init: trans_client_init begin");
    trans_client_init();
    trans_boot_trace("bt_ble_init: trans_client_init done");
#endif

#if CONFIG_BT_GATT_CLIENT_NUM && CONFIG_BT_SM_SUPPORT_ENABLE
    trans_boot_trace("bt_ble_init: trans_ios_services_init begin");
    trans_ios_services_init();
    trans_boot_trace("bt_ble_init: trans_ios_services_init done");
#endif

#if ATT_DATA_RECIEVT_FLOW
    log_info("att_server_flow_enable\n");
    att_server_flow_enable(1);
//    sys_timer_add(0, timer_trans_flow_test, 5000);
#endif

    trans_boot_trace("bt_ble_init: ble_module_enable(1) begin");
    ble_module_enable(1);
    trans_boot_trace("bt_ble_init: ble_module_enable(1) done");

#if CONFIG_BT_GATT_CLIENT_NUM && TCFG_AC6323A_PRODUCT_MODE_IS_SCAN
    trans_boot_trace("bt_ble_init: ble_gatt_client_scan_enable(1) begin");
    ble_gatt_client_scan_enable(1);
    trans_boot_trace("bt_ble_init: ble_gatt_client_scan_enable(1) done");
    log_info("scan-only: force scan enable");
#endif

#if TEST_TRANS_CHANNEL_DATA
    if (TEST_TRANS_TIMER_MS < 10) {
        sys_s_hi_timer_add(0, trans_test_send_data, TEST_TRANS_TIMER_MS);
    } else {
        sys_timer_add(0, trans_test_send_data, TEST_TRANS_TIMER_MS);
    }
#endif

}

/*************************************************************************************************/
/*!
 *  \brief      模块退出
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void bt_ble_exit(void)
{
    log_info("%s\n", __FUNCTION__);

#if CONFIG_BT_GATT_CLIENT_NUM
    trans_client_exit();
#endif

#if CONFIG_BT_GATT_CLIENT_NUM && CONFIG_BT_SM_SUPPORT_ENABLE
    trans_ios_services_exit();
#endif

    trans_set_params_stop_broadcast_timer_cancel();
    ble_module_enable(0);
    ble_comm_exit();
}

/*************************************************************************************************/
/*!
 *  \brief      模块开发使能
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void ble_module_enable(u8 en)
{
    ble_comm_module_enable(en);
}

int trans_uuid_info_get(uint8_t *uuid, uint16_t *major, uint16_t *minor, int8_t *tx_power,
                        uint8_t *profile, uint16_t *group_id, uint16_t *school_id, uint16_t *device_id,
                        uint8_t *lock, uint8_t *ready)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    (void)uuid;
    (void)major;
    (void)minor;
    (void)tx_power;
    (void)profile;
    (void)group_id;
    (void)school_id;
    (void)device_id;
    (void)lock;
    (void)ready;
    return -1;
#else
    if (uuid) {
        memcpy(uuid, trans_ibeacon_cfg.uuid, sizeof(trans_ibeacon_cfg.uuid));
    }
    if (major) {
        *major = trans_ibeacon_cfg.major;
    }
    if (minor) {
        *minor = trans_ibeacon_cfg.minor;
    }
    if (tx_power) {
        *tx_power = trans_ibeacon_cfg.tx_power;
    }
    if (profile) {
        *profile = trans_ibeacon_cfg.profile;
    }
    if (group_id) {
        *group_id = trans_ibeacon_cfg.group_id;
    }
    if (school_id) {
        *school_id = trans_ibeacon_cfg.school_id;
    }
    if (device_id) {
        *device_id = trans_ibeacon_cfg.device_id;
    }
    if (lock) {
        *lock = trans_ibeacon_cfg.lock;
    }
    if (ready) {
        *ready = trans_uuid_ready;
    }
    return 0;
#endif
}

static int trans_adv_apply_now(void)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    return -1;
#else
    ble_gatt_server_adv_enable(0);
    if (trans_adv_config_set()) {
        return -1;
    }
    if (trans_uuid_ready) {
        ble_gatt_server_adv_enable(1);
    }
    return 0;
#endif
}

static int trans_broadcast_stop_now(void)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    return -1;
#else
    trans_server_adv_config.adv_auto_do = 0;
    ble_gatt_server_set_adv_config(&trans_server_adv_config);
    ble_gatt_server_adv_enable(0);
    return 0;
#endif
}

static void trans_set_params_stop_adv_timeout(void *priv)
{
    (void)priv;

    trans_set_params_stop_adv_timer = 0;
    (void)trans_broadcast_stop_now();
    (void)trans_cmd_uart_send_text("{\"cmd\":\"stopBroadcast\"}\r\n");
}

int trans_broadcast_restart_now(void)
{
    return trans_adv_apply_now();
}

void trans_set_params_stop_broadcast_timer_restart(void)
{
    if (trans_set_params_stop_adv_timer) {
        sys_timeout_del(trans_set_params_stop_adv_timer);
        trans_set_params_stop_adv_timer = 0;
    }
    trans_set_params_stop_adv_timer = sys_timeout_add(NULL, trans_set_params_stop_adv_timeout,
                                    TRANS_SET_PARAMS_BROADCAST_WINDOW_MS);
}

void trans_set_params_stop_broadcast_timer_cancel(void)
{
    if (trans_set_params_stop_adv_timer) {
        sys_timeout_del(trans_set_params_stop_adv_timer);
        trans_set_params_stop_adv_timer = 0;
    }
}

int trans_badge_payload_set(const uint8_t *payload, uint8_t len)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    (void)payload;
    (void)len;
    return -4;
#else
    if (!payload || len != sizeof(trans_badge_adv_data)) {
        return -1;
    }
    if (trans_badge_cfg.lock) {
        return -2;
    }

    if (trans_badge_adv_data_valid &&
        !memcmp(trans_badge_adv_data, payload, sizeof(trans_badge_adv_data))) {
        return 0;
    }

    memcpy(trans_badge_adv_data, payload, sizeof(trans_badge_adv_data));
    trans_badge_adv_data_valid = 1;
    memcpy(trans_badge_cfg.payload, payload, sizeof(trans_badge_cfg.payload));
    trans_badge_cfg.valid = TRANS_BADGE_CFG_VALID;
    if (trans_badge_cfg_save()) {
        return -5;
    }
    if (trans_adv_apply_now()) {
        return -3;
    }
    return 0;
#endif
}

uint8_t trans_badge_payload_is_valid(void)
{
#if CONFIG_BT_GATT_SERVER_NUM
    return trans_badge_adv_data_valid ? 1 : 0;
#else
    return 0;
#endif
}

int trans_badge_payload_get(uint8_t *payload, uint8_t len)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    (void)payload;
    (void)len;
    return -4;
#else
    if (!payload || len < sizeof(trans_badge_adv_data)) {
        return -1;
    }
    if (!trans_badge_adv_data_valid) {
        return -2;
    }
    memcpy(payload, trans_badge_adv_data, sizeof(trans_badge_adv_data));
    return sizeof(trans_badge_adv_data);
#endif
}

int trans_badge_lock_get(uint8_t *lock, uint8_t *ready)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    (void)lock;
    (void)ready;
    return -4;
#else
    if (lock) {
        *lock = trans_badge_cfg.lock ? 1 : 0;
    }
    if (ready) {
        *ready = trans_badge_adv_data_valid ? 1 : 0;
    }
    return 0;
#endif
}

uint8_t trans_badge_is_locked(void)
{
#if CONFIG_BT_GATT_SERVER_NUM
    return trans_badge_cfg.lock ? 1 : 0;
#else
    return 0;
#endif
}

int trans_badge_lock_set(uint8_t lock)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    (void)lock;
    return -4;
#else
    uint8_t old_lock;

    if (lock > 1) {
        return -1;
    }
    if (!trans_badge_adv_data_valid) {
        return -5;
    }
    if (trans_badge_cfg.lock == lock) {
        return 0;
    }

    old_lock = trans_badge_cfg.lock;
    trans_badge_cfg.lock = lock;
    if (trans_badge_cfg_save()) {
        trans_badge_cfg.lock = old_lock;
        return -3;
    }
    return 0;
#endif
}

int trans_uuid_info_set_ex(const uint8_t *uuid, uint16_t major, uint16_t minor, int8_t tx_power,
                           uint8_t profile, uint16_t group_id, uint16_t school_id, uint16_t device_id,
                           uint8_t apply_adv_now)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    (void)uuid;
    (void)major;
    (void)minor;
    (void)tx_power;
    (void)profile;
    (void)group_id;
    (void)school_id;
    (void)device_id;
    (void)apply_adv_now;
    return -4;
#else
    if (!uuid) {
        return -1;
    }
    if (profile != TRANS_UUID_PROFILE_PAIR) {
        return -1;
    }
    if (trans_ibeacon_cfg.lock) {
        return -2;
    }
    if (trans_uuid_ready &&
        !memcmp(trans_ibeacon_cfg.uuid, uuid, sizeof(trans_ibeacon_cfg.uuid)) &&
        trans_ibeacon_cfg.major == major &&
        trans_ibeacon_cfg.minor == minor &&
        trans_ibeacon_cfg.tx_power == tx_power &&
        trans_ibeacon_cfg.profile == profile &&
        trans_ibeacon_cfg.group_id == group_id &&
        trans_ibeacon_cfg.school_id == school_id &&
        trans_ibeacon_cfg.device_id == device_id) {
        return 0;
    }

    memcpy(trans_ibeacon_cfg.uuid, uuid, sizeof(trans_ibeacon_cfg.uuid));
    trans_ibeacon_cfg.major = major;
    trans_ibeacon_cfg.minor = minor;
    trans_ibeacon_cfg.tx_power = tx_power;
    trans_ibeacon_cfg.profile = profile;
    trans_ibeacon_cfg.group_id = group_id;
    trans_ibeacon_cfg.school_id = school_id;
    trans_ibeacon_cfg.device_id = device_id;
    trans_ibeacon_cfg.valid = TRANS_IBEACON_CFG_VALID;
    trans_uuid_ready = 1;

    if (trans_uuid_cfg_save()) {
        return -3;
    }
    if (apply_adv_now && trans_adv_apply_now()) {
        return -3;
    }
    return 0;
#endif
}

int trans_uuid_info_set(const uint8_t *uuid, uint16_t major, uint16_t minor, int8_t tx_power,
                        uint8_t profile, uint16_t group_id, uint16_t school_id, uint16_t device_id)
{
    return trans_uuid_info_set_ex(uuid, major, minor, tx_power, profile, group_id, school_id, device_id, 1);
}

int trans_uuid_lock_set(uint8_t lock)
{
#if !CONFIG_BT_GATT_SERVER_NUM
    (void)lock;
    return -4;
#else
    uint8_t old_lock;

    if (lock > 1) {
        return -1;
    }
    if (!trans_uuid_ready) {
        return -5;
    }
    if (trans_ibeacon_cfg.lock == lock) {
        return 0;
    }

    old_lock = trans_ibeacon_cfg.lock;
    trans_ibeacon_cfg.lock = lock;
    if (trans_uuid_cfg_save()) {
        trans_ibeacon_cfg.lock = old_lock;
        return -3;
    }
    return 0;
#endif
}

uint8_t trans_uuid_is_locked(void)
{
#if CONFIG_BT_GATT_SERVER_NUM
    return trans_ibeacon_cfg.lock ? 1 : 0;
#endif
    return 0;
}

int trans_scan_filter_get(uint16_t *school_id, uint16_t *device_id, uint8_t *enable)
{
#if !CONFIG_BT_GATT_CLIENT_NUM
    (void)school_id;
    (void)device_id;
    (void)enable;
    return -2;
#else
    if (school_id) {
        *school_id = trans_scan_filter_cfg.school_id;
    }
    if (device_id) {
        *device_id = trans_scan_filter_cfg.device_id;
    }
    if (enable) {
        *enable = trans_scan_filter_cfg.enable ? 1 : 0;
    }
    return 0;
#endif
}

int trans_scan_filter_set(uint16_t school_id, uint16_t device_id, uint8_t enable)
{
#if !CONFIG_BT_GATT_CLIENT_NUM
    (void)school_id;
    (void)device_id;
    (void)enable;
    return -2;
#else
    if (enable > 1) {
        return -1;
    }
    trans_scan_filter_cfg.school_id = school_id;
    trans_scan_filter_cfg.device_id = device_id;
    trans_scan_filter_cfg.enable = enable;
    if (trans_scan_filter_cfg_save()) {
        return -3;
    }
    return 0;
#endif
}

uint8_t trans_uuid_is_ready(void)
{
#if CONFIG_BT_GATT_SERVER_NUM
    return trans_uuid_ready;
#else
    return 0;
#endif
}

/*************************************************************************************************/
/*!
 *  \brief      testbox 按键测试
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void ble_server_send_test_key_num(u8 key_num)
{
    if (trans_con_handle) {
        if (get_remote_test_flag()) {
            ble_op_test_key_num(trans_con_handle, key_num);
        } else {
            log_info("-not conn testbox\n");
        }
    }
}


#endif
