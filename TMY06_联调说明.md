# ESP32-S3 多机型 BLE 模拟器联调说明

本工程将一块 ESP32-S3 模拟为五种 TEMAI 冰箱 BLE 设备。无需重新烧录，长按开发板 BOOT 按键即可循环切换旧协议 Y06 和四种新协议设备。

## 快速切换设备

BOOT 按键使用 GPIO0。长按约 2 秒，串口出现“请松开按键”后松开，设备会软重启并应用下一档：

```text
旧版 Y06 → 新版 Y17 → 新版 Y16 → 新版 TM-310 → 新版 Y06 ─┐
    ↑                                                        │
    └────────────────────────────────────────────────────────┘
```

档位保存在 NVS，断电或普通复位不会丢失。首次烧录或 NVS 中没有档位记录时，默认启动旧版 Y06。

必须松开 BOOT 后才会重启，避免 GPIO0 保持低电平使 ESP32-S3 进入下载模式。短按 BOOT 不执行操作。

## 五种档位

| 档位 | 广播名称 | 新协议 Service Data | APP 写入 | ESP32 通知 | 状态帧 |
| --- | --- | --- | --- | --- | --- |
| 旧版 Y06 | `Aurora Y06_XXXX` | 无 | `FFF7` | `FFF7` | 19 字节，不含电压，含氛围灯设置 |
| 新版 Y17 | `GLACIER Y_XXXX JUNIPER` | `54 4D 01 03` | `FFF1` | `FFF2` | 14 字节，含电压 |
| 新版 Y16 | `GLACIER Y_XXXX` | `54 4D 01 02` | `FFF1` | `FFF2` | 14 字节，含电压 |
| 新版 TM-310 | `GLACIER 3_XXXX` | `54 4D 01 01` | `FFF1` | `FFF2` | 14 字节，含电压 |
| 新版 Y06 | `AURORA Y06_XXXX` | `54 4D 01 04` | `FFF1` | `FFF2` | 19 字节，不含电压，含氛围灯设置 |

`XXXX` 自动取 ESP32-S3 BLE 固定身份 MAC 地址最后两个字节，使用大写十六进制且不包含冒号。所有档位的主服务均为 `FFF0`，最多同时连接两台手机。

BLE 的 16-bit Service Data 原始 AD 结构以小端 UUID 开头，因此抓取到的新协议原始载荷为：

```text
F0 FF 54 4D 01 XX
```

APP 解析 `FFF0` UUID 后获得的 Service Data 内容为协议规定的 `54 4D 01 XX`。

## GATT 属性

新协议严格按协议文件提供：

- `FFF1`：Write，APP → ESP32。
- `FFF2`：Notify，ESP32 → APP。

旧版行为保持不变：

- 读、写特征 UUID 均为 `FFF7`；写特征支持 Read、Write、Write Without Response，通知特征支持 Read、Write、Notify。

APP 连接后先订阅当前档位的通知特征，再写入协议原始字节。ESP32 支持完整帧、拆包，以及一次写入包含多帧的粘包；帧头、长度、异或校验或帧尾错误会被丢弃并自动重新寻找下一帧头。

## 初始模拟状态

所有档位启动后均初始化为：

- 开机、摄氏度、无故障。
- 冷藏/制冷模式。
- 目标温度 `-5`，实际温度 `3`。
- Y06 童锁未锁。
- Y17、Y16、TM-310 实际电压默认为 `12.67 V`，协议传输值为 `1267`。

Y06 初始状态帧：

```text
A5 5A 10 00 40 00 FF FB 00 03 00 00 00 FF FF 00 00 A8 AA
```

Y17、Y16、TM-310 初始状态帧：

```text
A5 5A 0B 00 40 00 FF FB 00 03 04 F1 46 AA
```

ESP32 接受当前型号支持的任何有效命令后，都会通过当前通知特征回传一帧 `0x00` 全状态。

