# App Wi-Fi 控制：BLE 读取 MQTT SN

本扩展只适用于 TEMAI 统一新 BLE 协议设备；旧 BLE 协议设备不支持 Wi-Fi 控制，也不响应本扩展。共用 FFF0 服务、FFF1 写入和 FFF2 通知。
App 在首次 BLE 连接后读取 SN，并与蓝牙设备 ID 关联保存，随后可远程通过 MQTT 控制。

通用帧：`A5 5A LEN CMD DATA XOR AA`。`LEN = DATA字节数 + 3`，包含 CMD、DATA、XOR、AA。
XOR 从 A5 开始，覆盖帧头、LEN、CMD 和全部 DATA，不包含 XOR 和 AA。

| 指令 | 方向 | DATA |
| --- | --- | --- |
| D0 | App → ESP32 | 空，无参数 |
| D1 | ESP32 → App | SN 的 ASCII 原始字节，不带结束符；身份尚不可用时为空 |

SN 长度 1～64，仅允许大写字母、数字、短横线、下划线；长度由 LEN-3 计算。
不再附加 requestId、result、snLen。D0 带有任何数据时忽略；D1 空数据段表示读取失败。
回复仅发送给请求连接，长 SN 沿用 BLE Notify 分片。

SN 由 mqtt_service_init 解析后通过 ble_set_mqtt_sn 提供，优先使用 NVS mqtt_cfg/sn；
开发模式缺少 NVS 时使用当前型号的测试 SN。BLE 与 MQTT 始终使用同一个解析结果。

无参数查询：

```text
A5 5A 03 D0 2C AA
```

身份尚不可用：

```text
A5 5A 03 D1 2D AA
```

App 同时只保留一个 SN 查询，校验当前请求设备、帧长度和 SN 格式，等待上限 5 秒。
未在查询中的回复或其他设备的回复不能更新当前设备身份。没有请求关联 ID，无法区分同一设备上一次查询的迟到回复，因此同一设备的 SN 必须稳定。
App 自动查询并保存，不要求用户手动输入 SN。App 与固件须同步升级为此简化格式；不兼容原先带 requestId/result/snLen 的格式。
