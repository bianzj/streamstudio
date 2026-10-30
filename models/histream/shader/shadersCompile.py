import sys
import os
import glob
import subprocess

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

inputShaderFiles = []
outputShaderFiles = []
for exts in ('*.vert', '*.frag', '*.comp', '*.geom', '*.rgen','*.rchit','*.rmiss','*.rahit','*.rint'):
	inputShaderFiles.extend(glob.glob(os.path.join(inPath, exts)))


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



