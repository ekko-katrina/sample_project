# App Wi-Fi 扩展控制协议（完整 JSON，版本 1）

MQTT 请求和回复均使用具名 JSON 字段，不传输蓝牙帧、十六进制包或校验字节。设备收到 JSON 后直接调用业务接口；App 收到 JSON 后直接更新业务状态。BLE 继续使用自己的协议。

## 连接及能力

- App 与同一设备的 BLE、MQTT 控制连接互斥，先关闭旧连接再连接新通道；ESP32 与车机的蓝牙连接独立保留。
- Broker、设备 SN、viewer 心跳、基础温控等内容见 [MQTT 对接协议](MQTT%20对接协议.md)。
- GET_STATE 的新鲜 state 回复包含 `settingsJsonVersion: 1`，表示支持本文协议。App 和设备固件均需更新；不兼容旧扩展封装。基础控制命令保持兼容。
- 扩展请求发到 `fridge/{sn}/cmd`，回复发到 `fridge/{sn}/event`，均为 QoS 1、Retain=false。拒绝 retained 控制命令；客户端忽略 retained 事件。

## 公共请求和独立回复

```json
{"cmd":"GET_WIFI_STATUS","params":{}}
```

| 字段 | 类型 | 规则 |
| --- | --- | --- |
| cmd | string | 下文列出的指令名称，大小写敏感 |
| params | object | 指令参数；无参数指令可省略或传 `{}`；设备忽略未知附加字段，已知字段必须类型正确 |

参数错误、未知命令或型号不支持时，沿用基础 MQTT 协议：不修改状态，只记录设备本地日志，不发布 MQTT 错误消息。不发送通用受理回复。业务执行中发生的失败（如认证失败、扫描失败、OTA 下载失败）仍通过各自的过程事件报告。

所有业务回复仅包含 `type` 和 `data`，按设备 Topic 分发；控制请求不包含 sessionId、requestId 或 operationId。MQTT Client ID 和 viewer HTTP 标识属于连接管理，不放进业务报文。MQTT PUBACK 只确认传输，不代表设备执行成功；App 按对应状态、过程事件及页面超时判断结果。viewer 离线时不发布业务回复，重新在线后需重新查询，不能依赖历史事件。

### 与 BLE 一致的独立状态边界

BLE 中单独查询、单独回复的功能，在 MQTT 上也保持独立。`GET_STATE` 仅返回完整冰箱基础控制状态及能力标记，不触发下表回复，不包含这些功能的状态字段。

| 功能 | MQTT 查询 | 独立回复 |
| --- | --- | --- |
| 氛围灯（BLE 0x07 / 0x08） | GET_AMBIENT_LIGHT | AMBIENT_LIGHT_STATUS |
| Wi-Fi 网络 | GET_WIFI_STATUS | WIFI_STATUS |
| 固件版本 | GET_FIRMWARE_VERSION | FIRMWARE_VERSION |
| 车机连接 | GET_TESLA_STATUS | TESLA_STATUS |

设置氛围灯后回复独立灯光状态；网络扫描、配网、清除、固件升级及车机配置使用下文各自的过程/结果事件。所有数据都是具名 JSON 字段，不封装 BLE 包。

控制端应按功能流程依次发起操作，通过对应状态或过程事件确认结果；恢复连接后重新查询当前状态，不重发旧控制。设备在当前 MQTT 连接内利用 DUP 标记和最近 64 个入站 Packet ID 抑制重复控制包，不需要业务请求 ID。同一设备的多个客户端可查看状态，但控制结果不能区分来源或请求批次，因此控制操作应由单个客户端依次发起。

## 氛围灯

| cmd | params | 回复 |
| --- | --- | --- |
| GET_AMBIENT_LIGHT | 无 | AMBIENT_LIGHT_STATUS |
| SET_AMBIENT_LIGHT | 下列字段至少一项 | AMBIENT_LIGHT_STATUS |

SET_AMBIENT_LIGHT 可选字段：enabled（boolean）、effect（STEADY 或 BREATHING）、coolingColor/heatingColor（含 r/g/b 的 object，各为 0～255 整数）。未传字段保留原值。关闭时保留灯效和颜色；不支持氛围灯的型号只记录日志，不修改状态、不发送回复。

