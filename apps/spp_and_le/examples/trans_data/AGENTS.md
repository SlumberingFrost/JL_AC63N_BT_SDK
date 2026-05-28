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
