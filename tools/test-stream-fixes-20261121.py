"""Targeted checks of the 2026-11-21 Studio STREAM fixes (B-S1, B10, C9, S-1, S-2, S-3).

Run:  python -m pytest tools/test-stream-fixes-20261121.py
      (STREAM_RUNTIME overrides the engine dir, default models/bin_x64/Release; STREAM_FIX_CASE overrides the case,
       default assets/examples/modis_beijing_eb)
Against the pre-fix baseline the S-1 / S-3 / B10 / C9 / B-S1 checks are expected to fail (they document the bugs).
"""
import json
import os
import subprocess
from pathlib import Path

import numpy as np
import pytest
from osgeo import gdal

gdal.UseExceptions()
ROOT = Path(__file__).resolve().parents[1]
RUNTIME = Path(os.environ.get('STREAM_RUNTIME', ROOT / 'models/bin_x64/Release'))
CASE = Path(os.environ.get('STREAM_FIX_CASE', ROOT / 'assets/examples/modis_beijing_eb'))
C1, C2 = 1.1910439340652e8, 14388.291040407


def planck(t, wl):
    return C1 / (wl ** 5 * np.expm1(C2 / (wl * t)))


def inverse_planck(radiance, wl):
    return C2 / (wl * np.log1p(C1 / (radiance * wl ** 5)))


def raster(path, values):
    values = np.asarray(values, np.float32).reshape(-1, 1, 1)
    ds = gdal.GetDriverByName('GTiff').Create(str(path), 1, 1, len(values), gdal.GDT_Float32)
    for i, v in enumerate(values, 1):
        ds.GetRasterBand(i).WriteArray(v)
    ds = None
    return str(path)


def run(tmp_path, tag, config, data_dir=None):
    out = tmp_path / tag
    out.mkdir()
    config = dict(config, kind='stream-image-config', schemaVersion=2, outputDir=str(out),
                  dataDir=str(data_dir or RUNTIME / 'stream-defined'))
    path = tmp_path / f'{tag}.json'
    path.write_text(json.dumps(config), encoding='utf-8')
    env = dict(os.environ, PATH=str(RUNTIME) + os.pathsep + os.environ['PATH'])
    result = subprocess.run([str(RUNTIME / 'stream.exe'), 'run', str(path)], capture_output=True,
                            encoding='utf-8', errors='replace', env=env, timeout=300)
    return result, out


def read(path, band=1):
    with gdal.Open(str(path)) as ds:
        return float(ds.GetRasterBand(band).ReadAsArray()[0, 0])


def rt_config(tmp_path, tag, *, components=(305.,) * 4, lai=2., terrain=None, thermal=True, inversion=None, vza=0., vaa=0.,
              klass=10, overpass='2019-07-15T03:00:00Z', sky=None):
    surface = dict(type=dict(file=raster(tmp_path / f'{tag}_type.tif', [klass])), lai=dict(value=lai),
                   soilMoisture=dict(value=.2), latitude=dict(value=40.), longitude=dict(value=116.), dem=dict(value=0.))
    if terrain:
        surface['terrain'] = terrain
    config = dict(overpass=overpass, surface=surface, view=dict(vza=dict(value=vza), vaa=dict(value=vaa)),
                  optical=dict(enabled=False),
                  thermal=dict(enabled=thermal, bands=[dict(name='TIR', center=10.5, fwhm=.5)] if thermal else []),
                  threads=1,
                  temperature=dict(mode='rt', components={k: dict(value=v) for k, v in zip(
                      ('soilSunlit', 'soilShaded', 'leafSunlit', 'leafShaded'), components)},
                      energy=dict(soilEmissivityModel='hapke')))
    if sky is not None:
        config['skyLongwave'] = sky
    if inversion:
        config['inversion'] = inversion
    return config


