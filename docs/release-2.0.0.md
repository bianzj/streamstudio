# HiStream Studio 2.0.0 — 品牌与运行时升级

> 历史发布说明，保留当时的 HiStream Studio 名称；产品此后更名为 Stream Studio（应用标识 `cn.stream.studio`，环境变量前缀 `STREAM_STUDIO_*`，`HISTREAM_STUDIO_*` 继续兼容，`STREAMSIM_*` 已停用），详见 README。

2.0.0 是产品命名和运行时标识的大版本。计算内核、工程格式和数值流程延续 1.1.4，本版本不引入新的物理模型或数值算法。

## 产品命名

- 产品名：**StreamSim → HiStream Studio**
- 底层引擎：**HiStream Engine**，继续使用 `histream.exe`
- 桌面可执行文件：`HiStreamStudio.exe`
- 安装包：`HiStream-Studio-Setup-<version>-x64-slim.exe`
- 快捷方式：**HiStream Studio**
- 应用标识：`cn.streamsim.desktop → cn.histream.studio`

由于 `appId` 改变，Windows 会把 2.0 视为新的应用。安装 2.0 前请先卸载 StreamSim 1.x；旧版用户数据会在首次启动时从 `%APPDATA%\StreamSim` 迁移到 `%APPDATA%\HiStream Studio`，桌面数据目录改为 `%LOCALAPPDATA%\HiStreamStudio`。

## 环境变量

新的应用级前缀为 `HISTREAM_STUDIO_*`，旧的 `STREAMSIM_*` 在本版本继续兼容：

| 新名称 | 兼容旧名称 |
|---|---|
| `HISTREAM_STUDIO_RESOURCE_ROOT` | `STREAMSIM_RESOURCE_ROOT` |
| `HISTREAM_STUDIO_DATA_ROOT` | `STREAMSIM_DATA_ROOT` |
| `HISTREAM_STUDIO_PORT` | `STREAMSIM_PORT` |
| `HISTREAM_STUDIO_SERVER_AUTOSTART` | `STREAMSIM_SERVER_AUTOSTART` |
| `HISTREAM_STUDIO_ENGINE_ALLOWLIST` | `STREAMSIM_ENGINE_ALLOWLIST` |
| `HISTREAM_STUDIO_PROXY_MANIFEST` | `STREAMSIM_HISTREAM_PROXY_MANIFEST` |
| `HISTREAM_STUDIO_PROXY_RUNNER` | `STREAMSIM_HISTREAM_PROXY_RUNNER` |
| `HISTREAM_STUDIO_PROXY_PYTHON` | `STREAMSIM_HISTREAM_PROXY_PYTHON` |

引擎和着色器内部仍使用 `STREAMSIM_*` 协议变量（例如 `STREAMSIM_FACET_STEP_SYNC`），这些属于引擎接口，不在本版本重命名。

## 保留的兼容标识

以下标识在 2.0 中保持不变，旧工程和 USD 文件仍可直接使用：

- 工程格式 `streamsim-project` 与 `schemaVersion = 6`；
- USD 自定义属性 `streamsim:*` 和 `customLayerData.streamsim.project`；
- 运行清单 `streamsim-run`；
- 构建清单 `kind = streamsim-build`；
- 引擎二进制 `histream.exe` 和 `models/histream` 源码目录；
- 临时目录前缀 `.streamsim-runtime-`、`.streamsim-transient-`。

## 计算设备

- `control.gpu` 的序号改为“独立显卡优先、排除 CPU 设备”后的顺序，面元与体元模式各有一套编号（界面“计算设备”列表按当前模式显示）。多 GPU 机器上，1.x 工程中 `gpu ≥ 1` 的设置可能对应另一块显卡，打开旧工程后请在“计算设备”中确认。
- CPU 后端通过随包附带的 Mesa lavapipe 运行，但仍需要系统中已有 Vulkan loader（`vulkan-1.dll`，随显卡驱动或 Vulkan Runtime 安装）。自动模式依赖 loader 1.3.234 及以上版本识别 `VK_ADD_DRIVER_FILES`；较旧的 loader 下请直接选择 CPU。

## USD 工具

OpenUSD 读取工具的输出名从 `streamsim-usd.exe` 改为 `histream-usd.exe`。服务端优先查找新名称，并回退到旧名称，便于过渡期使用旧安装包。

## 发布检查

发布 2.0.0 前应重新构建：

- GUI：`npm run build:gui`
- 引擎与 USD 工具：`npm run build:engine`
- 打包：`npm run stage:package && electron-builder --win nsis`
- SHA256SUMS 和 `release/StreamSim/...` 旧产物清理

2.0.0 不包含测试、SCOPE 对比或新的数值验证结论。
