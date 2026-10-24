# ESP32 MQTT 对接协议

本文档统一定义 ESP32 与 App、客户端等客户端之间的 MQTT 连接、Topic、基础控制命令、独立功能查询及状态/过程回复。所有 MQTT 业务报文使用具名 UTF-8 JSON 字段，不传输 BLE 帧、十六进制包或校验字节。

所有使用 TEMAI 统一新 BLE 协议的设备都支持 Wi-Fi 控制。基础控制与氛围灯、Wi-Fi 网络、固件 OTA、车机连接遵守同一套公共规则，各功能的状态边界与 BLE 一致，按型号实现相应命令和事件。

## MQTT 连接

ESP32 在以下条件满足后启动 MQTT：

1. Wi-Fi 已连接并获得 IP；
2. 已取得合法设备 SN。

设备通电并且网络正常，ESP32 就持续连接 Broker。

当前开发环境参数：

| 项目 | 值 |
| --- | --- |
| Broker URI | `mqtt://8.138.244.51:1883` |
| MQTT 版本 | MQTT 3.1.1 |
| Keep Alive | 60 秒 |
| Clean Session | `true` |
| 用户名/密码 | 当前不使用 |
| Payload | UTF-8 JSON |
| Client ID | 设备 SN |

生产环境应改用 TLS、每设备独立凭据和 Topic ACL。

### SN

正式设备从 NVS 读取 SN：

```text
namespace: mqtt_cfg
key:       sn
type:      string
```

SN 必须满足：

- 同时作为 MQTT Client ID 和 Topic 中的 `{sn}`；
- 每台设备必须唯一。

## Topic

ESP32 使用以下 Topic：

| ESP32 动作 | Topic | QoS | Retain | 用途 |
| --- | --- | ---: | ---: | --- |
| 订阅 | `fridge/{sn}/cmd` | 1 | 必须否 | 接收设备控制命令 |
| 订阅 | `fridge/{sn}/viewer` | 1 | 是 | 接收客户端在线状态 |
| 发布 | `fridge/{sn}/online` | 1 | 是 | 发布 ESP32 在线状态 |
| 发布 | `fridge/{sn}/state` | 1 | 是 | 发布完整冰箱基础控制状态 |
| 发布 | `fridge/{sn}/event` | 1 | 否 | 独立功能的状态回复和过程事件 |

示例 SN `TMY06-TEST-001`：

```text
fridge/TMY06-TEST-001/cmd
fridge/TMY06-TEST-001/viewer
fridge/TMY06-TEST-001/online
fridge/TMY06-TEST-001/state
fridge/TMY06-TEST-001/event
```

ESP32 不订阅通配符 Topic，不接收其他设备的消息。

## 启动流程

```text
等待 Wi-Fi 获得 IP
  → 读取 SN
  → 使用 SN 作为 Client ID
  → 配置 online=false 离线遗嘱
  → 连接 Broker
  → 订阅 fridge/{sn}/cmd
  → 订阅 fridge/{sn}/viewer
  → 等待两个订阅均确认成功
  → 发布 online=true
  → 等待 viewer 在线消息
```

连接成功后不能立即发布 `state`。只有收到有效的 `viewer.online=true` 后才开始状态上报。

## ESP32 在线状态

Topic：`fridge/{sn}/online`

上线 Payload：

```json
{"online":true}
```

离线 Payload：

```json
{"online":false}
```

通讯规则：

- QoS 为 1；
- Retain 为 `true`；
- 首次连接且两个订阅成功后发布 `online=true`；
- 每次重连并重新订阅成功后发布 `online=true`；
- 主动重启或切换模拟档位前发布 `online=false`；
- 主动离线最多等待 1 秒的 QoS 1 发布确认；
- 异常断网或掉电由 Broker 发布离线遗嘱；
- `online` 不做固定周期上报。

离线遗嘱必须在 MQTT CONNECT 之前配置：

```text
Topic:   fridge/{sn}/online
QoS:     1
Retain:  true
Payload: {"online":false}
```

## 客户端在线状态

ESP32 订阅：`fridge/{sn}/viewer`

允许开始上报：

```json
{"online":true}
```

停止上报：

```json
{"online":false}
```

ESP32 处理规则：