## 型号能力差异

| 型号 | 支持模式 | 童锁命令 | 故障码 |
| --- | --- | --- | --- |
| 旧版/新版 Y06 | 制冷、制热 | 支持 `0x06` | `0x00`～`0x03` |
| Y17 | 冷藏、MAX、ECO、制热 | 不支持 | `0x00`～`0x07` |
| Y16 | 冷藏、MAX、ECO | 不支持 | `0x00`～`0x07` |
| TM-310 | 冷藏、MAX、ECO、制热 | 不支持 | `0x00`～`0x07` |

不属于当前型号的模式、童锁命令或非法位值不会修改状态，也不会发送成功状态回包。

## 串口控制台

UART0 使用 115200 波特率。在 `idf.py monitor` 中输入 `help` 查看命令。机身模拟操作会立即通过当前通知特征上报：

```text
status                         显示档位和完整状态
power on|off|toggle            开关机
lock on|off|toggle             Y06 童锁操作
mode cool|max|eco|heat|toggle  切换当前型号支持的模式
unit c|f|toggle                切换温度单位
target -5                      设置目标温度
actual 3                       设置实际温度
fault 0                        设置故障码
voltage 1265                   设置电压为 12.65 V，仅非 Y06 型号
```

日志标签主要为 `FRIDGE-BLE`、`FRIDGE-CONSOLE` 和 `PROFILE-SWITCH`。收到和回传的协议帧都会以十六进制打印。

## 固件接口

其他任务仍可使用原有状态接口：

```c
ble_tmy06_set_actual_temperature(25);
ble_tmy06_set_fault(TMY06_FAULT_NTC_SHORT);

tmy06_state_t state;
ble_tmy06_get_state(&state);
state.target_temperature = -10;
ble_tmy06_set_state(&state);
```

非 Y06 档位可以设置百分之一伏单位的实际电压：

```c
ble_fridge_set_actual_voltage(1265);  // 12.65 V
```

## 编译、烧录和监视

在 ESP-IDF 5.5 环境终端中运行：

```powershell
idf.py build
idf.py -p COMx flash monitor
```

后续切换五种设备只需要长按 BOOT，不需要再次烧录。

## Y06 氛围灯联调

统一协议和非统一协议 Y06 均支持 0x07，四字节数据段为操作类型 + RGB：00 修改制冷颜色、01 修改制热颜色、10 常亮、11 呼吸。详细帧格式见 [Y06 协议](TM-Y06%20APP蓝牙通讯协议.md)。

灯效和两套颜色保存到 NVS `y06_light/settings_v1`，两种 Y06 档位共用配置。设置完成后可断开 APP，用控制台 `mode` 切换制冷/制热验证自动切色，重启后确认配置仍保留。

模拟器使用现有 GPIO48 板载 WS2812 预览常亮/呼吸，约 3 秒一个呼吸周期。`CONFIG_TMY06_AMBIENT_BOARD_PREVIEW` 可关闭板载预览；接实际冰箱灯控时，需要将 RGB 输出适配到真实硬件，不能直接假定外接灯也使用 GPIO48。

### 板载 RGB 预览超时

如果日志在 `广播已启动` 后出现 `rmt_tx_wait_all_done: flush timeout`，检查是否仍在使用旧版 LED 驱动的 1ms 等待和 `ESP_ERROR_CHECK`。新版驱动将发送等待改为 100ms，并返回错误；板载预览初始化失败时，主程序记录警告后继续启动 BLE、控制台和其他服务。运行中发送失败则停止预览任务，避免重复排队或修改仍被 RMT 引用的数据。

这类错误不会清除 NVS 中的灯效和颜色。若需单独排查板载 RGB，可在 `menuconfig -> TMY06 firmware settings` 关闭 `Preview Y06 ambient light on onboard RGB LED (GPIO48)`，重新编译烧录后继续验证 BLE 设置与回读。
