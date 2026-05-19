#ifndef __TRANS_CMD_UART_H__
#define __TRANS_CMD_UART_H__

#include <stdint.h>

typedef int (*trans_cmd_uart_line_handler_t)(const uint8_t *line, uint16_t len);

int trans_cmd_uart_init(void);
void trans_cmd_uart_rx_handler(void);
int trans_cmd_uart_send_bytes(const uint8_t *packet, uint16_t len);
int trans_cmd_uart_send_text(const char *text);
void trans_cmd_uart_register_line_handler(trans_cmd_uart_line_handler_t handler);

#endif
