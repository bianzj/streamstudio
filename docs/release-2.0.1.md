# Stream Studio 2.0.1 发布说明

原发布材料日期为 2026-10-31，本机用户交付整理为 2026-11-01。版本仍为 2.0.1，安装器保持原 SHA256，不重新构建、不创建新 GitHub Release。

## 用户交付

统一入口为 D:/发布/streamstudio/2.0.1，包含 Windows x64 安装器、用户使用手册、模型理论与方法手册、LICENSE、开始使用说明、清单与 examples。四个随附影像案例已按默认 4×4 区域通过打包引擎检查，详见 [案例验收记录](testing/user-release-cases-20261101.md)。请先把案例复制到全英文可写路径再运行；当前 ERA5 读取在无短文件名的中文路径下会失败。

五个原有内置案例仍在安装器中。原 D:/examples 九个培训候选保持原位，旧独立九例 ZIP 和此前混入发布目录的研究资料保存在 D:/cases/StreamStudio/发布整理归档/20261101-开发与研究资料。

## 验证范围

原 2026-10-31 报告记录设备枚举、命令行用法、无效配置拒绝、桌面加载、MODIS EB、Landsat EB、MODIS RT 共七项检查，三个 4×4 区域；原始证据在项目 .test/release-2.0.1-historical-records/，正文见 [原报告](testing/release-2.0.1-original-report.md)。

本轮新增四个实际外部影像案例的 4×4 检查；两项 EB 各 16 像元收敛，两项 RT 使用随附组分温度文件。EB 的 ERA5 包缺少地表位势 z，交付配置关闭高程订正。

- 干净 Windows 环境的安装、升级、卸载及跨显卡兼容性尚未验收，安装器未进行 Authenticode 签名。
- HiStream 全部三维模式的完整数值回归、同化、温度反演、USD 往返和流体耦合专项验证尚未覆盖。
- 没有完成全图重算或独立观测精度评估；本轮通过的是影像引擎和输入配置检查，不是新增桌面端到端验收。

## 开发与复现

代码、测试和开发文档以 C:/work/streamstudio 为入口，通过 GitHub 做小更新，不自动变成正式发布。规则见 [开发更新与用户发布](development-and-release.md)。

已有构建清单标记 dirty=true，315 个源码指纹用于定位原构建内容；单独 Git HEAD 不能重建该发布工作树。[原 GitHub v2.0.1 发布页](https://github.com/bianzj/streamstudio/releases/tag/v2.0.1) 的既有资产保持不变，源码 ZIP 含当时本地修改的 nvpro_core。原构建和源文件快照保存在 D:/cases/StreamStudio/发布整理归档/20261101-开发与研究资料。本次本机案例默认配置和目录整理尚未同步到原 GitHub 发布资产。
