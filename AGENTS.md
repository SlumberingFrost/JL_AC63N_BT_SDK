# 仓库说明

- 用户要求：在此工作区内不要执行任何编译验证。
- 这包括 `make`、IDE 编译、单文件编译检查、链接验证，以及其他主动触发构建的命令。
- 除非用户后续明确撤销该要求，否则只允许查看日志、阅读代码和修改源码，不运行编译。
- 当回答中引用本工作区内的 SDK 文件、日志、脚本、配置文件时，必须优先使用可点击的 Markdown 本地文件链接，格式为 `[文件名](/绝对路径/文件:行号)`。
- 路径必须指向当前工作区中的实际文件，并尽量附带准确行号，便于用户点击后直接跳转到对应位置。
- 不要只写纯文本文件名、`line 46` 这类不可点击描述；如果能定位到具体位置，就直接给出可点击链接。

## AW6323A 单广播功能步骤 1-15

以下步骤用于在当前 SDK 中实现“普通广播/单广播”功能，并以 `AC6323A + 主控串口` 方案为准。当前约定是：主控只通过 `UART0(PA1/PA2)` 与蓝牙芯片通信，不再依赖 `UART_DB(PA3/PA4)` 作为控制口。

1. 应用边界固定为 `apps/spp_and_le/examples/trans_data`，保持 `spp_le + trans_data` 这条应用链，不切到 `at_com` 或 `at_char_com`。
2. 板型固定为 `CONFIG_BOARD_AC6323A_FMY`，通过 `apps/spp_and_le/board/bd19/board_config.h` 生效。
3. 主控通信串口固定复用 `UART0`，引脚为 `TX=PA1`、`RX=PA2`、波特率 `115200`；该串口同时承载启动日志和后续命令通信。
4. 若当前硬件没有实际 ADKEY 电阻梯，需关闭 `TCFG_ADKEY_ENABLE`，避免 `PA9` 被误判为持续按键，导致日志反复输出 `app_key_evnet`。
5. 控制串口入口统一收敛到 `apps/spp_and_le/examples/trans_data/trans_cmd_uart.c/.h`，不再新开独立的 `UART_DB` 设备。
6. `trans_cmd_uart` 必须复用 `cpu/bd19/setup.c` 中已经打开的 `UART0` RX hook，通过 `uart_db_regiest_recieve_callback()` 挂接接收回调。
7. `trans_cmd_uart` 的接收策略固定为“按行分帧”，以 `\r`/`\n` 结束一条命令；超长命令整条丢弃，并回 `ERR:UNKNOWN_CMD`，避免残包串包。
8. `apps/spp_and_le/examples/trans_data/app_spp_and_le.c` 必须处理 `DEVICE_EVENT_FROM_AT_UART`，并在该事件中调用 `trans_cmd_uart_rx_handler()`。
9. `BT_STATUS_INIT_OK` 时必须调用 `trans_cmd_uart_init()`，保证设备上电后无需 BLE 建链就能接收主控命令。
10. `apps/spp_and_le/examples/trans_data/ble_trans.c` 里旧的“串口数据直通 BLE”测试路径不能再重绑 `UART0` RX hook，否则会抢走主控命令入口。
11. 广播数据组织继续以 `ble_trans.c` 为核心：ADV 维持 iBeacon 主体，RSP 负责承载 `school_id/device_id`，后续如需 `BADGE` 也在这一层补齐。
12. 掉电存储继续以 `ble_trans_cfg.c/.h` 为核心：配置结构、默认值、版本迁移、保存/加载都留在 cfg 层，命令层不直接写 `syscfg`。
13. 运行时接口统一由 `ble_trans.c/.h` 暴露，例如 `UUID/BADGE/LOCK` 的 `get/set` 接口；命令层只调用这些接口，并在修改后立即停播、重组、重开广播。
14. 串口命令实现顺序固定为两阶段：
    第一阶段：`CMD:MODE?`、`CMD:UUID?`、`CMD:SCHOOL_ID=`、`CMD:DEVICE_ID=`、`{"cmd":"setParams",...}`
    第二阶段：`CMD:BADGE?`、`CMD:BADGE=`、`CMD:UUID:LOCK?`、`CMD:UUID:LOCK=`、`CMD:BADGE:LOCK?`、`CMD:BADGE:LOCK=`
15. 当前阶段的联调验收标准：
    上电日志中出现 `ready: uart0 baud=115200, tx=1, rx=2`
    主控向 `PA1/PA2` 发送 `PING\r\n` 后，日志里出现 `rx[6]` 与 `cmd[4]: PING`
    同一串口回 `ERR:UNKNOWN_CMD\r\n`
    这三项成立后，才允许继续叠加真实业务命令解析。

## 当前源码状态

- 串口控制链路已完成步骤 `2、4、5、6、7、8、9、10`：主控经 `UART0(PA1/PA2)` 可直接进入 `trans_cmd_uart`，且 BLE 建链不会再抢占该入口。
- 广播组包已完成步骤 `11`：`ADV` 固定承载 iBeacon 主体，`RSP` 按 `BADGE -> school_id/device_id -> Name` 的优先级填充，空间不足时优先保留业务字段并截断名称。