TERRAINS = {
    'single': dict(mode='single', slope=dict(value=35.), aspect=dict(value=100.)),
    'hill': dict(mode='hill', slope=dict(value=30.), density=dict(value=.6)),
    'ridge': dict(mode='ridge', slope=dict(value=30.), aspect=dict(value=60.), density=dict(value=.5), endFraction=.2),
}


# ---------- S-1: LST / LSCT retrieval uses the imaging operator with terrain ----------
@pytest.mark.parametrize('name', ['flat'] + list(TERRAINS))
def test_lst_retrieval_closes_with_terrain(tmp_path, name):
    terrain = TERRAINS.get(name)
    fwd, out = run(tmp_path, 'fwd', rt_config(tmp_path, 'fwd', terrain=terrain, vza=30., vaa=200., sky=dict(value=380.)))
    assert fwd.returncode == 0, fwd.stderr + fwd.stdout
    observed = read(out / 'tir_brightness_temperature.tif')
    inv = dict(target='lst', observations=[dict(kind='brightness', value=dict(value=observed), wavelength=10.5, sigma=.5)])
    ret, out2 = run(tmp_path, 'ret', rt_config(tmp_path, 'ret', terrain=terrain, vza=30., vaa=200., thermal=False,
                                                inversion=inv, sky=dict(value=380.)))
    assert ret.returncode == 0, ret.stderr + ret.stdout
    lst = read(out2 / 'lst.tif')
    print(f'{name}: observed BT {observed:.4f} K, retrieved LST {lst:.4f} K, error {lst - 305.:+.4f} K')
    assert abs(lst - 305.) < 5e-3


@pytest.mark.parametrize('name', ['flat'] + list(TERRAINS))
def test_lsct_retrieval_residual_with_terrain(tmp_path, name):
    terrain = TERRAINS.get(name)
    truth = (318., 301., 309., 297.)
    views = [(0., 0.), (40., 90.), (45., 250.)]
    observations = []
    for i, (z, a) in enumerate(views):
        fwd, out = run(tmp_path, f'fwd{i}', rt_config(tmp_path, f'fwd{i}', components=truth, terrain=terrain, vza=z, vaa=a,
                                                     sky=dict(mode='none')))
        assert fwd.returncode == 0, fwd.stderr + fwd.stdout
        observations.append(dict(kind='brightness', value=dict(value=read(out / 'tir_brightness_temperature.tif')),
                                 wavelength=10.5, sigma=.3, vza=dict(value=z), vaa=dict(value=a)))
    inv = dict(target='lsct', observations=observations, prior=dict(sd=5., correlation=0.))
    ret, out2 = run(tmp_path, 'ret', rt_config(tmp_path, 'ret', components=truth, terrain=terrain, thermal=False,
                                                inversion=inv, sky=dict(mode='none')))
    assert ret.returncode == 0, ret.stderr + ret.stdout
    residual = read(out2 / 'lsct_qa.tif', 2)
    posterior = [read(out2 / 'lsct.tif', k) for k in range(1, 5)]
    print(f'{name}: residual RMS {residual:.5f} K, posterior - truth {np.round(np.array(posterior) - truth, 4)}')
    assert residual < 1e-3


