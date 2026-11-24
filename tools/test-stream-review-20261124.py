"""Regression checks for the 2026-11-24 STREAM engine review fixes (M1 LAI fill codes, M2 cold-scene temperature limit,
E1/R5 robustness items). Minimal synthetic data on the 3 x 3 region of the MODIS Beijing example.

Run:  python -m pytest tools/test-stream-review-20261124.py
      (STREAM_RUNTIME overrides the engine dir, default models/bin_x64/Release; STREAM_FIX_CASE the case)
Against the pre-fix engine the M1 / M2 checks fail (they document the bugs).
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
CASE, run, eb_config = base.CASE, base.run, base.eb_config
REGION = (slice(85, 88), slice(107, 110))


def clone(src, dst, array, dtype, scale=None, nodata=None):
    with gdal.Open(str(src)) as ds:
        out = gdal.GetDriverByName('GTiff').Create(str(dst), ds.RasterXSize, ds.RasterYSize, 1, dtype)
        out.SetGeoTransform(ds.GetGeoTransform())
        out.SetProjection(ds.GetProjection())
        band = out.GetRasterBand(1)
        if scale is not None:
            band.SetScale(scale)
        if nodata is not None:
            band.SetNoDataValue(nodata)
        band.WriteArray(array)
        out = None
    return str(dst)


def summary(out):
    return json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))


# ---------- M1: packed-integer LAI fill codes and water pixels with LAI nodata ----------
def test_lai_fill_codes_and_water_nodata(tmp_path):
    with gdal.Open(str(CASE / 'inputs/lai.tif')) as ds:
        lai0 = ds.GetRasterBand(1).ReadAsArray().astype(float)
    with gdal.Open(str(CASE / 'inputs/type.tif')) as ds:
        type0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    raw = np.clip(np.round(lai0 * 10), 0, 100).astype(np.uint8)
    # row 85: valid 2.0 / 10.0 / 0.0 (bare ground stays valid); row 86: fill codes 249, 250, 254; row 87: nodata 255,
    # then two water pixels (LAI nodata / LAI 2.0)
    raw[REGION] = np.array([[20, 100, 0], [249, 250, 254], [255, 40, 20]], np.uint8)
    lai = clone(CASE / 'inputs/lai.tif', tmp_path / 'lai_modis.tif', raw, gdal.GDT_Byte, scale=0.1, nodata=255)
    land = int(np.bincount(type0[REGION].astype(int).ravel()).argmax())
    t = type0.copy()
    t[REGION] = land
    t[87, 108] = 17   # water, LAI 4.0
    t[87, 109] = 17   # water, LAI 2.0
    # (87, 107) is land with LAI nodata
    type_path = clone(CASE / 'inputs/type.tif', tmp_path / 'type.tif', t, gdal.GDT_Float32)
    config = eb_config()
    config['surface'] = dict(type=dict(file=type_path), lai=dict(file=lai), dem=dict(file=str(CASE / 'inputs/dem.tif')))
    ret, out = run(tmp_path, 'm1', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    leaf = comp[2]
    assert np.isfinite(comp[0][0]).all(), 'valid LAI 2.0 / 10.0 / 0.0 must be solved'
    assert np.isfinite(leaf[0, :2]).all() and np.isnan(leaf[0, 2]), 'LAI = 0 is bare ground: soil only, no leaf temperature'
    assert np.isnan(comp[:, 1, :]).all(), 'fill codes 249/250/254 (24.9-25.4 after scaling) must be invalid'
    assert np.isnan(comp[:, 2, 0]).all(), 'nodata LAI must be invalid'
    assert np.isfinite(comp[:, 2, 1:]).all(), 'water pixels are solved from the skin temperature'
    counts = summary(out)
    assert 'LAI 超出有效范围' in ret.stdout and '[0, 20]' in ret.stdout
    assert counts['counts']['laiOutOfRangePixels'] == 3
    assert counts['counts']['laiNoDataPixels'] == 1


def test_water_pixel_with_nodata_lai_gets_skin_temperature(tmp_path):
    with gdal.Open(str(CASE / 'inputs/lai.tif')) as ds:
        lai0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    with gdal.Open(str(CASE / 'inputs/type.tif')) as ds:
        type0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    lai0[REGION] = np.nan
    t = type0.copy()
    t[REGION] = 17
    lai = clone(CASE / 'inputs/lai.tif', tmp_path / 'lai_nan.tif', lai0, gdal.GDT_Float32, nodata=-9999)
    type_path = clone(CASE / 'inputs/type.tif', tmp_path / 'type.tif', t, gdal.GDT_Float32)
    config = eb_config()
    config['surface'] = dict(type=dict(file=type_path), lai=dict(file=lai), dem=dict(file=str(CASE / 'inputs/dem.tif')))
    ret, out = run(tmp_path, 'water', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    assert np.isfinite(comp).all() and np.allclose(comp[0], comp[3])
    with gdal.Open(str(out / 'tir_surface_temperature.tif')) as ds:
        assert np.isfinite(ds.GetRasterBand(1).ReadAsArray()[REGION]).all()


# ---------- M2: a very cold scene is no longer pinned at 240 K ----------
def cold_pack(tmp_path, delta, lw_scale=0.45):
    import netCDF4
    folder = tmp_path / f'pack_cold{delta}'
    folder.mkdir()
    index = json.loads((CASE / 'era5' / 'era5-pack.json').read_text(encoding='utf-8'))
    for item in index['files']:
        with netCDF4.Dataset(CASE / 'era5' / item['path']) as a, netCDF4.Dataset(folder / item['path'], 'w') as b:
            for n, dim in a.dimensions.items():
                b.createDimension(n, None if dim.isunlimited() else len(dim))
            for n, var in a.variables.items():
                if n in ('t2m', 'd2m', 'strd', 'skt', 'stl1', 'stl2'):
                    out = b.createVariable(n, 'f4', var.dimensions)
                    out.setncatts({k: var.getncattr(k) for k in var.ncattrs() if k not in ('scale_factor', 'add_offset', '_FillValue')})
                    data = np.ma.filled(var[:], np.nan).astype('f4')
                    out[:] = data * lw_scale if n == 'strd' else data - delta
                else:
                    fill = var.getncattr('_FillValue') if '_FillValue' in var.ncattrs() else None
                    out = b.createVariable(n, var.dtype, var.dimensions, fill_value=fill)
                    out.setncatts({k: var.getncattr(k) for k in var.ncattrs() if k != '_FillValue'})
                    out.set_auto_maskandscale(False)
                    var.set_auto_maskandscale(False)
                    out[:] = var[:]
        item['bytes'] = (folder / item['path']).stat().st_size
    (folder / 'era5-pack.json').write_text(json.dumps(index), encoding='utf-8')
    return folder / 'era5-pack.json'


def test_cold_night_converges_below_240_k(tmp_path):
    config = eb_config()
    config['overpass'] = '2019-08-15T16:00:00Z'
    config['temperature']['era5']['pack'] = str(cold_pack(tmp_path, 60))
    ret, out = run(tmp_path, 'cold', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        fl = ds.ReadAsArray()[:, 85:88, 107:110]
    converged, sensible = fl[5], fl[1]  # band order: net radiation, sensible, latent, ground, iterations, converged
    print('cold components min', np.nanmin(comp), 'max', np.nanmax(comp), 'sensible', np.nanmin(sensible), np.nanmax(sensible))
    assert np.nanmin(comp) < 238.0, 'components must not be clamped at the former 240 K limit'
    assert np.nanmin(comp) > 200.0
    assert converged.min() == 1.0
    assert np.abs(sensible).max() < 150.0
    assert summary(out)['temperature']['nonConvergedPixels'] == 0


def test_unconverged_pixels_are_reported(tmp_path):
    ret, out = run(tmp_path, 'maxit1', eb_config(maxIterations=1))
    assert ret.returncode == 0, ret.stderr + ret.stdout
    temperature = summary(out)['temperature']
    assert temperature['nonConvergedPixels'] > 0
    assert temperature['nonConvergedPixels'] == temperature['solvedPixels'] - temperature['convergedPixels']
    assert 'warning' in temperature and 'did not converge' in temperature['warning']
    assert '未收敛' in ret.stdout and '警告' in ret.stdout
    assert 'temperatureLimitedPixels' in temperature
