"""Package valid native body observations and a common-time observer comparison."""
import argparse,csv,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from package_mesh_sequence import checked
from prepare_thermal_case import require,sha
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('--ffmpeg',type=Path,required=True);add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    case=a.case.resolve();config=read(case/'case.json');ref=Path(config['reference']);run=case/'result_final';m=read(run/'run-manifest.json');out=case/'video'
    require(m['status']=='complete' and m['firstFrame']==1 and m['lastFrame']==config['validFrames'],'Incomplete native body camera')
    require(m['frameCount']==config['validFrames'] and m['fps']==config['fps'],'Body camera time mismatch')
    count,fps,total=config['validFrames'],config['fps'],config['frameCount'];require(not out.exists(),'Use a new video directory')
    native={}
    for folder in ['frames','fixed_frames','optical_frames','observed_ir','observed_optical','detector_ir']:
        require(len(list((run/folder).glob('*.png')))==count,'Body frame count mismatch')
        for n in range(1,count+1):
            f=run/folder/f'frame_{n:04d}.png'
            with Image.open(f) as im:
                expected=tuple(m['cameraObservation']['ir']['pixels']) if folder=='detector_ir' else (m['width'],m['height']);require(im.size==expected,'Body image size')
                if folder=='detector_ir':require(np.asarray(im).max()<=16383,'Body IR ADC range')
                else:im.verify()
            native[str(f.relative_to(run))]=sha(f)
    out.mkdir();sheets=out/'sync_frames';sheets.mkdir();font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',20);small=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',16)
    sync=[]
    for n in range(1,total+1):
        t=(n-1)/fps;valid=t<m['impactTime'] and n<=count
        sheet=Image.new('RGB',(1280,800),'#10171d');draw=ImageDraw.Draw(sheet)
        panels=[(ref/'result_final/observed_optical','外部相机 · RGB'),(run/'observed_optical','弹体前视 · RGB'),(ref/'result_final/observed_ir','外部相机 · IR'),(run/'observed_ir','弹体前视 · IR 384×216 / 14 bit / 2%拉伸')]
        for i,(folder,label) in enumerate(panels):
            x,y=i%2*640,i//2*400;draw.text((x+12,y+8),label,font=font,fill='white')
            if i%2==0 or valid:
                with Image.open(folder/f'frame_{n:04d}.png') as im:sheet.paste(im.convert('RGB').resize((640,360),Image.Resampling.LANCZOS),(x,y+40))
            else:
                draw.rectangle((x,y+40,x+639,y+399),fill='#18222a');draw.text((x+90,y+170),'信号中断 · 首次接触后无有效观测',font=font,fill='#d6b48d');draw.text((x+90,y+210),f'接触时刻 {m["impactTime"]:.4f} s',font=small,fill='#a9bbc7')
        draw.text((1110,9),f't={t:.3f}s',font=small,fill='#d1e2e8');sheet.save(sheets/f'frame_{n:04d}.png',compress_level=2)
        sync.append(dict(frame=n,presentationTimeSeconds=t,bodyObservationValid=valid,bodyFrame=n if valid else None))
    (out/'synchronization.json').write_text(json.dumps(sync,indent=2),encoding='utf-8')
    videos={}
    for name,folder,frames in [('onboard_rgb',run/'observed_optical',count),('onboard_ir',run/'observed_ir',count),('synchronized',sheets,total)]:
        target=out/(name+'.mp4')
        checked([a.ffmpeg,'-y','-v','error','-framerate',fps,'-start_number',1,'-i',folder/'frame_%04d.png','-frames:v',frames,*encoder_args(video_codec,17,'medium'),'-pix_fmt','yuv420p','-movflags','+faststart',target],out/(name+'-encode.log'))
        decoded=checked([a.ffmpeg,'-v','error','-i',target,'-map','0:v:0','-f','null','-','-progress','pipe:1','-nostats'],out/(name+'-decode.log'))
        fields=dict(x.split('=',1) for x in decoded.splitlines() if '=' in x)
        require(fields.get('progress')=='end' and int(fields.get('frame',0))==frames,'Video decoding incomplete')
        require(abs(float(fields['out_time_us'])/1e6-frames/fps)<1/fps,'Video duration mismatch')
        videos[name]=dict(path=str(target),sha256=sha(target),decodedFrames=frames,seconds=frames/fps)
    for n in [1,min(25,count),min(49,count),count]:
        with Image.open(sheets/f'frame_{n:04d}.png') as im:im.save(out/f'preview_{n:04d}.png')
    receipt=dict(status='passed',videos=videos,nativeFrameSha256=native,validObservationFrames=count,totalTimelineFrames=total,firstContactSeconds=m['impactTime'],postContactFramesAreSignalLossCards=True,scope='Authored fixed-orientation body and generic camera demonstration; no device or flight-model validation')
    (out/'verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    (case/'index.html').write_text('''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>弹体同步摄像头</title><style>body{max-width:1280px;margin:24px auto;padding:12px;background:#10171d;color:#e5edf2;font:16px/1.7 system-ui}video,img{width:100%;display:block;margin:16px 0}a{color:#9ddaff}</style><h1>外部与弹体前视 · 同步相机</h1><p>沿用原运动与场景，镜头挂载在弹体前端，逐曝光采样同步位置和固定朝向。撞击后显示信号中断，未生成存活弹载观测。</p><video controls src="video/synchronized.mp4" poster="video/preview_0025.png"></video><h2>有效弹载红外</h2><video controls src="video/onboard_ir.mp4"></video><h2>有效弹载光学</h2><video controls src="video/onboard_rgb.mp4"></video><p>光学清晰；红外原生384×216/14位ADC，轻度模糊、2%显示拉伸。原轨迹为预设直线与固定姿态，未增加制导或六自由度动力学。</p><p><a href="README.md">说明</a> · <a href="sequence.json">配置</a> · <a href="result_final/camera.csv">采样位姿</a> · <a href="final-verification.json">核验</a></p></html>''',encoding='utf-8')
    print(json.dumps(videos,indent=2))
if __name__=='__main__':main()
