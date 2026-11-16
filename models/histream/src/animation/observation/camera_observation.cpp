#include "camera_observation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace animation {
namespace {
using Json=nlohmann::json;constexpr double PI=3.14159265358979323846;
double planck(double l,double t){return 1.191043934e8/(std::pow(l,5)*std::expm1(14388.29104/(l*t)));}
double real(const Json& j,const char* key,double fallback,double lo,double hi){double x=j.value(key,fallback);if(!std::isfinite(x)||x<lo||x>hi)throw std::runtime_error(std::string("cameraObservation ")+key+" outside supported range");return x;}
int integer(const Json& j,const char* key,int fallback,int lo,int hi){if(j.contains(key)&&!j[key].is_number_integer())throw std::runtime_error(std::string("cameraObservation ")+key+" must be integer");return int(real(j,key,fallback,lo,hi));}
void validate(const ObservationOptions& o){
    auto bounded=[](double v,double lo,double hi){return std::isfinite(v)&&v>=lo&&v<=hi;};
    if(o.width<32||o.height<32||o.width>4096||o.height>4096||o.exposure<=0||o.exposure>.5||!std::isfinite(o.exposure))throw std::runtime_error("Invalid cameraObservation exposure/source");
    if(!bounded(o.wavelength,3,14)||!bounded(o.fieldScale,.8,1)||!bounded(o.jitter,0,.05)||!bounded(o.jitterFast,0,.02)||!bounded(o.rollDegrees,0,1)||o.seed<1||o.seed>2147483646)throw std::runtime_error("Invalid cameraObservation optics/motion");
    if(!bounded(o.impactTime,-1,100000)||!bounded(o.impactShake,0,.02)||(o.jitterOnImpact&&o.impactTime<0)||(o.impactShake>0&&!o.jitterOnImpact))throw std::runtime_error("Invalid cameraObservation impact motion");
    if(o.regional.enabled){
        if(o.regional.onImpact&&o.impactTime<0)throw std::runtime_error("regional blur requires impact time");
        for(int i=0;i<2;i++)if(!bounded(o.regional.center[i],-100,100)||!bounded(o.regional.radius[i],.02,2))throw std::runtime_error("Invalid regional blur centre/radius");
        for(int i=0;i<4;i++)if(!bounded(o.regional.sigma[i],0,4))throw std::runtime_error("Invalid regional blur sigma");
        if(std::hypot(o.regional.sigma.x,o.regional.sigma.z)>4.0001||std::hypot(o.regional.sigma.y,o.regional.sigma.w)>4.0001)throw std::runtime_error("regional blur exceeds source kernel radius");
    }
    for(const auto* d:{&o.ir,&o.rgb}){
        if(d->width<16||d->height<16||d->width>o.width||d->height>o.height||std::abs(double(d->width)/d->height-double(o.width)/o.height)>1e-6)throw std::runtime_error("cameraObservation detector aspect/size must match source FOV");
        const bool ir=d==&o.ir;const double noiseMax=ir?5:1;
        if(!bounded(d->psfSigma,0,2)||!bounded(d->aperture,.1f,1)||!bounded(d->gainFpn,0,.1f)||!bounded(d->noise,0,noiseMax)||!bounded(d->offset,0,noiseMax)||!bounded(d->rowNoise,0,noiseMax)||!bounded(d->response,0,.1f)||!bounded(d->rollDegrees,-1,1)||!std::isfinite(d->boresight.x)||!std::isfinite(d->boresight.y)||std::abs(d->boresight.x)+std::abs(d->boresight.y)>20)throw std::runtime_error("Invalid cameraObservation detector parameters");
        if(d->psfSigma*o.fieldScale*o.width/d->width>4.0001f)throw std::runtime_error("cameraObservation PSF exceeds12source-pixel radius");
        if(d->adcBits<8||d->adcBits>16||float(d->low)>=float(d->high)||d->low<0||!std::isfinite(float(d->low))||!std::isfinite(float(d->high)))throw std::runtime_error("Invalid cameraObservation ADC range");
    }
}
glm::vec3 motion(const ObservationOptions& o,double t){
    const double onset=o.shakeOnset();
    if(o.jitterOnImpact&&t<=onset)return glm::vec3(0);
    double phase=std::fmod(o.seed*.61803398875,2*PI),s=std::sqrt(2.);
    double yaw=o.jitter*s*(.8*std::sin(2*PI*.42*t+phase)+.6*std::sin(2*PI*.17*t+phase*1.3))+o.jitterFast*s*std::sin(2*PI*8.3*t+phase*.7);
    double pitch=o.jitter*s*(.8*std::sin(2*PI*.31*t+phase*.9)+.6*std::sin(2*PI*.11*t+phase*1.7))+o.jitterFast*s*std::sin(2*PI*11.1*t+phase*1.1);
    double roll=o.rollDegrees*PI/180*s*std::sin(2*PI*.34*t+phase*1.9);
    if(o.jitterOnImpact){double age=t-onset,g=(1-std::exp(-age/.08))*(.6+.4*std::exp(-age/2.5));yaw=yaw*g+o.impactShake*std::exp(-age/.8)*std::sin(2*PI*5.5*age);pitch=pitch*g+o.impactShake*.75*std::exp(-age/.9)*std::sin(2*PI*6.8*age);roll=roll*g+o.impactShake*.35*std::exp(-age/1.2)*std::sin(2*PI*4.2*age);}
    return {float(yaw),float(pitch),float(roll)};
}
double regionalActivation(const ObservationOptions& o,double t){return !o.regional.enabled?0:!o.regional.onImpact?1:t<=o.impactTime?0:1-std::exp(-(t-o.impactTime)/.12);}
struct FilterPush{glm::ivec4 image;glm::vec4 sigma;};
struct RegionalPush{glm::ivec4 image;glm::vec4 region,sigma,activation;};
struct IntegratePush{glm::ivec4 image,detector;glm::vec4 optics,rotation,offset;};
struct DetectorPush{glm::ivec4 image;glm::vec4 conversion,noise;glm::ivec4 flags;};
struct DisplayPush{glm::ivec4 image;glm::vec4 stretch,options;};
static_assert(sizeof(FilterPush)==32&&sizeof(IntegratePush)==80&&sizeof(DetectorPush)==64&&sizeof(DisplayPush)==48);
static_assert(sizeof(RegionalPush)==64);
std::vector<VkDescriptorType> storage(int n){return std::vector<VkDescriptorType>(n,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);}
}
ObservationOptions::ObservationOptions(){
    ir.low=planck(10,250);ir.high=planck(10,650);
    rgb.width=960;rgb.height=540;rgb.adcBits=12;rgb.psfSigma=.55f;rgb.aperture=.95f;rgb.noise=.002f;rgb.gainFpn=.001f;rgb.offset=.0004f;rgb.rowNoise=.0005f;rgb.response=0;rgb.boresight={0,0};rgb.rollDegrees=0;rgb.low=0;rgb.high=4;
}
ObservationOptions ObservationOptions::parse(const Json& j,int width,int height,double fps,double wavelength,double impactTime){
    if(!j.is_object())throw std::runtime_error("cameraObservation must be object");ObservationOptions o;o.width=width;o.height=height;o.wavelength=wavelength;
    const auto displayMode=j.value("irDisplay",std::string("legacy-log"));
    if(displayMode!="legacy-log"&&displayMode!="linear-percent-2")throw std::runtime_error("Invalid cameraObservation irDisplay");o.irLinearPercent2=displayMode=="linear-percent-2";
    // Default exposure 1/60 s, shortened to the frame interval for high physical frame rates (slow-motion presentation).
    o.exposure=real(j,"exposureSeconds",std::min(1./60,1./fps),1e-5,1./fps);o.jitter=real(j,"jitterRmsRadians",.003,0,.05);o.jitterFast=real(j,"jitterFastRadians",.0012,0,.02);o.rollDegrees=real(j,"rollRmsDegrees",.06,0,1);o.fieldScale=real(j,"fieldScale",.985,.8,1);o.seed=integer(j,"seed",20261108,1,2147483646);
    if(j.contains("jitterOnImpact")&&!j["jitterOnImpact"].is_boolean())throw std::runtime_error("jitterOnImpact must be boolean");o.jitterOnImpact=j.value("jitterOnImpact",false);o.impactTime=impactTime;o.impactShake=real(j,"impactShakeRadians",0,0,.02);
    if(j.contains("blastArrival")){
        if(!j["blastArrival"].is_string())throw std::runtime_error("cameraObservation blastArrival must be \"contact\", \"acoustic\" or \"blast\"");
        o.blastArrival=j["blastArrival"].get<std::string>();if(o.blastArrival!="contact"&&o.blastArrival!="acoustic"&&o.blastArrival!="blast")throw std::runtime_error("cameraObservation blastArrival must be \"contact\", \"acoustic\" or \"blast\"");
        if(o.blastArrival!="contact"&&!o.jitterOnImpact)throw std::runtime_error("cameraObservation blastArrival=acoustic/blast requires jitterOnImpact");
        o.blastArrivalSupplied=true;
    }
    o.soundSpeed=real(j,"soundSpeedMetersPerSecond",343,200,2000);
    if(j.contains("regionalBlur")){
        const auto& r=j["regionalBlur"];if(!r.is_object()||(r.contains("enabled")&&!r["enabled"].is_boolean())||(r.contains("onImpact")&&!r["onImpact"].is_boolean()))throw std::runtime_error("Invalid regional blur options");
        o.regional.enabled=r.value("enabled",false);o.regional.onImpact=r.value("onImpact",true);
        if(r.contains("radiusNormalized")){auto v=r["radiusNormalized"].get<std::vector<float>>();if(v.size()!=2)throw std::runtime_error("regional radius requires2values");o.regional.radius={v[0],v[1]};}
        o.regional.sigma={float(real(r,"irSigmaSourcePixels",2.3,0,4)),float(real(r,"rgbSigmaSourcePixels",3.2,0,4)),float(real(r,"irEdgeSigmaSourcePixels",1,0,4)),float(real(r,"rgbEdgeSigmaSourcePixels",1.3,0,4))};
    }
    for(int kind=0;kind<2;kind++){
        const auto k=kind==0?"ir":"rgb";auto q=j.value(k,Json::object());if(!q.is_object())throw std::runtime_error("cameraObservation detector options must be object");auto& d=kind==0?o.ir:o.rgb;
        d.width=integer(q,"width",d.width,16,width);d.height=integer(q,"height",d.height,16,height);d.adcBits=integer(q,"adcBits",d.adcBits,8,16);
        d.psfSigma=float(real(q,"psfSigmaPixels",d.psfSigma,0,2));d.aperture=float(real(q,"apertureFraction",d.aperture,.1,1));d.gainFpn=float(real(q,"gainFpn",d.gainFpn,0,.1));d.response=float(real(q,"responseSeconds",d.response,0,.1));d.rollDegrees=float(real(q,"rollDegrees",d.rollDegrees,-1,1));
        d.noise=float(real(q,kind==0?"noiseKelvin":"noiseLinear",d.noise,0,kind==0?5:1));d.offset=float(real(q,kind==0?"offsetKelvin":"offsetLinear",d.offset,0,kind==0?5:1));d.rowNoise=float(real(q,kind==0?"rowNoiseKelvin":"rowNoiseLinear",d.rowNoise,0,kind==0?5:1));
        if(q.contains("boresightPixels")){auto b=q["boresightPixels"].get<std::vector<float>>();if(b.size()!=2||!std::isfinite(b[0])||!std::isfinite(b[1])||std::abs(b[0])+std::abs(b[1])>20)throw std::runtime_error("Invalid cameraObservation boresightPixels");d.boresight={b[0],b[1]};}
        if(kind==0){d.low=planck(wavelength,real(q,"adcMinK",250,150,400));d.high=planck(wavelength,real(q,"adcMaxK",650,401,2000));}
        else {d.low=0;d.high=real(q,"adcMaxLinear",4,.1,100);}
    }validate(o);return o;
}
struct CameraObservation::Impl{
    struct Detector{
        HostBuffer accumulation,historyA,historyB,quantized,codes,display;int active=0;
        Detector(AppSetting& app,const DetectorOptions& d,int w,int h):accumulation(app,size_t(d.width)*d.height*16),historyA(app,size_t(d.width)*d.height*16),historyB(app,size_t(d.width)*d.height*16),quantized(app,size_t(d.width)*d.height*16),codes(app,size_t(d.width)*d.height*16),display(app,size_t(w)*h*4){std::memset(historyA.data(),0,size_t(d.width)*d.height*16);std::memset(historyB.data(),0,size_t(d.width)*d.height*16);}
    };
    ObservationOptions o;HostBuffer horizontal,filtered;ComputePass filter,integrator,detector,display;
    std::unique_ptr<ComputePass> regional;glm::vec2 regionalCenter{.5f};
    Detector ir,rgb;int frame=0;bool open=false,initialized=false;double time=0,lastTime=-1,weight=0,low=0,high=0,focal=0,derivative=0;std::vector<uint16_t> nativeCodes;
    Impl(AppSetting& app,const std::filesystem::path& shaders,const ObservationOptions& options):o(options),horizontal(app,size_t(o.width)*o.height*16),filtered(app,size_t(o.width)*o.height*16),filter(app,shaders/"filter.comp.spv",storage(4),32),integrator(app,shaders/"integrate.comp.spv",storage(2),80),detector(app,shaders/"detector.comp.spv",storage(5),64),display(app,shaders/"display.comp.spv",storage(2),48),ir(app,o.ir,o.width,o.height),rgb(app,o.rgb,o.width,o.height){derivative=planck(o.wavelength,300.5)-planck(o.wavelength,299.5);regionalCenter=o.regional.center;if(o.regional.enabled)regional=std::make_unique<ComputePass>(app,shaders/"regional_filter.comp.spv",storage(2),64);}
};
CameraObservation::CameraObservation(AppSetting& app,const std::filesystem::path& shaders,const std::filesystem::path& provenance,const ObservationOptions& options){
    validate(options);m=std::make_unique<Impl>(app,shaders,options);std::filesystem::create_directory(provenance);
    m->filter.snapshot(provenance/"filter.comp.spv");m->integrator.snapshot(provenance/"integrate.comp.spv");m->detector.snapshot(provenance/"detector.comp.spv");m->display.snapshot(provenance/"display.comp.spv");
    if(m->regional)m->regional->snapshot(provenance/"regional_filter.comp.spv");
}
CameraObservation::~CameraObservation()=default;
namespace {
struct ObservationScalars {int32_t frame,irActive,rgbActive,initialized;double time,lastTime,low,high;float regionalX,regionalY;};
}
std::vector<uint8_t> CameraObservation::saveState()const{
    if(m->open)throw std::runtime_error("cameraObservation checkpoint inside an open frame");
    const ObservationScalars s{m->frame,m->ir.active,m->rgb.active,m->initialized?1:0,m->time,m->lastTime,m->low,m->high,m->regionalCenter.x,m->regionalCenter.y};
    const size_t ir=size_t(m->o.ir.width)*m->o.ir.height*16,rgb=size_t(m->o.rgb.width)*m->o.rgb.height*16;
    std::vector<uint8_t> out(sizeof s+2*ir+2*rgb);uint8_t* p=out.data();
    std::memcpy(p,&s,sizeof s);p+=sizeof s;
    for(auto* d:{&m->ir,&m->rgb}){const size_t bytes=d==&m->ir?ir:rgb;std::memcpy(p,d->historyA.data(),bytes);p+=bytes;std::memcpy(p,d->historyB.data(),bytes);p+=bytes;}
    return out;
}
void CameraObservation::loadState(const std::vector<uint8_t>& state){
    const size_t ir=size_t(m->o.ir.width)*m->o.ir.height*16,rgb=size_t(m->o.rgb.width)*m->o.rgb.height*16;
    if(state.size()!=sizeof(ObservationScalars)+2*ir+2*rgb)throw std::runtime_error("cameraObservation checkpoint size mismatch");
    ObservationScalars s{};const uint8_t* p=state.data();std::memcpy(&s,p,sizeof s);p+=sizeof s;
    for(auto* d:{&m->ir,&m->rgb}){const size_t bytes=d==&m->ir?ir:rgb;std::memcpy(d->historyA.data(),p,bytes);p+=bytes;std::memcpy(d->historyB.data(),p,bytes);p+=bytes;}
    m->frame=s.frame;m->ir.active=s.irActive;m->rgb.active=s.rgbActive;m->initialized=s.initialized!=0;m->time=s.time;m->lastTime=s.lastTime;m->low=s.low;m->high=s.high;m->regionalCenter={s.regionalX,s.regionalY};m->open=false;m->weight=0;
}
void CameraObservation::setRegionalCenter(glm::vec2 center){if(!std::isfinite(center.x)||!std::isfinite(center.y)||std::abs(center.x)>100||std::abs(center.y)>100)throw std::runtime_error("Invalid regional blur projected centre");m->regionalCenter=center;}
void CameraObservation::beginFrame(int frame,double time){
    if(m->open||frame!=m->frame+1||!std::isfinite(time)||time<m->lastTime)throw std::runtime_error("Invalid cameraObservation frame order");m->open=true;m->frame=frame;m->time=time;m->lastTime=time;m->weight=0;
}
void CameraObservation::accumulate(VkDescriptorBufferInfo infrared,VkDescriptorBufferInfo rgb,double sampleTime,float fovY,float weight){
    if(!m->open||!std::isfinite(weight)||weight<=0||!std::isfinite(sampleTime)||sampleTime<0||sampleTime>m->time+1e-7||!std::isfinite(fovY)||fovY<=1||fovY>=150)throw std::runtime_error("Invalid cameraObservation exposure sample");
    const auto& o=m->o;FilterPush f{{o.width,o.height,0,0},{float(o.ir.psfSigma*o.fieldScale*o.width/o.ir.width),float(o.rgb.psfSigma*o.fieldScale*o.width/o.rgb.width),0,0}};
    m->filter.bind(0,infrared);m->filter.bind(1,rgb);m->filter.bind(2,m->filtered.info());m->filter.bind(3,m->horizontal.info());m->filter.dispatch(&f,(o.width+7)/8,(o.height+7)/8);
    f.image.z=1;m->filter.bind(2,m->horizontal.info());m->filter.bind(3,m->filtered.info());m->filter.dispatch(&f,(o.width+7)/8,(o.height+7)/8);
    if(m->regional){
        RegionalPush p{{o.width,o.height,0,0},{m->regionalCenter,o.regional.radius},o.regional.sigma,{float(regionalActivation(o,sampleTime)),0,0,0}};
        m->regional->bind(0,m->filtered.info());m->regional->bind(1,m->horizontal.info());m->regional->dispatch(&p,(o.width+7)/8,(o.height+7)/8);
        p.image.z=1;m->regional->bind(0,m->horizontal.info());m->regional->bind(1,m->filtered.info());m->regional->dispatch(&p,(o.width+7)/8,(o.height+7)/8);
    }
    m->focal=o.height/(2.*std::tan(fovY*PI/360));auto common=motion(o,sampleTime);m->integrator.bind(0,m->filtered.info());
    for(int kind=0;kind<2;kind++){
        const auto& d=kind==0?o.ir:o.rgb;auto& s=kind==0?m->ir:m->rgb;
        IntegratePush p{{o.width,o.height,m->weight==0?1:0,kind},{d.width,d.height,0,0},{float(m->focal),float(o.fieldScale),d.aperture,weight},{common.x,common.y,common.z+float(d.rollDegrees*PI/180),0},{d.boresight,0,0}};
        m->integrator.bind(1,s.accumulation.info());m->integrator.dispatch(&p,(d.width+7)/8,(d.height+7)/8);
    }m->weight+=weight;
}
Json CameraObservation::finish(double interval){
    if(!m->open||std::abs(m->weight-1)>1e-5||!std::isfinite(interval)||interval<=0||m->o.exposure>interval+1e-7)throw std::runtime_error("Invalid cameraObservation exposure weights/interval");
    const auto& o=m->o;std::vector<glm::vec4> signals;
    for(int kind=0;kind<2;kind++){
        const auto& d=kind==0?o.ir:o.rgb;auto& s=kind==0?m->ir:m->rgb;auto& previous=s.active==0?s.historyA:s.historyB;auto& next=s.active==0?s.historyB:s.historyA;
        double alpha=d.response>0?1-std::exp(-interval/d.response):1,unit=kind==0?m->derivative:1;
        DetectorPush p{{d.width,d.height,m->frame,m->initialized?0:1},{float(alpha),float(d.low),float(d.high),float((1u<<d.adcBits)-1)},{float(d.noise*unit),d.gainFpn,float(d.offset*unit),float(d.rowNoise*unit)},{kind,o.seed,0,0}};
        m->detector.bind(0,s.accumulation.info());m->detector.bind(1,previous.info());m->detector.bind(2,next.info());m->detector.bind(3,s.quantized.info());m->detector.bind(4,s.codes.info());m->detector.dispatch(&p,(d.width+7)/8,(d.height+7)/8);s.active=1-s.active;
    }
    signals.resize(size_t(o.ir.width)*o.ir.height);std::memcpy(signals.data(),m->ir.quantized.data(),signals.size()*16);std::vector<float> histogram;histogram.reserve(signals.size());float maximum=0;
    for(auto s:signals){if(!std::isfinite(s.x))throw std::runtime_error("Nonfinite cameraObservation detector signal");histogram.push_back(s.x);maximum=std::max(maximum,s.x);}
    auto percentile=[&](double q){size_t k=size_t(q*(histogram.size()-1));std::nth_element(histogram.begin(),histogram.begin()+k,histogram.end());return double(histogram[k]);};
    double targetLow=percentile(.02),targetHigh=std::max({percentile(.993),targetLow+60*m->derivative,double(maximum)});
    if(!m->initialized){m->low=targetLow;m->high=targetHigh;}else{double a=1-std::exp(-interval/.35);m->low+=(targetLow-m->low)*a;m->high+=(targetHigh-m->high)*a;}
    if(o.irLinearPercent2){
        // Exact per-frame whole-detector percentiles, display only. The one-code
        // floor handles a constant histogram; never modify the native ADC data.
        m->low=targetLow;m->high=std::max(percentile(.98),m->low+(o.ir.high-o.ir.low)/double((1u<<o.ir.adcBits)-1));
    }
    for(int kind=0;kind<2;kind++){
        const auto& d=kind==0?o.ir:o.rgb;auto& s=kind==0?m->ir:m->rgb;
        DisplayPush p{{d.width,d.height,o.width,o.height},{float(m->low),float(m->high),float(10*m->derivative),float(kind)},{o.irLinearPercent2?1.f:0.f,0,255,1.6f}};
        m->display.bind(0,s.quantized.info());m->display.bind(1,s.display.info());m->display.dispatch(&p,(o.width+7)/8,(o.height+7)/8);
    }
    std::vector<glm::uvec4> codes(size_t(o.ir.width)*o.ir.height);std::memcpy(codes.data(),m->ir.codes.data(),codes.size()*16);m->nativeCodes.resize(codes.size());uint32_t minCode=65535,maxCode=0;
    for(size_t i=0;i<codes.size();i++){m->nativeCodes[i]=uint16_t(codes[i].x);minCode=std::min(minCode,codes[i].x);maxCode=std::max(maxCode,codes[i].x);}
    // Preserve legacy diagnostics; impact-gated motion reports the shutter end,
    // consistent with the activation/centre timestamp and causal quadrature.
    m->open=false;m->initialized=true;auto angle=motion(o,o.jitterOnImpact?m->time:std::max(0.,m->time-o.exposure*.5));
    Json stats={{"frame",m->frame},{"presentationTimeSeconds",m->time},{"jitterYawRadians",angle.x},{"jitterPitchRadians",angle.y},{"jitterRollRadians",angle.z},{"exposureWeightSum",m->weight},{"irAdcMin",minCode},{"irAdcMax",maxCode},{"irAgcLow",m->low},{"irAgcHigh",m->high},{"sourceFocalPixels",m->focal},{"irIfovCenterRadians",o.fieldScale*o.width/o.ir.width/m->focal},{"rgbIfovCenterRadians",o.fieldScale*o.width/o.rgb.width/m->focal}};
    if(o.jitterOnImpact||o.regional.enabled){stats["impactTimeSeconds"]=o.impactTime;stats["regionalActivation"]=regionalActivation(o,m->time);stats["regionalCenterX"]=m->regionalCenter.x;stats["regionalCenterY"]=m->regionalCenter.y;}
    if(o.jitterOnImpact&&o.blastArrival=="acoustic")stats["shakeStartSeconds"]=o.shakeOnset();
    return stats;
}
const uint32_t* CameraObservation::infraredDisplay()const{return static_cast<const uint32_t*>(m->ir.display.data());}
const uint32_t* CameraObservation::opticalDisplay()const{return static_cast<const uint32_t*>(m->rgb.display.data());}
const std::vector<uint16_t>& CameraObservation::infraredCodes()const{return m->nativeCodes;}
std::vector<glm::vec4> CameraObservation::infraredLinear()const{std::vector<glm::vec4> p(size_t(m->o.ir.width)*m->o.ir.height);std::memcpy(p.data(),m->ir.accumulation.data(),p.size()*16);return p;}
std::vector<glm::vec4> CameraObservation::infraredResponse()const{std::vector<glm::vec4> p(size_t(m->o.ir.width)*m->o.ir.height);auto& b=m->ir.active==0?m->ir.historyA:m->ir.historyB;std::memcpy(p.data(),b.data(),p.size()*16);return p;}
Json CameraObservation::description()const{
    const auto& o=m->o;auto detector=[](const DetectorOptions& d,bool ir){return Json{{"pixels",{d.width,d.height}},{"gaussianPsfSigmaDetectorPixels",d.psfSigma},{"linearApertureFraction",d.aperture},{"adcBits",d.adcBits},{"adcSignalRange",{d.low,d.high}},{"noiseStd",d.noise},{"noiseUnits",ir?"equivalentK at300K":"linearRGBsceneUnits"},{"gainFpnStd",d.gainFpn},{"offsetStd",d.offset},{"rowNoiseStd",d.rowNoise},{"responseSeconds",d.response},{"boresightPixels",{d.boresight.x,d.boresight.y}},{"rollDegrees",d.rollDegrees}};};
    Json result={{"enabled",true},{"backend","HiStream native Vulkan compute"},{"sourcePixels",{o.width,o.height}},{"ir",detector(o.ir,true)},{"rgb",detector(o.rgb,false)},{"exposureSeconds",o.exposure},{"temporalQuadrature","Simpson3 actual scene samples, shutter ends at timestamp; first timestamp clamped to0"},{"pixelQuadrature","4x4 normalized rectangular-aperture samples after Gaussian PSF"},{"psfKernel","separable normalized Gaussian;ceil3sigma radius,at most12source pixels"},{"slowJitterRmsPerAxisRadians",o.jitter},{"fastJitterRmsPerAxisRadians",o.jitterFast},{"rollRmsDegrees",o.rollDegrees},{"jitterModel","continuous fixed-seed band-limited angular rotations; common RGB/IR motion, separate boresight"},{"seed",o.seed},{"fieldScale",o.fieldScale},{"displayBits",8},{"displayReconstruction","bilinear detector display; source-edge clamping"},{"irWavelengthMicrometers",o.wavelength},{"irAdcContainerBits",16},{"limitations","generic parameters, not calibrated hardware/EMVA compliance; effective monochromaticIR, no spectral bandpass calibration/photon shot noise/dropped frames/rolling shutter;3sample fast-motion blur approximation"}};
    if(o.jitterOnImpact){
        result["impactMotion"]={{"startSeconds",o.shakeOnset()},{"preImpactAngularJitter",0},{"impactShakeRadians",o.impactShake},{"diagnosticTimestamp","shutter end/presentation time; image integrates causal exposure samples"},{"model","supplied post-contact angular response and damped transient; no structural acceleration calibration"},{"blastArrival",o.blastArrival}};
        if(o.blastArrival=="acoustic")result["impactMotion"]["arrival"]={{"contactSeconds",o.impactTime},{"cameraDistanceMeters",o.cameraDistance},{"soundSpeedMetersPerSecond",o.soundSpeed},{"delaySeconds",o.shakeOnset()-o.impactTime},
            {"note","onset = contact + camera distance / sound speed: an upper bound for the blast-wave arrival (a near-field shock is supersonic and arrives earlier); no Kingery-Bulmash arrival time or overpressure is computed"}};
        if(o.blastArrival=="blast")result["impactMotion"]["arrival"]={{"burstSeconds",o.impactTime},{"cameraDistanceMeters",o.cameraDistance},{"arrivalSeconds",o.shakeOnset()-o.impactTime},{"incidentOverpressurePa",o.blastOverpressurePa},
            {"shakeAmplitudeScale",o.blastShakeScale},{"note","impact-v1: onset = burst + Kingery-Bulmash arrival time at the camera distance; amplitude = impactShakeRadians x min(1, Ps / 6.895 kPa). Observation proxy, not a structural or mount acceleration"}};
    }
    if(o.irLinearPercent2)result["irDisplay"]={{"mode","linear-percent-2"},{"percentiles",{2,98}},{"histogram","all native detector pixels, including sky; floor-index quantile"},{"mapping","clamp((signal-P2)/max(P98-P2,oneADCstep),0,1), then8bit rounding"},{"temporalSmoothing",false},{"rawAdcModified",false}};
    if(o.regional.enabled)result["regionalBlur"]={{"enabled",true},{"radiusNormalized",{o.regional.radius.x,o.regional.radius.y}},{"sigmaSourcePixels",{o.regional.sigma.x,o.regional.sigma.y,o.regional.sigma.z,o.regional.sigma.w}},{"centralBlurOnImpact",o.regional.onImpact},{"edgeBlurAlways",true},{"model","normalized separable spatial Gaussian in linear signal; centre follows projected contact/plume; observation proxy, no calibrated lens MTF"}};
    return result;
}
}
