# 仓库说明

- 用户要求：在此工作区内不要执行任何编译验证。
- 这包括 `make`、IDE 编译、单文件编译检查、链接验证，以及其他主动触发构建的命令。
- 除非用户后续明确撤销该要求，否则只允许查看日志、阅读代码和修改源码，不运行编译。
- 当回答中引用本工作区内的 SDK 文件、日志、脚本、配置文件时，必须优先使用可点击的 Markdown 本地文件链接，格式为 `[文件名](/绝对路径/文件:行号)`。
- 路径必须指向当前工作区中的实际文件，并尽量附带准确行号，便于用户点击后直接跳转到对应位置。
- 不要只写纯文本文件名、`line 46` 这类不可点击描述；如果能定位到具体位置，就直接给出可点击链接。

## AW6323A 烧录打包说明

- 当前数传工程的一拖八烧录产物目录固定为 `cpu/bd19/tools/download/data_trans/`。
- 该目录下的 `jl_isd.fw` 是普通未加 KEY 的固件包；若烧录器提示“固件未被添加KEY”，不要继续使用这个文件。
- 若需要走 KEY/授权烧录流程，应优先使用同目录下的 `jl_isd_[已授权].fw`；`update.ufw` / `update_[已授权].ufw` 属于升级包，不作为一拖八主烧录文件。
- 当前 KEY 打包逻辑在 `cpu/bd19/tools/download/data_trans/download.bat`，默认配置是无 KEY。
- 如需启用 KEY，至少确认两点：
  1. `download.bat` 里的 `KEY_FILE` 指向有效的 `.key` 文件。
  2. 将 `if "%KEY_MODE%"=="" set KEY_MODE=OFF` 改为 `if "%KEY_MODE%"=="" set KEY_MODE=ON`，或直接将 `KEY_MODE` 固定为 `ON`，使脚本进入 `call :run_download_with_retry -key "%KEY_FILE%"` 分支。
- 如果只需要普通直烧或本地打包，不要开启 KEY；当前脚本默认的无 KEY 行为是预期配置。

## AW6323A 单广播功能测试步骤

以下步骤用于验证当前 `AW6323A + UART0(PA1/PA2)` 单广播方案。测试时，主控或串口工具统一通过 `UART0` 与蓝牙芯片通信，参数固定为 `115200 8N1`。当前串口分帧对 `\r` 和 `\n` 都会收尾，但测试口径统一要求所有命令以 `\r\n` 结束。

