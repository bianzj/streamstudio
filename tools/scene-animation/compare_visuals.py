"""Compare native scene versions visually; does not assert physical accuracy."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont,ImageOps
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def font(size):return ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',size)


def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('previous',type=Path);p.add_argument('--ffmpeg',required=True,type=Path);add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    old=a.previous/'final_result';new=a.case/'result'
    audits=[json.loads((r/'verification.json').read_text(encoding='utf-8')) for r in [old,new]]
    assert all(v['status']=='passed' and v['videoFramesDecoded']==288 and v['fps']==24 for v in audits)
    frames=a.case/'comparison_frames';frames.mkdir(exist_ok=False);stills=[]
    for n in range(1,289):
        canvas=Image.new('RGB',(1280,424),(15,15,15));d=ImageDraw.Draw(canvas)
        d.text((16,5),'上一版：简化街区 / 固定显示',font=font(21),fill='white')
        d.text((656,5),'本版：场景细节 / 通用相机成像',font=font(21),fill='white')
        for j,r in enumerate([old,new]):
            with Image.open(r/'frames'/f'frame_{n:04d}.png') as image:
                canvas.paste(image.convert('RGB').resize((640,360),Image.Resampling.LANCZOS),(j*640,36))
        d.text((16,400),f'T + {(n-1)/24:05.2f} s  |  场景、视角与显示链条均有调整；仅对比视觉表现，不代表精度验证。',font=font(14),fill=(190,190,190))
        canvas.save(frames/f'frame_{n:04d}.png')
        if n in [37,85,253]:stills.append(canvas.copy())
    sheet=Image.new('RGB',(1280,424*len(stills)))
    for i,im in enumerate(stills):sheet.paste(im,(0,424*i))
    sheet.save(a.case/'visual_comparison.jpg',quality=94)
    video=a.case/'visual_comparison.mp4'
    r=subprocess.run([str(a.ffmpeg),'-hide_banner','-y','-framerate','24','-i',str(frames/'frame_%04d.png'),
        *encoder_args(video_codec,18,'medium'),'-pix_fmt','yuv420p','-movflags','+faststart',str(video)],capture_output=True)
    (a.case/'comparison-encode.log').write_bytes(r.stderr);assert r.returncode==0,r.stderr
    r=subprocess.run([str(a.ffmpeg),'-hide_banner','-v','error','-i',str(video),'-progress','pipe:1','-f','null','-'],capture_output=True)
    (a.case/'comparison-decode.log').write_bytes(r.stdout+r.stderr)
    decoded=[int(v) for v in re.findall(rb'^frame=(\d+)',r.stdout,re.M)]
    assert r.returncode==0 and decoded[-1]==288
    geometry=Image.new('RGB',(1280,936),(15,15,15));d=ImageDraw.Draw(geometry)
    for j,(root,crop,label) in enumerate([(a.previous,(530,290,790,495),'上一版破口'),(a.case,(420,280,700,515),'本版破口与材质（隐藏烟雾）')]):
        d.text((j*640+16,6),label,font=font(21),fill='white')
        with Image.open(root/'structure_after'/'frames'/'frame_0288.png') as image:
            geometry.paste(image.convert('RGB').resize((640,360),Image.Resampling.LANCZOS),(j*640,36))
            detail=ImageOps.contain(image.crop(crop).convert('RGB'),(640,510),Image.Resampling.LANCZOS)
            geometry.paste(detail,(j*640+(640-detail.width)//2,396+(510-detail.height)//2))
    d.text((16,912),'几何查看图：破损为预设资产；没有计算结构应力、承重失效或倒塌。',font=font(14),fill=(190,190,190))
    geometry.save(a.case/'geometry_comparison.jpg',quality=94)
    report={'status':'passed','decodedVideoFrames':288,'durationSeconds':12,'fps':24,
        'comparisonSha256':hashlib.sha256(video.read_bytes()).hexdigest(),
        'previousRun':str(old),'newRun':str(new),'scope':'visual comparison only; scene/camera/display differ','physicalValidation':False}
    (a.case/'visual-comparison-verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report,ensure_ascii=False))


if __name__=='__main__':main()
