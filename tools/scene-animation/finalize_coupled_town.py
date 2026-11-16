"""Verify and seal a completed coupled game-town case, without changing inputs."""
import argparse,html,json,shutil,subprocess
from datetime import datetime,timezone
from pathlib import Path
import numpy as np
from PIL import Image
from coupled_town_prepare import read,write,sha
from run_coupled_town import native_inputs,validate_pair,runtime_shaders
from integrity import require


def finalize(case,work):
    case=Path(case).resolve();work=Path(work).resolve()
    if (case/'SHA256SUMS.json').exists():raise FileExistsError('Already sealed')
    pair=read(case/'native_pair.json');feedback=read(case/'coupling_feedback.json');video=read(case/'video_verification.json')
    require(pair['status']==feedback['status']==video['status']=='passed', "check failed: pair['status']==feedback['status']==video['status']=='passed'")
    require(feedback['passivePairMaxDifferenceK']==0, 'Paired control must be exactly identical before heat starts')
    frozen=validate_pair(case)
    require(pair['frozenPairInputs']==frozen, "check failed: pair['frozenPairInputs']==frozen")
    runtime=read(case/'runtime/receipt.json');engine=Path(runtime['engine'])
    require(sha(engine)==sha(case/'runtime/histream.exe')==runtime['engineSha256'], "check failed: sha(engine)==sha(case/'runtime/histream.exe')==runtime['engineSha256']")
    require(runtime_shaders(engine)==runtime['runtimeShaders'], "check failed: runtime_shaders(engine)==runtime['runtimeShaders']")
    for name,h in runtime['sourceSnapshots'].items():require(sha(case/'source'/name)==h, "check failed: sha(case/'source'/name)==h")
    for name,h in runtime['runtimeShaders'].items():require(sha(case/'runtime'/name)==h, "check failed: sha(case/'runtime'/name)==h")
    checks=[];minimum=1e9;maximum=-1e9;max_exchange_relative=0.;max_core_relative=0.
    for label in ['on','off']:
        root=case/'native'/label;receipt=read(root/'run_receipt.json');index=read(root/'output/faceteb_run.json')
        require(receipt['actualInputHashes']==native_inputs(root/'project.json')==frozen[label], "check failed: receipt['actualInputHashes']==native_inputs(root/'project.json')==frozen[label]")
        require(receipt['engineSha256']==runtime['engineSha256'] and receipt['inputsUnchanged'], "check failed: receipt['engineSha256']==runtime['engineSha256'] and receipt['inputsUnchanged']")
        require(index['status']=='completed' and len(index['completed'])==38 and all(n['converged'] for n in index['completed']), "check failed: index['status']=='completed' and len(index['completed'])==38 and all(n['converged'] for n in index['completed'])")
        for node in index['completed']:
            d=read(root/'output/diagnostics'/f'facet_physics_node={node["node"]}.json')
            require(d['lumpedThermalMaxSurfaceResidual_W_m2']<2, "check failed: d['lumpedThermalMaxSurfaceResidual_W_m2']<2")
            require(node['energyFailingFacetFraction']<.001, "check failed: node['energyFailingFacetFraction']<.001")
            for b in d['lumpedThermal']:
                scale=max(1.,abs(b['inputPower_W']),abs(b['conductedToSkin_W']),abs(b['storage_W']))
                core=abs(b['coreResidual_W'])/scale
                exchange=abs(b['conductedToSkin_W']-b['surfaceReceived_W'])/max(1.,abs(b['conductedToSkin_W']))
                max_core_relative=max(max_core_relative,core);max_exchange_relative=max(max_exchange_relative,exchange)
                require(abs(b['coreResidual_W'])<=max(.001,1e-8*scale), lambda: (label,node['node'],b))
                require(abs(b['conductedToSkin_W']-b['surfaceReceived_W'])<=max(.001,1e-6*abs(b['conductedToSkin_W'])), lambda: (label,node['node'],b))
            r=np.fromfile(root/'output/.facet_steps'/('energy_T='+node['token']+'.bin'),dtype='<f4').reshape(-1,2,13)
            require(np.isfinite(r).all(), 'check failed: np.isfinite(r).all()')
            minimum=min(minimum,float(r[:,:,0].min()));maximum=max(maximum,float(r[:,:,0].max()))
            require(r[:,:,0].min()>=150 and r[:,:,0].max()<=1500, 'check failed: r[:,:,0].min()>=150 and r[:,:,0].max()<=1500')
        checks.append(dict(label=label,nodes=38,maxSurfaceResidualWm2=receipt['maxCoupledSurfaceResidualWm2'],maxCoreResidualW=receipt['maxCoreResidualW'],
            maxConductiveExchangeMismatchW=receipt['maxConductiveExchangeMismatchW'],maxEnergyResidualWm2=receipt['maxEnergyResidualWm2']))
    original_hashes=read(case/'source_inputs.json')
    for path,h in original_hashes.items():require(sha(path)==h, lambda: path)
    original=Path(next(p for p in original_hashes if p.endswith('scene.json'))).parent
    source_manifest=read(original/'SHA256SUMS.json');source_manifest=source_manifest.get('files',source_manifest)
    for name,item in source_manifest.items():require(sha(original/name)==(item['sha256'] if isinstance(item,dict) else item), lambda: name)
    scene=read(case/'scene.json');previous=read(original/'scene.json')
    require(scene['cameras']==previous['cameras'] and scene['instances']==previous['instances'] and scene['materials']==previous['materials'], "check failed: scene['cameras']==previous['cameras'] and scene['instances']==previous['instances'] and scene['materials']==previous['materials']")
    require(scene['assets']==previous['assets'], "check failed: scene['assets']==previous['assets']")
    for asset in scene['assets']:require(sha(case/asset['path'])==asset['sha256'], "check failed: sha(case/asset['path'])==asset['sha256']")
    node_count=len(read(case/'timeline.json'));require(node_count==21, 'check failed: node_count==21')
    files=list((case/'radiance').glob('*.npz'));require(len(files)==84, 'check failed: len(files)==84')
    for p in files:
        with np.load(p) as data:
            require(data['radiance'].shape==(720,1280) and np.isfinite(data['radiance']).all(), "check failed: data['radiance'].shape==(720,1280) and np.isfinite(data['radiance']).all()")
            require(data['adc'].shape==(180,320) and data['adc'].dtype==np.uint16 and data['adc'].max()<=4095, "check failed: data['adc'].shape==(180,320) and data['adc'].dtype==np.uint16 and data['adc'].max()<=4095")
    frames=list((case/'video_frames').glob('*.png'));require(len(frames)==288, 'check failed: len(frames)==288')
    for p in frames:
        with Image.open(p) as im:require(im.size==(2560,1600), 'check failed: im.size==(2560,1600)');im.verify()
    require(sha(case/'town_coupled_heat_cool.mp4')==video['sha256'], "check failed: sha(case/'town_coupled_heat_cool.mp4')==video['sha256']")
    # Keep the immutable startup snapshots. Current tools and tests are a
    # separately labeled development/reproduction snapshot.
    current=case/'source/current_tools';current.mkdir(parents=True,exist_ok=False)
    tools=Path(__file__).parent
    names=['coupled_town_prepare.py','run_coupled_town.py','render_coupled_town.py','thermal_imaging.py','finalize_coupled_town.py']
    for name in names:shutil.copyfile(tools/name,current/name)
    tests=current/'tests';tests.mkdir()
    test_names=['lumped_thermal_fixture.cpp','test_lumped_scene.py','test_lumped_high_temperature.py','test_lumped_legacy.py','test_coupled_mesh.py','test_coupled_radiometry.py','test_coupled_receipts.py','test_visibility_repeat.py','test_passive_pair.py']
    for name in test_names:shutil.copyfile(tools/'tests'/name,tests/name)
    logs=case/'checks';logs.mkdir()
    for name in ['tests_core_result.log','native_green_deterministic.log','high_temperature_final.log','native_legacy_final.log','tests_mesh_final.log','tests_radiometry_final.log','tests_receipts_final.log','visibility_repeat_green_r02.log','tests_passive_pair_green.log','build_deterministic_faceteb.log']:
        p=work/name
        require(p.is_file(), lambda: p)
        shutil.copyfile(p,logs/name)
    verification=dict(status='passed',completedAtUtc=datetime.now(timezone.utc).isoformat(),nativeRuns=checks,totalNativeNodes=76,
        passiveControlBitIdentical=True,originalAssetsExact=len(scene['assets']),originalInstancesExact=len(scene['instances']),originalCamerasExact=4,
        originalInputManifestFilesVerified=len(source_manifest),sourceInputsUnchanged=True,nativeEnergyRecordsFinite=True,nativeSurfaceTemperatureRangeK=[minimum,maximum],
        maxCoreRelativeClosure=max_core_relative,maxConductiveExchangeRelativeMismatch=max_exchange_relative,
        maxGroundFeedbackDeltaK=feedback['maxGroundFeedbackDeltaK'],idealProducts=84,detectorDimensions=[320,180],adcBits=12,combinedDisplayFrames=288,
        videoFullDecode=True,radiation='native Vulkan GPU coupled shortwave/longwave',thermalNetwork='CPU implicit core/surface RC',
        mwir='CPU 17-point graybody band mapping from coupled native state; equivalent spectrum for native local longwave and assumed solar spectrum',
        fieldAccuracyValidation='not performed',gridAndTimeConvergenceStudy='not performed',sourceVersion=runtime['sourceSnapshots'])
    write(case/'verification.json',verification)
    text=f'''# 原小镇 · 传导、能量平衡与辐射双向耦合 v5

本轮采用同一原场景：{len(scene['assets'])}个资产、{len(scene['instances'])}个实例、原四个相机。8个活动游戏道具、3个被动深层地面热库。旧案例保留。

## 查看

- index.html：结果总览与视频。
- town_coupled_heat_cool.mp4：12秒／24fps／288帧；10分钟开启热源、10分钟关闭冷却。
- *_rgb_ir_p2.png：原RGB参考与耦合3–5 μm相机图，P2—P98仅显示增强。
- *_rgb_ir.png、*_off_on.png：固定辐射显示范围与同条件关闭／开启对照。
- *_ground_feedback.png：数值计算域地面温差，来自真实求解状态相减。
- thermal_curves.png、heat_history.csv：道具温升、冷却与地面反馈。
- radiance/：四视角各21个物理节点，共84份理想辐亮度、对照、12bit ADC和地面温差NPZ。
- video_frames/：288张连续合成PNG；中间帧为相邻60秒节点的辐亮度插值，并非新增物理解算。
- native/on、native/off：两组完整原生解算和逐节点诊断；runtime/保存实际引擎、53份SPIRV和来源记录。

## 实际耦合

内部节点满足 Cc·(Tc−Tc_old)/dt = P − G·(Tc−Tskin_weighted) − Ga·(Tc−Ta)。
每个表面增加与内部节点相反的导热功率，再与吸收短波、环境长波、热辐射、对流及储热联立。
表面温度更新后重新计算场景长波交换，因此道具发热能反馈到地面与其他可见表面。
地面薄层与各地面实例的等效深层热库交换；附加原生土壤边界使用一维分层导热。

计算先预热36小时，随后以60秒步长计算20分钟；共38节点／组、两组76节点。
热源关闭组保持几何、气象、热容、热导和材质相同，只关闭工作功率。
热源开启前两组温度、日照比例与可见性图逐位一致；开启后的最大地面温差为{feedback['maxGroundFeedbackDeltaK']:.6g} K。
原生能量收敛沿用原规则（残差2 W/m²，失败面元比例严格低于0.1%），并单独要求所有热节点关联面残差小于2 W/m²。

上述增温是局部峰值。20分钟时计算域地面面积平均温差约为{feedback['timeline'][-1]['groundAreaMeanDeltaK']:.6g} K；
最小差值{feedback['timeline'][-1]['groundMinDeltaK']:.6g} K也保留在诊断中，未将小负差解释为已证实的冷却机制。

## 图像方法与范围

原生GPU求遮挡、短波／长波辐射；CPU求新增热网络和表面能量平衡。
成像读取收敛的日照／阴影温度、当地长波与短波通量，CPU做3–5 μm的17点Simpson积分。
当地长波用等效灰体谱、太阳用5772 K假定谱估计，未声称逐波长全场多次反射或大气求解。
相机为320×180、12bit，含PSF、像元平均及通用噪声；P2—P98仅改变显示。
RGB为沿用的原静态可见光场景参考，其照明不是此次重新标定的光学产品。

## 数值网格与限制

原显示几何、材质表、实例变换、相机和资产哈希不变。独立热网格为797271个三角形，
数值区域[-230,-230,230,120]m；主要地面最大边长4m，裁剪的远景地面20m。
thermal/mapping.npz保存原三角形／实例映射。ROI外显示地面使用两组共同的被动边界温度，不将其当作解析出的局地热反馈。
深层地面热库是集中参数近似，没有为每个地面面元求完整土柱，也没有三维固体内部传导。
植被按干燥不透明表面处理，没有叶片蒸腾／生化耦合；气象、材质与工作功率均为示例假设。
空气温度与风速是外部边界，本轮未将空气流场及空气增温反馈一并联立。
本次做数值守恒、确定性、配对反馈和存储检查，未做实测精度、独立时间步／网格收敛研究。

## 重复性修正与保留记录

共面面片曾引发GPU A-buffer顺序不稳定，已在RT和EB两套resolve中加入深度／side-ID稳定排序，
并统一CSR累加顺序、补全读回同步。4096个共面面元连续4次输出逐位一致。
trials/保留360K旧被动温度上限失败试算和修复前非确定性试算；它们不作为本版结果。
工作道具声明500K上限，地面保持360K；能量收敛标准未放宽。

## 复现

正式工具在 C:/work/streamstudio/tools/scene-animation；独立准备、运行和包装均使用新目录：

```powershell
python coupled_town_prepare.py <原v4案例目录> <actors.json> <新案例目录>
python run_coupled_town.py <新案例目录> <histream.exe>
python render_coupled_town.py <新案例目录> <histream.exe> --ffmpeg <ffmpeg.exe>
```

所用C++、shader和启动脚本保存在source/；当前复现工具与测试在source/current_tools/。
启动源码快照保持原样，后续仅修正OBJ材质声明的准备工具单独保存；其几何和原生输入不变。
生成后检查verification.json与SHA256SUMS.json。不要将已有封存目录作为新运行输出。
'''
    (case/'README.md').write_text(text,encoding='utf-8')
    cards=''.join(f'<section><h2>{v}</h2><a href="{v}_rgb_ir.png"><img src="{v}_rgb_ir.png"></a><p><a href="{v}_rgb_ir_p2.png">P2—P98显示</a> · <a href="{v}_off_on.png">开关对照</a> · <a href="{v}_ground_feedback.png">地面反馈</a></p></section>' for v in ['overview','runway_view','road_view','high_view'])
    page=f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>原小镇 · 双向热耦合 v5</title><style>body{{margin:0;background:#eef2f1;color:#19323d;font:16px/1.7 "Microsoft YaHei",sans-serif}}main{{max-width:1280px;margin:auto;padding:32px 24px}}h1{{font-size:31px}}section{{background:white;padding:22px;margin:24px 0;border-top:3px solid #196b65}}img,video{{width:100%;height:auto;display:block}}video{{max-height:760px;background:#13232a}}a{{color:#07695f}}.note{{padding:16px;background:#dceae6}}table{{width:100%;border-collapse:collapse}}td{{padding:8px;border-bottom:1px solid #c6d7d7}}small{{color:#546d78}}</style><main><h1>原小镇 · 传导、能量平衡与辐射双向耦合</h1><p>内部热源 → 表皮导热 → 场景短波／长波与表面能量平衡 → 中红外成像</p><p><a href="README.md">方法与范围</a> · <a href="verification.json">验证记录</a> · <a href="coupling.json">热参数</a> · <a href="heat_history.csv">温度曲线数据</a> · <a href="source/current_tools/">代码与测试</a></p><div class="note">原四相机与显示布局保留。两组76个原生节点收敛；热源开启前配对结果逐位一致。最大地面反馈 {feedback['maxGroundFeedbackDeltaK']:.4g} K，属于本案例参数下的模拟结果。</div><section><h2>升温与冷却 · 20分钟压缩至12秒</h2><video controls preload="metadata" poster="road_view_rgb_ir.png"><source src="town_coupled_heat_cool.mp4" type="video/mp4"></video><small>IR使用固定辐射显示范围，另存P2—P98静态图；原始辐亮度和12bit数据独立存储。中间视频帧由原生分钟节点插值。</small></section><section><h2>温度与地面反馈</h2><img src="thermal_curves.png"></section>{cards}<section><h2>计算范围</h2><p>原生GPU辐射＋CPU热网络／能量平衡。热网格独立细分，地面为薄层与等效深层热库；材质、气象与功率为示例假设。未加入三维导热、大气、蒸腾或实测标定。</p><p><a href="native_pair.json">配对原生记录</a> · <a href="coupling_feedback.json">反馈诊断</a> · <a href="video_verification.json">视频检查</a> · <a href="trials/README.md">保留试算说明</a></p></section></main></html>'''
    (case/'index.html').write_text(page,encoding='utf-8')
    # Seal only after screenshots/keyframes are reviewed separately.
    write(case/'ready_for_visual_review.json',dict(status='passed',physicalChecks='passed',index='index.html',shaSealed=False))
    print(json.dumps(verification,ensure_ascii=False),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('work',type=Path);a=p.parse_args();finalize(a.case,a.work)
