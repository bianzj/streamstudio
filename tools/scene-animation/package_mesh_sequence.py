"""Caption/encode native mesh frames and verify complete video decoding."""
import argparse
import csv
import json
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image,ImageDraw,ImageFont

from prepare_thermal_case import require,sha
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def checked(command,log):
    p=subprocess.run(list(map(str,command)),capture_output=True);log.write_bytes(p.stdout+p.stderr)
    require(p.returncode==0,'Video command failed: '+str(log));return p.stdout.decode(errors='replace')


def main():
    p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('output',type=Path);p.add_argument('--ffmpeg',required=True,type=Path);p.add_argument('--old-video','--comparison-video',dest='old_video',type=Path);p.add_argument('--optical',action='store_true');add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    run=a.run.resolve();out=a.output.resolve();m=json.loads((run/'run-manifest.json').read_text(encoding='utf-8'))
    require(m['status']=='complete' and m['backend']=='meshSequence','Native mesh sequence incomplete')
    require(not a.optical or m.get('optical',{}).get('enabled') is True,'Native optical frames required')
    frame_kind='optical_frames' if a.optical else 'frames';primary='histream_mesh_impact_visible' if a.optical else 'histream_mesh_impact_infrared'
    count=m['timelineFrameCount'];fps=m['fps'];require(m['firstFrame']==1 and m['lastFrame']==count,'Full sequence required')
    require(not out.exists(),'Packaging output already exists; use a new directory')
    rows=list(csv.DictReader((run/'frames.csv').open(encoding='utf-8')));require([int(r['frame']) for r in rows]==list(range(1,count+1)),'Native frame receipt has gaps')
    records={}
    for kind in ['frames','fixed_frames']+(['optical_frames'] if a.optical else []):
        require(len(list((run/kind).glob('*.png')))==count,'Unexpected native frame count')
        for n in range(1,count+1):
            f=run/kind/f'frame_{n:04d}.png'
            with Image.open(f) as im:require(im.size==(m['width'],m['height']),'Invalid frame dimensions');im.verify()
            records[str(f.relative_to(run))]=sha(f)
    replay=json.loads((run/'provenance/replay.json').read_text(encoding='utf-8'))
    for n in replay['sequence']['exportFrames']:
        f=run/'radiance'/f'frame_{n:04d}.f32';values=np.fromfile(f,dtype='<f4');require(values.size==m['width']*m['height'] and np.isfinite(values).all() and values.min()>0,'Invalid selected radiance')
    out.mkdir(parents=True,exist_ok=False)
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',22);small=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',17)
    backend=m.get('timeline',{}).get('motionBackend');impact_v1=backend=='impact-v1';rigid=backend in ('rigid-v1','impact-v1')
    # impact-v1: physics at the physical speed, frames sampled every presentationTimeScale/fps physical seconds.
    scale=float(m.get('impactModel',{}).get('presentation',{}).get('presentationTimeScale',1)) if impact_v1 else 1.0
    slow=1/scale;impact_presentation=m['impactTime']/scale
    captioned=out/'captioned';captioned.mkdir()
    for n in range(1,count+1):
        time=(n-1)/fps;age=time-m['impactTime'];label='飞行接近' if age<0 else '撞击闪光' if age<.25 else '火团与碎片' if age<2 else '烟尘与持续破口'
        if impact_v1:
            physical=time*scale;age=physical-m['impactTime'];label='飞行接近' if age<0 else '起爆闪光' if age<.03 else '火球与碎片' if age<.6 else '烟尘与破口'
        with Image.open(run/frame_kind/f'frame_{n:04d}.png') as image:
            image=image.convert('RGB');draw=ImageDraw.Draw(image);draw.rectangle((0,0,image.width,40),fill=(10,15,19));draw.rectangle((0,image.height-27,image.width,image.height),fill=(10,15,19))
            # The authored/non-predictive status is part of every title (review P0-01): no "physics simulation" framing.
            title='建筑撞击效果演示 · 可见光 RGB · 作者设定，非物理预测' if a.optical else '建筑撞击效果演示 · 红外 · 作者设定，非物理预测'
            if impact_v1:title=('建筑撞击事件 · 可见光 RGB' if a.optical else '建筑撞击事件 · 红外')+f' · 经验模型估算，未经独立验证 · 慢放 ×{slow:g}'
            stamp=f'{label}  物理 t={time*scale:06.3f}s' if impact_v1 else f'{label}  t={time:05.2f}s'
            draw.text((16,7),title,font=font,fill='white');draw.text((image.width-(330 if impact_v1 else 305),9),stamp,font=small,fill=(218,228,236))
            footer='HiStream GPU 红外/烟场 + CPU 刚体碰撞 · 预设破口、热源与初始运动' if rigid else '原生GPU热/烟示踪 + 背景能量平衡 · 预设碎裂与运动，非结构破坏预测'
            if impact_v1:footer='impact-v1：Young 侵彻 · Kingery–Bulmash 爆炸荷载 · 冲量破坏 · 刚体碎片 · 能量预算火球/热源'
            if a.optical:footer='HiStream GPU 可见光 RGB 合成 · 火焰与烟尘外观预设，非光学传感器标定结果'
            draw.text((15,image.height-24),footer,font=small,fill=(203,215,226));image.save(captioned/f'frame_{n:04d}.png',compress_level=2)
    common=[a.ffmpeg,'-y','-hide_banner','-framerate',str(fps),'-start_number','1']
    videos={}
    for name,folder in [(primary,captioned),('histream_mesh_impact_fixed',run/'fixed_frames')]:
        target=out/(name+'.mp4');checked(common+['-i',str(folder/'frame_%04d.png'),'-frames:v',str(count),*encoder_args(video_codec,18,'medium'),'-pix_fmt','yuv420p','-movflags','+faststart',target],out/(name+'-encode.log'));videos[name]=target
    if a.old_video:
        target=out/('visible_infrared_comparison.mp4' if a.optical else 'old_new_comparison.mp4');checked([a.ffmpeg,'-y','-hide_banner','-i',a.old_video,'-i',videos[primary],'-filter_complex','[0:v]scale=640:360[old];[1:v]scale=640:360[new];[old][new]hstack=inputs=2[v]','-map','[v]','-frames:v',str(count),*encoder_args(video_codec,18),'-pix_fmt','yuv420p','-movflags','+faststart',target],out/'comparison-encode.log');videos['comparison']=target
    video_counts={name:count for name in videos};detail_receipt=None
    if rigid:
        first=max(1,int((impact_presentation-.4)*fps)+1);last=min(count,int((impact_presentation+5)*fps)+1)
        crop_w=max(2,int(m['width']*.625));crop_h=max(2,int(m['height']*.625))
        x=min(int(m['width']*.21875),m['width']-crop_w);y=min(int(m['height']*.3472222222),m['height']-crop_h)
        detail=out/'collision_detail_frames';detail.mkdir();detail_count=last-first+1
        for index,n in enumerate(range(first,last+1),1):
            with Image.open(run/frame_kind/f'frame_{n:04d}.png') as image:
                image=image.convert('RGB').crop((x,y,x+crop_w,y+crop_h)).resize((m['width'],m['height']),Image.Resampling.LANCZOS)
                draw=ImageDraw.Draw(image);draw.rectangle((0,0,image.width,40),fill=(10,15,19));draw.rectangle((0,image.height-27,image.width,image.height),fill=(10,15,19))
                title='可见光撞击细节 · 2 倍慢放 · 裁切放大 · 作者设定' if a.optical else '撞击与落地细节 · 2 倍慢放 · 裁切放大 · 作者设定'
                if impact_v1:title=('可见光撞击细节' if a.optical else '撞击与落地细节')+f' · 共慢放 ×{2*slow:g} · 裁切放大 · 经验模型估算'
                draw.text((16,7),title,font=font,fill='white');draw.text((image.width-210,9),f'物理 t={(n-1)/fps*scale:06.3f}s' if impact_v1 else f't={(n-1)/fps:05.2f}s',font=small,fill='white')
                draw.text((15,image.height-24),'经验事件模型：冲量破坏块按到达时刻释放 · 刚体碰撞' if impact_v1 else '几何接触触发 · 碎片刚体碰撞 · 预设破口与初始运动',font=small,fill=(203,215,226));image.save(detail/f'frame_{index:04d}.png',compress_level=2)
        target=out/('optical_collision_closeup_slow.mp4' if a.optical else 'collision_closeup_slow.mp4')
        checked([a.ffmpeg,'-y','-hide_banner','-framerate',str(fps/2),'-i',detail/'frame_%04d.png','-frames:v',str(detail_count),*encoder_args(video_codec,18,'medium'),'-pix_fmt','yuv420p','-movflags','+faststart',target],out/'collision-detail-encode.log')
        videos['collision_closeup']=target;video_counts['collision_closeup']=detail_count
        detail_receipt=dict(firstNativeFrame=first,lastNativeFrame=last,nativeFps=fps,videoFps=fps/2,slowMotionFactor=2*slow,cropPixels=[x,y,crop_w,crop_h],scope='Crop/enlargement of native frames; no additional spatial or temporal samples')
    decoding={}
    for name,target in videos.items():
        output=checked([a.ffmpeg,'-v','error','-i',target,'-map','0:v:0','-f','null','-','-progress','pipe:1','-nostats'],out/(name+'-decode.log'))
        values={}
        for line in output.splitlines():
            if '=' in line:k,v=line.split('=',1);values[k]=v
        require(int(values.get('frame',0))==video_counts[name] and values.get('progress')=='end','Incomplete video decode')
        decoding[name]=dict(frames=int(values['frame']),durationSeconds=float(values['out_time_us'])/1e6,sha256=sha(target),path=str(target))
    selected=[1,max(1,round((impact_presentation-.15)*fps)+1),round((impact_presentation+.125)*fps)+1,round((impact_presentation+.5)*fps)+1,round((impact_presentation+2)*fps)+1,count]
    selected=[min(count,n) for n in selected]
    sheet=Image.new('RGB',(1920,800),'#10171d');draw=ImageDraw.Draw(sheet)
    for k,n in enumerate(selected):
        x=(k%3)*640;y=(k//3)*400
        with Image.open(run/frame_kind/f'frame_{n:04d}.png') as im:sheet.paste(im.convert('RGB').resize((640,360),Image.Resampling.LANCZOS),(x,y+40))
        draw.text((x+14,y+7),f'物理 t={(n-1)/fps*scale:.3f}s' if impact_v1 else f't={(n-1)/fps:.2f}s',font=font,fill='white')
    sheet.save(out/'stages.jpg',quality=94)
    (out/'verification.json').write_text(json.dumps(dict(status='passed',nativeFrameCount=count,fps=fps,nativeFrameSha256=records,videos=decoding,
        fixedRadianceSeparate=True,presentation='visibleRGB' if a.optical else 'infrared',collisionDetail=detail_receipt,comparisonScope='Qualitative comparison; matching poses must be verified separately',fieldValidation=False,structuralPrediction=False,
        physicsValidated=m.get('physicsValidated',False),impactModel=m.get('impactModel',{'kind':'authored','predictive':False}),videoEncoder=video_codec,
        presentationTimeScale=scale,slowMotionFactor=slow,
        captionLabel=f'经验模型估算 · 未经独立验证 · 慢放 ×{slow:g}' if impact_v1 else '作者设定效果演示 · 非物理预测'),indent=2),encoding='utf-8')
    print(json.dumps(decoding,indent=2))


if __name__=='__main__':main()
