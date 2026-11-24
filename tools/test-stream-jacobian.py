"""Gradient regression test of the assimilation EB Jacobian (review fix M3/A4, 2026-11-24).

Run:  python -m pytest tools/test-stream-jacobian.py
Builds the stream_jacobian_check target (models/stream/tests/jacobian_check.cpp, physics sources only, no GDAL) with the
existing build/stream tree and runs it against the engine's stream-defined data. It checks, for minimal synthetic scenes
(crop LAI 3 and 1, forest LAI 4, C4 grass), that dR/dLAI of the Ad residual equals the finite difference of the double
residual with the aerodynamic resistances recomputed at each LAI (the resistances carry Ad derivatives since M3).
STREAM_RUNTIME overrides the data dir parent (default models/bin_x64/Release); the test is skipped when the build tree
or cmake is missing.
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = Path(os.environ.get('STREAM_RUNTIME', ROOT / 'models/bin_x64/Release'))
BUILD = ROOT / 'build' / 'stream'
CMAKE = shutil.which('cmake') or r'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'


@pytest.mark.skipif(not BUILD.exists() or not Path(CMAKE).exists(), reason='build/stream tree or cmake not available')
def test_ad_jacobian_matches_finite_differences():
    built = subprocess.run([CMAKE, '--build', str(BUILD), '--config', 'Release', '--target', 'stream_jacobian_check'],
                           capture_output=True, encoding='utf-8', errors='replace', timeout=900)
    assert built.returncode == 0, built.stdout[-2000:] + built.stderr[-1000:]
    exe = BUILD / 'tests' / 'Release' / 'stream_jacobian_check.exe'
    ran = subprocess.run([str(exe), str(RUNTIME / 'stream-defined')], capture_output=True, encoding='utf-8', errors='replace', timeout=300)
    print(ran.stdout)
    assert ran.returncode == 0, ran.stdout[-3000:]
    assert '0 failed gradient checks' in ran.stdout