- Payload 必须是合法 UTF-8 JSON；
- 根节点必须是对象；
- `online` 必须存在且类型为 boolean；
- 非法消息只记录日志，不改变当前状态；
- 从离线变为在线时，立即发布一次最新完整 `state`；
- 在线期间再次收到 `online=true`，只刷新在线有效期，不重复发布状态；
- 收到 `online=false`，立即停止发布 `state` 和 `event`；
- 连续 90 秒未收到新的 `online=true`，按客户端离线处理；
- MQTT 断开时立即将本地客户端状态置为离线；
- MQTT 重连后等待新的或 Broker 保留的 `viewer` 消息，不沿用断线前的在线状态。

发送端应在客户端在线期间以小于 90 秒的周期刷新 `online=true`，当前推荐周期为 30 秒。

## 状态上报条件

ESP32 只有同时满足以下条件才允许发布 `state` 和独立功能的 `event`：

```text
MQTT 已连接
且 cmd、viewer 均订阅成功
且 viewer 在线状态未超时
```

客户端在线期间，以下情况发布完整 `state`：

- `viewer` 从离线变为在线；
- 收到 `GET_STATE`；
- MQTT 冰箱基础设置命令执行成功；
- BLE 控制改变冰箱基础状态；
- 串口或机身模拟操作改变设备状态；
- 当前温度、电压或故障码变化；
- 距离上次周期起点达到 5 分钟。

状态变化时立即发布，不等待 5 分钟周期。

客户端离线期间：

- ESP32 继续维护本地最新状态；
- 不发布状态变化；
- 不执行 5 分钟兜底上报；
- 不保存待补发的历史消息；
- 客户端再次在线时，只发布一条当时的最新完整状态。

## 完整冰箱基础控制状态格式

Topic：`fridge/{sn}/state`。QoS 为 1，Retain 为 `true`。每次必须发布完整冰箱基础控制状态，不能只发布发生变化的字段。Wi-Fi、氛围灯、固件和车机等在 BLE 中独立查询的功能，MQTT 也独立查询并通过 `event` 回复，不包含在此 `state` 中。

公共字段：

| 字段 | JSON 类型 | 要求 | 说明 |
| --- | --- | --- | --- |
| `powerOn` | boolean | 必填 | 开关机状态 |
| `unit` | string | 必填 | `CELSIUS` 或 `FAHRENHEIT` |
| `mode` | string | 必填 | 当前型号支持的工作模式 |
| `targetTemp` | integer | 必填 | 目标温度，单位跟随 `unit` |
| `currentTemp` | integer | 必填 | 当前温度，单位跟随 `unit` |
| `fault` | integer | 必填 | 当前故障码，0 表示无故障 |
| `rssi` | integer | 可选 | Wi-Fi RSSI，单位 dBm |
| `uptime` | integer | 必填 | 本次启动后的运行秒数 |

TM-Y06 示例：

```json
{
  "powerOn": true,
  "unit": "CELSIUS",
  "mode": "COOL",
  "childLock": false,
  "targetTemp": -5,
  "currentTemp": 3,
  "fault": 0,
  "rssi": -55,
  "uptime": 120
}
```

TM-Y16、TM-Y17、TM-310 示例：

```json
{
  "powerOn": true,
  "unit": "CELSIUS",
  "mode": "ECO",
  "targetTemp": -5,
  "currentTemp": 3,
  "voltage": 12.67,
  "fault": 0,
  "rssi": -48,
  "uptime": 120
}
```

型号字段规则：

- TM-Y06 必须发送 `childLock`，不发送 `voltage`；
- TM-Y16、TM-Y17、TM-310 必须发送 `voltage`，不发送 `childLock`；
- `voltage` 单位为伏，例如内部值 1267 上报为 `12.67`。

故障不使用独立 `event` Topic。故障发生、故障码改变和故障恢复均通过完整 `state.fault` 上报。

## 公共命令格式和基础控制

ESP32 从 `fridge/{sn}/cmd` 接收命令。命令使用 QoS 1，Retain 必须为 `false`。Payload 最大 1024 字节。

无参数命令可省略 `params` 或传空对象 `{}`；有参数命令必须提供对象类型的 `params`。字段名及 `cmd` 大小写敏感，已知参数必须类型正确，设备忽略未知附加字段。