```json
{"cmd":"SET_AMBIENT_LIGHT","params":{"enabled":true,"effect":"BREATHING","coolingColor":{"r":0,"g":128,"b":255},"heatingColor":{"r":255,"g":80,"b":0}}}
```

```json
{"type":"AMBIENT_LIGHT_STATUS","data":{"enabled":true,"effect":"BREATHING","coolingColor":{"r":0,"g":128,"b":255},"heatingColor":{"r":255,"g":80,"b":0}}}
```

回复包含以上全部字段，enabled=false 时 effect 仍表示保存的灯效。

## Wi-Fi 网络

网络请求不包含操作 ID。App 按当前扫描、配网或清除流程接收相应类型事件；空闲时忽略扫描、配网和清除的迟到事件。

| cmd | params 字段 | 回复 |
| --- | --- | --- |
| GET_WIFI_STATUS | 无 | WIFI_STATUS |
| SCAN_WIFI | includeHidden：可选 boolean，默认 false | WIFI_SCAN_EVENT |
| CONFIGURE_WIFI | ssid、password 必填；hidden、bssid、lockBssid 可选 | WIFI_CONFIGURE_EVENT |
| CLEAR_WIFI | disconnect：可选 boolean，默认 true | WIFI_CLEAR_RESULT |

CONFIGURE_WIFI：ssid 最多 32 UTF-8 字节，非隐藏网络不得为空；password 为空表示开放网络，否则为 8～63 UTF-8 字节或 64 位十六进制 PSK。hidden、lockBssid 默认为 false；bssid 格式为 AA:BB:CC:DD:EE:FF，lockBssid=true 时必须提供非全零 BSSID。密码不出现在状态、事件和日志中。

### 当前网络

```json
{"type":"WIFI_STATUS","data":{"state":3,"errorCode":0,"ssid":"Home WiFi","rssi":-52,"ip":"192.168.1.100"}}
```

state：0 未配置、1 已保存但离线、2 正在连接、3 已连接。rssi 为 dBm 整数，-128 表示未知；ip 为 IPv4 字符串，0.0.0.0 表示没有 IP。

### 扫描

```json
{"cmd":"SCAN_WIFI","params":{"includeHidden":false}}
```

```json
{"type":"WIFI_SCAN_EVENT","data":{"errorCode":0,"event":"AP_FOUND","ssid":"Home WiFi","rssi":-52,"authMode":3,"bssid":"AA:BB:CC:DD:EE:FF"}}
```

事件 event：STARTED、AP_FOUND（每个 AP 一条）、FINISHED、FAILED。所有事件都包含 errorCode；AP_FOUND 包含 ssid、rssi、authMode、bssid，其他阶段不要求 AP 字段。

authMode：0 OPEN、1 WEP、2 WPA-PSK、3 WPA2-PSK、4 WPA/WPA2、5 企业认证、6 WPA3-PSK、7 WPA2/WPA3、255 未知。当前 App 不支持 WEP 和企业认证。

### 更换网络

```json
{"cmd":"CONFIGURE_WIFI","params":{"ssid":"Home WiFi","password":"example-password","hidden":false,"lockBssid":true,"bssid":"AA:BB:CC:DD:EE:FF"}}
```

```json
{"type":"WIFI_CONFIGURE_EVENT","data":{"stage":3,"result":1,"errorCode":0,"ssid":"Home WiFi","rssi":-52,"ip":"192.168.1.100"}}
```

stage：0 ACCEPTED、1 ASSOCIATING、2 OBTAINING_IP、3 SUCCESS、4 FAILED。result：0 处理中、1 成功、2 失败。每条事件都包含 stage、result、errorCode、ssid、rssi、ip；仅成功取得 IP 时 ip 为实际地址。

远程更换网络可能使 MQTT 断开。设备恢复上线后，App 使用 GET_WIFI_STATUS 核对目标 SSID、state 和 IP；不能把断线当成成功。

### 清除网络

```json
{"cmd":"CLEAR_WIFI","params":{"disconnect":true}}
```

```json
{"type":"WIFI_CLEAR_RESULT","data":{"result":1,"errorCode":0,"state":0}}
```

