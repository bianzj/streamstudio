"""Inspect native feedback and render sensor images from solved town states."""
import argparse,csv,json,math,shutil,subprocess
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from coupled_town_prepare import read,write,sha
from prepare_thermal_case import load_world
from thermal_imaging import band,outgoing_band,observe_radiance,camera_rays,solar_fraction,SIGMA,energy_record_width
from integrity import require
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def display(radiance,low,high):return np.rint(np.clip((radiance-low)/(high-low),0,1)*255).astype(np.uint8)


def p2_display(radiance):
    low,high=np.percentile(radiance,[2,98]);high=max(high,low+1e-9)
    return display(radiance,float(low),float(high))


def run(case,engine,ffmpeg,video_encoder='auto'):
    # Encoder probed before any rendering work (a missing libx264 used to fail at the first video frame).
    video_codec=pick_encoder(ffmpeg,video_encoder);print('video encoder: '+video_codec,flush=True)
    case=Path(case).resolve()
    if (case/'SHA256SUMS.json').exists():raise FileExistsError('Sealed case cannot be overwritten')
    require(read(case/'native_pair.json')['status']=='passed', "check failed: read(case/'native_pair.json')['status']=='passed'")
    config=read(case/'coupling.json');source=Path(next(k for k in read(case/'source_inputs.json') if k.endswith('scene.json'))).parent
    scene=read(case/'scene.json');proxy,t,mids,_=load_world(case/'thermal/scene.json');n=len(t)
    mapping=np.load(case/'thermal/mapping.npz');source_instance=mapping['source_instance'];areas=mapping['areas']
    ids=[i['id'] for i in scene['instances']];active=np.isin(source_instance,[ids.index(a['id']) for a in read(case/'actors.json')['actors']])
    ground=np.isin(source_instance,[ids.index(x) for x in ['base_ground','scenery_expanded_ground','runway_road_field']])
    far=source_instance==ids.index('scenery_expanded_ground');normals=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0])
    eps=np.asarray([m['emissivity'] for m in proxy['materials']])[mids];albedo=np.asarray([m['thermalAlbedo'] for m in proxy['materials']])[mids]
    indices={name:read(case/'native'/name/'output/faceteb_run.json') for name in ['on','off']}
    record_widths={name:energy_record_width(read(case/'native'/name/'project.json')) for name in ['on','off']}
    require(len(set(record_widths.values()))==1, lambda: record_widths)
    record_width=record_widths['on']
    geometry_checks={}
    for label,index in indices.items():
        require(index['status']=='completed' and len(index['completed'])==38 and all(x['converged'] for x in index['completed']), "check failed: index['status']=='completed' and len(index['completed'])==38 and all(x['converged'] for x in index['completed'])")
        geo=read(case/'native'/label/'output/faceteb.json')
        require(geo['converged'] and geo['graph']['fragmentOverflow']==0 and geo['graph']['hashOverflow']==0, "check failed: geo['converged'] and geo['graph']['fragmentOverflow']==0 and geo['graph']['hashOverflow']==0")
        vertices=np.asarray(geo['vertexPositions'],dtype=np.float32).reshape(-1,3,3)
        require(len(vertices)==n+2, 'check failed: len(vertices)==n+2')
        converted=vertices[:n,:,[0,2,1]].copy();converted[:,:,1]*=-1;converted[:,:,2]-=.6
        error=float(np.max(np.abs(converted-t)));require(error<=1e-4, lambda: error)
        require(np.all(np.sum(np.cross(converted[:,1]-converted[:,0],converted[:,2]-converted[:,0])*normals,axis=1)>0), 'check failed: np.all(np.sum(np.cross(converted[:,1]-converted[:,0],converted[:,2]-converted[:,0])*normals,axis=1)>0)')
        geometry_checks[label]=dict(maxVertexErrorM=error,nativeTriangles=len(vertices),thermalTriangles=n,fragmentOverflow=0,hashOverflow=0)
        del geo,vertices,converted
    def records(label,node):
        info=next(x for x in indices[label]['completed'] if x['node']==node)
        return np.fromfile(case/'native'/label/'output/.facet_steps'/('energy_T='+info['token']+'.bin'),dtype='<f4').reshape(n+2,2,record_width)[:n]
    # Numerical paired-run noise floor is measured before any working heat starts.
    warm_on=records('on',17);warm_off=records('off',17)
    warm_max=float(np.max(np.abs(warm_on[:,:,0]-warm_off[:,:,0])))
    require(warm_max<.01, lambda: ('Unmatched passive initial states',warm_max))
    del warm_on,warm_off
    out_geometry=case/'thermal_gpu_geometry'
    if not out_geometry.exists():
        with (case/'thermal_geometry.log').open('wb') as log:
            proc=subprocess.run([str(engine),'eSceneAnimation',str(case/'thermal/scene.json'),str(out_geometry)],stdout=log,stderr=subprocess.STDOUT)
        require(proc.returncode==0, 'Inspect thermal_geometry.log')
    manifest=read(out_geometry/'run-manifest.json');require(manifest['status']=='complete', "check failed: manifest['status']=='complete'")
    require(manifest['sourceSha256']==sha(case/'thermal/scene.json') and manifest['executableSha256']==sha(engine), "check failed: manifest['sourceSha256']==sha(case/'thermal/scene.json') and manifest['executableSha256']==sha(engine)")
    views=[c['id'] for c in scene['cameras']];width=scene['width'];height=scene['height'];cache={}
    for view in views:
        camera=next(c for c in scene['cameras'] if c['id']==view)
        hit=np.fromfile(out_geometry/(view+'_hits.f32'),dtype='<f4').reshape(height,width,8)
        original_hit=np.fromfile(source/'gpu_geometry'/(view+'_hits.f32'),dtype='<f4').reshape(height,width,8)
        valid=hit[:,:,0]>=0;original_valid=original_hit[:,:,0]>=0
        triangle=np.clip(hit[:,:,7].astype(int),0,n-1);rays=camera_rays(camera,width,height)
        side=(np.sum(normals[triangle]*rays,axis=-1)>=0).astype(int)
        gap=valid&original_valid
        maxgap=float(np.max(np.abs(hit[:,:,0][gap]-original_hit[:,:,0][gap])))
        # Refined numerical planes preserve the display geometry; a few edge rays
        # can choose a different coincident layer and are recorded, not concealed.
        far_mask=original_valid&~valid;sky=~original_valid
        cache[view]=dict(valid=valid,triangle=triangle,side=side,far=far_mask,sky=sky,
            maxHitDistanceDifferenceM=maxgap,farBoundaryPixels=int(far_mask.sum()),skyPixels=int(sky.sum()))
        (case/'rgb').mkdir(exist_ok=True);shutil.copyfile(source/'gpu_geometry'/(view+'_visible.png'),case/'rgb'/(view+'.png'))
    (case/'radiance').mkdir(exist_ok=True);(case/'display').mkdir(exist_ok=True)
    visible_triangles=np.unique(np.concatenate([c['triangle'][c['valid']] for c in cache.values()]))
    compact=np.full(n,-1,dtype=int);compact[visible_triangles]=np.arange(len(visible_triangles))
    require(all(np.all(compact[c['triangle'][c['valid']]]>=0) for c in cache.values()), "check failed: all(np.all(compact[c['triangle'][c['valid']]]>=0) for c in cache.values())")
    low=float(band(285));high=float(band(390));fraction=solar_fraction();weather=np.loadtxt(case/'native/on/meteorology.txt',skiprows=1)
    timeline=[];stats=[];histories=[];max_ground_delta=0.;hot_means={};cool_means={}
    font=ImageFont.truetype(r'C:\Windows\Fonts\msyh.ttc',28);small=ImageFont.truetype(r'C:\Windows\Fonts\msyh.ttc',21)
    for node in range(17,38):
        physical=(node-17)*60
        on=records('on',node);off=records('off',node);delta=on[:,0,0].astype(float)-off[:,0,0]
        max_ground_delta=max(max_ground_delta,float(delta[ground].max()))
        diagnostic_on=read(case/'native/on/output/diagnostics'/f'facet_physics_node={node}.json')
        diagnostic_off=read(case/'native/off/output/diagnostics'/f'facet_physics_node={node}.json')
        by_on={b['id']:b for b in diagnostic_on['lumpedThermal']};by_off={b['id']:b for b in diagnostic_off['lumpedThermal']}
        for actor in read(case/'actors.json')['actors']:
            mask=source_instance==ids.index(actor['id']);a=areas[mask]
            skin_on=float(np.average(on[mask,0,0],weights=a));skin_off=float(np.average(off[mask,0,0],weights=a))
            if physical==600:hot_means[actor['id']]=skin_on-skin_off
            if physical==1200:cool_means[actor['id']]=skin_on-skin_off
            histories.append(dict(seconds=physical,id=actor['id'],coreOnK=by_on[actor['id']]['coreTemperature_K'],coreOffK=by_off[actor['id']]['coreTemperature_K'],
                skinOnK=skin_on,skinOffK=skin_off,inputPowerW=by_on[actor['id']]['inputPower_W']))
        stats.append(dict(seconds=physical,groundMinDeltaK=float(delta[ground].min()),groundMaxDeltaK=float(delta[ground].max()),groundAreaMeanDeltaK=float(np.average(delta[ground],weights=areas[ground])),
            backgroundMaxDeltaK=float(delta[~active].max()),backgroundAreaMeanDeltaK=float(np.average(delta[~active],weights=areas[~active])),
            warmupMaxDifferenceK=warm_max))
        sky_k=float((weather[node,6]/SIGMA)**.25)
        # Imaging evaluates only actually visible triangles; all temperatures
        # and energy feedback above still come from the full native domain.
        rad_on=outgoing_band(on[visible_triangles],eps[visible_triangles],albedo[visible_triangles],sky_k,fraction)
        rad_off=outgoing_band(off[visible_triangles],eps[visible_triangles],albedo[visible_triangles],sky_k,fraction)
        # Both runs share the same passive far-field boundary outside the ROI.
        far_t=float(np.average(off[far,0,0],weights=areas[far]))
        far_r=.95*float(band(far_t))+.05*float(band(sky_k))+.05*weather[node,5]*fraction/np.pi
        for vi,view in enumerate(views):
            c=cache[view];ii=c['triangle'];ss=c['side'];jj=np.maximum(compact[ii],0);rr=rad_on[jj,ss];reference=rad_off[jj,ss]
            rr[c['sky']]=float(band(sky_k));reference[c['sky']]=float(band(sky_k))
            rr[c['far']]=far_r;reference[c['far']]=far_r
            observed=observe_radiance(rr,low,high,seed=5100+vi,frame=node-17)
            np.savez_compressed(case/'radiance'/f'{view}_{physical:04d}.npz',radiance=rr.astype('<f4'),control=reference.astype('<f4'),
                adc=observed['codes'],groundDeltaK=np.where(c['valid']&ground[ii],delta[ii],np.nan).astype('<f4'))
            image=Image.fromarray(display(observed['radiance'],low,high)).resize((width,height),Image.Resampling.BILINEAR)
            image.save(case/'display'/f'{view}_{physical:04d}.png')
            if physical==600:
                rgb=Image.open(case/'rgb'/(view+'.png')).convert('RGB');panel=Image.new('RGB',(width*2,height+80),'#101c26');d=ImageDraw.Draw(panel)
                d.text((18,10),'原小镇 · RGB场景参考',font=font,fill='white');d.text((width+18,10),'双向热耦合 · 3–5 μm IR',font=font,fill='white')
                d.text((18,48),'内部热源—表皮传导 / 场景能量平衡 / 长波反馈；开启热源10分钟',font=small,fill='#c4d5df')
                panel.paste(rgb,(0,80));panel.paste(image.convert('RGB'),(width,80));panel.save(case/(view+'_rgb_ir.png'))
                enhanced=Image.fromarray(p2_display(observed['radiance'])).resize((width,height),Image.Resampling.BILINEAR)
                panel.paste(enhanced.convert('RGB'),(width,80))
                d.rectangle((width,0,width*2,79),fill='#101c26');d.text((width+18,10),'双向热耦合 · 3–5 μm IR · P2—P98',font=font,fill='white')
                d.text((width+18,48),'仅显示增强；原始辐亮度与12bit数字量保持独立',font=small,fill='#c4d5df');panel.save(case/(view+'_rgb_ir_p2.png'))
                compare=Image.new('RGB',(width*2,height+80),'#101c26');d=ImageDraw.Draw(compare)
                d.text((18,10),'热源关闭 · 同气象与热参数',font=font,fill='white');d.text((width+18,10),'热源开启 · 同一辐射显示范围',font=font,fill='white')
                compare.paste(Image.fromarray(display(reference,low,high)).convert('RGB'),(0,80));compare.paste(Image.fromarray(display(rr,low,high)).convert('RGB'),(width,80));compare.save(case/(view+'_off_on.png'))
                gd=np.where(c['valid']&ground[ii],delta[ii],np.nan);finite=np.isfinite(gd)
                maximum=max(.05,float(np.nanpercentile(gd,99.5)) if finite.any() else .05)
                scaled=np.nan_to_num(np.clip(gd/maximum,0,1));colors=np.stack([scaled*255,scaled**.55*175,np.zeros_like(scaled)],axis=-1).astype('uint8')
                colors[~finite]=np.array([12,22,30]);gimg=Image.fromarray(colors);gp=Image.new('RGB',(width,height+80),'#101c26');d=ImageDraw.Draw(gp)
                d.text((18,10),'地面温差 ΔT = 热源开启 − 关闭',font=font,fill='white');d.text((18,48),f'仅数值计算域地面；橙色上限 {maximum:.3f} K；其余区域留空',font=small,fill='#c4d5df');gp.paste(gimg,(0,80));gp.save(case/(view+'_ground_feedback.png'))
        timeline.append(dict(node=node,seconds=physical,weatherLocalDoy=float(weather[node,0]),skyK=sky_k))
        print(f'Coupled image products {node-16}/21, t={physical}s',flush=True)
    require(max_ground_delta>max(.005,10*warm_max), lambda: ('Ground feedback is not above the paired-run numerical difference',max_ground_delta,warm_max))
    require(all(v>.2 for v in hot_means.values()), lambda: hot_means)
    require(all(cool_means[k]<hot_means[k] for k in hot_means), lambda: (hot_means,cool_means))
    write(case/'coupling_feedback.json',dict(status='passed',passivePairMaxDifferenceK=warm_max,maxGroundFeedbackDeltaK=max_ground_delta,
        hotPropMeanDeltaK=hot_means,coolPropMeanDeltaK=cool_means,timeline=stats,geometry=geometry_checks,
        viewSampling={v:{k:c[k] for k in ['maxHitDistanceDifferenceM','farBoundaryPixels','skyPixels']} for v,c in cache.items()},
        radianceMethod='3-5 um 17-point Simpson; native solved sunlit/shaded temperatures, native broadband local irradiance reconstructed as an equivalent graybody spectrum; assumed solar shape; no atmosphere',
        rgbScope='Original static RGB scene reference; thermal solar forcing is independently recorded',
        farBoundary='Same passive ground state for on/off outside thermal ROI; not claimed as locally resolved feedback'))
    with (case/'heat_history.csv').open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(histories[0]));w.writeheader();w.writerows(histories)
    write(case/'timeline.json',timeline)
    with (case/'timeline.csv').open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(timeline[0]));w.writeheader();w.writerows(timeline)
    plot_curves(case,histories,stats)
    video(case,Path(ffmpeg),low,high,font,small,video_codec)


