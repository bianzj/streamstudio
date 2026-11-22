"""Small synthetic RT assimilation checks; no field accuracy claim.

Run with C:/work/miniconda/python.exe -m pytest tools/test-assimilation-contract.py
after building build/stream (Release).
"""
import json
import os
from pathlib import Path
import subprocess

import numpy as np
import pytest
from osgeo import gdal

gdal.UseExceptions()
ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'models/bin_x64/Release'


def raster(path, values):
    values = np.asarray(values, np.float32).reshape(-1, 1, 1)
    ds = gdal.GetDriverByName('GTiff').Create(str(path), 1, 1, len(values), gdal.GDT_Float32)
    for i, v in enumerate(values, 1):
        ds.GetRasterBand(i).WriteArray(v)
    ds = None
    return str(path)


def run(tmp_path, extra, *, inversion=False, brightness=False):
    out = tmp_path / 'out'
    out.mkdir()
    config = dict(kind='stream-image-config', schemaVersion=2, outputDir=str(out),
        dataDir=str(RUNTIME / 'stream-defined'), overpass='2019-07-15T04:00:00Z',
        surface=dict(type=dict(file=raster(tmp_path/'type.tif', [0])), lai=dict(value=2.),
                     soilMoisture=dict(value=.3), latitude=dict(value=40.), longitude=dict(value=116.), dem=dict(value=0.)),
        view=dict(vza=dict(value=0.), vaa=dict(value=0.)), optical=dict(enabled=False),
        thermal=dict(enabled=False), threads=1,
        temperature=dict(mode='rt', components={k: dict(value=300.) for k in ('soilSunlit','soilShaded','leafSunlit','leafShaded')}),
        assimilation=dict(observation='components', data=raster(tmp_path/'obs.tif', [310.] * 4)))
    config['assimilation'].update(extra)
    if inversion:
        config.pop('assimilation')
        config['inversion'] = dict(target='lsct', observations=[dict(
            kind='brightness', value=dict(value=300.), wavelength=10.5, sigma=1.)])
    if brightness:
        config['assimilation']['observation'] = 'brightness'
        config['thermal'] = dict(enabled=True, bands=[dict(name='TIR', center=10.5, fwhm=.5)])
    path = tmp_path/'config.json'
    path.write_text(json.dumps(config), encoding='utf-8')
    env = dict(os.environ, PATH=str(RUNTIME) + os.pathsep + os.environ['PATH'])
    result = subprocess.run([str(RUNTIME/'stream.exe'), 'run', str(path)], capture_output=True,
                            encoding='utf-8', errors='replace', env=env, timeout=60)
    return result, out


@pytest.mark.parametrize('extra', [dict(sigmaComponentK=0), dict(lstWavelengthUm=0),
    dict(observationLevel='toa'), dict(sourceIds=['scene'], priorSourceIds=['scene'])])
def test_reject_invalid_contract(tmp_path, extra):
    result, _ = run(tmp_path, extra)
    assert result.returncode != 0, result.stdout


def test_raster_errors_control_the_analysis(tmp_path):
    sigma = raster(tmp_path/'sigma.tif', [20.] * 4)
    result, out = run(tmp_path, dict(uncertainty=sigma, lstWavelengthUm=12.))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    t = ds.ReadAsArray().reshape(-1)
    # Equal innovations lie along the all-ones eigenvector of B:
    # eigenvalue = 25 * (1 + 3 * .5) = 62.5 K², R = 400 K².
    np.testing.assert_allclose(t, 300. + 10.*62.5/(400.+62.5), atol=3e-4)
    summary = json.loads((out/'scene_summary.json').read_text(encoding='utf-8'))['assimilation']
    assert summary['defaults']['lstWavelengthUm'] == 12.


@pytest.mark.parametrize('value', [256., float('nan')])
def test_qa_rejection_preserves_background(tmp_path, value):
    qa = raster(tmp_path/'qa.tif', [value])
    result, out = run(tmp_path, dict(qa=qa))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    np.testing.assert_allclose(ds.ReadAsArray(), 300.)


@pytest.mark.parametrize('value', [0., -1., float('nan')])
def test_invalid_error_never_falls_back_to_default(tmp_path, value):
    sigma = raster(tmp_path/'sigma.tif', [value] * 4)
    result, out = run(tmp_path, dict(uncertainty=sigma))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    np.testing.assert_allclose(ds.ReadAsArray(), 300.)


def test_correlated_component_errors_are_not_four_independent_measurements(tmp_path):
    covariance = np.full((4, 4), 200.)
    np.fill_diagonal(covariance, 400.)
    path = raster(tmp_path/'covariance.tif', covariance.reshape(-1))
    result, out = run(tmp_path, dict(covariance=path))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    # R's all-ones eigenvalue is 400 + 3 * 200 = 1000 K².
    np.testing.assert_allclose(ds.ReadAsArray(), 300. + 10.*62.5/(1000.+62.5), atol=3e-4)


