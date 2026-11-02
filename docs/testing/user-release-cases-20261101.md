# 2.0.1 用户案例运行核验

完成时间（UTC）：2026-11-01T15:26:25.547Z。打包的 stream.exe SHA256：7a7595b4a176b538d7772467c0efb8dc7c56c4e25570d8f4482f7b84ecc431df。

本次通过项目的配置转换接口调用 2.0.1 已打包影像引擎，执行交付副本的原样 project.json，四个案例均为 4×4 像元、2 个线程。MODIS 区域行为 [85,89)、列为 [107,111)；Landsat 行为 [299,303)、列为 [296,300)。使用随附 ERA5 / 组分温度输入，无常数温度替代。

| 案例 | 光学 / 热红外像元 | EB 收敛 / RT 输入 | 用时 |
| --- | --- | --- | --- |
| MODIS-ERA5 | 16 / 16 | 16 / 16 | 1.583 s |
| Landsat-ERA5 | 16 / 16 | 16 / 16 | 1.569 s |
| MODIS-LSCT | 16 / 16 | 读取随附 components.tif | 1.357 s |
| Landsat-LSCT | 16 / 16 | 读取随附 components.tif | 1.472 s |

四例均 exit 0，无 invalidInputPixels、unknownTypePixels、solarFailedPixels、thermalFailedPixels；两例 EB 各 16 像元收敛，failedPixels 和 noForcingPixels 均为 0。TIFF 输出存在且非空；逐输入、工程和输出 SHA256、日志、实际配置及引擎摘要保存在项目 .test/user-release-cases-20261101-ascii/。

初次在中文“案例”路径运行时，两例 ERA5 因无短文件名而被引擎拒绝；原始失败证据保留在 .test/user-release-cases-20261101/。交付副本改用 examples 名称，并明确要求用户复制到全英文路径后运行。D:/发布 本身有中文，不能省略这一步。此处是已知运行限制的交付适配，未修改或重新编译引擎。

EB 输入不含地表位势 z，交付副本明确设置 elevationCorrection=false。reference 目录仍为历史参考，不属于本次小区域输出。LSCT 由旧 EB 结果提供温度，不是温度反演或独立观测真值。原 D:/examples 九个案例均保留。

这四项检查不覆盖桌面按钮流程、干净机器安装/升级/卸载、跨显卡、三维案例完整回归、全图计算和科学精度验证。安装器的五个原有内置案例仍保留；其覆盖范围沿用原发布证据，不因本次影像案例通过而扩大。

手工复核入口（显式执行，不挂接构建流程）：将交付 examples 复制到全英文可写路径，然后运行：

```powershell
cd C:\work\streamstudio
node tools/check-user-release-cases.mjs D:/StreamStudioExamples .test/user-release-check-new
```

第二个参数必须是尚不存在的记录目录。历史发布七项检查保留在 .test/release-2.0.1-historical-records/，原报告正文见 [原发布报告](release-2.0.1-original-report.md)。
