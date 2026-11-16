"""Compare matching old/new native views and fully decode the comparison video."""
import argparse,json
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
from package_mesh_sequence import checked
from prepare_thermal_case import require,sha
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('case',type=Path);p.add_argument('--ffmpeg',type=Path,required=True)
    p.add_argument('--left-label',default='旧版 · 团块树冠、规则火焰');p.add_argument('--right-label',default='新版 · 叶片树冠、不规则火舌')
    p.add_argument('--scope',default='Qualitative appearance comparison; identical existing rigid timeline, different tree geometry and4versus1scene samples per pixel; no physical calibration');add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    old,new=a.reference.resolve(),a.case.resolve();out=new/'comparison';out.mkdir(exist_ok=False)
    left=json.loads((old/'result_final/run-manifest.json').read_text());right=json.loads((new/'result_final/run-manifest.json').read_text())
    require(left['timeline']==right['timeline'] and left['fps']==right['fps'] and left['frameCount']==right['frameCount'],'Comparison poses/timing differ')
    count,fps=right['frameCount'],right['fps'];video=out/'old_new_rgb.mp4'
    checked([a.ffmpeg,'-y','-hide_banner','-i',old/'video/observed_rgb.mp4','-i',new/'video/observed_rgb.mp4','-filter_complex','[0:v]setsar=1[a];[1:v]setsar=1[b];[a][b]hstack=inputs=2[v]','-map','[v]','-frames:v',count,*encoder_args(video_codec,17),'-pix_fmt','yuv420p','-movflags','+faststart',video],out/'encode.log')
    decoded=checked([a.ffmpeg,'-v','error','-i',video,'-map','0:v:0','-f','null','-','-progress','pipe:1','-nostats'],out/'decode.log');fields=dict(line.split('=',1) for line in decoded.splitlines() if '=' in line)
    require(fields.get('progress')=='end' and int(fields.get('frame',0))==count,'Comparison decode incomplete')
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',25);frame=193
    for kind,name in [('observed_optical','old_new_observed.png'),('optical_frames','old_new_reference.png')]:
        sheet=Image.new('RGB',(right['width']*2,right['height']+48),'#10171d');draw=ImageDraw.Draw(sheet)
        for index,root,label in [(0,old,a.left_label),(1,new,a.right_label)]:
            with Image.open(root/'result_final'/kind/f'frame_{frame:04d}.png') as im:sheet.paste(im.convert('RGB'),(index*right['width'],48))
            draw.text((index*right['width']+18,10),label,font=font,fill='white')
        sheet.save(out/name)
    receipt=dict(status='passed',frames=count,seconds=count/fps,path=str(video),sha256=sha(video),left=str(old),right=right.get('appearanceVersion','legacy'),scope=a.scope)
    (out/'verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
