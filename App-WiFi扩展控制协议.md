# App Wi-Fi 扩展控制

App 与 ESP32 的 BLE、MQTT 控制会话互斥；ESP32 与车机的 BLE 链路不受影响。
切换前由 App 确认旧连接关闭。固件不会关闭 NimBLE 或车机扫描/连接功能。

## 能力与消息

state 新增 `protocolFrames: 1`，原有基础控制命令兼容。
App 向 `fridge/{sn}/cmd` 发送 QoS 1、Retain=false：

```json
{"cmd":"PROTOCOL_FRAME","sessionId":"app-xxx","requestId":"app-xxx-1","params":{"frame":"a55a03c03caa"}}
```

frame 为完整 A5 5A 协议帧十六进制字符串，6～160 字节。保留原长度、异或校验和 AA 尾字节。
仅开放 07（氛围灯）、A0/A2（固件）、B0/B2/B4/B6（网络）、C0/C2/C4（车机）。
`app_process_remote_frame` 使用虚拟连接标识 FFFE 调用既有业务逻辑，不建立手机 BLE 连接。

设备向 `fridge/{sn}/event` 发布 QoS 1、Retain=false：

```json
{"type":"result","sessionId":"app-xxx","requestId":"app-xxx-1","accepted":true}
{"type":"frame","sessionId":"app-xxx","frame":"..."}
```

accepted 表示完整帧已交付业务入口，不代表业务成功；实际结果由 08/A1/A3/B1/B3/B5/B7/C1/C3 帧确认。
不支持或非法完整帧返回 accepted=false 和 error。业务参数错误沿既有业务结果上报。
最近 16 个已受理 requestId 去重，不重复执行；App 重连不重发旧控制，重新查询状态。

扩展异步事件发给最近一次扩展请求的 sessionId。当前面向单 App 扩展控制，多 App 并发时不保证各自持续收到扩展事件。
切换回 BLE 后车机通知目标通过 C0 重新注册，车机连接本身继续运行。

## 网络和 OTA

远程更换/清除网络前留出 500ms 发出受理及过程消息。该时间不是交付保证，App 必须根据实际回复或恢复上线后的查询确认，不能将掉线当作成功。
更换网络恢复 MQTT 后查询 B0，比对目标 SSID 和 IP；清除网络离线后通过 BLE 重新配网。
OTA 仍由设备自行下载、验证和重启；A3 经 MQTT 上报。App 等待 MQTT 恢复，通过 A0/A1 验证实际版本，不连接手机 BLE。

不记录 Wi-Fi 密码，不保留配置命令和事件。Broker 沿用当前开发配置；生产 TLS、身份认证与 Topic ACL 沿用主 MQTT 协议文档要求，本改动不新增账户授权体系。

## 控制台日志

- `FRIDGE-BLE`：手机 BLE 连接事件、BLE 收到的指令和实际 BLE Notify 分发。
- `FRIDGE-MQTT`：MQTT 收到的指令及扩展回包，RX/TX 日志包含 `transport=MQTT`、命令码和长度；受理日志包含 requestId 及重复标记，不输出完整凭据帧。
- `FRIDGE-APP`：与通道无关的共用状态日志，例如氛围灯状态更新、OTA 阶段，使用 `scope=shared` 标识，不代表手机通过 BLE 连接。

版本查询通过 MQTT 到达时，输出 `FRIDGE-MQTT: 收到App命令：查询固件版本 ...`；通过 BLE 到达时输出对应 `FRIDGE-BLE` 标签。
MQTT TX 的 messageId 表示已交给 MQTT 客户端，不代表 App 已收到；失败单独输出 TX failed。
