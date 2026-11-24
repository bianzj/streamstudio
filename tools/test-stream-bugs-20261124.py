"""Regression checks for the second batch of 2026-11-24 STREAM engine fixes (C4, C5, T1, ...). Small synthetic scenes.

Run:  python -m pytest tools/test-stream-bugs-20261124.py
      (STREAM_RUNTIME overrides the engine dir, default models/bin_x64/Release; STREAM_FIX_CASE the case)
"""
import importlib.util
import json
from pathlib import Path

import numpy as np
import pytest
from osgeo import gdal

gdal.UseExceptions()
HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location('fixes20261121', HERE / 'test-stream-fixes-20261121.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)
CASE, run, eb_config, rt_config, read = base.CASE, base.run, base.eb_config, base.rt_config, base.read


def summary(out):
    return json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))


# ---------- C4: the deprecated maxStabilityIterations no longer takes part in range validation ----------
@pytest.mark.parametrize('value', [0, -3])
def test_deprecated_max_stability_iterations_is_not_validated(tmp_path, value):
    ret, _ = run(tmp_path, 'msi', eb_config(maxStabilityIterations=value))
    assert ret.returncode == 0, ret.stderr + ret.stdout


# ---------- C5: RT imaging reads energy.diffuseFraction; -1 (Erbs) needs shortwave and is rejected ----------
def rt_optical(tmp_path, tag, **energy):
    config = rt_config(tmp_path, tag, thermal=False)
    config['optical'] = dict(enabled=True, bands=[dict(name='R', center=650, fwhm=40)])
    config['temperature']['energy'].update(energy)
    return config


def test_rt_diffuse_fraction_is_read(tmp_path):
    values = {}
    for name, extra in (('d0', dict(diffuseFraction=0.0)), ('d1', dict(diffuseFraction=1.0)), ('d02', dict(diffuseFraction=0.2)), ('def', {})):
        ret, out = run(tmp_path, name, rt_optical(tmp_path, name, **extra))
        assert ret.returncode == 0, ret.stderr + ret.stdout
        values[name] = read(out / 'optical_reflectance.tif')
    assert values['d0'] != values['d1'], values
    assert values['def'] == values['d02']  # the default stays 0.2


@pytest.mark.parametrize('value', [-1, 1.5])
def test_rt_diffuse_fraction_erbs_is_rejected(tmp_path, value):
    ret, _ = run(tmp_path, 'rtd', rt_optical(tmp_path, 'rtd', diffuseFraction=value))
    assert ret.returncode != 0
    assert 'diffuseFraction' in ret.stderr and 'RT' in ret.stderr, ret.stderr

# ---------- A2: a pixel with no usable observation is distinguishable from invalid inputs ----------
def contract():
    spec = importlib.util.spec_from_file_location('contract', HERE / 'test-assimilation-contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_no_observation_pixel_has_its_own_qa_bit(tmp_path):
    c = contract()
    qa = c.raster(tmp_path / 'qa.tif', [float('nan')])   # the only observation is rejected
    result, out = c.run(tmp_path, dict(qa=qa))
    assert result.returncode == 0, result.stderr + result.stdout
    flags = int(read(out / 'analysis_qa.tif', 1))
    assert flags & 1 and flags & 4096, flags              # not analysed (bit 0) because nothing was observed (bit 12)
    sub = tmp_path / 'ok'
    sub.mkdir()
    ok, out2 = c.run(sub, dict())
    assert ok.returncode == 0, ok.stderr + ok.stdout
    assert int(read(out2 / 'analysis_qa.tif', 1)) & 4096 == 0


# ---------- I1/I2: LSCT background failure is "retrieval failed" only; stop reasons are split like the assimilation ----------
def lsct(tmp_path, tag, components):
    inv = dict(target='lsct', observations=[dict(kind='brightness', value=dict(value=300.), wavelength=10.5, sigma=.3)],
               prior=dict(sd=5., correlation=0.))
    config = rt_config(tmp_path, tag, components=components, thermal=False, inversion=inv, sky=dict(mode='none'))
    ret, out = run(tmp_path, tag, config)
    return ret, out


def test_lsct_background_operator_failure_is_not_ill_conditioned(tmp_path):
    ret, out = lsct(tmp_path, 'bad', (1., 1., 1., 1.))   # B(1 K) underflows: the operator cannot be evaluated
    assert ret.returncode == 0, ret.stderr + ret.stdout
    qa = int(read(out / 'lsct_qa.tif', 1))
    assert qa & 8 and not qa & 16, qa
    counts = summary(out)['inversion']['counts']
    assert counts['failedPixels'] == 1 and counts['illConditionedPixels'] == 0, counts


def test_lsct_counts_split_bit9_stops(tmp_path):
    ret, out = lsct(tmp_path, 'ok', (305.,) * 4)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    counts = summary(out)['inversion']['counts']
    assert counts['notConvergedPixels'] == counts['iterationLimitPixels'] + counts['stepEvaluationFailedPixels']
    assert counts['convergedPixels'] + counts['lineSearchStalledPixels'] + counts['notUpdatedPixels'] + counts['notConvergedPixels'] == 1