def test_non_positive_definite_covariance_keeps_background(tmp_path):
    covariance = np.full((4, 4), 2.)
    np.fill_diagonal(covariance, 1.)
    result, out = run(tmp_path, dict(covariance=raster(tmp_path/'bad_cov.tif', covariance.reshape(-1))))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    np.testing.assert_allclose(ds.ReadAsArray(), 300.)


def test_weight_fractions_and_radiance_fractions_are_distinct_products(tmp_path):
    result, out = run(tmp_path, {}, inversion=True)
    assert result.returncode == 0, result.stderr + result.stdout
    with gdal.Open(str(out/'lsct_fractions.tif')) as ds:
        weights = ds.ReadAsArray().reshape(-1)
    with gdal.Open(str(out/'lsct.tif')) as ds:
        temperatures = ds.ReadAsArray().reshape(-1)
    with gdal.Open(str(out/'lsct_radiance_fractions.tif')) as ds:
        actual = ds.ReadAsArray().reshape(-1)
    radiation = 1.1910439340652e8/(10.5**5*np.expm1(14388.291040407/(10.5*temperatures)))
    expected = weights*radiation
    np.testing.assert_allclose(actual, expected/expected.sum(), atol=1e-6)


# ---- thermal forward operator (2026-11-20): reflected sky, night weights, retrieval consistency ----
C1, C2, SIGMA = 1.1910439340652e8, 14388.291040407, 5.670374419e-8


def planck(t, wl):
    return C1/(wl**5*np.expm1(C2/(wl*t)))


def inverse_planck(radiance, wl):
    return C2/(wl*np.log1p(C1/(radiance*wl**5)))


def run_thermal(tmp_path, tag, *, components, lai=0., overpass='2019-07-15T04:00:00Z', sky=None, inversion=None,
                thermal=True, soil_moisture=.05):
    out = tmp_path/tag
    out.mkdir()
    config = dict(kind='stream-image-config', schemaVersion=2, outputDir=str(out),
        dataDir=str(RUNTIME/'stream-defined'), overpass=overpass,
        surface=dict(type=dict(file=raster(tmp_path/f'{tag}_type.tif', [10])), lai=dict(value=lai),
                     soilMoisture=dict(value=soil_moisture), latitude=dict(value=40.), longitude=dict(value=116.),
                     dem=dict(value=0.)),
        view=dict(vza=dict(value=0.), vaa=dict(value=0.)), optical=dict(enabled=False),
        thermal=dict(enabled=thermal, bands=[dict(name='TIR', center=10.5, fwhm=.5)] if thermal else []), threads=1,
        temperature=dict(mode='rt', components={k: dict(value=v) for k, v in zip(
            ('soilSunlit', 'soilShaded', 'leafSunlit', 'leafShaded'), components)},
            energy=dict(soilEmissivityModel='constant')))
    if sky is not None:
        config['skyLongwave'] = sky
    if inversion:
        config['inversion'] = inversion
    path = tmp_path/f'{tag}.json'
    path.write_text(json.dumps(config), encoding='utf-8')
    env = dict(os.environ, PATH=str(RUNTIME) + os.pathsep + os.environ['PATH'])
    result = subprocess.run([str(RUNTIME/'stream.exe'), 'run', str(path)], capture_output=True,
                            encoding='utf-8', errors='replace', env=env, timeout=60)
    assert result.returncode == 0, result.stderr + result.stdout
    return out


def read_band(path, band=1):
    with gdal.Open(str(path)) as ds:
        return float(ds.GetRasterBand(band).ReadAsArray()[0, 0])


def test_bare_soil_brightness_includes_reflected_sky(tmp_path):
    # Dry bare soil, constant emissivity 0.955 (classes.json), isothermal 300 K, configured L↓ = 350 W m-2.
    out = run_thermal(tmp_path, 'sky', components=[300.]*4, sky=dict(value=350.))
    t_sky = (350./SIGMA) ** .25
    expected = inverse_planck(.955*planck(300., 10.5) + .045*planck(t_sky, 10.5), 10.5)
    assert abs(read_band(out/'tir_brightness_temperature.tif') - expected) < 2e-3
    # The LST product stays emitted radiance / emissivity.
    assert abs(read_band(out/'tir_surface_temperature.tif') - 300.) < 2e-3
    legacy = run_thermal(tmp_path, 'nosky', components=[300.]*4, sky=dict(mode='none'))
    assert abs(read_band(legacy/'tir_brightness_temperature.tif') - inverse_planck(.955*planck(300., 10.5), 10.5)) < 2e-3


def test_night_thermal_has_no_sunlit_components(tmp_path):
    night = '2019-07-15T16:00:00Z'  # local midnight at 116 E
    a = run_thermal(tmp_path, 'night_a', components=[320., 290., 315., 292.], lai=2., overpass=night, sky=dict(mode='none'))
    b = run_thermal(tmp_path, 'night_b', components=[250., 290., 250., 292.], lai=2., overpass=night, sky=dict(mode='none'))
    assert abs(read_band(a/'tir_brightness_temperature.tif') - read_band(b/'tir_brightness_temperature.tif')) < 1e-4


