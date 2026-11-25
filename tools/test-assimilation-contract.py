# ==========================================================================
# 文件：test-assimilation-contract.py
# 功能：STREAM 原生引擎（models/bin_x64/Release/stream.exe）的同化 / 热红外正演 / 反演契约测试（pytest），
#       使用 1×1 像元的合成栅格，只验证契约与数值一致性，不声称野外精度
# 运行：C:/work/miniconda/python.exe -m pytest tools/test-assimilation-contract.py（需先构建 stream 的 Release 版）；
#       Node 侧配置编译测试见 test-assimilation-config.mjs
# 主要模块：
#   1. raster / run：写合成 GeoTIFF、拼 stream-image-config JSON、调用 stream.exe run
#   2. 同化契约：无效参数被拒；栅格误差/协方差控制分析值；QA 剔除、非法误差、非正定协方差保持背景值
#   3. 反演产物：lsct 权重分数与辐亮度分数是两个不同产品
#   4. 热红外正演：run_thermal + Planck 函数，检查反射天空、夜间无阳照分量、LST 反演与正演互逆
#   5. 跨仓库集成：LS4T 导出 → 工作台配置编译 → 原生再次分析（需 LS4T_ROOT，缺失则跳过）
# 约定：温度 K；波长 µm；分量顺序 soilSunlit / soilShaded / leafSunlit / leafShaded；
#       背景协方差缺省为方差 25 K²、分量间协方差 12.5 K²（相关 0.5，来自测试中的解析推导）；
#       Planck 常数 C1 = 1.1910439340652e8 W µm4 m-2 sr-1，C2 = 14388.291040407 µm K。
# ==========================================================================

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


# 作用：把 values 写成 1×1 像元、len(values) 个波段的 Float32 GeoTIFF，返回路径字符串。
def raster(path, values):
    values = np.asarray(values, np.float32).reshape(-1, 1, 1)
    ds = gdal.GetDriverByName('GTiff').Create(str(path), 1, 1, len(values), gdal.GDT_Float32)
    for i, v in enumerate(values, 1):
        ds.GetRasterBand(i).WriteArray(v)
    ds = None
    return str(path)


# 作用：生成同化场景配置并运行 stream.exe，返回 (进程结果, 输出目录)。
# extra 覆盖/追加 assimilation 字段；inversion=True 改为 lsct 反演（去掉同化块）；
# brightness=True 改用亮温观测并打开 10.5 µm 热红外通道。超时 60 s。
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


# 无效契约：分量误差为 0、波长为 0、观测层级未知、来源与先验来源重复，引擎都应返回非零退出码。
@pytest.mark.parametrize('extra', [dict(sigmaComponentK=0), dict(lstWavelengthUm=0),
    dict(observationLevel='toa'), dict(sourceIds=['scene'], priorSourceIds=['scene'])])
def test_reject_invalid_contract(tmp_path, extra):
    result, _ = run(tmp_path, extra)
    assert result.returncode != 0, result.stdout


# 观测增量 +10 K、观测误差 σ=20 K（R=400 K²）；背景协方差的全 1 特征值为 25·(1+3·0.5)=62.5 K²，
# 分析增量 = 10·62.5/(400+62.5)（见下方断言）。
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
# QA 位 256（被剔除）或 NaN 的观测不参与同化，分析值保持背景 300 K。
def test_qa_rejection_preserves_background(tmp_path, value):
    qa = raster(tmp_path/'qa.tif', [value])
    result, out = run(tmp_path, dict(qa=qa))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    np.testing.assert_allclose(ds.ReadAsArray(), 300.)


@pytest.mark.parametrize('value', [0., -1., float('nan')])
# 不合法的观测误差（0、负、NaN）不得退回缺省误差，而是丢弃该观测，保持背景。
def test_invalid_error_never_falls_back_to_default(tmp_path, value):
    sigma = raster(tmp_path/'sigma.tif', [value] * 4)
    result, out = run(tmp_path, dict(uncertainty=sigma))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    np.testing.assert_allclose(ds.ReadAsArray(), 300.)