无参数命令：

```json
{"cmd":"GET_STATE"}
```

有参数命令：

```json
{"cmd":"SET_TARGET_TEMPERATURE","params":{"temperature":-5}}
```

| 命令 | 参数 | ESP32 行为 |
| --- | --- | --- |
| `GET_STATE` | 无 | 客户端在线时发布当前完整状态 |
| `SET_POWER` | `powerOn`: boolean | 设置开关机状态 |
| `SET_MODE` | `mode`: string | 设置当前型号支持的模式 |
| `SET_TEMPERATURE_UNIT` | `unit`: string | 设置温标并换算两个温度值 |
| `SET_TARGET_TEMPERATURE` | `temperature`: integer | 设置当前温标下的目标温度 |
| `SET_CHILD_LOCK` | `locked`: boolean | 设置童锁，仅TM-Y06 支持 |

业务命令不包含 `id`、`sessionId`、`requestId` 或 `operationId`，协议不定义 `reply` Topic，也不发送 `COMMAND_RESULT` 等通用受理回复。基础控制命令成功后，以新的完整冰箱基础控制 `state` 作为最终结果。独立功能使用下文各自的查询和状态/过程回复。MQTT PUBACK 只确认传输，不代表设备执行成功。

ESP32 无论 `viewer` 是否在线都会接收并执行合法命令；但 `viewer` 离线时不发布执行后的 `state` 或 `event`。

### 命令校验

```text
校验 UTF-8
  → 解析 JSON
  → 校验 cmd
  → 校验 params 和 JSON 类型
  → 校验当前型号能力
  → 执行对应业务接口
  → viewer 在线时：基础控制发布完整 state，独立功能发布对应 event
```

规则：

- `cmd` 必须是非空字符串，最大 64 字节；
- 必填参数必须存在且类型正确；无参数命令可省略 params 或传空对象；
- 未知附加字段可以忽略；
- 未知命令、参数错误或型号不支持时不修改状态；
- 非法命令只记录本地日志，不发布 MQTT 错误消息；扫描、认证、下载等业务执行失败仍由各自的过程事件报告；
- QoS 1 允许重复投递，因此命令必须保持绝对值设置语义；
- 不使用 `TOGGLE`、递增、递减等非幂等设置；设备在当前连接内利用 DUP 标记和最近 64 个入站 Packet ID 抑制重复控制包，此机制不提供跨重连的业务去重保证；
- 扫描、配网、升级等过程操作按功能流程依次发起，恢复连接后重新查询当前状态，不自动重发旧控制；
- 同一设备的多个客户端可查看状态，但回复不区分请求来源或批次，控制操作应由单个客户端依次发起；
- App 与同一设备的 BLE、MQTT 控制连接互斥，先关闭旧连接再连接新通道；ESP32 与车机的 BLE 连接独立保留；
- MQTT Client ID 和 viewer HTTP 标识属于连接管理，不放进业务报文。

## 独立功能回复

独立查询、设置和过程命令仍发送到 `fridge/{sn}/cmd`；回复统一发到 `fridge/{sn}/event`，QoS 1、Retain=false。客户端忽略 retained 事件。回复仅包含 `type` 和 `data`，字段使用各业务节规定的完整 JSON，不封装 BLE 数据。

`viewer` 离线时暂停业务回复；重新在线后通过各功能查询命令获取当前状态。统一新 BLE 协议设备实现当前型号支持的独立 JSON 命令与事件。

### 独立查询与回复边界

`GET_STATE` 返回完整冰箱基础控制状态。各独立功能的查询与回复如下：

| 功能 | MQTT 查询 | 独立回复 |
| --- | --- | --- |
| 氛围灯（BLE 0x07 / 0x08） | GET_AMBIENT_LIGHT | AMBIENT_LIGHT_STATUS |
| Wi-Fi 网络 | GET_WIFI_STATUS | WIFI_STATUS |
| 固件版本 | GET_FIRMWARE_VERSION | FIRMWARE_VERSION |
| 车机连接 | GET_TESLA_STATUS | TESLA_STATUS |