1. 上电基础检查预期串口先输出启动日志，并出现 `ready: uart0 baud=115200, tx=1, rx=2`。若日志持续反复输出按键事件，先确认 `ADKEY` 已关闭。
2. 广播初始状态检查预期启动日志中出现 `trans_adv_data(...)` 与 `rsp_data(...)`。其中 `ADV` 应承载 iBeacon 主体，`RSP` 应至少包含 `school_id/device_id`；若已配置 `BADGE`，则 `RSP` 还会先携带 `BADGE` 数据。
3. 串口链路检查发送 `PING\r\n`。预期日志中出现 `uart_rx_data`、`[TRANS_CMD_UART]rx[...]`、`[TRANS_CMD_UART]cmd[4]: PING`，并在同一串口回 `ERR:UNKNOWN_CMD`。
4. 半帧自恢复检查先发送不带结束符的 `CMD:UUID?`，再发送 `CMD:MODE?\r\n`。预期日志出现 `recover half line: old_len=...`，且本次只执行后一条完整命令并回 `MODE:ADV`。
5. 第一阶段命令检查依次发送 `CMD:MODE?\r\n`、`CMD:UUID?\r\n`、`CMD:SCHOOL_ID=1229\r\n`、`CMD:DEVICE_ID=1\r\n`、`{\"cmd\":\"setParams\",\"schoolId\":1229,\"deviceId\":1}\r\n`、`{\"cmd\":\"setUuid\",\"uuid\":\"415733314E424C4555554F217FA01361\",\"major\":4969,\"minor\":32672,\"tx\":-59,\"schoolId\":1229,\"deviceId\":1}\r\n`。预期都能得到协议定义中的正常回包，且修改类命令成功后会重新打印新的 `trans_adv_data(...)` / `rsp_data(...)`。
6. UUID 整包写入检查发送 `{\"cmd\":\"setUuid\",\"uuid\":\"415733314E424C4555554F217FA01361\",\"major\":4969,\"minor\":32672,\"tx\":-59,\"schoolId\":1229,\"deviceId\":1}\r\n`。预期回 `{"cmd":"setUuidResult","retCode":0,...}`。随后发送 `CMD:UUID?\r\n`，预期回读值与刚写入内容一致；其中 `SCHOOLID/DEVICEID` 都应按十进制回显。
7. UUID 锁功能检查先发 `CMD:UUID:LOCK?\r\n`，确认当前锁状态；再发 `CMD:UUID:LOCK=1\r\n`，预期回 `ACK:UUID:LOCK=1`。锁定后再次发送 `CMD:SCHOOL_ID=1229\r\n` 或 `CMD:DEVICE_ID=1\r\n`，预期回 `ERR:UUID_LOCKED`。最后发 `CMD:UUID:LOCK=0\r\n` 解锁，并确认相关写入恢复成功。
8. BADGE 读写检查先发 `CMD:BADGE?\r\n`。若未配置，预期回 `ERR:BADGE_UNSET`。再发 `CMD:BADGE=AA0301026910C20101ED\r\n`，预期回 `ACK:BADGE,OK,ADV=ON`。随后再次发 `CMD:BADGE?\r\n`，预期回 `BADGE:HEX=AA0301026910C20101ED`。
9. BADGE 锁功能检查先发 `CMD:BADGE:LOCK?\r\n`，再发 `CMD:BADGE:LOCK=1\r\n`，预期回 `ACK:BADGE:LOCK=1`。锁定后发送新的 `CMD:BADGE=11223344556677889900\r\n`，预期回 `ERR:BADGE_LOCKED`。随后发 `CMD:BADGE:LOCK=0\r\n` 解锁，再次写入 `CMD:BADGE=11223344556677889900\r\n`，预期回 `ACK:BADGE,OK,ADV=ON`。
10. 广播刷新检查每次成功修改 `UUID`、`school_id`、`device_id` 或 `BADGE` 后，日志都应重新打印 `trans_adv_data(...)` / `rsp_data(...)`。其中 `ADV` 应保持 iBeacon 主体不变，`RSP` 应按 `BADGE -> school_id/device_id -> Name` 的优先级更新；若名称过长，日志中可看到名称被截断的提示。
11. 验收结论
    当以上步骤全部通过时，可认定当前 `AW6323A` 单广播功能已完成联调验收：主控串口链路正常，命令协议正常，锁保护正常，广播重组与掉电配置更新路径正常。

## AW6323A 扫描功能测试步骤

以下步骤用于验证当前 `AW6323A + UART0(PA1/PA2)` 扫描方案。测试前需确保设备烧录的是扫描模式固件，即 `TCFG_AC6323A_PRODUCT_MODE` 选为 `SCAN`；主控或串口工具统一通过 `UART0` 与蓝牙芯片通信，参数固定为 `115200 8N1`，所有命令必须以 `\r\n` 结束。

