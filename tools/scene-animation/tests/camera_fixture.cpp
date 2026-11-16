#include "animation/observation/camera_observation.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace animation;
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
double B(double t){return 1.191043934e8/(std::pow(10.,5)*std::expm1(14388.29104/(10*t)));}
struct Runtime{AppSetting app;bool started=false;~Runtime(){if(started)app.destroy();}};
int main(int argc,char** argv){try{
    check(argc==3,"Usage:histream_camera_tests SHADER_FOLDER NEW_OUTPUT");std::filesystem::path shaders=argv[1],out=argv[2];check(!std::filesystem::exists(out),"Fixture output exists");std::filesystem::create_directories(out);
    Runtime r;r.started=true;r.app.init(0,ComputeBackend::Gpu);HostBuffer ir(r.app,64*64*4),rgb(r.app,64*64*16);auto* raw=static_cast<float*>(ir.data());auto* color=static_cast<glm::vec4*>(rgb.data());
    auto fill=[&](float value){std::fill(raw,raw+4096,value);std::fill(color,color+4096,glm::vec4(.2,.3,.4,1));};
    ObservationOptions o;o.width=o.height=64;o.jitter=o.jitterFast=o.rollDegrees=0;o.fieldScale=1;
    for(auto* d:{&o.ir,&o.rgb}){d->width=d->height=32;d->psfSigma=0;d->boresight={0,0};d->rollDegrees=d->noise=d->gainFpn=d->offset=d->rowNoise=d->response=0;d->adcBits=16;}
    auto expose=[&](CameraObservation& camera,int frame){camera.beginFrame(frame,(frame-1)/24.);camera.accumulate(ir.info(),rgb.info(),(frame-1)/24.,55,1);return camera.finish(1./24);};
    fill(float(B(300)));CameraObservation flat(r.app,shaders,out/"flat",o);expose(flat,1);double flatError=0;for(auto p:flat.infraredLinear())flatError=std::max(flatError,std::abs(double(p.x)-B(300)));check(flatError<1e-4,"Constant field not preserved by PSF/aperture");
    int expected=int(std::round((B(300)-o.ir.low)/(o.ir.high-o.ir.low)*65535));for(auto code:flat.infraredCodes())check(std::abs(int(code)-expected)<=1,"ADC conversion outside1LSB");
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)raw[y*64+x]=float(6.+6.*(x+.5)/64.);
    ObservationOptions offset=o;offset.ir.boresight.x=.25;CameraObservation ramp(r.app,shaders,out/"ramp",offset);expose(ramp,1);auto values=ramp.infraredLinear();double gridError=0;
    for(int y=2;y<30;y++)for(int x=2;x<30;x++)gridError=std::max(gridError,std::abs(double(values[y*32+x].x)-(6.+6.*(x+.5+.25)/32.)));check(gridError<1e-4,"Fractional detector registration/ramp incorrect");
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)raw[y*64+x]=x<32?6:12;
    CameraObservation sharp(r.app,shaders,out/"sharp",o);expose(sharp,1);auto s=sharp.infraredLinear();auto blurred=o;blurred.ir.psfSigma=.85;
    CameraObservation blur(r.app,shaders,out/"blur",blurred);expose(blur,1);auto b=blur.infraredLinear();double spread=0,mean=0;for(size_t i=0;i<b.size();i++){spread+=std::abs(b[i].x-s[i].x);mean+=b[i].x;}mean/=b.size();check(spread>1&&std::abs(mean-9)<.001,"PSF edge spread/energy normalization incorrect");
    auto cropped=blurred;cropped.fieldScale=.8;CameraObservation crop(r.app,shaders,out/"crop",cropped);expose(crop,1);auto c=crop.infraredLinear();double cropError=0;
    for(int x=13;x<=18;x++)cropError=std::max(cropError,std::abs(double(c[16*32+x].x-b[16*32+x].x)));check(cropError<.13,"PSF width changes in detector pixels when FOV is cropped");
    for(int invalid=0;invalid<3;invalid++){auto bad=o;float nan=std::numeric_limits<float>::quiet_NaN();if(invalid==0)bad.ir.psfSigma=nan;if(invalid==1)bad.jitter=nan;if(invalid==2)bad.ir.noise=nan;bool rejected=false;try{CameraObservation reject(r.app,shaders,out/("invalid"+std::to_string(invalid)),bad);}catch(const std::runtime_error&){rejected=true;}check(rejected,"Nonfinite direct ObservationOptions accepted");}
    auto lag=o;lag.ir.response=.012;fill(6);CameraObservation response(r.app,shaders,out/"response",lag);expose(response,1);fill(12);expose(response,2);double target=12+(6-12)*std::exp(-(1./24)/.012);double responseError=0;
    for(auto p:response.infraredResponse())responseError=std::max(responseError,std::abs(p.x-target));check(responseError<1e-4,"Detector temporal response is not applied before electronics");
    auto noisy=o;noisy.ir.noise=.5;fill(float(B(300)));CameraObservation noiseA(r.app,shaders,out/"noiseA",noisy),noiseB(r.app,shaders,out/"noiseB",noisy);expose(noiseA,1);expose(noiseB,1);check(noiseA.infraredCodes()==noiseB.infraredCodes(),"Noise seed not deterministic");auto first=noiseA.infraredCodes();expose(noiseA,2);check(first!=noiseA.infraredCodes(),"Temporal noise frozen between frames");
    auto regional=o;regional.regional.enabled=true;regional.regional.onImpact=false;
    regional.regional.sigma={2.3,3.2,1,1.3};fill(float(B(300)));
    CameraObservation regionFlat(r.app,shaders,out/"regional_flat",regional);expose(regionFlat,1);double regionError=0;
    for(auto p:regionFlat.infraredLinear())regionError=std::max(regionError,std::abs(double(p.x)-B(300)));check(regionError<1e-4,"Regional blur does not preserve a constant field");
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)raw[y*64+x]=x<32?6:12;
    CameraObservation regionEdge(r.app,shaders,out/"regional_edge",regional);expose(regionEdge,1);auto edge=regionEdge.infraredLinear();double centralSpread=0,outerSpread=0;
    for(int x=12;x<20;x++){centralSpread+=std::abs(edge[16*32+x].x-s[16*32+x].x);outerSpread+=std::abs(edge[1*32+x].x-s[1*32+x].x);}
    check(centralSpread>outerSpread+.1,"Regional blur did not soften the selected area more strongly");
    auto gated=o;gated.jitter=.003;gated.jitterFast=.0012;gated.jitterOnImpact=true;gated.impactTime=.5;gated.impactShake=.004;
    gated.regional.enabled=true;CameraObservation contact(r.app,shaders,out/"impact_gate",gated);
    for(int frame=1;frame<=3;frame++){double t=frame==1?0:frame==2?.5:.55;contact.beginFrame(frame,t);contact.accumulate(ir.info(),rgb.info(),t,55,1);auto stats=contact.finish(1./24);
        if(frame<3){for(const char* key:{"jitterYawRadians","jitterPitchRadians","jitterRollRadians","regionalActivation"})check(stats[key].get<double>()==0,"Camera moved/central region softened at or before contact");}
        else check(std::abs(stats["jitterYawRadians"].get<double>())>0&&stats["regionalActivation"].get<double>()>0,"Post-contact camera effects inactive");}
    CameraObservation crossing(r.app,shaders,out/"contact_crossing",gated);crossing.beginFrame(1,.505);
    crossing.accumulate(ir.info(),rgb.info(),.505-gated.exposure,55,1.f/6);crossing.accumulate(ir.info(),rgb.info(),.505-gated.exposure*.5,55,4.f/6);crossing.accumulate(ir.info(),rgb.info(),.505,55,1.f/6);
    auto crossed=crossing.finish(1./24);check(std::abs(crossed["jitterYawRadians"].get<double>())>0&&crossed["regionalActivation"].get<double>()>0,"Contact-crossing end-time receipt lost actual post-impact sample motion");
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){raw[y*64+x]=float(4.+16.*(y*64+x)/4095.);color[y*64+x]=glm::vec4(x%8<4?.1:.9,.3,.4,1);}
    auto percent=o;percent.irLinearPercent2=true;CameraObservation linear(r.app,shaders,out/"linear_percent2",percent),legacyDisplay(r.app,shaders,out/"legacy_display",o);
    auto percentStats=expose(linear,1);expose(legacyDisplay,1);check(linear.infraredCodes()==legacyDisplay.infraredCodes(),"Display enhancement modified native ADC");
    check(std::equal(linear.opticalDisplay(),linear.opticalDisplay()+4096,legacyDisplay.opticalDisplay()),"IR enhancement modified RGB display");
    auto codes=linear.infraredCodes(),sorted=codes;std::sort(sorted.begin(),sorted.end());double step=(o.ir.high-o.ir.low)/65535.;
    double low=o.ir.low+sorted[size_t(.02*(sorted.size()-1))]*step,high=o.ir.low+sorted[size_t(.98*(sorted.size()-1))]*step;
    check(std::abs(percentStats["irAgcLow"].get<double>()-low)<1e-4&&std::abs(percentStats["irAgcHigh"].get<double>()-high)<1e-4,"IR2% endpoints incorrect");
    auto pixel=[&](int x,int y){x=std::clamp(x,0,31);y=std::clamp(y,0,31);return std::round(std::clamp((o.ir.low+codes[y*32+x]*step-low)/(high-low),0.,1.)*255);};double displayError=0;
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){double qx=(x+.5)*.5-.5,qy=(y+.5)*.5-.5;int ix=int(std::floor(qx)),iy=int(std::floor(qy));double fx=qx-ix,fy=qy-iy;
        double expected=(pixel(ix,iy)*(1-fx)+pixel(ix+1,iy)*fx)*(1-fy)+(pixel(ix,iy+1)*(1-fx)+pixel(ix+1,iy+1)*fx)*fy;
        displayError=std::max(displayError,std::abs(double(linear.infraredDisplay()[y*64+x]&255u)-std::round(expected)));}
    check(displayError<=1,"IR GPU display is not linear2% stretch");
    fill(float(B(300)));CameraObservation percentFlat(r.app,shaders,out/"percent_flat",percent);auto flatStats=expose(percentFlat,1);
    check(flatStats["irAgcHigh"].get<double>()>flatStats["irAgcLow"].get<double>(),"Constant histogram has zero display span");for(int i=0;i<4096;i++)check((percentFlat.infraredDisplay()[i]&255u)<=1,"Constant histogram display not black");
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){raw[y*64+x]=x<32?6:12;color[y*64+x]=glm::vec4(x%8<4?.1:.9,.3,.4,1);}
    auto irOnly=o;irOnly.regional.enabled=true;irOnly.regional.onImpact=false;irOnly.regional.sigma={2.3,0,1,0};
    CameraObservation isolated(r.app,shaders,out/"ir_only_blur",irOnly),clear(r.app,shaders,out/"clear_rgb",o);expose(isolated,1);expose(clear,1);
    check(std::equal(isolated.opticalDisplay(),isolated.opticalDisplay()+4096,clear.opticalDisplay()),"IR-only blur changed clear RGB");check(isolated.infraredCodes()!=clear.infraredCodes(),"IR-only blur inactive");
    nlohmann::json result={{"status","passed"},{"constantFieldError",flatError},{"fractionalGridError",gridError},{"psfStepDifference",spread},{"psfMean",mean},{"cropPsfEdgeError",cropError},{"detectorResponseError",responseError},{"adcErrorLSB",1},{"regionalConstantError",regionError},{"regionalCentralSpread",centralSpread},{"regionalOuterSpread",outerSpread},{"impactMotionGate",true},{"crossingExposureSamples",3},{"crossingEndJitterYaw",crossed["jitterYawRadians"]},{"percentDisplayMaxError8bit",displayError},{"percentDisplayRawAdcExact",true},{"percentDisplayRgbExact",true},{"percentFlatGuard",true},{"irOnlyBlurRgbExact",true},{"nonfiniteDirectOptionsRejected",true},{"independentDeviceCalibration",false}};std::ofstream(out/"verification.json")<<result.dump(2)<<'\n';std::cout<<result.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
