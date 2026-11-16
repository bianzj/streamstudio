"""Assemble verified native runs and labeled geometry inspection views."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

from PIL import Image,ImageDraw,ImageFont
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def font(size):
    return ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',size)


def main():
    p=argparse.ArgumentParser()
    p.add_argument('case',type=Path)
    p.add_argument('--ffmpeg',required=True,type=Path)
    add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    runs=[a.case/'baseline',a.case/'final_result']
    audits=[json.loads((r/'verification.json').read_text(encoding='utf-8')) for r in runs]
    assert all(x['status']=='passed' for x in audits)
    assert audits[0]['fps']==audits[1]['fps'] and audits[0]['videoFramesDecoded']==audits[1]['videoFramesDecoded']
    for key in ['width','height','impactTime','geometry','thermal']:
        if key!='thermal':assert audits[0]['nativeGpuManifest'][key]==audits[1]['nativeGpuManifest'][key]
    for key in ['bandMode','displayMinK','displayMaxK','bandMinMicrometers','bandMaxMicrometers']:
        assert audits[0]['nativeGpuManifest']['thermal'][key]==audits[1]['nativeGpuManifest']['thermal'][key]
    count=audits[0]['videoFramesDecoded'];fps=audits[0]['fps']
    frames=a.case/'comparison_frames';frames.mkdir(exist_ok=False)
    stills=[]
    for i in range(1,count+1):
        canvas=Image.new('RGB',(1280,424),(13,13,13));d=ImageDraw.Draw(canvas)
        d.text((18,5),'预设烟热场',font=font(22),fill='white')
        d.text((658,5),'GPU 输运与表面冷却',font=font(22),fill='white')
        for j,run in enumerate(runs):
            with Image.open(run/'video_frames'/f'frame_{i:04d}.png') as image:
                canvas.paste(image.resize((640,360),Image.Resampling.LANCZOS),(j*640,36))
        d.text((18,400),'相同几何、相机、波段和显示范围；温度来源不同。本图为视觉对比，不代表精度验证。',font=font(14),fill=(180,180,180))
        canvas.save(frames/f'frame_{i:04d}.png')
        if i in [85,253]:stills.append(canvas.copy())
    sheet=Image.new('RGB',(1280,424*len(stills)))
    for i,im in enumerate(stills):sheet.paste(im,(0,424*i))
    sheet.save(a.case/'comparison.jpg',quality=94)
    video=a.case/'comparison.mp4'
    r=subprocess.run([str(a.ffmpeg),'-hide_banner','-y','-framerate',str(fps),'-i',str(frames/'frame_%04d.png'),
        *encoder_args(video_codec,18,'medium'),'-pix_fmt','yuv420p','-movflags','+faststart',str(video)],capture_output=True)
    (a.case/'comparison-encode.log').write_bytes(r.stderr);assert r.returncode==0,r.stderr
    r=subprocess.run([str(a.ffmpeg),'-hide_banner','-v','error','-i',str(video),'-progress','pipe:1','-f','null','-'],capture_output=True)
    (a.case/'comparison-decode.log').write_bytes(r.stdout+r.stderr)
    decoded=[int(x) for x in re.findall(rb'^frame=(\d+)',r.stdout,re.M)]
    assert r.returncode==0 and decoded[-1]==count,(r.returncode,decoded)
    structure=Image.new('RGB',(1280,924),(15,15,15));d=ImageDraw.Draw(structure)
    for j,(name,frame,label) in enumerate([('structure_before',1,'撞击前'),('structure_after',288,'预设破口与局部构件缺损')]):
        d.text((j*640+18,8),label,font=font(22),fill='white')
        with Image.open(a.case/name/'frames'/f'frame_{frame:04d}.png') as image:
            structure.paste(image.convert('RGB').resize((640,360),Image.Resampling.LANCZOS),(j*640,42))
            structure.paste(image.crop((530,290,790,495)).convert('RGB').resize((640,505),Image.Resampling.LANCZOS),(j*640,402))
    d=ImageDraw.Draw(structure)
    d.text((18,906),'结构查看图：隐藏体积介质以便观察几何；未求解结构应力、承重失效或倒塌。',font=font(13),fill=(190,190,190))
    structure.save(a.case/'structure_comparison.jpg',quality=94)
    report=dict(status='passed',decodedComparisonFrames=decoded[-1],fps=fps,sourceRuns=[str(x) for x in runs],
        comparisonSha256=hashlib.sha256(video.read_bytes()).hexdigest(),scientificValidation=False)
    (a.case/'comparison-verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))


if __name__=='__main__':main()
