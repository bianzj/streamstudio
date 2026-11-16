# Simulation Entry Implementation Plan

> 执行方式：本任务内顺序实施；用户明确不需要确认。保留现有功能分支、其他改动与全部旧案例，不提交或发布。

**Goal:** 已有模拟能力可查询，两条运行链可从一个受检查入口运行。
**Architecture:** 标准库Python负责注册、检查和调度；现有C++/GPU/Python求解与成像实现不变。
**Spec:** [设计](../specs/2026-11-09-simulation-entry-design.md)

## 约束

正式代码C:/work/streamstudio；检查与记录C:/cases/StreamStudio/simulation_catalog_20261109。复用现有codex/animation-mesh-isolation分支，新增文件为主。没有模型或shader变更，不构建或重跑长模拟。旧全面原生归并计划不标为完成。

## 步骤

- [x] 1. 在tools/scene-animation/tests/test_simulation_entry.py先编写输入、输出保护、来源变化和错误状态检查，观察缺少入口的失败。
- [x] 2. 新建simulation/capabilities.json、catalog.py、jobs.py及run_case.py；实现features/check/run，复用原生场景及耦合小镇工具。
- [x] 3. 新建samples/*.job.json和CURRENT_FEATURES.md，更新现有README/原生文档入口，区分物理求解与预设能力。
- [x] 4. 相关检查通过，真实小网格经统一入口产生RGB/IR；只读检查已封存小镇派生任务，核验其封存清单未改动，记录实际验证范围。

## 检查重点

check不得创建输出；未知字段/波段选项拒绝；所有运行子进程使用参数数组；已有输出拒绝；进程零退出不等于结果完成；源文件变化标记失败并保留日志。功能表源码和检查路径必须实际存在。

## 完成与验证

功能表15项，所有源码/检查路径存在。最终21项入口检查通过；独立审查的复制路径逃逸、视频部分帧范围和启动异常步骤状态三项均补充RED→GREEN回归检查。两次真实小网格GPU运行成功，最终记录在native_smoke_final，64×48 RGB/IR及原始辐亮度有限且非平坦。coupled-town配置只读检查通过；没有重算或改写旧小镇，v5封存清单SHA保持不变。

证据：C:/cases/StreamStudio/simulation_catalog_20261109/{entry_final.log,verification.json,progress.md,native_smoke_final/run.json}。没有改物理公式/着色器，没有构建、全量历史回归、现场精度验证、提交或发布。之前全面原生归并计划未执行。
