// ==========================================================================
// 文件：stream_core.hpp
// 功能：StreamEarth 的 STREAM 观测核心（热红外 + 光学 + 微波）的 header-only 孤立副本，仅供参考。
//       本引擎并不包含或调用它（见下方原有说明）；Studio 的观测算子是 app/thermal_view.cpp +
//       physics/canopy.cpp + physics/radiosity.cpp。
// 主要模块：
//   1. 常数与输入检查：pi、Planck 常数 c1/c2、缺省发射率；bound() 越界抛 std::invalid_argument
//   2. planck / inverse_planck：黑体辐亮度与反演亮温（波长 μm，辐亮度 W m-2 sr-1 μm-1）
//   3. thermal()：四组分（光照土壤/阴影土壤/光照叶/阴影叶）面积份额、多次散射项、辐亮度与亮温
//   4. optical()：经验的冠层光学反射率（叶片分量 + 土壤分量）
//   5. mironov / microwave()：Mironov 土壤介电常数、粗糙面反射、tau-omega 植被的 L 波段亮温
// 注意：该核心与 TiRT 标准闭合不同（两层指数、阶梯式 alpha 表、无反射天空），不得当作 Studio 反演算子。
// 约定：温度 K，角度度，波长 μm，频率 GHz；所有对外函数先 bound() 检查物理范围。
// ==========================================================================

#pragma once
// FIX(2026-11-21 S-8): ORPHAN COPY — NOT USED BY THIS ENGINE. Nothing in models/stream includes this header (checked
// 2026-11-21); it is a byte-identical copy of StreamEarth observation/stream_core/stream_core.hpp kept for reference.
// The Studio thermal / optical observation operator is app/thermal_view.cpp + physics/canopy.cpp + physics/radiosity.cpp
// (TiRT standard closure). This core differs from TiRT (two-layer index 0.83 - 0.04 floor(LAI/0.5), step-table FR97
// alpha, no reflected sky; report C_inversion_obs.md E-2) and must not be used as a Studio retrieval operator.
// STREAM observation core 1.0.0. Double precision, K/degree/um/GHz.
// NumPy backend: kernel.py + microwave.py; verified with identical test vectors.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <stdexcept>