1. 上电基础检查预期串口先输出启动日志，并出现 `ready: uart0 baud=115200, tx=1, rx=2`、`bt_ble_init: trans_scan_filter_cfg_load begin/done`、`bt_ble_init: ble_gatt_client_scan_enable(1) begin/done`、`scan-only: force scan enable`、`proc ready: mode/uuid/badge/lock/setParams/scanFilter`。这表明扫描配置已加载、扫描已自动开启、串口命令处理已就绪。
2. 扫描过滤初始状态检查发送 `CMD:SCAN:FILTER?\r\n`。预期串口回 `{"cmd":"getResult","retCode":0,"schoolId":...,"deviceId":...,"enable":...}`。若为首次上电或配置损坏，启动日志中通常会先出现 `scan_filter_cfg load miss/invalid...use_default...`；若曾保存过配置，则应出现 `scan_filter_cfg load ok...`。
3. 串口链路检查发送 `PING\r\n`。预期日志中出现 `uart_rx_data`、`[TRANS_CMD_UART]rx[...]`、`[TRANS_CMD_UART]cmd[4]: PING`，并在同一串口回 `ERR:UNKNOWN_CMD`。
4. 半帧自恢复检查先发送不带结束符的 `CMD:SCAN:FILTER?`，再发送 `CMD:SCAN:SCHOOLID?\r\n`。预期日志出现 `recover half line: old_len=...`，且本次只执行后一条完整命令，并回 `{"cmd":"getResult","retCode":0,"schoolId":...,"deviceId":...,"enable":...}`。
5. 扫描过滤写入检查依次发送 `CMD:SCAN:SCHOOLID=1229\r\n`、`CMD:SCAN:DEVICEID=1\r\n`、`CMD:SCAN:FILTER=ON\r\n`、`CMD:SCAN:FILTER=SCHOOLID=1229,DEVICEID=1,ENABLE=1\r\n`。预期每条都回 `{"cmd":"setResult","retCode":0,"schoolId":1229,"deviceId":1,"enable":...}`；随后再次发送 `CMD:SCAN:FILTER?\r\n`，预期回读值与刚写入内容一致。
6. JSON 兼容写入检查发送 `{"cmd":"setParams","schoolId":1229,"deviceId":1}\r\n`。预期回 `{"cmd":"setParamsResult","retCode":0,"schoolId":1229,"deviceId":1}`。随后发送 `CMD:SCAN:FILTER?\r\n`，预期 `schoolId/deviceId` 已同步更新，且 `enable` 被置为 `1`。
7. 空结果上报检查在周围没有满足过滤条件的广播设备时，保持过滤开启并等待至少两个扫描结果上报周期。预期串口回 `{"cmd":"scanResult","data":[]}`。若前一时刻仍有命中缓存，需等待缓存超时后再观察空结果。
8. 命中过滤结果检查准备另一台按同协议发包的设备，使其广播内携带 `school_id=1229`，并建议同时携带 `device_id=1`。预期扫描端按约 `1s` 周期输出 `{"cmd":"scanResult","data":[{"schoolId":1229,"deviceId":1}]}`。若对端广播仅带 `school_id` 未带 `device_id`，则预期 `deviceId` 显示为 `-1`。
9. 过滤生效边界检查将对端改为广播不同的 `school_id`，或将本端过滤改成 `CMD:SCAN:FILTER=SCHOOLID=9999,DEVICEID=1,ENABLE=1\r\n`。预期不再出现该目标的非空 `scanResult`。随后发送 `CMD:SCAN:FILTER=OFF\r\n`，预期再次收到周围设备的 `scanResult`。当前实现仅按 `schoolId` 做命中过滤，`deviceId` 会被保存和上报，但不会单独拦截结果。
10. 掉电配置保持检查完成过滤写入后重启设备，再发送 `CMD:SCAN:FILTER?\r\n`。预期仍回到上次保存的 `schoolId/deviceId/enable`；同时启动日志应重新打印 `scan_filter_cfg load ok, enable=..., school_id=0x..., device_id=0x...`。
11. 验收结论
    当以上步骤全部通过时，可认定当前 `AW6323A` 扫描功能已完成联调验收：主控串口链路正常，扫描自动拉起正常，过滤配置读写与掉电保存正常，扫描结果上报与过滤链路正常。