# ---------- S-3: LST sigma belongs to the returned temperature ----------
def test_lst_sigma_is_evaluated_at_the_solution(tmp_path):
    observed = 300.
    sigma, sigma_e, wl = .5, .05, 10.5
    inv = dict(target='lst', emissivitySd=sigma_e, maxIterations=1,
               observations=[dict(kind='brightness', value=dict(value=observed), wavelength=wl, sigma=sigma)])
    config = rt_config(tmp_path, 'bare', lai=0., thermal=False, inversion=inv, sky=dict(mode='none'), klass=16)
    config['temperature']['energy'] = dict(soilEmissivityModel='constant')
    ret, out = run(tmp_path, 'bare', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    t = read(out / 'lst.tif')
    sd = read(out / 'lst_uncertainty.tif')
    emissivity = read(out / 'lst_qa.tif', 4)

    def predicted(temp):
        value = inverse_planck(emissivity * planck(temp, wl), wl)
        dbt = lambda x: planck(x, wl) * (C2 / (wl * x)) / x * np.exp(C2 / (wl * x)) / np.expm1(C2 / (wl * x))
        slope = emissivity * dbt(temp) / dbt(value)
        term = sigma_e * planck(temp, wl) / dbt(value)
        return 1. / np.sqrt(slope ** 2 / (sigma ** 2 + term ** 2))
    print(f'LST {t:.4f} K (1 iteration), sigma {sd:.6f}; expected at solution {predicted(t):.6f}, at first guess {predicted(observed):.6f}')
    assert abs(sd - predicted(t)) < 2e-5


# ---------- C9: crown / row parameters are reported, not ignored silently ----------
def test_crown_parameters_are_reported(tmp_path):
    data = tmp_path / 'defined'
    data.mkdir()
    for f in (RUNTIME / 'stream-defined').iterdir():
        (data / f.name).write_bytes(f.read_bytes())
    table = json.loads((data / 'classes.json').read_text(encoding='utf-8'))
    for c in table['classes']:
        if c['code'] == 10:
            c['canopy'].update(treeStand=0.05, canopyHeight=4.0, crownShape=1.5)
    (data / 'classes.json').write_text(json.dumps(table, ensure_ascii=False), encoding='utf-8')
    ret, out = run(tmp_path, 'crown', rt_config(tmp_path, 'crown'), data_dir=data)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    assert '按均质冠层处理' in ret.stdout
    summary = json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))
    assert summary['canopyStructure']['model'] == 'homogeneous'
    assert summary['canopyStructure']['ignoredClassParameters'][0]['code'] == 10


# ---------- B-S1 / B10: EB configuration (MODIS example, 3 x 3 region) ----------
def eb_config(**energy_extra):
    rel = lambda p: str(CASE / p)
    return dict(overpass='2019-08-15T02:30:00Z',
                surface=dict(type=dict(file=rel('inputs/type.tif')), lai=dict(file=rel('inputs/lai.tif')),
                             dem=dict(file=rel('inputs/dem.tif'))),
                view=dict(vza=dict(value=0.), vaa=dict(value=0.)), optical=dict(enabled=False),
                thermal=dict(enabled=True, bands=[dict(name='B31', center=11.03, fwhm=.5)]), threads=1,
                region=dict(startRow=85, endRow=88, startColumn=107, endColumn=110),
                temperature=dict(mode='eb', era5=dict(pack=rel('era5/era5-pack.json')), soilMoisture=dict(source='era5'),
                                 energy=dict(scheme='tirt', **energy_extra)))


@pytest.mark.parametrize('value', [0, -50, 'abc', 1e7])
def test_invalid_soil_resistance_is_rejected(tmp_path, value):
    ret, _ = run(tmp_path, 'rss', eb_config(soilResistance=dict(method='constant', value=value)))
    assert ret.returncode != 0, ret.stdout
    assert 'soilResistance.value' in ret.stderr


def test_valid_soil_resistance_runs(tmp_path):
    ret, _ = run(tmp_path, 'rss', eb_config(soilResistance=dict(method='constant', value=150)))
    assert ret.returncode == 0, ret.stderr + ret.stdout


def test_spinup_falls_back_to_pack_start_with_warning(tmp_path):
    ret, out = run(tmp_path, 'spin', eb_config())
    assert ret.returncode == 0, ret.stderr + ret.stdout
    t = json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))['temperature']
    # default 7 nodes before the 02:30 overpass; the one-day pack only has 00, 01, 02 UTC
    assert t['spinupNodesRequested'] == 7 and t['spinupNodesUsed'] == 3 and abs(t['spinupHoursUsed'] - 2.5) < 1e-9
    assert t['ebNodeCount'] == 4 and any('起转不足' in note for note in t['notes'])
    assert '起转' in ret.stdout and '警告' in ret.stdout
    assert '调试' not in ret.stdout


