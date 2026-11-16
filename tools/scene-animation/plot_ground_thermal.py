"""Physical temperature maps and same-parameter canopy counterfactual statistics."""
import argparse,json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties


def main():
    p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    folder=a.run/'ground_thermal';m=json.loads((folder/'verification.json').read_text());j=json.loads((folder/'inputs.json').read_text());nx,ny=m['gridPixels'];f=np.fromfile(folder/'cells.f32',dtype='<f4').reshape(ny,nx,20);valid=f[:,:,3]>=0
    fields=[np.where(valid,f[:,:,10]-273.15,np.nan),np.where(valid,f[:,:,16]-273.15,np.nan),np.where(valid,f[:,:,17]-273.15,np.nan),np.where(valid,f[:,:,18],np.nan)]
    low=min(np.nanmin(x) for x in fields[:3]);high=max(np.nanmax(x) for x in fields[:3]);limit=max(.1,float(np.nanmax(np.abs(fields[3]))));origin=j['originMeters'];spacing=j['cellMeters'];extent=[origin[0],origin[0]+nx*spacing,origin[1],origin[1]+ny*spacing]
    font=FontProperties(fname='C:/Windows/Fonts/msyh.ttc');fig,axes=plt.subplots(2,2,figsize=(13,12),constrained_layout=True)
    titles=['原场景地温赋值（含原背景／预设土草温度）','局地模型：不含树冠遮挡','局地模型：包含实际枝叶遮挡','树冠影响：有树 − 无树（同一局地模型）']
    for i,(ax,t,v) in enumerate(zip(axes.flat,titles,fields)):
        image=ax.imshow(v,origin='lower',extent=extent,cmap='coolwarm' if i==3 else 'inferno',vmin=-limit if i==3 else low,vmax=limit if i==3 else high,interpolation='nearest');ax.set_title(t,fontproperties=font,fontsize=11);ax.set_xlabel('x (m)');ax.set_ylabel('y (m)');ax.set_facecolor('#d9dfe1');fig.colorbar(image,ax=ax,shrink=.82,label='K' if i==3 else '°C')
    fig.suptitle('地表能量平衡与植被遮阴 · 合成气象36h / 0.5m格网',fontproperties=font,fontsize=15)
    fig.savefig(a.output/'ground_temperature_maps.png',dpi=170);plt.close(fig)
    results=[];shade=valid&(f[:,:,12]-f[:,:,13]>.1)&(f[:,:,12]>0);sun=valid&(f[:,:,12]>.99)&(f[:,:,13]>.99)
    for mid,name in enumerate(m['materialNames']):
        for label,group in [('current_canopy_shadow',shade),('current_direct_sun',sun)]:
            mask=group&(f[:,:,3]==mid);n=int(mask.sum())
            if not n:continue
            d=f[:,:,18][mask];results.append(dict(material=name,group=label,gridCells=n,projectedAreaM2=n*spacing**2,localWithoutMeanK=float(f[:,:,16][mask].mean()),localWithMeanK=float(f[:,:,17][mask].mean()),pairedMeanDeltaK=float(d.mean()),pairedMedianDeltaK=float(np.median(d)),pairedMinDeltaK=float(d.min()),pairedMaxDeltaK=float(d.max()),positiveDeltaFraction=float((d>0).mean())))
    d=f[:,:,18][shade];receipt=dict(status='passed',gridPixels=[nx,ny],cellMeters=spacing,validCells=int(valid.sum()),currentCanopyShadowCells=int(shade.sum()),shadowAreaM2=int(shade.sum())*spacing**2,currentShadowPairedMeanDeltaK=float(d.mean()) if len(d) else None,currentShadowPairedMinDeltaK=float(d.min()) if len(d) else None,currentShadowPairedMaxDeltaK=float(d.max()) if len(d) else None,byMaterial=results,sampleSupport='spatial grid cells, not independent field samples',attribution='only paired local with/without-canopy difference attributes tree effects; old-vs-local also changes thermal model',limitations=m['scope'],fieldValidation=False)
    (a.output/'shade-statistics.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in receipt.items() if k!='byMaterial'},indent=2))


if __name__=='__main__':main()
