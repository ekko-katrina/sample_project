# ESP32-S3 固件构建

本项目在 ESP-IDF **v5.5.4** 上验证，目标芯片为 **ESP32-S3**。

## 首次构建

安装 ESP-IDF v5.5.4 及其工具链，并在已激活 ESP-IDF 环境的终端中执行：

```sh
idf.py set-target esp32s3
idf.py build
```

`sdkconfig.defaults` 提供首次构建的默认配置，包括蓝牙、NimBLE 和 16 MiB 分区配置。`sdkconfig` 和 `build/` 是本地生成文件，不在 Git 中；换电脑时不要复制旧构建缓存。

`components/Middlewares` 原来是本地空目录，项目不依赖它。ESP-IDF 会自动发现 `components/BSP` 和 `components/tesla_ble`。

`tesla_ble` 依赖的 `protobuf-c` 组件已包含在 ESP-IDF v5.5.4 中。`proto-c/` 下的生成源码已提交，正常构建无需安装 `protoc-c`。只有修改协议定义或缺少生成源码时，才需要按 [协议源码说明](components/tesla_ble/proto/README.md) 重新生成。

## 编辑器与烧录

仓库中的 `.vscode` 设置包含本机 SDK/串口路径及原开发电脑的 Windows 工具路径。换电脑后需要选择本机 ESP-IDF 环境与串口，并检查编辑器配置。具体修改文件、Windows/macOS 路径示例及排错步骤见 [两台电脑构建与烧录环境切换说明](两台电脑构建与烧录环境切换说明.md)。

在 ESP-IDF 终端中也可直接指定本机串口：

```sh
idf.py -p <本机串口> flash monitor
```
