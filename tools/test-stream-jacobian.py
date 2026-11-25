# ==========================================================================
# 文件：test-stream-jacobian.py
# 功能：同化 EB（能量平衡）雅可比的梯度回归测试（评审修复 M3/A4，2026-11-24）。
#   不直接调用 stream.exe，而是编译并运行 C++ 检查程序 stream_jacobian_check
#   （models/stream/tests/jacobian_check.cpp，只含物理源码、不依赖 GDAL），
#   由它比较自动微分（Ad）残差对 LAI 的导数与双精度残差的有限差分。
# 主要步骤：
#   1. 定位构建树 build/stream 与 cmake（找不到则 pytest skip）
#   2. cmake --build 构建目标 stream_jacobian_check（Release，超时 900 s）
#   3. 以引擎的 stream-defined 数据目录为参数运行检查程序（超时 300 s）
#   4. 断言退出码为 0 且输出含 "0 failed gradient checks"
# 关系：检查场景由 jacobian_check.cpp 内置（作物 LAI 3 与 1、森林 LAI 4、C4 草）；
#   有限差分时每个 LAI 下重算空气动力阻力（M3 之后阻力也携带 Ad 导数）。
# 约定：环境变量 STREAM_RUNTIME 指定引擎目录（缺省 models/bin_x64/Release），
#   其下的 stream-defined 提供类别表等数据。
# ==========================================================================

"""Gradient regression test of the assimilation EB Jacobian (review fix M3/A4, 2026-11-24).

Run:  python -m pytest tools/test-stream-jacobian.py
Builds the stream_jacobian_check target (models/stream/tests/jacobian_check.cpp, physics sources only, no GDAL) with the
existing build/stream tree and runs it against the engine's stream-defined data. It checks, for minimal synthetic scenes
(crop LAI 3 and 1, forest LAI 4, C4 grass), that dR/dLAI of the Ad residual equals the finite difference of the double
residual with the aerodynamic resistances recomputed at each LAI (the resistances carry Ad derivatives since M3).
STREAM_RUNTIME overrides the data dir parent (default models/bin_x64/Release); the test is skipped when the build tree
is missing and fails with a clear message when cmake cannot be found (PATH, vswhere, Program Files).
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = Path(os.environ.get('STREAM_RUNTIME', ROOT / 'models/bin_x64/Release'))
BUILD = ROOT / 'build' / 'stream'
# 作用：定位 cmake。顺序：PATH → vswhere（Visual Studio Installer 自带）找到的 VS 安装 → 常见安装目录
# （Program Files 下任意版本/版本号的 Visual Studio）。都找不到返回 None（由测试给出清楚的错误信息）。
_VS_CMAKE = Path('Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')


def find_cmake():
    found = shutil.which('cmake')
    if found:
        return found
    for base in filter(None, (os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles'))):
        vswhere = Path(base) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
        if not vswhere.exists():
            continue
        try:
            out = subprocess.run([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'],
                                 capture_output=True, encoding='utf-8', errors='replace', timeout=30).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        for line in out.splitlines():
            candidate = Path(line.strip()) / _VS_CMAKE
            if line.strip() and candidate.exists():
                return str(candidate)
    for base in filter(None, (os.environ.get('ProgramFiles'), os.environ.get('ProgramFiles(x86)'))):
        # 例如 C:\Program Files\Microsoft Visual Studio\18\Professional 或 ...\2022\Community
        for candidate in sorted((Path(base) / 'Microsoft Visual Studio').glob('*/*/' + _VS_CMAKE.as_posix()), reverse=True):
            return str(candidate)
    return None


CMAKE = find_cmake()


# 作用：构建并运行 stream_jacobian_check，失败时断言信息只保留输出尾部（便于阅读）。
# build/stream 不存在时跳过；构建树在但找不到 cmake 时报错并列出已查找的位置（而不是静默跳过）。
@pytest.mark.skipif(not BUILD.exists(), reason='build/stream tree not available')
def test_ad_jacobian_matches_finite_differences():
    if not CMAKE:
        pytest.fail('找不到 cmake：已依次查找 PATH、vswhere 报告的 Visual Studio 安装、Program Files 下的 Visual Studio；'
                    '请把 cmake 加入 PATH 后重试')
    built = subprocess.run([CMAKE, '--build', str(BUILD), '--config', 'Release', '--target', 'stream_jacobian_check'],
                           capture_output=True, encoding='utf-8', errors='replace', timeout=900)
    assert built.returncode == 0, built.stdout[-2000:] + built.stderr[-1000:]
    exe = BUILD / 'tests' / 'Release' / 'stream_jacobian_check.exe'
    ran = subprocess.run([str(exe), str(RUNTIME / 'stream-defined')], capture_output=True, encoding='utf-8', errors='replace', timeout=300)
    print(ran.stdout)
    assert ran.returncode == 0, ran.stdout[-3000:]
    assert '0 failed gradient checks' in ran.stdout
