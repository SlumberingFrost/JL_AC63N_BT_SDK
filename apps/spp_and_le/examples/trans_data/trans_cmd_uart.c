#include "system/includes.h"
#include "app_config.h"
#include "trans_cmd_uart.h"

#if CONFIG_APP_SPP_LE

#define LOG_TAG_CONST       TRANS_CMD_UART
#define LOG_TAG             "[TRANS_CMD_UART]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"

#define TRANS_CMD_UART_BAUD_RATE         115200
#define TRANS_CMD_UART_RX_SIZE           0x100
#define TRANS_CMD_UART_LINE_BUF_SIZE     0x100

extern void uart_db_regiest_recieve_callback(void *rx_cb);

struct trans_cmd_uart_cfg {
    uint32_t baudrate;
    int flowcontrol;
    const char *dev_name;
};

struct trans_cmd_uart_hdl {
    struct trans_cmd_uart_cfg config;
    void *pRxBuffer;
    uint16_t data_length;
};

static struct trans_cmd_uart_hdl trans_cmd_uart_hdl;
#define __this (&trans_cmd_uart_hdl)

static uint8_t trans_cmd_uart_rx_cache[TRANS_CMD_UART_RX_SIZE] __attribute__((aligned(4)));
static uint8_t trans_cmd_uart_rx_buffer[TRANS_CMD_UART_RX_SIZE] __attribute__((aligned(4)));
static uint8_t trans_cmd_uart_line_buffer[TRANS_CMD_UART_LINE_BUF_SIZE];

static uint16_t trans_cmd_uart_line_len;
static uint8_t trans_cmd_uart_line_overflow;
static uint8_t trans_cmd_uart_inited;
static uint8_t trans_cmd_uart_rx_overflow;
static trans_cmd_uart_line_handler_t trans_cmd_uart_line_handler;

static int trans_cmd_uart_send_unknown_cmd(void)
{
    /*
     * 产品级保护：
     * 当前阶段仅接入“控制串口骨架”，业务命令解析稍后再补。
     * 对完整串口帧立即返回 UNKNOWN，可避免产测/联调工具长期等待而误判设备死机。
     */
    return trans_cmd_uart_send_text("ERR:UNKNOWN_CMD\r\n");
}

static void trans_cmd_uart_reset_line_state(void)
{
    trans_cmd_uart_line_len = 0;
    trans_cmd_uart_line_overflow = 0;
    trans_cmd_uart_line_buffer[0] = '\0';
}

static void trans_cmd_uart_dispatch_line(const uint8_t *line, uint16_t len)
{
    int ret = -1;

    if (!line || !len) {
        return;
    }

    log_info("cmd[%u]: %s", len, line);

    if (trans_cmd_uart_line_handler) {
        ret = trans_cmd_uart_line_handler(line, len);
    }

    if (ret) {
        trans_cmd_uart_send_unknown_cmd();
    }
}

static void trans_cmd_uart_finish_line(void)
{
    if (trans_cmd_uart_line_overflow) {
        /*
         * 产品级保护：
         * 超长帧直接整条丢弃到行结束，避免前一条残留字节串到下一条命令，
         * 造成量产阶段“偶发误命中”的隐蔽问题。
         */
        log_info("drop oversize line");
        trans_cmd_uart_send_unknown_cmd();
    } else if (trans_cmd_uart_line_len) {
        trans_cmd_uart_line_buffer[trans_cmd_uart_line_len] = '\0';
        trans_cmd_uart_dispatch_line(trans_cmd_uart_line_buffer, trans_cmd_uart_line_len);
    }

    trans_cmd_uart_reset_line_state();
}

static void trans_cmd_uart_feed_bytes(const uint8_t *packet, uint16_t len)
{
    uint16_t i;
    uint8_t ch;

    if (!packet || !len) {
        return;
    }

    for (i = 0; i < len; i++) {
        ch = packet[i];

        if (ch == '\r' || ch == '\n') {
            trans_cmd_uart_finish_line();
            continue;
        }

        if (trans_cmd_uart_line_overflow) {
            continue;
        }

        if (trans_cmd_uart_line_len >= (TRANS_CMD_UART_LINE_BUF_SIZE - 1)) {
            trans_cmd_uart_line_overflow = 1;
            continue;
        }

        /*
         * 产品级约束：
         * 控制口以“按行分帧”为唯一入口，后续文本命令和 JSON 命令都共享这一层，
         * 这样可以保证串口半包、粘包时的行为稳定且可预测。
         */
        trans_cmd_uart_line_buffer[trans_cmd_uart_line_len++] = ch;
    }
}

