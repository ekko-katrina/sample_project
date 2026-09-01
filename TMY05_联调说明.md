# TM-Y05 ESP32-S3 蓝牙模拟端

本工程把 ESP32-S3 模拟为 TM-Y05 冰箱蓝牙模块，供已经完成的 App 联调。

## 蓝牙接口

- 广播名称：`Aurora Y05_XXXX`，`XXXX` 自动取 ESP32 蓝牙 MAC 地址最后 4 个十六进制字符（大写）。
- 主服务：`0xFFF0`
- App 写入：`0xFFF6`，支持 Read、Write 和 Write Without Response。
- ESP32 回传：`0xFFF7`，支持 Read、Write 和 Notify。
- 最多同时连接 2 台手机。

App 连接后应先订阅 `FFF7` 通知，再向 `FFF6` 写入协议原始字节。ESP32 支持单帧、拆包和多个帧粘在一次写入中的情况；帧头、长度、异或校验或帧尾错误会被丢弃并自动重新找帧头。

## 初始模拟状态

- 开机
- 摄氏度
- 制冷
- 童锁未锁
- 无故障
- 目标温度：-5 ℃
- 实际温度：3 ℃

初始状态回传帧与协议示例一致：

```text
A5 5A 09 00 40 00 FF FB 00 03 B1 AA
```

例如，App 设置目标温度为 -5 ℃：

```text
A5 5A 06 05 00 FF FB F8 AA
```

ESP32 接受每条有效的 `0x01` 至 `0x06` 指令后，都会通过 `FFF7` 回传一帧 `0x00` 全状态。App 也可以直接读取 `FFF7` 获取当前状态。

## 在固件中模拟温度和故障变化

其他任务可调用以下接口，调用后会立即向所有已订阅的 App 上报新状态：

```c
ble_tmy05_set_actual_temperature(25);  // 实际温度改为 25
ble_tmy05_set_fault(TMY05_FAULT_NTC_SHORT);
ble_tmy05_set_fault(TMY05_FAULT_NONE);
```

也可使用 `ble_tmy05_get_state()` 读取完整状态，修改后通过 `ble_tmy05_set_state()` 一次性更新。

## 编译、烧录和监视

在已加载 ESP-IDF 5.5 环境的终端中运行：

```powershell
idf.py build
idf.py -p COMx flash monitor
```

日志标签为 `TMY05-BLE`。收到和回传的完整协议帧都会以十六进制打印，方便与 App 日志逐字节比对。

## 串口控制台模拟机身操作

串口控制台使用 UART0、115200 波特率。在 `idf.py monitor` 中输入 `help` 可查看命令。每次操作都会修改冰箱状态，并立即通过 `FFF7` 通知 App：

```text
status                  显示完整状态
power on                开机
power off               关机
power toggle            模拟开关键
lock on                 童锁上锁
lock off                童锁解锁
lock toggle             模拟童锁键
mode cool               制冷模式
mode heat               制热模式
mode toggle             切换制冷/制热
unit c                  摄氏度
unit f                  华氏度
unit toggle             切换温度单位
target -5               目标温度改为 -5
actual 3                实际温度改为 3
fault 0                 无故障
fault 1                 NTC 短路
fault 2                 NTC 开路
fault 3                 75 度高温保护
```

App 发来的控制指令也会打印中文说明，例如 `收到App命令：开机`、`收到App命令：童锁上锁`，并紧接着打印当前完整状态。