def test_lst_retrieval_inverts_the_forward_operator_with_sky(tmp_path):
    sky = dict(value=380.)
    forward = run_thermal(tmp_path, 'forward', components=[305.]*4, lai=2., sky=sky)
    observed = read_band(forward/'tir_brightness_temperature.tif')
    retrieval = run_thermal(tmp_path, 'retrieve', components=[305.]*4, lai=2., sky=sky, thermal=False,
        inversion=dict(target='lst', observations=[dict(kind='brightness', value=dict(value=observed), wavelength=10.5, sigma=.5)]))
    assert abs(read_band(retrieval/'lst.tif') - 305.) < 5e-3


def test_rt_mode_reads_soil_emissivity_settings(tmp_path):
    # Constant vs Hapke soil emissivity must change an oblique bare-soil brightness in RT mode as well.
    out = run_thermal(tmp_path, 'const', components=[300.]*4, sky=dict(mode='none'))
    assert abs(read_band(out/'tir_brightness_temperature.tif') - inverse_planck(.955*planck(300., 10.5), 10.5)) < 2e-3


@pytest.mark.skipif(os.name != 'nt', reason='Windows case-insensitive file identity')
def test_native_rejects_same_brightness_file_with_different_case(tmp_path):
    result, _ = run(tmp_path, dict(views=[dict(data=str(tmp_path/'OBS.tif'),
        vza=dict(value=40.), vaa=dict(value=0.))]), brightness=True)
    assert result.returncode != 0, result.stdout


def test_ls4t_export_through_workbench_to_native_analysis(tmp_path, monkeypatch):
    ls4t_root = Path(os.environ.get('LS4T_ROOT', 'C:/work/ls4t'))
    if not (ls4t_root/'base/component_map.py').is_file():
        pytest.skip('set LS4T_ROOT for the cross-repository integration test')
    monkeypatch.syspath_prepend(str(ls4t_root))
    from base.component_map import retrieve_components_map, export_component_map_for_stream
    from base.radiometry import planck_radiance
    ls4t = retrieve_components_map(np.full((1, 1, 4), planck_radiance(10.8, 310.)),
        np.eye(4).reshape(1, 1, 4, 4), 10.8, background_temperature_k=[300.]*4,
        background_covariance_k2=np.eye(4)*25, observation_uncertainty=.1,
        observation_source_ids=['satellite-scene'], background_source_ids=['ls4t-background'])
    template = raster(tmp_path/'template.tif', [0.])
    block = export_component_map_for_stream(ls4t, template, tmp_path/'bundle', utc_time='2019-07-15T04:00:00Z')
    block['priorSourceIds'] = ['independent-stream-background']
    # Native helper creates the source scene and a first native read of the bundle.
    result, out = run(tmp_path, block)
    assert result.returncode == 0, result.stderr + result.stdout
    scene = json.loads((tmp_path/'config.json').read_text(encoding='utf-8'))
    scene['locationSource'] = 'input'
    scene_path = tmp_path/'workbench-scene.json'
    scene_path.write_text(json.dumps(scene), encoding='utf-8')
    js = """import fs from 'node:fs';
const {createProjectStore}=await import(process.argv[1]);
const scene=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
const store=createProjectStore({normalizeHostPath:p=>p,windowsPath:p=>p});
process.stdout.write(JSON.stringify(store.buildEngineConfig(scene,{outputDir:scene.outputDir,dataDir:scene.dataDir})));
"""
    compiled = subprocess.run(['node', '--input-type=module', '-e', js,
        (ROOT/'server/stream/config.mjs').as_uri(), str(scene_path)], capture_output=True,
        encoding='utf-8', errors='replace', timeout=30)
    assert compiled.returncode == 0, compiled.stderr
    native = json.loads(compiled.stdout)
    assert native['assimilation']['covariance'] == block['covariance']
    (tmp_path/'config.json').write_text(json.dumps(native), encoding='utf-8')
    env = dict(os.environ, PATH=str(RUNTIME) + os.pathsep + os.environ['PATH'])
    rerun = subprocess.run([str(RUNTIME/'stream.exe'), 'run', str(tmp_path/'config.json')],
                          env=env, capture_output=True, timeout=60)
    assert rerun.returncode == 0, rerun.stderr
    with gdal.Open(str(out/'analysis_components.tif')) as ds:
        actual = ds.ReadAsArray().reshape(4)
    b = np.full((4, 4), 12.5)
    np.fill_diagonal(b, 25.)
    p = ls4t.posterior_covariance_k2[0, 0]
    expected = 300. + b @ np.linalg.solve(b+p, ls4t.temperature_k[0, 0]-300.)
    np.testing.assert_allclose(actual, expected, atol=5e-4)
