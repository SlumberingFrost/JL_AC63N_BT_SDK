# trans_data 目录补充说明

## 广播模式 `stopBroadcast` 联动

- 当前 `AC6323A` 普通广播模式下，主控通过 `setParams`、`CMD:SCHOOL_ID=`、`CMD:DEVICE_ID=`、`CMD:UUID=` 等成功修改广播身份参数后，会立即刷新广播内容。
- 刷新成功后会启动一个一次性 `10s` 定时器；超时后模组会主动停止当前广播，并通过业务串口回 `{"cmd":"stopBroadcast"}`。
- 该回包是设计内联动信号，用于提示主控执行后续协同动作；它不是异常日志，也不表示参数写入失败。
- 对应实现位于 [ble_trans.c](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/apps/spp_and_le/examples/trans_data/ble_trans.c:1278) 和 [trans_cmd_proc.c](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/apps/spp_and_le/examples/trans_data/trans_cmd_proc.c:1339)。

## 广播模式参数口径

- 当前普通广播模式下，`UUID?` 回读中的 `SCHOOLID/DEVICEID` 统一按十进制输出，和 `setParams` JSON 的 number 类型保持一致。
- 当前控制口同时支持 `{"cmd":"setUuid","uuid":"32HEX","major":x,"minor":y,"tx":z,"schoolId":s,"deviceId":d}` 形式的 JSON 写 UUID；现场联调优先推荐该 JSON 入口，避免长文本 `CMD:UUID=` 在串口工具里被不可见字符或分包行为污染。
- `setUuid` 中 `schoolId/deviceId/tx` 都允许省略；若设备内已有 UUID 配置，则会沿用旧值。串口分帧对 `\r` 和 `\n` 都会收尾，但联调文档统一仍按 `\r\n` 发送。
- `MAJOR/MINOR/TX` 仍保持现有十六进制口径，不在这次变更范围内。

## 当前已确认问题：UUID 长命令串口残帧

- 当前现场联调已确认：`CMD:MODE?`、`CMD:UUID?`、`setParams` 等短命令可正常执行，但 `CMD:UUID=...` 和 `{"cmd":"setUuid",...}` 这类长命令仍可能失败。
- 失败时并不是 UUID 底层存储或广播刷新逻辑不支持，而是 `UART0/debug` 复用接收链路没有稳定收到完整长帧，设备常常只拿到中段或尾段残片。
- 典型日志特征：
  - `TRACE:UART:DISPATCH_FAIL len=33 head=54.58.3D.43.35.2C.53.43`
  - `TRACE:UART:LINE_CORRUPT len=65 head=32.43.33.34.30.36.2C.4D`
  - `TRACE:UART:DISPATCH_FAIL len=27 head=72.22.3A.34.30.39.37.2C`
- 上述 `head` 分别对应 `TX=C5,SC...`、`2C3406,M...`、`r":4097,` 一类命令中段/尾段内容，说明业务层并未收到从 `CMD:UUID=` 或 `{"cmd":"setUuid"` 开始的完整报文。
- 对应日志样例见 [Device.log](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/文档/Device.log:50)、[Device.log](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/文档/Device.log:58)、[Device.log](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/文档/Device.log:107)。
- 当前结论：UUID 无效的主因在 [trans_cmd_uart.c](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/apps/spp_and_le/examples/trans_data/trans_cmd_uart.c:16) 这一层的长帧接收稳定性，不在 [trans_cmd_proc.c](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/apps/spp_and_le/examples/trans_data/trans_cmd_proc.c:1901) 的 UUID 业务解析本身。
- 后续排查应优先关注：
  - `uart_db_regiest_recieve_callback(...)` 对长帧的分段行为，见 [trans_cmd_uart.c](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/apps/spp_and_le/examples/trans_data/trans_cmd_uart.c:307)
  - `DEVICE_EVENT_FROM_AT_UART` 上报到应用线程后的拼帧链路，见 [app_spp_and_le.c](/d:/RD/AC63N_BT/fw-AC63_BT_SDK-master/apps/spp_and_le/examples/trans_data/app_spp_and_le.c:476)
  - 长命令中是否夹带控制字符、是否被串口工具拆包