def plot_curves(case,histories,stats):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(2,1,figsize=(12,8),layout='constrained')
    for name in dict.fromkeys(r['id'] for r in histories):
        r=[x for x in histories if x['id']==name]
        axes[0].plot([x['seconds']/60 for x in r],[x['skinOnK']-x['skinOffK'] for x in r],label=name)
    axes[0].set_ylabel('Area-mean prop skin increment (K)');axes[0].legend(ncol=4,fontsize=9);axes[0].set_title('Coupled town: matched working-heat on/off comparison')
    axes[1].plot([x['seconds']/60 for x in stats],[x['groundMaxDeltaK'] for x in stats],label='Ground maximum',color='#ad4c20')
    axes[1].plot([x['seconds']/60 for x in stats],[x['groundAreaMeanDeltaK'] for x in stats],label='Ground area mean',color='#176c78')
    axes[1].set_ylabel('Ground temperature increment (K)');axes[1].set_xlabel('Simulated time after warmup (min)');axes[1].legend()
    for ax in axes:ax.axvline(10,ls='--',color='#666',lw=1);ax.grid(alpha=.2);ax.set_xlim(0,20)
    fig.savefig(case/'thermal_curves.png',dpi=160);plt.close(fig)


def video(case,ffmpeg,low,high,font,small,video_codec):
    frames=case/'video_frames';frames.mkdir(exist_ok=False);width=1280;height=720
    views=['road_view','runway_view'];rgb={v:Image.open(case/'rgb'/(v+'.png')).convert('RGB') for v in views}
    # Store all ideal keyframes above; video interpolates solved radiance between
    # minute nodes. Physical sampling and presentation frames remain distinct.
    keys={v:[np.load(case/'radiance'/f'{v}_{t:04d}.npz')['radiance'] for t in range(0,1201,60)] for v in views}
    output=case/'town_coupled_heat_cool.mp4'
    proc=subprocess.Popen([str(ffmpeg),'-v','error','-f','rawvideo','-pix_fmt','rgb24','-s','2560x1600','-r','24','-i','pipe:0',
        '-frames:v','288',*encoder_args(video_codec,19,'veryfast'),'-threads','4','-pix_fmt','yuv420p','-movflags','+faststart',str(output)],stdin=subprocess.PIPE,stderr=subprocess.PIPE)
    for frame in range(288):
        seconds=1200*frame/287;index=min(19,int(seconds//60));alpha=(seconds-index*60)/60
        panel=Image.new('RGB',(2560,1600),'#101c26');d=ImageDraw.Draw(panel)
        phase='热源开启' if seconds<=600 else '关闭热源后冷却'
        for row,view in enumerate(views):
            radiance=(1-alpha)*keys[view][index]+alpha*keys[view][index+1]
            obs=observe_radiance(radiance,low,high,seed=5100+['overview','runway_view','road_view','high_view'].index(view),frame=frame)
            ir=Image.fromarray(display(obs['radiance'],low,high)).resize((width,height),Image.Resampling.BILINEAR)
            y=row*800;d.text((18,y+10),view+' · RGB场景参考',font=font,fill='white')
            d.text((1298,y+10),f'耦合 IR 固定范围 · {phase} · t={seconds/60:.1f} min',font=font,fill='white')
            d.text((18,y+48),'原场景、原视角；内部热导＋表面能量平衡＋场景长波反馈',font=small,fill='#c4d5df')
            panel.paste(rgb[view],(0,y+80));panel.paste(ir.convert('RGB'),(1280,y+80))
        panel.save(frames/f'frame_{frame:04d}.png',compress_level=1)
        try:proc.stdin.write(panel.tobytes())
        except (BrokenPipeError,OSError) as exc:
            try:proc.stdin.close()
            except OSError:pass
            raise RuntimeError(f'ffmpeg ({video_codec}) failed at frame {frame} (exit {proc.wait()}): '+proc.stderr.read().decode(errors='replace')[:1200]) from exc
    proc.stdin.close();error=proc.stderr.read();code=proc.wait();require(code==0, lambda: f'ffmpeg ({video_codec}) exit {code}: '+error.decode(errors='replace')[:1200])
    decoded=subprocess.run([str(ffmpeg),'-v','error','-i',str(output),'-f','null','-'],capture_output=True)
    require(decoded.returncode==0, lambda: f'{output.name} does not decode completely: '+decoded.stderr.decode(errors='replace')[:1200])
    write(case/'video_verification.json',dict(status='passed',completeDecode=True,frames=288,fps=24,seconds=12,simulatedSeconds=1200,encoder=video_codec,
        intermediateFrames='linear radiance interpolation of 60-second native coupled nodes; not new physical solves',
        display='fixed band-radiance bounds corresponding to285-390K; optional P2-P98 still images separate; ideal radiance and ADC retained',sha256=sha(output)))
    print('PASS video full decode; 288 display PNGs preserved',flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('engine',type=Path);p.add_argument('--ffmpeg',type=Path,required=True);add_encoder_argument(p);a=p.parse_args()
    run(a.case,a.engine,a.ffmpeg,a.video_encoder)


if __name__=='__main__':main()
