#pragma once
// STREAM observation core 1.0.0. Double precision, K/degree/um/GHz.
// NumPy backend: kernel.py + microwave.py; verified with identical test vectors.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace stream_core {
constexpr double pi = 3.14159265358979323846, rd = pi / 180;
constexpr double c1 = 119104393.40652, c2 = 14388.291040407;
constexpr double soil_emissivity = .96, leaf_emissivity = .975;
inline double bound(double x, double lo, double hi) {
    if (!std::isfinite(x) || x < lo || x > hi) throw std::invalid_argument("STREAM core: invalid physical input");
    return x;
}
inline double rel(double x) { return std::abs(x) <= 1e-10 ? 1 : -std::expm1(-x) / x; }
inline double planck(double t, double w) {
    bound(t, 1, 10000); bound(w, .1, 1000000);
    return c1 / std::pow(w, 5) / std::expm1(std::min(c2 / (w * t), 700.0));
}
inline double inverse_planck(double r, double w) {
    bound(r, 1e-300, 1e100); bound(w, .1, 1000000);
    return c2 / (w * std::log1p(c1 / (r * std::pow(w, 5))));
}
struct Thermal {
    double radiance, brightness_temperature, surface_temperature, effective_emissivity;
    std::array<double, 4> fractions{}, weights{};
};
inline Thermal thermal(const std::array<double, 4>& temperatures, double lai,
                       double vza, double sza, double raa, double w=10.5,
                       double es=soil_emissivity, double el=leaf_emissivity,
                       double hotspot=.2, double width=0,
                       double ls=-1, double lv=-1, double lh=-1) {
    bound(lai,0,100); bound(vza,0,89.9); bound(sza,0,180); bound(raa,-360,360);
    bound(es,.001,1); bound(el,.001,1); bound(hotspot,.0001,10);
    bound(w,.1,100); bound(width,0,2*(w-.1));
    ls = ls == -1 ? lai : bound(ls,0,100);
    lv = lv == -1 ? lai : bound(lv,0,100);
    lh = lh == -1 ? lai : bound(lh,0,100);
    const double ms=std::max(std::cos(std::min(sza,89.0)*rd),.01), mv=std::max(std::cos(vza*rd),.01);
    const double ks=.5/ms, kv=.5/mv, as=ks*ls, av=kv*lv;
    const double bs=std::exp(-as), bv=std::exp(-av);
    const double phase=ms*mv+std::sin(std::min(sza,89.0)*rd)*std::sin(vza*rd)*std::cos(raa*rd);
    const double delta=std::sqrt(std::max(1/(ms*ms)+1/(mv*mv)-2*phase/(ms*mv),0.0));
    const double overlap=std::sqrt(as*av)*rel(delta/(hotspot*(ks+kv)/2));
    const double index=std::clamp(.83-.04*std::floor(lai/.5),0.0,1.0);
    const double hu_s=as>1e-12 ? -std::log1p(-(1-bs)*index)/as : 0;
    const double hu_v=av>1e-12 ? -std::log1p(-(1-bv)*index)/av : 0;
    const double hu=std::sqrt(hu_s*hu_v);
    const double ss=sza<90 ? std::min(std::exp(-(as+av-overlap)),bv) : 0;
    const double cs=sza<90 ? std::clamp(-std::expm1(-av*hu)-std::expm1(-overlap*(1-hu))*std::exp(-(as+av-overlap)*hu),0.0,1-bv) : 0;
    Thermal result{};
    result.fractions={ss,bv-ss,cs,1-bv-cs};
    constexpr double alpha[]={.2885375,.2885375,.2964427,.3003953,.3083004,.3201581,.3399209,.3715415,.4189723,1};
    const double m=std::exp(-.825*lh);
    const double scatter=(bv*(1-m)*(1-es)+(1-alpha[std::min(int(vza/10),9)])*(1-bv*m)*(1-bv)*(1-el))*el;
    const double sun=sza<90 ? rel(as) : 0;
    for(int k=0;k<4;++k) result.weights[k]=result.fractions[k]*(k<2 ? es : el);
    result.weights[2]+=scatter*sun; result.weights[3]+=scatter*(1-sun);
    double sum=0; for(double value:result.weights) sum+=value;
    for(double& value:result.weights) value/=std::max(sum,1.0);
    result.effective_emissivity=std::min(sum,1.0);
    const std::array<double,3> wavelengths={w-width/2,w,w+width/2}, q={1.0/6,4.0/6,1.0/6};
    result.radiance=0;
    for(int band=0;band<3;++band) for(int k=0;k<4;++k) {
        const double t=result.weights[k]>1e-14 ? temperatures[k] : 300;
        result.radiance+=q[band]*result.weights[k]*planck(t,wavelengths[band]);
    }
    const auto inverse=[&](double r) {
        if(width==0) return inverse_planck(r,w);
        double lo=1,hi=10000;
        for(int k=0;k<52;++k) {
            const double mid=(lo+hi)/2;
            double estimate=0;
            for(int b=0;b<3;++b) estimate+=q[b]*planck(mid,wavelengths[b]);
            if(estimate<r) lo=mid; else hi=mid;
        }
        return (lo+hi)/2;
    };
    result.brightness_temperature=inverse(result.radiance);
    result.surface_temperature=inverse(result.radiance/result.effective_emissivity);
    return result;
}
struct Optical { double reflectance, leaf_component, soil_component; };
inline Optical optical(double lai,double sza,double vza,double raa,double lr,double lt,double sr) {
    bound(lai,0,100); bound(sza,0,180); bound(vza,0,89.9); bound(raa,-360,360);
    bound(lr,0,1); bound(lt,0,1); bound(sr,0,1); bound(lr+lt,0,1+1e-9);
    if(sza>=90) return {0,0,0};
    const double ms=std::max(std::cos(sza*rd),.05), mv=std::max(std::cos(vza*rd),.05);
    const double phase=std::clamp(ms*mv+std::sin(sza*rd)*std::sin(vza*rd)*std::cos(raa*rd),-1.0,1.0);
    const double gs=std::exp(-.5*lai/ms),gv=std::exp(-.5*lai/mv);
    const double joint=gs*gv+std::exp(-std::acos(phase)/.2)*(std::min(gs,gv)-gs*gv);
    const double backward=.5*(1+phase);
    double leaf=(1-joint)*(lr*(.65+.35*backward)+lt*(.2+.3*(1-backward))+.12*(lr+lt)*(lr+lt)*(1-std::exp(-.7*lai)));
    double soil=sr*joint,scale=std::max(leaf+soil,1.0);
    leaf/=scale; soil/=scale;
    return {leaf+soil,leaf,soil};
}
inline double qhn_q(double f) {
    bound(f,.001,1000);
    if(std::abs(f-1.41)<.5) return .0327;
    if(std::abs(f-6.925)<.5) return .2783;
    if(std::abs(f-10.65)<.5) return .3143;
    throw std::invalid_argument("STREAM core: unsupported microwave frequency");
}
inline std::complex<double> mironov(double f,double sm,double clay) {
    bound(f,.001,1000); bound(sm,0,.8); bound(clay,0,1);
    const double hz=f*1e9, nd=1.634-.539*clay+.2748*clay*clay, kd=.03952-.04038*clay;
    const double transition=.02863+.30673*clay;
    const auto water=[&](double e0,double tau,double sigma) {
        const double omega=2*pi*hz*tau,delta=(e0-4.9)/(1+omega*omega);
        const double re=4.9+delta,im=delta*omega+sigma/(2*pi*8.854e-12*hz),mag=std::hypot(re,im);
        return std::array<double,2>{std::sqrt((mag+re)/2),std::sqrt((mag-re)/2)};
    };
    const auto b=water(79.8-85.4*clay+32.7*clay*clay,1.062e-11+3.450e-12*clay,.3112+.467*clay);
    const auto u=water(100,8.5e-12,.3631+1.217*clay);
    const double bw=std::min(sm,transition),fw=std::max(sm-transition,0.0);
    const double n=nd+(b[0]-1)*bw+(u[0]-1)*fw,k=kd+b[1]*bw+u[1]*fw;
    return {n*n-k*k,2*n*k};
}
struct Microwave { double brightness_temperature_h_k,brightness_temperature_v_k,surface_emissivity_h,surface_emissivity_v; };
inline Microwave microwave(double sm,double ts,double tc,double lai,double angle,
                           double f=1.41,double clay=.3,double rms=20,double correlation=200,
                           double omega=.05,double b=.1,double vwc_per_lai=.5,double max_vwc=5) {
    bound(sm,0,.8); bound(ts,100,400); bound(lai,0,100); bound(angle,0,70);
    if(lai==0) tc=ts; else bound(tc,100,400);
    bound(clay,0,1); bound(rms,0,10000); bound(correlation,.001,1000000);
    bound(omega,0,1); bound(b,0,100); bound(vwc_per_lai,0,100); bound(max_vwc,0,1000);
    const double q=qhn_q(f),mu=std::cos(angle*rd);
    const auto root=std::sqrt(mironov(f,sm,clay));
    const auto sin_t=std::sin(angle*rd)/root,cos_t=std::sqrt(1.0-sin_t*sin_t);
    const double rh=std::norm((mu-root*cos_t)/(mu+root*cos_t));
    const double rv=std::norm((cos_t-root*mu)/(cos_t+root*mu));
    const double a=std::exp(-1.3972*std::pow(rms/correlation,.5879)*mu*mu);
    const double eh=std::clamp(1-((1-q)*rh+q*rv)*a,0.0,1.0),ev=std::clamp(1-((1-q)*rv+q*rh)*a,0.0,1.0);
    const double gamma=std::exp(-b*std::min(lai*vwc_per_lai,max_vwc)/mu),ve=(1-omega)*(1-gamma);
    const auto tb=[&](double e){return (ve+ve*gamma*(1-e))*tc+e*gamma*ts;};
    return {tb(eh),tb(ev),eh,ev};
}
} // namespace stream_core
