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

# ---------- L5: the facet-method border ring is reported ----------
def geo_raster(path, array):
    array = np.asarray(array, np.float32)
    ds = gdal.GetDriverByName('GTiff').Create(str(path), array.shape[1], array.shape[0], 1, gdal.GDT_Float32)
    ds.SetGeoTransform((116.0, 0.001, 0.0, 40.0, 0.0, -0.001))
    srs = __import__('osgeo.osr', fromlist=['osr']).SpatialReference()
    srs.ImportFromEPSG(4326)
    ds.SetProjection(srs.ExportToWkt())
    ds.GetRasterBand(1).WriteArray(array)
    ds = None
    return str(path)


def test_facet_border_ring_is_reported(tmp_path):
    config = rt_config(tmp_path, 'facet', terrain=dict(mode='facet'))
    rows = np.arange(5, dtype=np.float32)[:, None] * 30.0 + np.zeros((5, 5), np.float32)   # a 30 m per pixel ramp
    config['surface']['type'] = dict(file=geo_raster(tmp_path / 'type5.tif', np.full((5, 5), 10)))
    config['surface']['dem'] = dict(file=geo_raster(tmp_path / 'dem5.tif', rows))
    config['surface'].pop('latitude'), config['surface'].pop('longitude')
    ret, out = run(tmp_path, 'facet', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    assert '最外一圈' in ret.stdout and '16 个' in ret.stdout, ret.stdout
    # a region that excludes the ring produces no warning
    config['region'] = dict(startRow=1, endRow=4, startColumn=1, endColumn=4)
    sub = tmp_path / 'inner'
    sub.mkdir()
    ret2, _ = run(sub, 'facet', config)
    assert ret2.returncode == 0, ret2.stderr + ret2.stdout
    assert '最外一圈' not in ret2.stdout

# ---------- ice melt: snow / ice classes are capped at 273.15 K, the surplus is a melt flux ----------
def ice_scene(tmp_path, lai_value=0.0, **energy):
    spec = importlib.util.spec_from_file_location('review', HERE / 'test-stream-review-20261124.py')
    review = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(review)
    with gdal.Open(str(CASE / 'inputs/type.tif')) as ds:
        type0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    with gdal.Open(str(CASE / 'inputs/lai.tif')) as ds:
        lai0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    region = review.REGION
    type0[region] = 15
    lai0[region] = lai_value
    config = eb_config(**energy)
    config['surface'] = dict(type=dict(file=review.clone(CASE / 'inputs/type.tif', tmp_path / 'ice_type.tif', type0, gdal.GDT_Float32)),
                             lai=dict(file=review.clone(CASE / 'inputs/lai.tif', tmp_path / 'ice_lai.tif', lai0, gdal.GDT_Float32)),
                             dem=dict(file=str(CASE / 'inputs/dem.tif')))
    return config, region


@pytest.mark.parametrize('lai', [0.0, 1.0])
def test_ice_is_capped_at_the_melting_point(tmp_path, lai):
    config, region = ice_scene(tmp_path, lai)
    ret, out = run(tmp_path, 'ice', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    assert np.nanmax(comp) <= 273.15 + 1e-9, np.nanmax(comp)
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        fl = ds.ReadAsArray()[:, 85:88, 107:110]
        assert ds.RasterCount == 19
    melt = fl[18]
    assert (melt > 50).all(), melt                            # midday: a large energy surplus goes into melting
    # the balance closes with the melt flux: Rn - H - LE - G - melt = 0 (bands: Rn, H, LE, G)
    assert np.abs(fl[0] - fl[1] - fl[2] - fl[3] - melt).max() < 1.0
    counts = summary(out)['temperature']
    assert counts['iceMelt'] is True and counts['iceCappedPixels'] == 9 and counts['iceMeltPixels'] == 9
    assert '冰雪融化' in ret.stdout


def test_ice_melt_can_be_switched_off(tmp_path):
    config, region = ice_scene(tmp_path, 0.0, iceMelt=False)
    ret, out = run(tmp_path, 'ice_off', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
        assert np.nanmax(comp) > 275.0                         # the old behaviour: snow warmer than 0 degC
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        assert ds.RasterCount == 18


def test_scene_without_ice_has_no_melt_band(tmp_path):
    ret, out = run(tmp_path, 'plain', eb_config())
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        assert ds.RasterCount == 18
    assert 'iceMeltPixels' not in summary(out)['temperature']