// 命名空间 stream_core：与引擎的 stream 命名空间互不相干，避免误用。
namespace stream_core {
// 常数：rd 度→弧度；c1、c2 为 Planck 辐射常数（波长取 μm，辐亮度 W m-2 sr-1 μm-1，c2 单位 μm K）；
//       soil_emissivity / leaf_emissivity 为缺省宽带发射率。
constexpr double pi = 3.14159265358979323846, rd = pi / 180;
constexpr double c1 = 119104393.40652, c2 = 14388.291040407;
constexpr double soil_emissivity = .96, leaf_emissivity = .975;
// 输入检查：x 非有限或超出 [lo,hi] 时抛异常，否则原样返回。
inline double bound(double x, double lo, double hi) {
    if (!std::isfinite(x) || x < lo || x > hi) throw std::invalid_argument("STREAM core: invalid physical input");
    return x;
}
// 辅助：(1 - e^-x)/x，x → 0 时取极限 1，用于冠层截获/平均透过率。
inline double rel(double x) { return std::abs(x) <= 1e-10 ? 1 : -std::expm1(-x) / x; }
// Planck 黑体辐亮度 B(T, λ)：t 为温度（K），w 为波长（μm）；指数项上限 700 以防溢出。
// inverse_planck 为其反函数（辐亮度 → 亮温）。
inline double planck(double t, double w) {
    bound(t, 1, 10000); bound(w, .1, 1000000);
    return c1 / std::pow(w, 5) / std::expm1(std::min(c2 / (w * t), 700.0));
}
inline double inverse_planck(double r, double w) {
    bound(r, 1e-300, 1e100); bound(w, .1, 1000000);
    return c2 / (w * std::log1p(c1 / (r * std::pow(w, 5))));
}
// thermal() 的结果：radiance 辐亮度、brightness_temperature 亮温、surface_temperature（辐亮度/有效发射率的反演温度，
// 不含反射天空）、effective_emissivity 有效发射率、fractions 四组分面积份额、weights 发射权重。
struct Thermal {
    double radiance, brightness_temperature, surface_temperature, effective_emissivity;
    std::array<double, 4> fractions{}, weights{};
};
// 作用：冠层热红外方向辐亮度/亮温。组分顺序 [光照土壤, 阴影土壤, 光照叶片, 阴影叶片]。
// 输入：四组分温度（K）、LAI、观测天顶角 vza、太阳天顶角 sza、相对方位 raa（度）、波长 w 与波宽 width（μm）、
//       土壤/叶片发射率 es/el、热点参数 hotspot；ls/lv/lh 为分别用于太阳路径、观测路径、半球散射项的 LAI（缺省 = lai，含义待核对）。
// 步骤 1：输入范围检查；2：几何（余弦、光学厚度、间隙率、热点重叠）；3：两层指数 index；
//       4：四组分面积份额；5：多次散射项 scatter；6：发射权重；7：三点波长积分；8：反演亮温与表面温度。
inline Thermal thermal(const std::array<double, 4>& temperatures, double lai,
                       double vza, double sza, double raa, double w=10.5,
                       double es=soil_emissivity, double el=leaf_emissivity,
                       double hotspot=.2, double width=0,
                       double ls=-1, double lv=-1, double lh=-1) {
    // 步骤 1：物理范围检查（越界直接抛异常，不做静默限幅）。
    bound(lai,0,100); bound(vza,0,89.9); bound(sza,0,180); bound(raa,-360,360);
    bound(es,.001,1); bound(el,.001,1); bound(hotspot,.0001,10);
    bound(w,.1,100); bound(width,0,2*(w-.1));
    ls = ls == -1 ? lai : bound(ls,0,100);
    lv = lv == -1 ? lai : bound(lv,0,100);
    lh = lh == -1 ? lai : bound(lh,0,100);
    // 步骤 2：ms/mv 太阳/观测方向余弦（下限 0.01，太阳天顶角上限 89°）；ks/kv = G/cos（G = 0.5）；
    //         as/av 为光学厚度，bs/bv 为太阳/观测方向间隙率；delta 为热点角距，overlap 为太阳-观测路径重叠项。
    const double ms=std::max(std::cos(std::min(sza,89.0)*rd),.01), mv=std::max(std::cos(vza*rd),.01);
    const double ks=.5/ms, kv=.5/mv, as=ks*ls, av=kv*lv;
    const double bs=std::exp(-as), bv=std::exp(-av);
    const double phase=ms*mv+std::sin(std::min(sza,89.0)*rd)*std::sin(vza*rd)*std::cos(raa*rd);
    const double delta=std::sqrt(std::max(1/(ms*ms)+1/(mv*mv)-2*phase/(ms*mv),0.0));
    const double overlap=std::sqrt(as*av)*rel(delta/(hotspot*(ks+kv)/2));
    // 步骤 3：两层近似指数，随 LAI 按 0.5 一档递减（TiRT 标准闭合不用此式）；hu 为由此得到的热点有效份额（待核对）。
    const double index=std::clamp(.83-.04*std::floor(lai/.5),0.0,1.0);
    const double hu_s=as>1e-12 ? -std::log1p(-(1-bs)*index)/as : 0;
    const double hu_v=av>1e-12 ? -std::log1p(-(1-bv)*index)/av : 0;
    const double hu=std::sqrt(hu_s*hu_v);
    // 步骤 4：光照土壤 ss（受间隙率 bv 限制）、光照叶 cs；太阳在地平线以下时二者为 0。
    //         fractions = {ss, bv-ss, cs, 1-bv-cs}，总和为 1。
    const double ss=sza<90 ? std::min(std::exp(-(as+av-overlap)),bv) : 0;
    const double cs=sza<90 ? std::clamp(-std::expm1(-av*hu)-std::expm1(-overlap*(1-hu))*std::exp(-(as+av-overlap)*hu),0.0,1-bv) : 0;
    Thermal result{};
    result.fractions={ss,bv-ss,cs,1-bv-cs};
    // 步骤 5：多次散射项 scatter：按观测天顶角（每 10° 一档）查 alpha 表（FR97 阶梯表，含义待核对），
    //         按土壤与叶片反射率 (1-ε) 计入冠层内部多次散射；其中光照部分按 sun = rel(as) 分配。
    constexpr double alpha[]={.2885375,.2885375,.2964427,.3003953,.3083004,.3201581,.3399209,.3715415,.4189723,1};
    const double m=std::exp(-.825*lh);
    const double scatter=(bv*(1-m)*(1-es)+(1-alpha[std::min(int(vza/10),9)])*(1-bv*m)*(1-bv)*(1-el))*el;
    const double sun=sza<90 ? rel(as) : 0;
    // 步骤 6：发射权重 = 面积份额 × 发射率 + 散射项；权重和超过 1 时整体归一，有效发射率 = min(权重和,1)。
    for(int k=0;k<4;++k) result.weights[k]=result.fractions[k]*(k<2 ? es : el);
    result.weights[2]+=scatter*sun; result.weights[3]+=scatter*(1-sun);
    double sum=0; for(double value:result.weights) sum+=value;
    for(double& value:result.weights) value/=std::max(sum,1.0);
    result.effective_emissivity=std::min(sum,1.0);
    // 步骤 7：用 w±width/2 三点（权重 1/6, 4/6, 1/6）近似波段积分；权重为 0 的组分温度以 300 K 占位，不影响结果。
    const std::array<double,3> wavelengths={w-width/2,w,w+width/2}, q={1.0/6,4.0/6,1.0/6};
    result.radiance=0;
    for(int band=0;band<3;++band) for(int k=0;k<4;++k) {
        const double t=result.weights[k]>1e-14 ? temperatures[k] : 300;
        result.radiance+=q[band]*result.weights[k]*planck(t,wavelengths[band]);
    }
    // 步骤 8：亮温反演。width = 0 用解析反函数；否则在 1–10000 K 内二分 52 次，使三点积分辐亮度等于 r。
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
// optical() 的结果：总反射率及其叶片/土壤分量。
struct Optical { double reflectance, leaf_component, soil_component; };
// 作用：经验的光学冠层反射率。输入 LAI、太阳/观测天顶角、相对方位（度）、叶片反射 lr/透射 lt、土壤反射 sr。
// 说明：joint 为太阳-观测联合间隙率（含热点项）；叶片项为 (1-joint) 乘经验的反向/前向散射组合，
//       总和超过 1 时按比例缩回；太阳在地平线以下返回 0。公式为经验式，物理来源待核对。
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
// QHN 粗糙面极化混合参数 Q（只支持约 1.41、6.925、10.65 GHz 三个频率，容差 0.5，其他频率抛异常）。
inline double qhn_q(double f) {
    bound(f,.001,1000);
    if(std::abs(f-1.41)<.5) return .0327;
    if(std::abs(f-6.925)<.5) return .2783;
    if(std::abs(f-10.65)<.5) return .3143;
    throw std::invalid_argument("STREAM core: unsupported microwave frequency");
}
// 作用：Mironov 土壤介电常数（复数 ε）。输入频率 f（GHz）、体积含水量 sm、黏土含量 clay；
// 说明：按束缚水与自由水两部分（转折含水量 transition 由黏土含量决定）分别计折射率与消光后叠加；系数来自文献，待核对。
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
// microwave() 的结果：H、V 极化亮温（K）与地表发射率。
struct Microwave { double brightness_temperature_h_k,brightness_temperature_v_k,surface_emissivity_h,surface_emissivity_v; };
// 作用：L 波段等微波亮温（tau-omega 模型）。输入土壤含水量 sm、土壤温度 ts、冠层温度 tc（K）、LAI、入射角（度）。
// 步骤：1) Mironov 介电常数 → Fresnel 反射 rh/rv；2) 粗糙度衰减因子 a（rms、相关长度）与 Q 混合得 eh/ev；
//       3) 植被光学厚度 gamma = exp(-b·VWC/μ)（VWC = min(LAI·vwc_per_lai, max_vwc)），散射反照率 omega；
//       4) 亮温 = 植被发射 + 植被衰减后的地表发射 + 植被向下发射被地表反射的部分。
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
