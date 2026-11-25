# ==========================================================================
# 文件：shadersCompile.py
# 功能：开发用脚本：用 glslangValidator 把某个着色器子目录下的 GLSL 源文件批量编译为 SPIR-V。
# 步骤：
#   1. 由 versionName 选定子目录（如 voxeleb、voxelrt、RayTracing），输入与输出目录相同
#   2. 收集 *.vert/frag/comp/geom/rgen/rchit/rmiss/rahit/rint
#   3. 逐个调用 glslangValidator -V ... --target-env vulkan1.3，输出同名 .spv
#   4. 汇总成功/失败列表
# 用法：修改 versionName 后直接运行；需要 glslangValidator 在 PATH 中。
# 注意：仅开发时手工使用，不参与正式构建流程（待核对）；输出路径拼接使用反斜杠，面向 Windows。
# ==========================================================================

import sys
import os
import glob
import subprocess

# 编译目标子目录：取消注释需要的一行（只保留一个有效的 versionName）。
# versionName = r'D:\code\filed\shader\voxelet\\'
# versionName = 'RayTracing_Lambert'
# versionName = 'VoxelTracing'
# versionName = 'VoxelTriTracing'
versionName = "voxeleb"
# versionName = "voxelrt"
# versionName = "RayTracing"

inPath = os.path.join(os.path.dirname(__file__),versionName)
outPath = os.path.join(os.path.dirname(__file__),versionName)
# outPath = r"C:\\Users\\14836\\Documents\\GitHub\bin_x64\\Debug\shader\\" + versionName + "\\"
# outPath = r"C:\\Users\\14836\\Documents\\GitHub\bin_x64\\Release\shader\\" + versionName + "\\"

# 步骤 2：按扩展名收集着色器源文件。
inputShaderFiles = []
outputShaderFiles = []
for exts in ('*.vert', '*.frag', '*.comp', '*.geom', '*.rgen','*.rchit','*.rmiss','*.rahit','*.rint'):
	inputShaderFiles.extend(glob.glob(os.path.join(inPath, exts)))


# 步骤 3：逐个编译，记录失败的文件。
failedshaders = []
for shaderfile in inputShaderFiles:
		print("\n-------- %s --------\n" % shaderfile)
		outshaderfile = outPath + '\\' + os.path.basename(shaderfile)
		if subprocess.call("glslangValidator -V %s -o %s.spv  --target-env vulkan1.3" % (shaderfile, outshaderfile), shell=True) != 0:
			failedshaders.append(shaderfile)

print("\n-------- Compilation result --------\n")

if len(failedshaders) == 0:
	print("SUCCESS: All shaders compiled to SPIR-V")
else:
	print("ERROR: %d shader(s) could not be compiled:\n" % len(failedshaders))
	for failedshader in failedshaders:
		print("\t" + failedshader)



