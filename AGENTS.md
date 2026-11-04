# Agent instructions

- 日常开发按用户要求检验到相关测试级别；只执行与修改和当前任务有关的检查，不扩大为全量回归、SCOPE 对比或正式发布。低影响文档整理不自动触发模型计算。
- 代码、测试和开发文档保留在 C 盘项目，通过 GitHub 做日常更新；小更新不自动升级版本、打安装包或创建 Release。用户发布统一放 D:/releases/streamstudio/<版本>，包含安装包、可行案例和必要用户说明。研究历史和维护归档不混入用户发布，详见 docs/development-and-release.md。
- 只保留一组 SCOPE 对比资料，位置是 D:\cases\StreamStudio\模型对比\SCOPE；仅当用户明确说需要与 SCOPE 对比时才运行所指定的范围。不得把对比案例、脚本或结果放回项目本体。
- 修改模型代码或共享着色器接口后，按需构建 histream 和 histream_runtime_resources；构建不附带测试或 SCOPE 对比。
- 功能源码、内置使用示例以及用户的原始工程和数据需保留，不把它们视为待删除的测试产物。
