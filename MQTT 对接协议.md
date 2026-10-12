# ESP32 MQTT 对接协议

本文档只说明 ESP32 固件如何连接 MQTT Broker、订阅和发布消息，以及必须遵守的通讯规则。

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

ESP32 只使用以下四个 Topic：

| ESP32 动作 | Topic | QoS | Retain | 用途 |
| --- | --- | ---: | ---: | --- |
| 订阅 | `fridge/{sn}/cmd` | 1 | 必须否 | 接收设备控制命令 |
| 订阅 | `fridge/{sn}/viewer` | 1 | 是 | 接收浏览器在线状态 |
| 发布 | `fridge/{sn}/online` | 1 | 是 | 发布 ESP32 在线状态 |
| 发布 | `fridge/{sn}/state` | 1 | 是 | 发布完整设备状态 |

示例 SN `TMY06-TEST-001`：

```text
fridge/TMY06-TEST-001/cmd
fridge/TMY06-TEST-001/viewer
fridge/TMY06-TEST-001/online
fridge/TMY06-TEST-001/state
```

ESP32 不订阅通配符 Topic，不接收其他设备的消息。

## 3. 启动流程

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

## 浏览器在线状态

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
- 收到 `online=false`，立即停止发布 `state`；
- 连续 90 秒未收到新的 `online=true`，按浏览器离线处理；
- MQTT 断开时立即将本地浏览器状态置为离线；
- MQTT 重连后等待新的或 Broker 保留的 `viewer` 消息，不沿用断线前的在线状态。

发送端应在浏览器在线期间以小于 90 秒的周期刷新 `online=true`，当前推荐周期为 30 秒。

## 状态上报条件

ESP32 只有同时满足以下条件才允许发布 `state`：

```text
MQTT 已连接
且 cmd、viewer 均订阅成功
且 viewer 在线状态未超时
```

浏览器在线期间，以下情况发布完整 `state`：

- `viewer` 从离线变为在线；
- 收到 `GET_STATE`；
- MQTT 设置命令执行成功；
- BLE 控制改变设备状态；
- 串口或机身模拟操作改变设备状态；
- 当前温度、电压或故障码变化；
- 距离上次周期起点达到 5 分钟。

状态变化时立即发布，不等待 5 分钟周期。

浏览器离线期间：

- ESP32 继续维护本地最新状态；
- 不发布状态变化；
- 不执行 5 分钟兜底上报；
- 不保存待补发的历史消息；
- 浏览器再次在线时，只发布一条当时的最新完整状态。

## 完整状态格式

Topic：`fridge/{sn}/state`。QoS 为 1，Retain 为 `true`。每次必须发布完整状态，不能只发布发生变化的字段。

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

## 命令格式

ESP32 从 `fridge/{sn}/cmd` 接收命令。命令使用 QoS 1，Retain 必须为 `false`。Payload 最大 1024 字节。

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
| `GET_STATE` | 无 | 浏览器在线时发布当前完整状态 |
| `SET_POWER` | `powerOn`: boolean | 设置开关机状态 |
| `SET_MODE` | `mode`: string | 设置当前型号支持的模式 |
| `SET_TEMPERATURE_UNIT` | `unit`: string | 设置温标并换算两个温度值 |
| `SET_TARGET_TEMPERATURE` | `temperature`: integer | 设置当前温标下的目标温度 |
| `SET_CHILD_LOCK` | `locked`: boolean | 设置童锁，仅TM-Y06 支持 |

命令不包含 `id`，协议不定义 `reply` Topic。命令成功后，以新的完整 `state` 作为最终结果。

ESP32 无论 `viewer` 是否在线都会接收并执行合法命令；但 `viewer` 离线时不发布执行后的 `state`。

### 命令校验

```text
校验 UTF-8
  → 解析 JSON
  → 校验 cmd
  → 校验 params 和 JSON 类型
  → 校验当前型号能力
  → 修改统一设备状态
  → viewer 在线时发布完整 state
```

规则：

- `cmd` 必须是非空字符串，最大 64 字节；
- 参数必须存在且类型正确；
- 未知附加字段可以忽略；
- 未知命令、参数错误或型号不支持时不修改状态；
- 非法命令只记录本地日志，不发布 MQTT 错误消息；
- QoS 1 允许重复投递，因此命令必须保持绝对值设置语义；
- 不得增加 `TOGGLE`、递增、递减等非幂等命令，除非重新增加命令去重机制。

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