@pytest.mark.parametrize('value', [2, 1.5])
def test_spinup_hours_is_respected(tmp_path, value):
    config = eb_config()
    config['temperature']['spinupHours'] = value  # nodes (non-integers round up): 01 and 02 UTC before the 02:30 overpass
    ret, out = run(tmp_path, 'spin', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    t = json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))['temperature']
    assert t['marchStart'] == '2019-08-15T01:00:00Z' and t['nodes'][0] == '2019-08-15T01:00:00Z'
    assert t['spinupNodesUsed'] == 2 and not t['notes']


def test_spinup_zero_nodes_runs_the_overpass_only(tmp_path):
    config = eb_config()
    config['temperature']['spinupHours'] = 0
    ret, out = run(tmp_path, 'spin', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    t = json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))['temperature']
    assert t['nodes'] == ['2019-08-15T02:30:00Z'] and t['spinupNodesUsed'] == 0


def test_spinup_without_earlier_data_is_an_error(tmp_path):
    config = eb_config()
    config['overpass'] = '2019-08-15T00:00:00Z'  # the pack starts at 00 UTC: no whole hour before the overpass
    ret, _ = run(tmp_path, 'spin', config)
    assert ret.returncode != 0 and '没有可用的整点时次' in ret.stderr


@pytest.mark.parametrize('value', [-1, 500, 'x'])
def test_invalid_spinup_is_rejected(tmp_path, value):
    config = eb_config()
    config['temperature']['spinupHours'] = value
    ret, _ = run(tmp_path, 'spin', config)
    assert ret.returncode != 0


# ---------- S-4 / S-5 QA: EB assimilation never raises the MAP cost; stops are classified ----------
def test_eb_assimilation_cost_and_stop_classes(tmp_path):
    # Observation = background LST + 3 K, so every pixel has an innovation to fit (MODIS example, 10 x 10 region).
    config = eb_config()
    config['region'] = dict(startRow=80, endRow=90, startColumn=100, endColumn=110)
    first, out0 = run(tmp_path, 'bg', dict(config, thermal=dict(enabled=True, bands=[dict(name='B31', center=11.03, fwhm=.5)])))
    assert first.returncode == 0, first.stderr + first.stdout
    with gdal.Open(str(out0 / 'tir_surface_temperature.tif')) as ds:
        lst = ds.GetRasterBand(1).ReadAsArray().astype(np.float32) + 3.0
        drv = gdal.GetDriverByName('GTiff')
        obs = drv.Create(str(tmp_path / 'obs.tif'), ds.RasterXSize, ds.RasterYSize, 1, gdal.GDT_Float32)
        obs.SetGeoTransform(ds.GetGeoTransform()); obs.SetProjection(ds.GetProjection())
        obs.GetRasterBand(1).WriteArray(lst); obs = None
    config['assimilation'] = dict(observation='lst', data=str(tmp_path / 'obs.tif'), lstWavelengthUm=11.03)
    ret, out = run(tmp_path, 'as', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'analysis_qa.tif')) as ds:
        assert ds.RasterCount == 9
        q = ds.ReadAsArray()
    ok = np.isfinite(q[0]) & np.isfinite(q[1])
    flags, jb, ja = q[0][ok].astype(int), q[7][ok], q[8][ok]
    assert ok.sum() > 50
    assert np.all(ja <= jb * (1 + 1e-6) + 1e-9)                    # the analysis never raises the MAP cost
    stalled, not_updated = (flags & 1024) > 0, (flags & 2048) > 0
    assert not np.any(stalled & ((flags & 512) > 0))                # a stall is not "not converged"
    assert np.all(ja[stalled] < jb[stalled])                        # stalled: cost lowered
    assert np.allclose(ja[not_updated], jb[not_updated])            # not updated: analysis = background
    stops = json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))['assimilation']['stopReasons']
    assert sum(stops.values()) == ok.sum()
    print('stop reasons', stops)