result 和 state 的含义同上。清除成功且断开网络后，远程控制会离线，需要手动切换 BLE 配网。更换/清除网络前设备留出 500ms 发送独立过程/结果消息，但这不是交付保证；未收到最终结果时应通过 BLE 确认。

网络 errorCode：0 无错误、1 忙、2 参数错误、3 扫描失败、4 找不到 AP、5 认证失败、6 关联超时、7 DHCP 失败、8 不支持认证方式、9 内部错误、10 保存失败、11 已取消、12 会话无效、255 未知。扫描 FAILED 时 App 也应显示扫描失败，即使 errorCode 为 0。

## 固件版本与 OTA

| cmd | params | 回复 |
| --- | --- | --- |
| GET_FIRMWARE_VERSION | 无 | FIRMWARE_VERSION |
| START_FIRMWARE_UPDATE | modelId：1～255 整数；version：三段十进制版本字符串，每段 0～255 | FIRMWARE_UPDATE_STATUS |

当前型号 ID：1 为新 Y17、2 为新 Y16、3 为新 TM-310、4 为新 Y06。升级目标必须与实际型号一致。

```json
{"cmd":"GET_FIRMWARE_VERSION","params":{}}
```

```json
{"type":"FIRMWARE_VERSION","data":{"modelId":4,"version":"1.0.7"}}
```

```json
{"cmd":"START_FIRMWARE_UPDATE","params":{"modelId":4,"version":"1.0.8"}}
```

```json
{"type":"FIRMWARE_UPDATE_STATUS","data":{"stage":"DOWNLOADING","errorCode":0}}
```

stage 为 PREPARING、DOWNLOADING、VERIFYING、RESTARTING 或 ERROR。当前只报告阶段，不提供下载百分比。ERROR 的 errorCode：1 网络、2 下载、3 校验、4 写入、5 型号/版本、6 空间不足、7 超时、8 未知；非 ERROR 阶段 errorCode=0。

固件由设备自行下载、验证并重启；MQTT 恢复后通过 GET_FIRMWARE_VERSION 核对实际版本，不自动连接手机 BLE，也不重发升级指令。

## 车机连接

| cmd | params | 回复 |
| --- | --- | --- |
| GET_TESLA_STATUS | 无 | TESLA_STATUS |
| CONFIGURE_TESLA | vin：17 位大写 VIN，不含 I/O/Q | TESLA_EVENT |
| STOP_TESLA | 无 | TESLA_EVENT |

```json
{"cmd":"CONFIGURE_TESLA","params":{"vin":"LRW3E7FA0MC123456"}}
```

```json
{"type":"TESLA_STATUS","data":{"configured":true,"paired":true,"connectionState":5,"stage":6,"errorCode":0,"vin":"LRW3E7FA0MC123456"}}
```

TESLA_EVENT 的 data 与 TESLA_STATUS 相同，全部字段必填：configured、paired 为 boolean；未配置时 vin 为空字符串。

connectionState：0 断开、1 扫描、2 连接中、3 已连接、4 服务已发现、5 已认证。

stage：0 IDLE、1 SAVED、2 SCANNING、3 CONNECTING、4 WAITING_APPROVAL、5 PAIRED、6 AUTHENTICATED、7 FAILED、8 STOPPED、9 STOPPING。

errorCode：0 无错误、1 忙、2 VIN 无效、3 保存失败、4 扫描超时、5 连接失败、6 配对失败、7 认证失败、8 内部错误、9 密钥未授权、10 数据不可用、255 未知。

车机配对仍可能要求用户在车内确认。只有 configured=true、paired=true、connectionState=5、stage=6、errorCode=0 同时成立，才显示已认证；受理或连接建立都不能代替认证成功。

## 日志与部署

FRIDGE-MQTT 输出 JSON 指令名、结果及业务事件名称；FRIDGE-BLE 用于手机 BLE 指令/连接；FRIDGE-APP 为共用状态日志。MQTT TX messageId 仅表示交给 MQTT 客户端，不代表 App 已收到。

本版本沿用当前开发 Broker。生产 TLS、身份认证和 Topic ACL 的配置要求见主 MQTT 文档。修改协议需同步更新 App 和设备固件；不需要改 viewer HTTP 接口。