设置氛围灯后回复独立灯光状态；网络扫描、配网、清除、固件升级及车机配置使用下文各自的过程/结果事件。所有数据都是具名 JSON 字段，不封装 BLE 包。

### 氛围灯

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

回复包含以上全部字段，enabled=false 时 effect 仍表示保存的灯效。当前板载预览呼吸周期约 6 秒（渐亮 3 秒、渐暗 3 秒）。

### Wi-Fi 网络

网络请求不包含操作 ID。App 按当前扫描、配网或清除流程接收相应类型事件；空闲时忽略扫描、配网和清除的迟到事件。

| cmd | params 字段 | 回复 |
| --- | --- | --- |
| GET_WIFI_STATUS | 无 | WIFI_STATUS |
| SCAN_WIFI | includeHidden：可选 boolean，默认 false | WIFI_SCAN_EVENT |
| CONFIGURE_WIFI | ssid、password 必填；hidden、bssid、lockBssid 可选 | WIFI_CONFIGURE_EVENT |
| CLEAR_WIFI | disconnect：可选 boolean，默认 true | WIFI_CLEAR_RESULT |

CONFIGURE_WIFI：ssid 最多 32 UTF-8 字节，非隐藏网络不得为空；password 为空表示开放网络，否则为 8～63 UTF-8 字节或 64 位十六进制 PSK。hidden、lockBssid 默认为 false；bssid 格式为 AA:BB:CC:DD:EE:FF，lockBssid=true 时必须提供非全零 BSSID。密码不出现在状态、事件和日志中。

#### 当前网络

```json
{"type":"WIFI_STATUS","data":{"state":3,"errorCode":0,"ssid":"Home WiFi","rssi":-52,"ip":"192.168.1.100"}}
```

state：0 未配置、1 已保存但离线、2 正在连接、3 已连接。rssi 为 dBm 整数，-128 表示未知；ip 为 IPv4 字符串，0.0.0.0 表示没有 IP。

#### 扫描

```json
{"cmd":"SCAN_WIFI","params":{"includeHidden":false}}
```

```json
{"type":"WIFI_SCAN_EVENT","data":{"errorCode":0,"event":"AP_FOUND","ssid":"Home WiFi","rssi":-52,"authMode":3,"bssid":"AA:BB:CC:DD:EE:FF"}}
```

扫描流程为 STARTED → AP_FOUND（每个 AP 一条）→ FINISHED；扫描失败发送 FAILED。设备完成整轮扫描后最多取 40 条热点记录，逐条发送，间隔约 35ms，不发送整个热点数组。所有事件都包含 errorCode；AP_FOUND 包含 ssid、rssi、authMode、bssid，其他阶段不要求 AP 字段。

authMode：0 OPEN、1 WEP、2 WPA-PSK、3 WPA2-PSK、4 WPA/WPA2、5 企业认证、6 WPA3-PSK、7 WPA2/WPA3、255 未知。当前 App 不支持 WEP 和企业认证。

#### 更换网络

```json
{"cmd":"CONFIGURE_WIFI","params":{"ssid":"Home WiFi","password":"example-password","hidden":false,"lockBssid":true,"bssid":"AA:BB:CC:DD:EE:FF"}}
```

```json
{"type":"WIFI_CONFIGURE_EVENT","data":{"stage":3,"result":1,"errorCode":0,"ssid":"Home WiFi","rssi":-52,"ip":"192.168.1.100"}}
```

stage：0 ACCEPTED、1 ASSOCIATING、2 OBTAINING_IP、3 SUCCESS、4 FAILED。result：0 处理中、1 成功、2 失败。每条事件都包含 stage、result、errorCode、ssid、rssi、ip；仅成功取得 IP 时 ip 为实际地址。

远程更换网络可能使 MQTT 断开。设备恢复上线后，App 使用 GET_WIFI_STATUS 核对目标 SSID、state 和 IP；不能把断线当成成功。

#### 清除网络

```json
{"cmd":"CLEAR_WIFI","params":{"disconnect":true}}
```

```json
{"type":"WIFI_CLEAR_RESULT","data":{"result":1,"errorCode":0,"state":0}}
```

result 和 state 的含义同上。清除成功且断开网络后，远程控制会离线，需要手动切换 BLE 配网。更换/清除网络前设备留出 500ms 发送独立过程/结果消息，但这不是交付保证；未收到最终结果时应通过 BLE 确认。