# 观测误差协方差 R（方差 400、协方差 200）使四个分量不能当作 4 个独立观测，全 1 特征值 400+3·200=1000 K²。
def test_correlated_component_errors_are_not_four_independent_measurements(tmp_path):
    covariance = np.full((4, 4), 200.)
    np.fill_diagonal(covariance, 400.)
    path = raster(tmp_path/'covariance.tif', covariance.reshape(-1))
    result, out = run(tmp_path, dict(covariance=path))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    # R's all-ones eigenvalue is 400 + 3 * 200 = 1000 K².
    np.testing.assert_allclose(ds.ReadAsArray(), 300. + 10.*62.5/(1000.+62.5), atol=3e-4)


# R 非正定（对角 1、非对角 2）时拒绝分析，保持背景值。
def test_non_positive_definite_covariance_keeps_background(tmp_path):
    covariance = np.full((4, 4), 2.)
    np.fill_diagonal(covariance, 1.)
    result, out = run(tmp_path, dict(covariance=raster(tmp_path/'bad_cov.tif', covariance.reshape(-1))))
    assert result.returncode == 0, result.stderr + result.stdout
    ds = gdal.Open(str(out/'analysis_components.tif'))
    np.testing.assert_allclose(ds.ReadAsArray(), 300.)


# lsct_radiance_fractions 应等于 权重 × Planck 辐亮度 再归一化，不等同于权重分数 lsct_fractions。
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
# Planck 辐射常数（C1：W µm4 m-2 sr-1；C2：µm K）与 Stefan–Boltzmann 常数（W m-2 K-4）。
C1, C2, SIGMA = 1.1910439340652e8, 14388.291040407, 5.670374419e-8


# Planck 光谱辐亮度（W m-2 sr-1 µm-1），t 为 K，wl 为 µm；inverse_planck 为其逆（辐亮度 → 亮温）。
def planck(t, wl):
    return C1/(wl**5*np.expm1(C2/(wl*t)))


def inverse_planck(radiance, wl):
    return C2/(wl*np.log1p(C1/(radiance*wl**5)))


# 作用：运行 RT 模式热红外正演（或反演）并返回输出目录，失败直接断言。components 依次为四个分量温度（K）；
# sky 为反射天空长波设置；土壤发射率取常数模型。地类 10 与 LAI 由参数决定。
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


# 读取 1×1 栅格指定波段的像元值。
def read_band(path, band=1):
    with gdal.Open(str(path)) as ds:
        return float(ds.GetRasterBand(band).ReadAsArray()[0, 0])


# 预期亮温：Planck 逆变换（ε·B(300 K) + (1-ε)·B(T_sky)），其中 T_sky = (L↓/σ)^(1/4)；
# sky 模式为 none 时回到仅有发射项的旧结果。
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


# 夜间（116°E 当地午夜）阳照分量不应出现在亮温中：改变阳照分量温度结果不变。
def test_night_thermal_has_no_sunlit_components(tmp_path):
    night = '2019-07-15T16:00:00Z'  # local midnight at 116 E
    a = run_thermal(tmp_path, 'night_a', components=[320., 290., 315., 292.], lai=2., overpass=night, sky=dict(mode='none'))
    b = run_thermal(tmp_path, 'night_b', components=[250., 290., 250., 292.], lai=2., overpass=night, sky=dict(mode='none'))
    assert abs(read_band(a/'tir_brightness_temperature.tif') - read_band(b/'tir_brightness_temperature.tif')) < 1e-4


# 一致性：用正演得到的亮温作观测做 LST 反演，应取回 305 K（含反射天空项）。
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


# Windows 文件名不区分大小写：views 中引用大小写不同的同一文件应被原生引擎拒绝。
@pytest.mark.skipif(os.name != 'nt', reason='Windows case-insensitive file identity')
def test_native_rejects_same_brightness_file_with_different_case(tmp_path):
    result, _ = run(tmp_path, dict(views=[dict(data=str(tmp_path/'OBS.tif'),
        vza=dict(value=40.), vaa=dict(value=0.))]), brightness=True)
    assert result.returncode != 0, result.stdout


# 跨仓库集成：LS4T 反演结果导出 → 工作台（server/stream/config.mjs）编译 → 原生再次分析，
# 结果应等于解析后验 300 + B (B+P)^-1 (T_ls4t - 300)。
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
    # 用 node 调用 createProjectStore().buildEngineConfig 编译场景，输出 JSON 供原生引擎重新读取。
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