# ---------- 2026-11-24: EB initial values come from the 2 m air temperature; skt/stl1/stl2 are optional ----------
def pack_without(tmp_path, drop):
    """Copy of the example ERA5 pack whose NetCDF lacks the `drop` variables (index bytes updated)."""
    import netCDF4
    src = CASE / 'era5'
    folder = tmp_path / 'era5_trimmed'
    folder.mkdir()
    index = json.loads((src / 'era5-pack.json').read_text(encoding='utf-8'))
    for item in index['files']:
        with netCDF4.Dataset(src / item['path']) as a, netCDF4.Dataset(folder / item['path'], 'w') as b:
            b.setncatts({k: a.getncattr(k) for k in a.ncattrs()})
            for name, dim in a.dimensions.items():
                b.createDimension(name, None if dim.isunlimited() else len(dim))
            for name, var in a.variables.items():
                if name in drop:
                    continue
                fill = var.getncattr('_FillValue') if '_FillValue' in var.ncattrs() else None
                out = b.createVariable(name, var.dtype, var.dimensions, fill_value=fill)
                out.setncatts({k: var.getncattr(k) for k in var.ncattrs() if k != '_FillValue'})
                out[:] = var[:]
        item['bytes'] = (folder / item['path']).stat().st_size
        item['variables'] = [v for v in item['variables'] if v not in drop]
    index['variables'] = {k: v for k, v in index['variables'].items() if k not in drop}
    (folder / 'era5-pack.json').write_text(json.dumps(index), encoding='utf-8')
    return folder / 'era5-pack.json'


def test_eb_runs_without_skt_and_soil_temperature(tmp_path):
    full, out_full = run(tmp_path, 'full', eb_config())
    assert full.returncode == 0, full.stderr + full.stdout
    config = eb_config()
    config['temperature']['era5']['pack'] = str(pack_without(tmp_path, {'skt', 'stl1', 'stl2'}))
    ret, out = run(tmp_path, 'trim', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    # the initial values never read skt / stl1 / stl2, so the result is bit-identical to the full pack
    for name in ('tir_surface_temperature.tif', 'eb_fluxes.tif'):
        with gdal.Open(str(out_full / name)) as a, gdal.Open(str(out / name)) as b:
            assert np.array_equal(a.ReadAsArray(), b.ReadAsArray(), equal_nan=True)
    assert '2 m 气温' in ret.stdout and '不使用 skt' in ret.stdout


def test_water_pixels_degrade_to_air_temperature_without_skt(tmp_path):
    with gdal.Open(str(CASE / 'inputs/type.tif')) as ds:
        water = np.full((ds.RasterYSize, ds.RasterXSize), 17, np.float32)
        out_ds = gdal.GetDriverByName('GTiff').Create(str(tmp_path / 'water.tif'), ds.RasterXSize, ds.RasterYSize, 1, gdal.GDT_Float32)
        out_ds.SetGeoTransform(ds.GetGeoTransform()); out_ds.SetProjection(ds.GetProjection())
        out_ds.GetRasterBand(1).WriteArray(water); out_ds = None
    config = eb_config()
    config['surface']['type'] = dict(file=str(tmp_path / 'water.tif'))
    ret, out = run(tmp_path, 'water_skt', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    config['temperature']['era5']['pack'] = str(pack_without(tmp_path, {'skt'}))
    ret2, out2 = run(tmp_path, 'water_t2m', config)
    assert ret2.returncode == 0, ret2.stderr + ret2.stdout
    t = json.loads((out2 / 'scene_summary.json').read_text(encoding='utf-8'))['temperature']
    assert any('水体降级' in note for note in t['notes']) and '没有 skt' in ret2.stdout
    def mean(path):
        with gdal.Open(str(path)) as ds:
            return float(np.nanmean(ds.GetRasterBand(1).ReadAsArray()))
    assert 200. < mean(out2 / 'tir_surface_temperature.tif') < 340. and 200. < mean(out / 'tir_surface_temperature.tif') < 340.
