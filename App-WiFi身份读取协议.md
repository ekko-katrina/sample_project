# App Wi-Fi 控制：BLE 读取 MQTT SN

本扩展只适用于新协议设备，旧协议 Y06 不响应。共用 FFF0 服务、FFF1 写入和 FFF2 通知。
App 在首次 BLE 连接后读取 SN，并与蓝牙设备 ID 关联保存，随后可远程通过 MQTT 控制。

通用帧：A5 5A LEN CMD DATA XOR AA。LEN 包含 CMD、DATA、XOR、AA。

| 指令 | 方向 | DATA |
| --- | --- | --- |
| D0 | App → ESP32 | requestId：2 字节，大端，非零 |
| D1 | ESP32 → App | requestId：2 字节；result：1 字节；snLen：1 字节；SN：snLen 个 ASCII 字节 |

result=0 表示成功，SN 长度 1～64，仅允许大写字母、数字、短横线、下划线。
result=1 表示 MQTT 身份尚不可用，snLen=0。
请求长度错误或 requestId=0 时忽略。回复仅发送给请求连接，长 SN 沿用 BLE Notify 分片。

SN 由 mqtt_service_init 解析后通过 ble_set_mqtt_sn 提供，优先使用 NVS mqtt_cfg/sn，
开发模式缺少 NVS 时使用当前型号的测试 SN。BLE 与 MQTT 始终使用同一个解析结果。

请求 requestId=1234（十六进制）：

```text
A5 5A 05 D0 12 34 0C AA
```

App 校验 requestId、请求设备、长度和 SN 格式；不匹配的过期回复不得更新当前设备身份。
固件更新后，App 自动查询并保存，不要求用户手动输入 SN。
