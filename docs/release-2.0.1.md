# Stream Studio 2.0.1 发布说明

发布材料日期：2026-10-31；本说明补充日期：2026-11-01。

## 交付内容

当前版本为 **2.0.1**。产品名称为 Stream Studio，应用标识为 `cn.stream.studio`，桌面程序为 `StreamStudio.exe`。STREAM 影像引擎为 `stream.exe`，HiStream 三维引擎为 `histream.exe`；当前命名与目录见 [项目说明](../README.md)。

Windows x64 安装器为 `Stream-Studio-Setup-2.0.1-x64-slim.exe`，随包提供用户使用手册和模型理论手册。测试报告、验证记录、交付清单和 SHA256 文件保存在本机 `D:\releases\StreamStudio\2.0.1`。安装器未进行 Authenticode 数字签名。

五个内置精选案例为玉米、森林、城市、MODIS-ERA5 和 Landsat-ERA5。九个外部培训案例单独分发，包名为 `StreamStudio-案例包-20261031.zip`；案例分类、输入与操作入口见 [案例库与培训目录](example-library.md)。LSCT 培训条目使用已有组分温度进行正向模拟。

## 已有发布验证

以下结果来自 2026-10-31 的 2.0.1 正式发布报告；2026-11-01 的文档整理未新增运行验证。

| 范围 | 已有记录 |
| --- | --- |
| 运行入口 | 设备枚举、命令行用法、无效配置拒绝、桌面加载、MODIS EB、Landsat EB、MODIS RT，共七项检查通过 |
| 数值区域 | MODIS EB、Landsat EB、MODIS RT 各 4×4 像元；两项 EB 共 32 像元收敛，保留残差和 QA 记录 |
| 包完整性 | 安装器内嵌关键文件与被测目录一致；原生依赖、模块和五个内置案例输入引用有核验记录 |

使用上述结果时应保留其范围：

- 干净 Windows 环境的安装、升级、卸载及跨显卡兼容性尚未验收。
- HiStream 五种三维模式的完整数值回归，以及同化、LST/LSCT 反演、USD 往返和流体耦合专项验证尚未覆盖。
- 当前版未完成 MODIS/Landsat 全图重算或独立观测精度评估。案例既有收敛率只描述其参考运行的数值状态。
- 两项 EB 发布运行因 ERA5 包缺少地表位势 `z` 而跳过高程订正；实际运行条件与日志、摘要一致。

原始证据位于版本目录的 `验证记录`，正式报告为 `StreamStudio-2.0.1-测试报告.pdf`，可编辑文本为 `可编辑文档/测试报告.md`。

## 源码与复现

当前源码入口为 `C:\work\streamstudio`。已有构建清单标记 `dirty=true`，315 个源码指纹用于定位已构建内容；单独使用 Git HEAD 不能重建该发布工作树。[GitHub v2.0.1 发布页](https://github.com/bianzj/streamstudio/releases/tag/v2.0.1) 提供对应源码 ZIP，包含本地修改过的 nvpro_core 依赖源码；编译环境仍需按 README 安装和配置。运行配置中的本机路径在其他机器上使用时需调整。

本次文档整理同步版本号、交付入口和验证范围，并将 README 中旧的土壤热方法描述改为代码采用的 7 h 表面温度历史卷积。物理方法详见 [STREAM 引擎说明](../models/stream/README.md) 和 [理论手册](theory-manual.md)。
