"""Band imaging of native coupled temperatures and broadband irradiance.

The equivalent-blackbody spectrum of local longwave irradiation and the assumed
solar spectrum are approximations. This is not calibrated spectral atmospheric RT.
"""
import numpy as np

SIGMA=5.670374419e-8
BAND_POINTS=17


def energy_record_width(project):
    """Floats per facet side in a native `.facet_steps/energy_T=*.bin` record.

    The native layout is 11 base values plus two state values per spectral band
    (facetrtcore.cpp). A single band is 13 floats; multi-band projects must not be
    read with the old hard-coded 13.
    """
    configuration = project.get('configuration', project) if isinstance(project, dict) else {}
    bands = configuration.get('sensor', {}).get('bands', '10000')
    if isinstance(bands, (list, tuple)):
        count = len(bands)
    else:
        text = str(bands).replace(';', ',')
        count = len([part for part in text.split(',') if part.strip()])
    if count < 1:
        raise ValueError('Native project declares no spectral bands')
    return 11 + 2 * count


def planck(wavelength_um,temperature):
    t=np.asarray(temperature,dtype=float)
    if np.any(t<=0) or not np.isfinite(t).all() or wavelength_um<=0:raise ValueError('Invalid Planck input')
    return 1.191043934e8/(wavelength_um**5*np.expm1(14388.29104/(wavelength_um*t)))


def band(temperature):
    t=np.asarray(temperature,dtype=float);result=np.zeros_like(t)
    for j,wl in enumerate(np.linspace(3,5,BAND_POINTS)):
        result+=(1 if j in (0,16) else 4 if j%2 else 2)*planck(wl,t)
    return result*(.125/3)


def solar_fraction():
    wave=np.linspace(.4,2.5,2101)
    denominator=np.trapezoid(1.191043934e8/(wave**5*np.expm1(14388.29104/(wave*5772))),wave)
    return float(band(5772)/denominator)


def outgoing_band(records,emissivity,shortwave_albedo,sky_k,solar_band_fraction):
    r=np.asarray(records,float);e=np.asarray(emissivity,float)[:,None];rho=1-e
    alpha=np.asarray(shortwave_albedo,float)[:,None]
    if r.ndim!=3 or r.shape[1:]!=(2,13) or np.any(alpha<=0) or np.any((e<0)|(e>1)) or not np.isfinite(r).all():
        raise ValueError('Invalid native energy records/materials')
    f=r[:,:,8];tl=r[:,:,9];ts=r[:,:,10]
    emission=e*(f*band(tl)+(1-f)*band(ts))
    broadband_emission=e*SIGMA*(f*tl**4+(1-f)*ts**4)
    irradiation=np.where(rho>1e-8,(r[:,:,3]-broadband_emission)/np.maximum(rho,1e-8),SIGMA*sky_k**4)
    equivalent_k=(np.maximum(irradiation,0)/SIGMA)**.25
    reflected=rho*band(np.maximum(equivalent_k,1))
    solar=np.maximum(r[:,:,2],0)/alpha*rho*solar_band_fraction/np.pi
    result=emission+reflected+solar
    if not np.isfinite(result).all():raise ValueError('Nonfinite band radiance')
    return result


def observe_radiance(image,low,high,seed,shape=(180,320),sigma=1.2,noise=.004,gain_fpn=.001,frame=0):
    a=np.asarray(image,float)
    if a.ndim!=2 or not np.isfinite(a).all() or high<=low or any(a.shape[i]%shape[i] for i in (0,1)):
        raise ValueError('Invalid camera input')
    if sigma>0:
        radius=int(np.ceil(3*sigma));x=np.arange(-radius,radius+1);weights=np.exp(-.5*(x/sigma)**2);weights/=weights.sum()
        for axis in (0,1):
            pad=[(0,0),(0,0)];pad[axis]=(radius,radius);p=np.pad(a,pad,mode='edge')
            # Difference form preserves DC exactly, including an ADC half-step.
            a=a+sum(w*(np.take(p,np.arange(a.shape[axis])+j,axis=axis)-a) for j,w in enumerate(weights))
    dh,dw=shape;a=a.reshape(dh,a.shape[0]//dh,dw,a.shape[1]//dw).mean(axis=(1,3))
    fixed=np.random.default_rng(seed).normal(1,gain_fpn,shape)
    temporal=np.random.default_rng(np.random.SeedSequence([seed,frame]))
    analog=a*fixed+temporal.normal(0,noise,shape)
    codes=np.rint(np.clip((analog-low)/(high-low),0,1)*4095).astype(np.uint16)
    return dict(codes=codes,radiance=low+codes.astype(float)*(high-low)/4095)


def camera_rays(camera,width,height):
    forward=np.asarray(camera['target'],float)-camera['position'];forward/=np.linalg.norm(forward)
    right=np.cross(forward,np.asarray(camera['up'],float));right/=np.linalg.norm(right);up=np.cross(right,forward)
    yy,xx=np.mgrid[:height,:width];u=((xx+.5)/width*2-1)*width/height;v=1-(yy+.5)/height*2
    ray=forward+np.tan(np.deg2rad(camera['fovYDegrees']/2))*(u[...,None]*right+v[...,None]*up)
    return ray/np.linalg.norm(ray,axis=-1,keepdims=True)
