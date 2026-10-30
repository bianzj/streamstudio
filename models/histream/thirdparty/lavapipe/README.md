# Mesa lavapipe (CPU Vulkan driver)

HiStream 的 VoxelRT / VoxelEB / Raytracing 在 `computeBackend` 为 `cpu` 或 `auto` 时，通过本目录的 Mesa lavapipe 软件 Vulkan 驱动在 CPU 上运行原有着色器与光线追踪管线。构建时 `histream_runtime_resources` 会把本目录复制到 `histream.exe` 旁的 `lavapipe/`。

- 来源：Mesa 26.2.4，pal1000/mesa-dist-win 发布包 `mesa3d-26.2.4-release-msvc.7z` 的 `x64/` 目录
  <https://github.com/pal1000/mesa-dist-win/releases/tag/26.2.4>
- 发布包 SHA-256：`351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695`
- `vulkan_lvp.dll` 仅依赖 Windows 系统 DLL（静态链接 LLVM 与 CRT）。
- 许可：Mesa 主体为 MIT 许可，个别文件按其 SPDX 标识（<https://docs.mesa3d.org/license.html>）；内置 LLVM 为 Apache-2.0 WITH LLVM-exception。

升级时替换 `vulkan_lvp.dll` 与 `lvp_icd.x86_64.json`，并更新上方版本与校验值。