static void trans_cmd_uart_notify_rx_event(void)
{
    struct sys_event e;

    e.type = SYS_DEVICE_EVENT;
    e.arg  = (void *)DEVICE_EVENT_FROM_AT_UART;
    e.u.dev.event = 0;
    e.u.dev.value = 0;
    sys_event_notify(&e);
}

static void trans_cmd_uart_uart0_rx_callback(u8 *packet, u32 size)
{
    uint16_t remain_len;
    uint16_t copy_len;

    if (!trans_cmd_uart_inited || !packet || !size) {
        return;
    }

    /*
     * 产品级约束：
     * PA1/PA2 是当前产品唯一对主控开放的串口。UART0 的底层接收已经由 debug 串口持有，
     * 这里仅挂接 RX hook，把数据复制到软件缓存后再切到应用线程处理，避免在中断上下文里做文本解析。
     */
    local_irq_disable();

    if (__this->data_length >= TRANS_CMD_UART_RX_SIZE) {
        trans_cmd_uart_rx_overflow = 1;
        local_irq_enable();
        trans_cmd_uart_notify_rx_event();
        return;
    }

    remain_len = TRANS_CMD_UART_RX_SIZE - __this->data_length;
    copy_len = (size > remain_len) ? remain_len : size;
    memcpy(&trans_cmd_uart_rx_cache[__this->data_length], packet, copy_len);
    __this->data_length += copy_len;
    if (copy_len < size) {
        trans_cmd_uart_rx_overflow = 1;
    }

    local_irq_enable();
    trans_cmd_uart_notify_rx_event();
}

void trans_cmd_uart_register_line_handler(trans_cmd_uart_line_handler_t handler)
{
    trans_cmd_uart_line_handler = handler;
}

int trans_cmd_uart_send_bytes(const uint8_t *packet, uint16_t len)
{
    if (!packet || !len || !trans_cmd_uart_inited) {
        return -1;
    }

    while (len--) {
        putchar(*packet++);
    }
    return 0;
}

int trans_cmd_uart_send_text(const char *text)
{
    if (!text) {
        return -1;
    }

    return trans_cmd_uart_send_bytes((const uint8_t *)text, strlen(text));
}

int trans_cmd_uart_init(void)
{
    if (trans_cmd_uart_inited) {
        return 0;
    }

    memset(__this, 0, sizeof(*__this));
    __this->pRxBuffer = trans_cmd_uart_rx_buffer;
    __this->config.baudrate = TRANS_CMD_UART_BAUD_RATE;
    __this->config.flowcontrol = 0;
    __this->config.dev_name = "trans-cmd-uart0";

    trans_cmd_uart_reset_line_state();
    trans_cmd_uart_rx_overflow = 0;
    uart_db_regiest_recieve_callback(trans_cmd_uart_uart0_rx_callback);

    trans_cmd_uart_inited = 1;
    log_info("ready: uart0 baud=%u, tx=%u, rx=%u",
             TRANS_CMD_UART_BAUD_RATE, TCFG_UART0_TX_PORT, TCFG_UART0_RX_PORT);
    return 0;
}

void trans_cmd_uart_rx_handler(void)
{
    uint16_t rx_len;

    if (!trans_cmd_uart_inited) {
        return;
    }

    local_irq_disable();
    rx_len = __this->data_length;
    if (rx_len > TRANS_CMD_UART_RX_SIZE) {
        rx_len = TRANS_CMD_UART_RX_SIZE;
    }

    if (rx_len) {
        memcpy(__this->pRxBuffer, trans_cmd_uart_rx_cache, rx_len);
    }
    __this->data_length = 0;
    local_irq_enable();

    if (!rx_len) {
        return;
    }

    if (trans_cmd_uart_rx_overflow) {
        /*
         * 产品级保护：
         * 主控发送超过软件缓存上限时，直接丢弃当前缓存并复位分帧状态，
         * 避免残缺指令被误拼成下一条合法命令。
         */
        log_error("rx overflow:%u", rx_len);
        trans_cmd_uart_rx_overflow = 0;
        trans_cmd_uart_reset_line_state();
        return;
    }

    log_info("rx[%u]", rx_len);
    log_info_hexdump(__this->pRxBuffer, rx_len);
    trans_cmd_uart_feed_bytes(__this->pRxBuffer, rx_len);
}

#endif
