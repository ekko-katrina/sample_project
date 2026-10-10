# Tesla BLE 官方协议移植

固件：1.0.7。基准仓库：https://github.com/teslamotors/vehicle-command

基准提交：`a4b43c1eff0e09d77deb9f2dce97031141fe8c8a`，2026-09-25。
工作区 `C:\code\vehicle-command` 已快进至此提交。已有中文说明和 IDE 文件保留。

## 连接与请求

1. 根据 VIN 生成 `S + SHA1(VIN)前8字节的十六进制 + C`，扫描并连接车辆。
2. 发现 0211 服务、0212 写特征、0213 收特征；在实际特征范围发现 2902 CCCD，写 `02 00` 订阅 Indication。发现或订阅失败则关闭车辆连接。
3. 交换 MTU；失败回退 MTU=23。每次按 MTU−3 切分两字节大端长度头和 protobuf 消息，使用 ATT Write Request，等待每片确认。超时关闭连接，取消操作可中断等待。
4. 接收端采用独立字节流和深度为 5 的消息队列，最大消息 1024 字节。保留同批到达的多帧；半帧间隔超过 1 秒时丢弃。解析快照不与接收回调共用写入缓冲。
5. 请求串行执行。VCSEC 每个请求使用新的随机路由地址；Infotainment 同时检查域、路由地址和 request_uuid。丢弃无关广播及旧请求回复。

## 会话验证

- SessionInfoRequest 发送设备公钥和随机 UUID，按照官方使用请求 UUID 作为 SessionInfo HMAC 的 challenge；不再发送另一个独立 challenge。
- 会话信息必须关联当前请求，且在请求创建后 4 秒内到达。
- P-256 ECDH 的共享 x 坐标经 SHA1 截取前 16 字节得到 AES 会话密钥。
- `K' = HMAC-SHA256(K, "session info")`。
- SessionInfo tag 验证：`HMAC-SHA256(K', TLV(signature_type=6, VIN, challenge=request_uuid) || FF || 原始SessionInfo字节)`，常量时间比较 32 字节 tag。
- 校验成功前，不安装车辆公钥、epoch、counter、clock，不上报认证成功，不保存 paired=1。已建立会话更新时拒绝替换车辆公钥，counter 不回退；同 epoch 的旧时钟信息不覆盖较新会话。
- 车辆附带的签名 SessionInfo 可在认证故障时更新会话，然后按官方错误分类重试。
- 与官方 GetError 一致，当前请求关联的明确 KEY_NOT_ON_WHITELIST / UNKNOWN_KEY_ID 是授权拒绝。拒绝不创建会话，转为 App error=09；超时、HMAC/GCM 失败不能作为钥匙被删除的证据。
- 不输出 ECDH 共享密钥和会话密钥。

## 查询和控制

- 所有已认证命令递增 counter，拒绝计数器溢出；默认有效期 5 秒，flags=2 请求加密回复，flags 同时参与 AAD。
- 加密回复校验 GCM、请求 tag 绑定及错误元数据，验证成功后更新该请求的 64 位防重放窗口，支持合法乱序，拒绝重复 counter。
- 按官方兼容 2024.38 之前忽略加密回复标志的固件：无 AES-GCM response signature 时按明文解析；存在 GCM 签名而格式错误或验证失败时不能降级明文。
- 仅官方列出的暂时故障可重试：BUSY、TIMEOUT、INVALID_SIGNATURE、INVALID_TOKEN_OR_COUNTER、INTERNAL、INCORRECT_EPOCH、TIME_EXPIRED、TIME_TO_LIVE_TOO_LONG。未知错误和权限、VIN、格式等永久错误不盲目重发。
- 锁车、解锁、唤醒等待 VCSEC 终结回复。成功的 CommandStatus ACK 本身不是最终执行结果；WAIT 可以重试，ERROR/nominalError 返回失败。发送后没有终结回复返回 `TESLA_ERR_COMMAND_UNCONFIRMED (0x7502)`，动作可能已执行，不自动重复物理控制指令。
- C0～C4 仅提供 VIN、查询和停止连接；App 目前没有暴露车辆控制命令，0x7502 是固件 C API 返回值，不是新增 App errorCode。
- 驾驶查询必须取得实际档位、存在的速度和功率字段，并检查 ActionStatus。速度浮点字段即使值为零，也按字段存在性优先选择，不能用非零判断字段是否存在。
- 保留 odometer/速度/功率存在性和车辆时间戳。带时间戳的重复或倒退样本不能刷新 15 秒有效期；旧固件不带时间戳时使用有效回复接收时间，不能推断车辆采样年龄。车速单位仍需实车对照确认。
- 车身状态回复只有解析成功才更新车身缓存，不会将驾驶数据标记为有效。

## App 契约

帧格式仍为现有 C0～C4，不新增字段或数值帧。App 成功条件为 configured=1、paired=1、state=05、stage=06、error=00。state=05 表示 VCSEC 会话通过上述校验；Infotainment 会话在驾驶查询前另行校验。stage=06 不代表首次驾驶数据已经取得。

既有 C2 操作边界、15 秒驾驶数据看门狗、每 5 秒 C3 心跳、C4 停止及 NVS auto_connect=0 保持不变。心跳不会刷新驾驶数据有效期。手机重连或 C0 不恢复已停止的车辆连接。

## 验证与实车验收

主机测试：`tests/tesla_official_protocol_test.py`。使用 Python cryptography 作为独立 P-256/HMAC/AES-GCM 车辆端，执行实际 C 会话、拆帧、解析代码及原样提取的请求交换函数；RTOS/ATT 和加密后端为测试桩。Windows TCC 0.9.27 对 DLL 自动聚合初始化有缺陷，测试临时副本使用等价的清零和 descriptor 赋值，固件源不作此替换。

```powershell
python tests/tesla_official_protocol_test.py --cc PATH\TO\tcc.exe --idf C:\esp\v5.5.4\esp-idf
```

App 验证：`node --experimental-default-type=module tests/tesla-vehicle.test.mjs`。

实车需覆盖：首次授权、已有钥匙重连、两个域认证、休眠唤醒/epoch 改变、删除钥匙、多帧广播、控制终结回复和结果未确认、C2/C4 取消、停止后重启、不同固件版本回复、驾驶时间戳和车速单位。编译及主机测试不替代实车验证。

官方原始基础 proto 位于 `proto/`；`car_server_slim.proto` 是保留官方字段号和存在性的精简定义。更新基础 proto 后运行 `proto/gen_proto.sh`，生成文件须一起提交。官方代码采用 Apache-2.0，许可证见 `vehicle-command-LICENSE.txt`。