网络 errorCode：0 无错误、1 忙、2 参数错误、3 扫描失败、4 找不到 AP、5 认证失败、6 关联超时、7 DHCP 失败、8 不支持认证方式、9 内部错误、10 保存失败、11 已取消、12 会话无效、255 未知。扫描 FAILED 时 App 也应显示扫描失败，即使 errorCode 为 0。

### 固件版本与 OTA

| cmd | params | 回复 |
| --- | --- | --- |
| GET_FIRMWARE_VERSION | 无 | FIRMWARE_VERSION |
| START_FIRMWARE_UPDATE | modelId：1～255 整数；version：三段十进制版本字符串，每段 0～255 | FIRMWARE_UPDATE_STATUS |

当前型号 ID：1 为新 TM-310、2 为新 Y16、3 为新 Y17、4 为新 Y06。升级目标必须与实际型号一致。

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

### 车机连接

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

车机状态在活动阶段每 5 秒主动上报一次，BLE 和 MQTT 规则一致。停止时立即上报 STOPPING，完成后上报 STOPPED；STOPPED 状态不再周期上报。GET_TESLA_STATUS 仍立即独立回复当前状态，再次发起连接后恢复周期上报。App 对已确认的 STOPPED/断开状态不做心跳超时判断；手机控制通道重连后仍需重新查询。

stage：0 IDLE、1 SAVED、2 SCANNING、3 CONNECTING、4 WAITING_APPROVAL、5 PAIRED、6 AUTHENTICATED、7 FAILED、8 STOPPED、9 STOPPING。

errorCode：0 无错误、1 忙、2 VIN 无效、3 保存失败、4 扫描超时、5 连接失败、6 配对失败、7 认证失败、8 内部错误、9 密钥未授权、10 数据不可用、255 未知。

车机配对仍可能要求用户在车内确认。只有 configured=true、paired=true、connectionState=5、stage=6、errorCode=0 同时成立，才显示已认证；受理或连接建立都不能代替认证成功。

## 型号能力

| 型号 | 支持模式 | `SET_CHILD_LOCK` | 状态特有字段 | fault 范围 |
| --- | --- | :---: | --- | ---: |
| TM-Y06 | `COOL`、`HEAT` | 支持 | `childLock` | 0～3 |
| TM-Y16 | `COOL`、`MAX`、`ECO` | 不支持 | `voltage` | 0～7 |
| TM-Y17 | `COOL`、`MAX`、`ECO`、`HEAT` | 不支持 | `voltage` | 0～7 |
| TM-310 | `COOL`、`MAX`、`ECO`、`HEAT` | 不支持 | `voltage` | 0～7 |

TM-Y06 故障码：

| fault | 含义 |
| ---: | --- |
| 0 | 无故障 |
| 1 | NTC 短路 |
| 2 | NTC 开路 |
| 3 | 高温保护 |

TM-Y16、TM-Y17、TM-310 的 `fault` 为 0～7，对应无故障和设备 Er0～Er6 定义。

## 断线重连

MQTT 断线后按以下间隔主动重连：

```text
2 秒 → 4 秒 → 8 秒 → 16 秒 → 32 秒 → 60 秒
```

达到 60 秒后保持 60 秒间隔。MQTT 连接成功后，退避时间恢复为 2 秒。

每次重连必须：

1. 清除本地 `viewer` 在线状态；
2. 重新订阅 `cmd`；
3. 重新订阅 `viewer`；
4. 等待两个订阅成功；
5. 发布 `online=true`；
6. 等待新的或 Broker 保留的 `viewer` 消息；
7. 不主动发布 `state`。

## 日志与部署

FRIDGE-MQTT 输出 JSON 指令名、处理日志及业务事件名称；FRIDGE-BLE 用于手机 BLE 指令/连接；FRIDGE-APP 为共用状态日志。MQTT TX messageId 仅表示交给 MQTT 客户端，不代表 App 已收到。

当前使用本文列出的开发 Broker。生产环境需配置 TLS、每设备独立凭据和 Topic ACL。协议变更需同步更新 App 与设备固件；本次文档整合不改变 viewer HTTP 接口。
