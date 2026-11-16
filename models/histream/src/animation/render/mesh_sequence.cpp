#include "mesh_sequence.h"
#include "compute_pass.h"
#include "animation/sequence/mesh_timeline.h"
#include "animation/bridge/histream_mesh_adapter.h"
#include "animation/transport/scene_transport.h"
#include "animation/sensor/scene_sensor.h"
#include "animation/thermal/background_field.h"
#include "animation/thermal/ground_thermal.h"
#include "animation/observation/camera_observation.h"
#include "animation/scene/fire_sources.h"
#include "animation/scene/supplemental_scenery.h"
#include "animation/scene/mesh_scene.h"
#include "animation/impact/event_model.h"
#include <gdal_priv.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <set>
#include <cctype>

namespace animation {
namespace {
using Json=nlohmann::json;
struct Context {AppSetting app;bool started=false;~Context(){if(started)app.destroy();}};
struct Pixel {glm::vec4 hit,normalRadiance;glm::uvec4 color;};
struct Push {glm::ivec4 image;glm::vec4 origin,forward,right,up,thermal,light,reserved;};
struct Effects {glm::vec4 contact,origin;glm::ivec4 grid;glm::vec4 wind,projectile,start,flightEnd;};
// impact-v1 appends the empirical fireball to SequenceParameters (read only by sequence_impact.comp).
struct ImpactEffects {Effects base;glm::vec4 fireball,fireballExtra,flags;};
// impact-v1 transport source (binding 4 of transport_impact.comp): burst point; heat amplitude K, sigma m, tracer amplitude.
struct ImpactSource {glm::vec4 burst,source;};
struct MaskPush {glm::vec4 origin;glm::ivec4 grid;};
// Wall-clock profile of the main loop (reported in run-manifest.json); no effect on results.
struct Profile {std::map<std::string,double> seconds;};
struct Scope {Profile& profile;const char* key;std::chrono::steady_clock::time_point begin=std::chrono::steady_clock::now();Scope(Profile& p,const char* k):profile(p),key(k){}~Scope(){profile.seconds[key]+=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();}};
static_assert(sizeof(Pixel)==48&&sizeof(Push)==128&&sizeof(Effects)==112&&sizeof(MaskPush)==32);
void json(const std::filesystem::path& p,const Json& value){std::ofstream f(p);f<<value.dump(2)<<'\n';if(!f)throw std::runtime_error("Cannot write sequence receipt");}
void binary(const std::filesystem::path& p,const void* data,size_t size){std::ofstream f(p,std::ios::binary);f.write(static_cast<const char*>(data),size);f.flush();if(!f)throw std::runtime_error("Cannot write sequence binary");}
void png(const std::filesystem::path& p,int w,int h,const uint32_t* data){
    auto* mem=GetGDALDriverManager()->GetDriverByName("MEM");auto* driver=GetGDALDriverManager()->GetDriverByName("PNG");if(!mem||!driver)throw std::runtime_error("Missing PNG driver");
    auto close=[](GDALDataset* d){if(d)GDALClose(d);};std::unique_ptr<GDALDataset,decltype(close)> image(mem->Create("",w,h,4,GDT_Byte,nullptr),close);
    if(!image||image->RasterIO(GF_Write,0,0,w,h,const_cast<uint32_t*>(data),w,h,GDT_Byte,4,nullptr,4,w*4,1,nullptr)!=CE_None)throw std::runtime_error("Cannot prepare sequence PNG");
    const char* options[]={"ZLEVEL=2",nullptr};std::unique_ptr<GDALDataset,decltype(close)> encoded(driver->CreateCopy(pathText(p).c_str(),image.get(),FALSE,const_cast<char**>(options),nullptr,nullptr),close);if(!encoded)throw std::runtime_error("Cannot write sequence PNG");
}
void codesPng(const std::filesystem::path& p,int w,int h,const uint16_t* data){
    auto close=[](GDALDataset* d){if(d)GDALClose(d);};auto* mem=GetGDALDriverManager()->GetDriverByName("MEM");auto* driver=GetGDALDriverManager()->GetDriverByName("PNG");
    std::unique_ptr<GDALDataset,decltype(close)> image(mem->Create("",w,h,1,GDT_UInt16,nullptr),close);
    if(!image||image->RasterIO(GF_Write,0,0,w,h,const_cast<uint16_t*>(data),w,h,GDT_UInt16,1,nullptr,0,0,0,nullptr)!=CE_None)throw std::runtime_error("Cannot prepare detector code PNG");
    const char* options[]={"ZLEVEL=2",nullptr};std::unique_ptr<GDALDataset,decltype(close)> encoded(driver->CreateCopy(pathText(p).c_str(),image.get(),FALSE,const_cast<char**>(options),nullptr,nullptr),close);if(!encoded)throw std::runtime_error("Cannot write detector code PNG");
}
// Encodes PNGs on worker threads (output bytes are independent of the thread count).
// Tasks own copies of their pixels; the first failure is rethrown to the render loop.
// Queued pixel bytes are bounded independently of the thread count so 4K frames and
// `outputThreads=16` cannot queue gigabytes of image copies.
class AsyncWriter {
public:
    explicit AsyncWriter(int threads,size_t budgetBytes=512ull*1024*1024):limitBytes(std::max<size_t>(budgetBytes,1)){for(int i=0;i<threads;i++)workers.emplace_back([this]{run();});}
    ~AsyncWriter(){try{finish();}catch(...){}}
    void submit(std::function<void()> task,size_t bytes=0){
        if(workers.empty()){task();return;}
        std::unique_lock<std::mutex> lock(m);
        room.wait(lock,[&]{return queuedBytes+bytes<=limitBytes||(queuedBytes==0&&queue.empty())||error;});
        if(error)std::rethrow_exception(error);
        queuedBytes+=bytes;queue.push(Task{std::move(task),bytes});lock.unlock();work.notify_one();
    }
    void finish(){
        {std::lock_guard<std::mutex> lock(m);closing=true;}work.notify_all();
        for(auto& w:workers)if(w.joinable())w.join();
        std::lock_guard<std::mutex> lock(m);if(error){auto e=error;error=nullptr;std::rethrow_exception(e);}
    }
private:
    struct Task{std::function<void()> run;size_t bytes;};
    void run(){
        for(;;){
            Task task;
            {std::unique_lock<std::mutex> lock(m);work.wait(lock,[&]{return closing||!queue.empty();});if(queue.empty())return;task=std::move(queue.front());queue.pop();}
            try{task.run();}catch(...){std::lock_guard<std::mutex> lock(m);if(!error)error=std::current_exception();}
            {std::lock_guard<std::mutex> lock(m);queuedBytes-=task.bytes;}room.notify_one();
        }
    }
    std::mutex m;std::condition_variable work,room;std::queue<Task> queue;std::vector<std::thread> workers;size_t limitBytes;size_t queuedBytes=0;bool closing=false;std::exception_ptr error;
};
double planck(double l,double t){return 1.191043934e8/(std::pow(l,5)*std::expm1(14388.29104/(l*t)));}
std::string token(int n,const char* extension){std::ostringstream s;s<<"frame_"<<std::setw(4)<<std::setfill('0')<<n<<extension;return s.str();}
void csv(std::ofstream& f,const Json& values,bool& header){if(!header){bool comma=false;for(auto& item:values.items()){if(comma)f<<',';f<<item.key();comma=true;}f<<'\n';header=true;}bool comma=false;for(auto& item:values.items()){if(comma)f<<',';f<<std::setprecision(10)<<item.value().get<double>();comma=true;}f<<'\n';if(!f)throw std::runtime_error("Cannot write sequence diagnostics");}
}
int runMeshSequence(const std::filesystem::path& input,const std::filesystem::path& rawOutput,const std::filesystem::path& shaders,const std::filesystem::path& executable){
    auto output=std::filesystem::absolute(rawOutput).lexically_normal();while(output.filename().empty()&&output.has_relative_path())output=output.parent_path();
    if(std::filesystem::exists(output))throw std::runtime_error("Sequence output already exists; use a new directory");
    MeshTimeline timeline(input);auto& base=timeline.base;const auto& seq=base.input.at("sequence");const auto* event=timeline.impactEvent();
    if(timeline.bodyMountedCamera()&&double(timeline.last-1)/timeline.fps*timeline.timeScale>=timeline.impact)throw std::runtime_error("Body camera frame range reaches first contact; end native footage before contact");
    if(seq.contains("optical")&&!seq.at("optical").is_boolean())throw std::runtime_error("sequence.optical must be boolean");
    const bool optical=seq.value("optical",false);
    if(seq.contains("physicalAppearance")&&!seq["physicalAppearance"].is_boolean())throw std::runtime_error("physicalAppearance must be boolean");
    bool physical=seq.value("physicalAppearance",false),observe=false;ObservationOptions observationOptions;
    const auto appearance=seq.value("appearanceVersion",std::string("legacy"));
    if(appearance!="legacy"&&appearance!="natural-v2"&&appearance!="varied-v3")throw std::runtime_error("Unsupported appearanceVersion");
    const bool varied=appearance=="varied-v3",natural=appearance=="natural-v2"||varied;
    bool groundEnabled=false;GroundThermalOptions groundOptions;
    if(seq.contains("groundThermal")){const auto& g=seq["groundThermal"];if(!g.is_object()||(g.contains("enabled")&&!g["enabled"].is_boolean()))throw std::runtime_error("Invalid groundThermal options");groundEnabled=g.value("enabled",false);if(groundEnabled){if(!varied)throw std::runtime_error("groundThermal requires varied-v3");groundOptions=GroundThermalOptions::load(g,input.parent_path());}}
    if(natural&&(!physical||!optical))throw std::runtime_error("natural appearance requires physicalAppearance and optical outputs");
    if(seq.contains("cameraObservation")){
        const auto& o=seq.at("cameraObservation");if(!o.is_object()||(o.contains("enabled")&&!o["enabled"].is_boolean()))throw std::runtime_error("cameraObservation enabled must be boolean");observe=o.value("enabled",false);
        if(observe){if(!optical||!timeline.sensor)throw std::runtime_error("cameraObservation requires optical and sensor outputs");observationOptions=ObservationOptions::parse(o,base.width,base.height,double(timeline.fps)/timeline.timeScale,base.wavelength,timeline.impact);
            if(observationOptions.blastArrival=="blast"&&!event)throw std::runtime_error("cameraObservation blastArrival=blast requires motionBackend impact-v1");
            if(event&&observationOptions.jitterOnImpact&&!timeline.bodyMountedCamera()&&(observationOptions.blastArrival=="blast"||!observationOptions.blastArrivalSupplied)){
                // impact-v1 default: the shake starts when the blast wave reaches the camera and scales with its overpressure.
                observationOptions.blastArrival="blast";observationOptions.cameraDistance=glm::length(timeline.view(timeline.impact).position-timeline.contact);
                const auto wave=event->at(observationOptions.cameraDistance);
                observationOptions.shakeStart=double(timeline.impact)+wave.arrivalTimeS;observationOptions.blastOverpressurePa=wave.incidentPressurePa;
                observationOptions.blastShakeScale=std::min(1.,wave.incidentPressurePa/6894.757);observationOptions.impactShake*=observationOptions.blastShakeScale;
            }
            if(observationOptions.blastArrival=="acoustic"){
                // Camera-to-contact distance at the contact instant; mounted cameras end before contact.
                if(timeline.bodyMountedCamera())throw std::runtime_error("blastArrival=acoustic is not supported for a body-mounted camera");
                observationOptions.cameraDistance=glm::length(timeline.view(timeline.impact).position-timeline.contact);
                observationOptions.shakeStart=double(timeline.impact)+observationOptions.cameraDistance/observationOptions.soundSpeed;
            }}
    }
    float fixedMin=seq.value("fixedDisplayMinK",280.f),fixedMax=seq.value("fixedDisplayMaxK",480.f);
    FireSources fire(seq.value("sustainedFire",Json::object()),timeline.contact,timeline.flowOrigin);
    if((fire.block.count.y!=0||fire.block.count.z!=0)&&!natural)throw std::runtime_error("Fire growth requires natural-v2 or varied-v3 appearance");
    for(int i=0;i<fire.block.count.x;i++)if(fire.block.sources[size_t(i)].flame.w>0&&!varied)throw std::runtime_error("Named fire profiles require varied-v3 appearance");
    if(fire.block.count.x&&!timeline.projectileV2)throw std::runtime_error("sustained fire requires separated-v2 appearance");
    if(fire.block.count.x&&!timeline.media)throw std::runtime_error("sustained fire requires media");
    std::unique_ptr<SupplementalScenery> scenery;
    if(seq.contains("scenery")){
        const auto& s=seq["scenery"];if(!s.is_object()||(s.contains("enabled")&&!s["enabled"].is_boolean()))throw std::runtime_error("scenery options/enabled invalid");
        if(s.value("enabled",true))scenery=std::make_unique<SupplementalScenery>(s,input.parent_path(),base);
    }
    if(!std::isfinite(fixedMin)||!std::isfinite(fixedMax)||fixedMin<150||fixedMax>2000||fixedMin>=fixedMax)throw std::runtime_error("Invalid fixed display bounds");
    // impact-v1 has its own render/transport variants (empirical fireball and heat/dust sources instead of the authored ones).
    if(event&&(natural||groundEnabled||fire.block.count.x||scenery))throw std::runtime_error("impact-v1 currently supports the legacy appearance only (no natural-v2/varied-v3, groundThermal, sustainedFire or scenery)");
    Context ctx;ctx.started=true;ctx.app.init(base.gpuIndex,ComputeBackend::Gpu);auto& app=ctx.app;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(app.m_context.m_physicalDevice,&properties);
    const size_t pixels=size_t(base.width)*base.height;if(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU||pixels*sizeof(Pixel)>properties.limits.maxStorageBufferRange)throw std::runtime_error("Sequence requires supported hardware GPU/image dimensions");
    HostBuffer gpuPixels(app,pixels*sizeof(Pixel)),display(app,pixels*4),rawFallback(app,pixels*4),parameters(app,event?sizeof(ImpactEffects):sizeof(Effects));
    HostBuffer linearRgb(app,optical?pixels*16:16);
    HostBuffer fireBuffer(app,sizeof(FireBlock));std::memcpy(fireBuffer.data(),&fire.block,sizeof(FireBlock));
    HostBuffer intactMask(app,64*56*56*4),damagedMask(app,64*56*56*4);
    MeshScene background=base;
    background.triangles.erase(std::remove_if(background.triangles.begin(),background.triangles.end(),[&](const auto& t){return t.ids.y==timeline.targetIndex();}),background.triangles.end());
    if(scenery)scenery->appendTo(background);
    HistreamMeshAdapter intact(app,base),shell(app,timeline.shell),cachedBackground(app,background);
    ComputePass mask(app,shaders/"mesh/mask.comp.spv",{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},sizeof(MaskPush));
    MaskPush maskPush{{timeline.flowOrigin,1},{64,56,56,0}};
    mask.bind(0,intact.tlas());mask.bind(1,intactMask.info());mask.dispatch(&maskPush,16,14,14);
    mask.bind(0,shell.tlas());mask.bind(1,damagedMask.info());mask.dispatch(&maskPush,16,14,14);
    std::vector<VkDescriptorType> renderBindings{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};if(groundEnabled){renderBindings.push_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);renderBindings.push_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);}
    // S3 instanced moving geometry (per-part BLAS built once, TLAS instance updates). Opt-in for the authored and
    // rigid-v1 backends (their default output stays byte-identical); default on for impact-v1.
    bool instanced=event!=nullptr;
    if(seq.contains("instancedGeometry")){if(!seq["instancedGeometry"].is_boolean())throw std::runtime_error("instancedGeometry must be boolean");instanced=seq["instancedGeometry"].get<bool>();}
    const std::string variant=event?"mesh/sequence_impact":groundEnabled?"mesh/sequence_ground":varied?"mesh/sequence_varied":natural?"mesh/sequence_natural":fire.block.count.x?"mesh/sequence_fire":"mesh/sequence";
    ComputePass renderer(app,shaders/(variant+(instanced?"_instanced":"")+".comp.spv"),renderBindings,sizeof(Push));
    renderer.bind(3,gpuPixels.info());renderer.bind(6,parameters.info());renderer.bind(7,cachedBackground.tlas());renderer.bind(8,cachedBackground.triangles());
    renderer.bind(9,linearRgb.info());
    renderer.bind(2,cachedBackground.materials());renderer.bind(10,fireBuffer.info());
    std::filesystem::create_directories(output.parent_path());if(!std::filesystem::create_directory(output))throw std::runtime_error("Sequence output already exists");
    for(auto name:{"frames","fixed_frames","provenance","radiance","geometry"})std::filesystem::create_directory(output/name);
    if(optical)std::filesystem::create_directory(output/"optical_frames");
    if(observe)for(auto name:{"observed_ir","observed_optical","detector_ir"})std::filesystem::create_directory(output/name);
    auto replay=base.input;for(size_t i=0;i<base.assets.size();i++){auto name="asset_"+std::to_string(i)+".obj";std::filesystem::copy_file(base.assets[i],output/"provenance"/name);if(sha256File(output/"provenance"/name)!=base.receipt["assets"][i]["sha256"])throw std::runtime_error("Sequence base asset changed");replay["assets"][i]["path"]=name;}
    replay["sequence"]=timeline.snapshot(output/"provenance");if(scenery)replay["sequence"]["scenery"]=scenery->snapshot(output/"provenance");if(base.background)replay["backgroundField"]=base.background->snapshot(output/"provenance");json(output/"provenance/replay.json",replay);
    std::filesystem::copy_file(input,output/"provenance/input.json");if(sha256File(output/"provenance/input.json")!=base.receipt["sourceSha256"])throw std::runtime_error("Sequence input changed");std::filesystem::copy_file(executable,output/"provenance/histream.exe");renderer.snapshot(output/"provenance/sequence.comp.spv");mask.snapshot(output/"provenance/mask.comp.spv");
    std::unique_ptr<GroundThermal> ground;
    if(groundEnabled){ground=std::make_unique<GroundThermal>(app,shaders/"ground_thermal.comp.spv",groundOptions,base,background,intact.tlas(),intact.triangles(),cachedBackground.tlas(),cachedBackground.triangles(),output/"ground_thermal");renderer.bind(11,ground->field());renderer.bind(12,ground->parameters());replay["sequence"]["groundThermal"]={{"enabled",true},{"path","../ground_thermal/replay-inputs.json"},{"sha256",sha256File(output/"ground_thermal/replay-inputs.json")}};json(output/"provenance/replay.json",replay);}
    SceneTransportOptions transportOptions;transportOptions.impactTime=timeline.impact;transportOptions.windX=timeline.wind.x;transportOptions.windY=timeline.wind.y;transportOptions.meshMask=intactMask.info().buffer;
    transportOptions.origin={timeline.flowOrigin.x,timeline.flowOrigin.y,timeline.flowOrigin.z};transportOptions.contact={timeline.contact.x,timeline.contact.y,timeline.contact.z};
    transportOptions.flightSmoke=timeline.projectileV2&&(!event||event->options.powered);transportOptions.flightStart={timeline.projectileStart.x,timeline.projectileStart.y,timeline.projectileStart.z};transportOptions.outwardAzimuth=timeline.outwardAzimuth;
    transportOptions.fireSources=fireBuffer.info().buffer;
    std::unique_ptr<HostBuffer> impactSource;
    if(event){ImpactSource s{{event->burstPoint,float(event->fireball.diameterM*.5)},{float(event->heatAmplitudeK),float(event->heatSigmaM),float(event->tracerAmplitude),float(event->fireball.durationS)}};impactSource=std::make_unique<HostBuffer>(app,sizeof(s));std::memcpy(impactSource->data(),&s,sizeof(s));transportOptions.impactSource=impactSource->info().buffer;}
    if(timeline.rigid()||event){auto end=timeline.projectile(timeline.impact);transportOptions.explicitFlightEnd=true;transportOptions.flightEnd={end.x,end.y,end.z};}
    SceneTransport transport(app,shaders/(event?"mesh/transport_impact.comp.spv":"mesh/transport.comp.spv"),output/"provenance/transport.comp.spv",transportOptions);
    std::unique_ptr<SceneSensor> sensor;SceneSensorOptions sensorOptions;sensorOptions.width=base.width;sensorOptions.height=base.height;sensorOptions.wavelength=base.wavelength;sensorOptions.minK=fixedMin;sensorOptions.maxK=fixedMax;sensorOptions.agc=true;
    if(seq.contains("sensorOptions")){
        const auto& o=seq.at("sensorOptions");
        if(o.contains("binning")&&!o["binning"].is_number_integer())throw std::runtime_error("Sensor binning must be integer");
        sensorOptions.binning=o.value("binning",2);sensorOptions.blurSigma=o.value("blurSigma",.7f);sensorOptions.noiseKelvin=o.value("noiseKelvin",.09f);sensorOptions.fixedPatternKelvin=o.value("fixedPatternKelvin",.05f);
        sensorOptions.agcMinimumSpanKelvin=o.value("agcMinimumSpanKelvin",20.f);sensorOptions.logarithmicDisplay=o.value("logarithmicDisplay",false);
        auto valid=[](float v,float lo,float hi){return std::isfinite(v)&&v>=lo&&v<=hi;};
        if(sensorOptions.binning<1||sensorOptions.binning>4||!valid(sensorOptions.blurSigma,0,2)||!valid(sensorOptions.noiseKelvin,0,5)||!valid(sensorOptions.fixedPatternKelvin,0,5)||!valid(sensorOptions.agcMinimumSpanKelvin,1,1000))throw std::runtime_error("Invalid sensorOptions");
    }
    if(timeline.sensor)sensor=std::make_unique<SceneSensor>(app,display.info().buffer,shaders/"sensor.comp.spv",output/"provenance/sensor.comp.spv",sensorOptions);
    renderer.bind(5,sensor?VkDescriptorBufferInfo{sensor->radianceBuffer(),0,sensor->radianceBytes()}:rawFallback.info());
    std::unique_ptr<CameraObservation> observation;if(observe)observation=std::make_unique<CameraObservation>(app,shaders/"camera",output/"provenance/camera",observationOptions);
    Json manifest=base.receipt;manifest["status"]="running";manifest["backend"]="meshSequence";manifest["device"]=properties.deviceName;manifest["renderMode"]="thermal";manifest["fps"]=timeline.fps;manifest["frameCount"]=timeline.last-timeline.first+1;manifest["timelineFrameCount"]=timeline.count;manifest["firstFrame"]=timeline.first;manifest["lastFrame"]=timeline.last;manifest["impactTime"]=timeline.impact;
    manifest["timeline"]=timeline.receipt();manifest["sensor"]=sensor?sensor->description():Json{{"enabled",false}};manifest["transport"]=SceneTransport::description();manifest["transport"]["originMeters"]={timeline.flowOrigin.x,timeline.flowOrigin.y,timeline.flowOrigin.z};manifest["transport"]["obstacles"]="six-ray coarse voxel mask of actual intact/preset remainder mesh; fragments render-only";manifest["transport"]["sourcePointMeters"]={timeline.contact.x,timeline.contact.y,timeline.contact.z};
    if(optical)manifest["optical"]={{"enabled",true},{"outputDirectory","optical_frames"},{"colorModel","RGB appearance; material albedo, sun shadows, ambient occlusion, prescribed smoke/dust scatter and jet/fire emission"},{"display","fixed exposure1.6; Reinhard tone curve then gamma2.2"},{"poseSource","same native mesh timeline as infrared"},{"limitations","single-scatter appearance; no measured spectral BRDF, combustion, volume shadowing, atmosphere or calibrated visible sensor response"}};
    if(physical)manifest["physicalAppearance"]={{"enabled",true},{"model","GGX/Smith/Schlick microfacet RGB, explicit roughness/metallic/IOR, footprint-filtered procedural albedo and resolved-tracer sun attenuation"},{"limitations","parameters and textures are prescribed; opaque glass proxy, approximate sky/AO and air attenuation, no full multiple-bounce or measured spectral material data"}};
    if(observe)manifest["cameraObservation"]=observation->description();
    if(timeline.bodyMountedCamera())manifest["cameraMount"]=timeline.cameraMountDescription();
    if(ground){manifest["groundThermal"]=ground->description();manifest["temperatureSource"]="FacetEB original building/background + absolute local ground energy-balance solution + existing impact perturbation; immutable baseline files retained";}
    if(natural){manifest["appearanceVersion"]=appearance;manifest["appearanceModel"]={{"trees","explicit curved leaf geometry and smooth branches; thin-leaf diffuse backlight proxy"},{"fire","advected multi-scale volume detail, intermittent emitting sheets/tips and mottled embers"},{"smoke","resolved tracer with fine appearance detail and approximate sunlight attenuation"},{"limitations","appearance refinement; no reactive combustion, tree motion/energy balance or measured spectral calibration"}};}
    if(fire.block.count.x)manifest["sustainedFire"]=fire.description();
    if(scenery){manifest["scenery"]=scenery->description();if(manifest.contains("backgroundField")){manifest["backgroundField"]["coverageScope"]="original bound FacetEB scene only; supplementary scenery uses prescribed materials";manifest["backgroundField"]["intactRenderSceneCoverageFraction"]=double(base.triangles.size())/(base.triangles.size()+manifest["scenery"]["triangleCount"].get<size_t>());}}
    manifest["projectile"]={{"appearance",timeline.projectileV2?"separated-v2":"legacy"},{"flightSmoke",timeline.projectileV2},{"trackingCamera",seq.contains("trackingCameraOffset")},{"temperaturesK",event?Json{{"body",event->bodyK},{"nose",event->noseK},{"nozzle",event->nozzleK},{"fins",event->finK},{"source","aerodynamic recovery temperature (event.json)"}}:timeline.projectileV2?Json{{"body",334},{"nose",342},{"nozzle",600},{"fins",330}}:Json{{"uniform",650}}}};
    if(timeline.projectileV2)manifest["transport"]["flightSource"]={{"tracerRate",8},{"heatRateKPerSecond",100},{"initialHistorySeconds",.3},{"particleAppearance","prescribed cooler particulate temperature; no separate particle energy solve"}};
    manifest["thermal"]={{"wavelengthMicrometers",base.wavelength},{"radianceUnits","W m-2 sr-1 um-1"},{"fixedDisplayMinK",fixedMin},{"fixedDisplayMaxK",fixedMax},{"fixedDisplayMapping","logarithmic radiance, constant scale; unsaturated float samples separate"},{"temperatureSources","native FacetEB background; evolved low-speed air/wall perturbation; supplied new fracture/fragment heating without wall double heating"}};
    manifest["geometryCache"]="background BLAS retained throughout; target/fragments updated per frame";manifest["mediaEnabled"]=timeline.media;manifest["scope"]="Native GPU mesh/heat-tracer and generic camera demonstration; authored fracture, projectile path and fragment kinematics; no weapon performance, blast, chemistry or structural response prediction";
    // Single top-level classification so packaging/reports cannot present the clip as a predictive impact simulation.
    manifest["physicsValidated"]=false;
    if(event){
        // impact-v1: computed from projectile/charge/wall inputs with public empirical formulas; still not validated.
        manifest["scope"]="Native GPU mesh/heat-tracer and generic camera rendering of an impact-v1 empirical event estimate (penetration, Kingery-Bulmash blast load, block failure, fragment launch, energy budget); no structural FEM, detonation CFD, combustion chemistry or weapon-effectiveness claim";
        manifest["thermal"]["temperatureSources"]="native FacetEB background; low-speed transport with the impact-v1 heat share injected at the burst; empirical fireball; aerodynamic recovery projectile temperatures; retained authored fracture-face/fragment surface heating";
        manifest["impactModel"]={{"kind","impact-v1"},{"predictive",false},{"classification","empirical engineering estimate"},{"implementedModels",Json::array({"Young concrete penetration (SAND97-2426)","Kingery-Bulmash blast (UFC 3-340-02)","impulse failure criterion","rigid-body fragment dynamics","energy-budget fireball/heat/dust"})},
            {"label","empirical model estimate, not independently validated"},{"event","event.json"},
            {"presentation",{{"physicalFlightSpeedMetersPerSecond",event->options.speedMps},{"presentationTimeScale",timeline.timeScale},{"equivalentSlowMotionFactor",1/timeline.timeScale},{"note","physics runs at the physical speed; frame n is sampled at physical time (n-1)/fps x presentationTimeScale, so the clip is slowed by 1/presentationTimeScale"}}}};
    }else
    manifest["impactModel"]={{"kind","authored"},{"predictive",false},{"implementedModels",Json::array({"authored",timeline.rigid()?"rigid-v1 contact dynamics with authored initial conditions":"authored kinematics"})},{"notImplemented","impact-v1 is a separate motionBackend (charge, penetration, blast load, failure, fragment launch from an energy budget)"},
        {"label","authored effect demonstration, not a physical prediction"},
        {"presentation",{{"flightSpeedMetersPerSecond",timeline.flightSpeed},{"referenceSpeedMetersPerSecond",250},{"equivalentSlowMotionFactor",250.0/timeline.flightSpeed},{"note","estimate: the authored flight is shown at flightSpeed; relative to a generic 250 m/s projectile the clip is an equivalent slow-motion presentation"}}}};
    auto warnings=[&](const Json& t){
        Json w=Json::array();
        if(t.contains("solver")){const auto& s=t["solver"];const auto kind=s.value("staticCollider",std::string());
            if(kind!="closedConvexParts"&&kind!="none")w.push_back("rigid static collider is "+kind+" ("+std::to_string(s.contains("staticLayout")?s["staticLayout"].value("skinTriangles",size_t(0)):size_t(0))+" thin-skin triangles): thin fragments may enter through back faces; set sequence.rigidRequireSolidStatic=true to refuse");
            if(s.contains("trackCache")&&s["trackCache"].value("writeOutcome",std::string())=="failed")w.push_back("rigid track cache was not written: "+s["trackCache"].value("writeDetail",std::string()));}
        if(t.contains("breach")&&t["breach"].is_object()&&!t["breach"].value("contactInsideAuthoredBreach",true))w.push_back("computed contact lies outside the authored breach; opening and effects do not match the contact point");
        if(event){
            size_t clamped=0;for(const auto& b:event->blocks)clamped+=b.clamped&&b.failed;
            if(clamped)w.push_back(std::to_string(clamped)+" failed blocks lie outside the Kingery-Bulmash fit range; their impulse is taken at the range limit (near-field loads underestimated)");
            if(event->fragmentKineticJ>event->blastJ)w.push_back("fragment kinetic energy exceeds the blast share of the charge energy; check criticalImpulsePaS / energySplit");
            if(event->heatSigmaWidened)w.push_back("heat source widened beyond the fireball size to stay below the transport temperature clamp");
            if(event->failed==0)w.push_back("no preset block failed: the charge does not open the wall under the given critical impulse");
            if(event->penetration.outsideValidity)w.push_back("projectile outside the Young penetration equation's validity range (mass < 5 kg, speed > 1219 m/s or depth < 3 calibres)");
        }
        return w;
    };
    manifest["warnings"]=warnings(manifest["timeline"]);
    if(event)json(output/"event.json",event->receipt());
    json(output/"run-manifest.json",manifest);
    const bool scaledTime=timeline.timeScale!=1;GDALAllRegister();bool maskChanged=false;Json heatClosure;
    std::ofstream transportLog(output/"transport.csv");bool transportHeader=false;
    std::ofstream rigidLog;bool rigidHeader=false;if(timeline.rigid())rigidLog.open(output/"rigid.csv");
    Profile profile;
    int outputThreads=std::min(4,int(std::max(1u,std::thread::hardware_concurrency())));
    if(seq.contains("outputThreads")){if(!seq["outputThreads"].is_number_integer()||seq["outputThreads"].get<int>()<0||seq["outputThreads"].get<int>()>16)throw std::runtime_error("outputThreads must be an integer 0..16 (0 = synchronous PNG encoding)");outputThreads=seq["outputThreads"].get<int>();}
    AsyncWriter writer(outputThreads);
    // Transport statistics: the GPU copy stays on the render thread, the CPU scan and transport.csv row run on
    // one ordered worker (FIFO, so rows keep frame order; values are computed exactly as before).
    AsyncWriter statsWriter(outputThreads>0?1:0,64ull*1024*1024);
    std::vector<Pixel> cache(pixels);std::vector<uint32_t> fixed(pixels),visible(pixels);std::vector<float> raw(pixels),temperature(pixels);std::unique_ptr<HistreamMeshAdapter> moving;
    // Persistent moving geometry (default) or the original per-sample rebuild (persistentGeometry=false, for A/B checks).
    bool persistentGeometry=true;
    if(seq.contains("persistentGeometry")){if(!seq["persistentGeometry"].is_boolean())throw std::runtime_error("persistentGeometry must be boolean");persistentGeometry=seq["persistentGeometry"].get<bool>();}
    bool batchedStrips=true;
    if(seq.contains("batchedStrips")){if(!seq["batchedStrips"].is_boolean())throw std::runtime_error("batchedStrips must be boolean");batchedStrips=seq["batchedStrips"].get<bool>();}
    std::map<size_t,std::unique_ptr<DynamicMeshAdapter>> dynamicAdapters;VkAccelerationStructureKHR activeTlas{};VkDescriptorBufferInfo activeTriangles{};
    std::map<std::pair<bool,size_t>,std::unique_ptr<InstancedMeshAdapter>> instancedAdapters;size_t instancedParts=0;
    std::vector<uint32_t> opticalPixels(optical?pixels:0);
    const auto start=std::chrono::steady_clock::now();const auto low=planck(base.wavelength,fixedMin),high=planck(base.wavelength,fixedMax);

    // ---- Cameras (S4): the primary camera writes to the output root exactly as before; sequence.additionalCameras
    // render the same samples (one transport/rigid/timeline update per sample time) into cameras/<id>/ with their own
    // sensor AGC and camera-observation state.
    struct CameraRun {
        std::string id;std::filesystem::path dir;MeshTimeline::CameraRig rig;int last=0;
        bool observe=false;ObservationOptions options;SceneSensor* sensor=nullptr;CameraObservation* observation=nullptr;
        std::unique_ptr<SceneSensor> ownedSensor;std::unique_ptr<CameraObservation> ownedObservation;
        std::ofstream frames,sensorLog,observationLog,cameraLog;bool sensorHeader=false,observationHeader=false,cameraHeader=false;
        double maxRadiance=0;size_t triangles=0;Json observationStats,sensorStats;
    };
    std::vector<std::unique_ptr<CameraRun>> cameras;
    auto openLogs=[&](CameraRun& c){
        c.frames.open(c.dir/"frames.csv");c.frames<<(scaledTime?"frame,presentation_time_seconds,physical_time_seconds,png,triangles\n":"frame,presentation_time_seconds,png,triangles\n");
        c.sensorLog.open(c.dir/"sensor.csv");if(c.observe)c.observationLog.open(c.dir/"observation.csv");if(c.rig.mounted)c.cameraLog.open(c.dir/"camera.csv");
    };
    {auto c=std::make_unique<CameraRun>();c->dir=output;c->rig=timeline.primaryRig();c->last=timeline.last;c->observe=observe;c->options=observationOptions;c->sensor=sensor.get();c->observation=observation.get();openLogs(*c);cameras.push_back(std::move(c));}
    if(seq.contains("additionalCameras")){
        const auto& list=seq["additionalCameras"];if(!list.is_array()||list.empty()||list.size()>7)throw std::runtime_error("additionalCameras must be an array of 1..7 camera objects");
        if(groundEnabled)throw std::runtime_error("additionalCameras are not supported with groundThermal");
        std::set<std::string> ids;
        for(const auto& spec:list){
            if(!spec.is_object())throw std::runtime_error("additionalCameras entries must be objects");
            for(auto& item:spec.items())if(item.key()!="id"&&item.key()!="camera"&&item.key()!="trackingCameraOffset"&&item.key()!="cameraMount"&&item.key()!="lastFrame"&&item.key()!="cameraObservation")throw std::runtime_error("Unknown additionalCameras field: "+item.key());
            auto c=std::make_unique<CameraRun>();c->id=spec.value("id",std::string());
            if(c->id.empty()||c->id.size()>32||!std::all_of(c->id.begin(),c->id.end(),[](char ch){return std::isalnum(static_cast<unsigned char>(ch))||ch=='_'||ch=='-';})||!ids.insert(c->id).second)throw std::runtime_error("additionalCameras id must be a unique [A-Za-z0-9_-]{1,32} name");
            c->rig=timeline.rigFrom(spec);c->dir=output/"cameras"/c->id;c->last=timeline.last;
            if(spec.contains("lastFrame")){if(!spec["lastFrame"].is_number_integer()||spec["lastFrame"].get<int>()<timeline.first||spec["lastFrame"].get<int>()>timeline.last)throw std::runtime_error("additionalCameras lastFrame outside firstFrame..lastFrame");c->last=spec["lastFrame"].get<int>();}
            if(c->rig.mounted){int lastValid=0;for(int n=1;n<=timeline.last;n++)if(double(n-1)/timeline.fps*timeline.timeScale<double(timeline.impact))lastValid=n;c->last=std::min(c->last,lastValid);if(c->last<timeline.first)throw std::runtime_error("additional body camera has no frame before contact");}
            if(spec.contains("cameraObservation")){
                const auto& o=spec["cameraObservation"];if(!o.is_object()||(o.contains("enabled")&&!o["enabled"].is_boolean()))throw std::runtime_error("additionalCameras cameraObservation enabled must be boolean");
                c->observe=o.value("enabled",false);
                if(c->observe){
                    if(!optical||!timeline.sensor)throw std::runtime_error("cameraObservation requires optical and sensor outputs");
                    c->options=ObservationOptions::parse(o,base.width,base.height,double(timeline.fps)/timeline.timeScale,base.wavelength,timeline.impact);
                    const bool blast=c->options.blastArrival=="blast"||(event&&c->options.jitterOnImpact&&!c->options.blastArrivalSupplied);
                    if(blast&&!event)throw std::runtime_error("cameraObservation blastArrival=blast requires motionBackend impact-v1");
                    if((c->options.blastArrival=="acoustic"||blast)&&c->options.jitterOnImpact){
                        if(c->rig.mounted)throw std::runtime_error("blastArrival acoustic/blast is not supported for a body-mounted camera");
                        c->options.cameraDistance=glm::length(timeline.view(c->rig,timeline.impact).position-timeline.contact);
                        if(blast){const auto wave=event->at(c->options.cameraDistance);c->options.blastArrival="blast";c->options.shakeStart=double(timeline.impact)+wave.arrivalTimeS;c->options.blastOverpressurePa=wave.incidentPressurePa;c->options.blastShakeScale=std::min(1.,wave.incidentPressurePa/6894.757);c->options.impactShake*=c->options.blastShakeScale;}
                        else c->options.shakeStart=double(timeline.impact)+c->options.cameraDistance/c->options.soundSpeed;
                    }
                }
            }
            std::filesystem::create_directories(c->dir);
            for(auto name:{"frames","fixed_frames","radiance","geometry","provenance"})std::filesystem::create_directory(c->dir/name);
            if(optical)std::filesystem::create_directory(c->dir/"optical_frames");
            if(c->observe)for(auto name:{"observed_ir","observed_optical","detector_ir"})std::filesystem::create_directory(c->dir/name);
            if(timeline.sensor){c->ownedSensor=std::make_unique<SceneSensor>(app,display.info().buffer,shaders/"sensor.comp.spv",c->dir/"provenance/sensor.comp.spv",sensorOptions);c->sensor=c->ownedSensor.get();}
            if(c->observe){c->ownedObservation=std::make_unique<CameraObservation>(app,shaders/"camera",c->dir/"provenance/camera",c->options);c->observation=c->ownedObservation.get();}
            openLogs(*c);cameras.push_back(std::move(c));
        }
    }
    const bool multiCamera=cameras.size()>1;

    // ---- Checkpoints (S6): exact solver/sensor/observation state at chosen frames, so a later partial run with
    // warm-up (sensor AGC / detector history) resumes from the nearest earlier checkpoint instead of frame 1.
    std::filesystem::path checkpointDirectory;int checkpointEvery=0;std::string checkpointKey;int resumedFrom=0;
    if(seq.contains("checkpoints")){
        const auto& c=seq["checkpoints"];
        if(!c.is_object()||!c.contains("directory")||!c["directory"].is_string()||(c.contains("everyFrames")&&!c["everyFrames"].is_number_integer())||(c.contains("enabled")&&!c["enabled"].is_boolean()))throw std::runtime_error("checkpoints requires {\"directory\": string, \"everyFrames\": integer, \"enabled\": bool}");
        for(auto& item:c.items())if(item.key()!="directory"&&item.key()!="everyFrames"&&item.key()!="enabled")throw std::runtime_error("Unknown checkpoints field: "+item.key());
        if(c.value("enabled",true)){
            if(groundEnabled)throw std::runtime_error("checkpoints are not supported with groundThermal (its state is not captured)");
            checkpointEvery=c.value("everyFrames",24);if(checkpointEvery<1||checkpointEvery>2000)throw std::runtime_error("checkpoints.everyFrames must be 1..2000");
            checkpointDirectory=std::filesystem::absolute(input.parent_path()/pathFrom(c["directory"].get<std::string>())).lexically_normal();std::filesystem::create_directories(checkpointDirectory);
            // Key: everything that changes the rendered state, but not the requested frame range or output-only options.
            Json keyed=base.input;for(const char* k:{"firstFrame","lastFrame","exportFrames","checkpoints","outputThreads","persistentGeometry","batchedStrips","rigidCache","rigidRequireSolidStatic"})keyed["sequence"].erase(k);
            std::string material=keyed.dump()+"|engine="+sha256File(executable);
            for(const char* s:{"sensor.comp.spv","camera/filter.comp.spv","camera/integrate.comp.spv","camera/detector.comp.spv","camera/display.comp.spv"})material+=std::string("|")+s+"="+(std::filesystem::exists(shaders/s)?sha256File(shaders/s):"-");
            material+="|render="+sha256File(output/"provenance/sequence.comp.spv")+"|transport="+sha256File(output/"provenance/transport.comp.spv");
            const auto scratch=output/"provenance/.checkpoint-key";{std::ofstream f(scratch,std::ios::binary);f<<material;}checkpointKey=sha256File(scratch).substr(0,24);std::filesystem::remove(scratch);
        }
    }
    auto checkpointPath=[&](int n){return checkpointDirectory/("ckpt_"+checkpointKey+"_"+std::to_string(n)+".bin");};
    auto writeBlock=[](std::ofstream& f,const std::vector<uint8_t>& b){uint64_t size=b.size();f.write(reinterpret_cast<const char*>(&size),sizeof size);f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));};
    auto readBlock=[](std::ifstream& f){uint64_t size=0;f.read(reinterpret_cast<char*>(&size),sizeof size);if(!f||size>(1ull<<32))throw std::runtime_error("Corrupt checkpoint");std::vector<uint8_t> b(size_t(size),0);f.read(reinterpret_cast<char*>(b.data()),std::streamsize(size));if(!f)throw std::runtime_error("Truncated checkpoint");return b;};
    const int warmFirst=timeline.sensor?1:timeline.first;int loopFirst=warmFirst;
    if(!checkpointKey.empty()&&timeline.first>1){
        int best=0;for(int n=timeline.first-1;n>=1&&!best;n--)if(std::filesystem::exists(checkpointPath(n)))best=n;
        if(best){
            std::ifstream f(checkpointPath(best),std::ios::binary);char magic[8]{};int32_t frame=0,mask=0,count=0;f.read(magic,8);f.read(reinterpret_cast<char*>(&frame),4);f.read(reinterpret_cast<char*>(&mask),4);f.read(reinterpret_cast<char*>(&count),4);
            if(!f||std::memcmp(magic,"HSCKPT01",8)!=0||frame!=best||count!=int32_t(cameras.size()))throw std::runtime_error("Checkpoint header mismatch: "+checkpointPath(best).string());
            transport.loadState(readBlock(f));if(mask){transport.setObstacleMask(damagedMask.info().buffer);maskChanged=true;}
            for(auto& c:cameras){auto s=readBlock(f);if(c->sensor)c->sensor->loadState(s);auto o=readBlock(f);if(c->observation)c->observation->loadState(o);}
            loopFirst=best+1;resumedFrom=best;
        }
    }
    auto writeCheckpoint=[&](int n){
        const auto path=checkpointPath(n);if(std::filesystem::exists(path))return;
        const auto tmp=path.string()+".tmp"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        {std::ofstream f(tmp,std::ios::binary);const int32_t frame=n,mask=maskChanged?1:0,count=int32_t(cameras.size());f.write("HSCKPT01",8);f.write(reinterpret_cast<const char*>(&frame),4);f.write(reinterpret_cast<const char*>(&mask),4);f.write(reinterpret_cast<const char*>(&count),4);
            writeBlock(f,transport.saveState());for(auto& c:cameras){writeBlock(f,c->sensor?c->sensor->saveState():std::vector<uint8_t>{});writeBlock(f,c->observation?c->observation->saveState():std::vector<uint8_t>{});}
            f.flush();if(!f)throw std::runtime_error("Cannot write checkpoint");}
        std::error_code ec;std::filesystem::rename(tmp,path,ec);if(ec)std::filesystem::remove(tmp,ec);
    };
    int checkpointsWritten=0;

    double lastGeometryTime=-1;bool lastGeometryCanonical=false;size_t dynamicTriangleCount=0;
    for(int n=loopFirst;n<=timeline.last;n++){
        // Presentation time (video clock) and physical time; identical unless impact-v1 sets presentationTimeScale.
        const double presentation=double(n-1)/timeline.fps,time=presentation*timeline.timeScale,interval=1./timeline.fps*timeline.timeScale;
        auto renderAt=[&](double sampleTime,bool canonical,CameraRun& cam){
        const auto impactStep=std::ceil(double(timeline.impact)*60-1e-6),targetStep=std::floor(sampleTime*60+1e-4);
        if(!maskChanged&&targetStep>=impactStep){transport.advanceTo(std::max(0.,(impactStep-1)/60+1e-6));transport.setObstacleMask(damagedMask.info().buffer);maskChanged=true;
            if(event){
                // Energy closure of the injected heat share: air excess heat across the injection step.
                const double before=transport.statistics()["air_excess_heat_J"].get<double>();transport.advanceTo(impactStep/60+1e-6);const double after=transport.statistics()["air_excess_heat_J"].get<double>();
                heatClosure={{"expectedInjectionJ",event->heatJ},{"airExcessHeatBeforeJ",before},{"airExcessHeatAfterJ",after},{"measuredIncreaseJ",after-before},{"ratio",event->heatJ>0?(after-before)/event->heatJ:0},
                    {"note","one 1/60 s transport step: includes that step's diffusion/radiation/boundary losses, the flight source and any heat placed in solid cells or outside the grid"}};
            }}
        {Scope z(profile,"transportAdvance");transport.advanceTo(sampleTime);}
        // Several cameras at the same sample time share one geometry update.
        if(!(multiCamera&&sampleTime==lastGeometryTime&&(!canonical||lastGeometryCanonical))){
            std::vector<MeshPart> parts;
            auto current=[&]{Scope z(profile,"timelineFrame");return timeline.frame(sampleTime,canonical,instanced?&parts:nullptr);}();
            current.triangles.erase(std::remove_if(current.triangles.begin(),current.triangles.end(),[&](const auto& t){return t.ids.y!=timeline.targetIndex();}),current.triangles.end());
            {Scope z(profile,"adapterBuild");
                if(instanced){
                    // Static target part first (all appended projectile/fragment triangles carry the target index).
                    uint32_t appended=0;for(const auto& p:parts)appended+=p.count;
                    const bool before=float(sampleTime)-timeline.impact<0;const uint32_t fixed=uint32_t(current.triangles.size())-appended;
                    std::vector<MeshPart> layout;if(fixed)layout.push_back({before?0u:1u,0,fixed,glm::mat4(1.f)});
                    for(auto p:parts){p.first+=fixed;layout.push_back(p);}
                    auto& slot=instancedAdapters[{before,current.triangles.size()}];
                    if(!slot)slot=std::make_unique<InstancedMeshAdapter>(app,current,layout);else if(!slot->update(current,layout))throw std::runtime_error("instanced mesh layout changed");
                    activeTlas=slot->tlas();activeTriangles=slot->triangles();instancedParts=std::max(instancedParts,slot->partCount());
                }else if(persistentGeometry){
                    // One persistent adapter per triangle count (pre-impact target+projectile, post-impact shell+fragments).
                    auto& slot=dynamicAdapters[current.triangles.size()];
                    if(!slot)slot=std::make_unique<DynamicMeshAdapter>(app,current);else if(!slot->update(current))throw std::runtime_error("dynamic mesh triangle count changed");
                    activeTlas=slot->tlas();activeTriangles=slot->triangles();
                }else{moving.reset();moving=std::make_unique<HistreamMeshAdapter>(app,current);activeTlas=moving->tlas();activeTriangles=moving->triangles();}
            }
            dynamicTriangleCount=current.triangles.size();lastGeometryTime=sampleTime;lastGeometryCanonical=canonical;
        }
        cam.triangles=dynamicTriangleCount;
        renderer.bind(0,activeTlas);renderer.bind(1,activeTriangles);renderer.bind(4,{transport.buffer(),0,transport.byteSize()});
        if(multiCamera)renderer.bind(5,cam.sensor?VkDescriptorBufferInfo{cam.sensor->radianceBuffer(),0,cam.sensor->radianceBytes()}:rawFallback.info());
        Effects effects{{timeline.contact,timeline.impact},{timeline.flowOrigin,1},{64,56,56,timeline.projectileV2?1:0},{timeline.wind.x,timeline.wind.y,timeline.outwardAzimuth,0},{timeline.projectile(sampleTime),float(sampleTime)<timeline.impact?1.f:0.f},{timeline.projectileStart,timeline.projectileV2?2.7f:0.f},{timeline.projectile(timeline.impact),timeline.rigid()||event?1.f:0.f}};
        if(event){
            // fireball: max radius, duration, peak temperature, growth time; extra: ambient K, extinction at full fill, -, visible light scale; flags: powered.
            ImpactEffects impactEffects{effects,{float(event->fireball.diameterM*.5),float(event->fireball.durationS),float(event->options.fireballPeakK),float(event->fireball.durationS*.1)},{300.f,3.f,0.f,18.f},{event->options.powered?1.f:0.f,0,0,0}};
            std::memcpy(parameters.data(),&impactEffects,sizeof(impactEffects));
        }else std::memcpy(parameters.data(),&effects,sizeof(effects));
        const auto camera=timeline.view(cam.rig,sampleTime);auto forward=glm::normalize(camera.target-camera.position),right=glm::normalize(glm::cross(forward,camera.up)),up=glm::normalize(glm::cross(right,forward));
        if(cam.rig.mounted&&n>=timeline.first){
            auto body=timeline.projectile(sampleTime);
            csv(cam.cameraLog,{{"frame",n},{"presentationTimeSeconds",presentation},{"sampleTimeSeconds",sampleTime},{"canonical",canonical?1:0},
                {"bodyX",body.x},{"bodyY",body.y},{"bodyZ",body.z},{"x",camera.position.x},{"y",camera.position.y},{"z",camera.position.z},
                {"forwardX",forward.x},{"forwardY",forward.y},{"forwardZ",forward.z},{"upX",up.x},{"upY",up.y},{"upZ",up.z},{"fovYDegrees",camera.fovY}},cam.cameraHeader);
        }
        if(cam.observe&&cam.options.regional.enabled){float age=float(std::max(0.,sampleTime-double(timeline.impact)));auto point=timeline.contact+glm::vec3(timeline.wind.x*std::min(age,5.f)*.3f,timeline.wind.y*std::min(age,5.f)*.3f,std::min(age*.6f,5.f));auto delta=point-camera.position;float depth=glm::dot(delta,forward),half=std::tan(camera.fovY*glm::pi<float>()/360.f);glm::vec2 center(-10);if(depth>.01f)center={.5f+.5f*glm::dot(delta,right)/(depth*half*float(base.width)/base.height),.5f-.5f*glm::dot(delta,up)/(depth*half)};if(!std::isfinite(center.x)||!std::isfinite(center.y)||std::abs(center.x)>100||std::abs(center.y)>100)center=glm::vec2(-10);cam.observation->setRegionalCenter(center);}
        Push push{{base.width,base.height,base.samples,0},{camera.position,std::tan(camera.fovY*glm::pi<float>()/360.f)},{forward,float(base.width)/base.height},{right,0},{up,0},{base.wavelength,fixedMin,fixedMax,base.skyK},{base.sunDirection,physical?1.f:0.f},{float(sampleTime),timeline.impact,timeline.media?1.f:0.f,optical?1.f:0.f}};
        {Scope z(profile,"renderDispatch");
            if(batchedStrips){
                // Same 32-row strips (each its own command buffer, so a single GPU submission stays as short as before
                // for Windows TDR), but all strips of a sample go in one vkQueueSubmit with one fence wait.
                std::vector<std::vector<uint8_t>> pushes;for(int row=0;row<base.height;row+=32){push.image.w=row;pushes.emplace_back(reinterpret_cast<const uint8_t*>(&push),reinterpret_cast<const uint8_t*>(&push)+sizeof(push));}
                renderer.dispatchStrips(pushes,(base.width+7)/8,4);
            }else for(int row=0;row<base.height;row+=32){push.image.w=row;renderer.dispatch(&push,(base.width+7)/8,4);}}
        return camera.fovY;
        };
        // Frame-end work of one camera, run right after its canonical (shutter-end) sample.
        auto finishCamera=[&](CameraRun& cam){
            cam.observationStats=Json();
            if(cam.observe){Scope z(profile,"observationFinish");cam.observationStats=cam.observation->finish(interval);}
            cam.sensorStats=Json();if(cam.sensor){Scope z(profile,"sensorProcess");cam.sensorStats=cam.sensor->process(n,interval);}
            if(n<timeline.first)return;
            auto readbackScope=std::make_unique<Scope>(profile,"readbackTonemap");
            // Per-pixel work is independent, so the copy and tone mapping run on row chunks in parallel; every
            // value is computed exactly as before (the maximum is order independent).
            const double logRange=std::log(high/low);
            {
                const size_t workers=outputThreads>0?size_t(std::max(1u,std::min(8u,std::thread::hardware_concurrency()))):1;
                struct Partial {double maxRadiance=0;bool invalid=false;};std::vector<Partial> partial(workers);
                const auto* source=static_cast<const Pixel*>(gpuPixels.data());
                auto work=[&](size_t k){
                    const size_t lo=pixels*k/workers,hi=pixels*(k+1)/workers;Partial& result=partial[k];
                    std::memcpy(cache.data()+lo,source+lo,(hi-lo)*sizeof(Pixel));
                    for(size_t i=lo;i<hi;i++){
                        const auto& p=cache[i];raw[i]=p.normalRadiance.w;temperature[i]=p.hit.w;visible[i]=p.color.x;
                        if(optical)opticalPixels[i]=p.color.w;
                        if(!std::isfinite(raw[i])||raw[i]<=0||!std::isfinite(temperature[i])){result.invalid=true;continue;}result.maxRadiance=std::max(result.maxRadiance,double(raw[i]));
                        float v=float(std::log(std::max(double(raw[i]),low)/low)/logRange);uint32_t gray=uint32_t(std::round(std::clamp(v,0.f,1.f)*255));fixed[i]=gray|(gray<<8)|(gray<<16)|0xff000000u;
                    }
                };
                if(workers==1)work(0);
                else{std::vector<std::thread> pool;for(size_t k=1;k<workers;k++)pool.emplace_back(work,k);work(0);for(auto& t:pool)t.join();}
                for(const auto& r:partial){if(r.invalid)throw std::runtime_error("Invalid sequence radiance/temperature");cam.maxRadiance=std::max(cam.maxRadiance,r.maxRadiance);}
            }
            readbackScope.reset();
            const auto name=token(n,".png");
            auto enqueue=[&](const char* folder,const uint32_t* data){std::vector<uint32_t> copy(data,data+pixels);auto path=cam.dir/folder/name;const int w=base.width,h=base.height;writer.submit([path,w,h,copy=std::move(copy)]{png(path,w,h,copy.data());},copy.size()*sizeof(uint32_t));};
            {Scope z(profile,"pngSubmit");
            enqueue("fixed_frames",fixed.data());enqueue("frames",cam.sensor?static_cast<uint32_t*>(display.data()):fixed.data());
            if(optical)enqueue("optical_frames",opticalPixels.data());
            if(cam.observe){enqueue("observed_ir",cam.observation->infraredDisplay());enqueue("observed_optical",cam.observation->opticalDisplay());{const auto& source=cam.observation->infraredCodes();std::vector<uint16_t> codes(source.begin(),source.end());auto path=cam.dir/"detector_ir"/name;const int w=cam.options.ir.width,h=cam.options.ir.height;writer.submit([path,w,h,codes=std::move(codes)]{codesPng(path,w,h,codes.data());},codes.size()*sizeof(uint16_t));}csv(cam.observationLog,cam.observationStats,cam.observationHeader);}
            if(std::find(timeline.exports.begin(),timeline.exports.end(),n)!=timeline.exports.end()){
                binary(cam.dir/"radiance"/token(n,".f32"),raw.data(),raw.size()*4);binary(cam.dir/"radiance"/token(n,"_surfaceK.f32"),temperature.data(),temperature.size()*4);enqueue("geometry",visible.data());
            }
            }
        };
        // Samples of every active camera, rendered in time order (the transport only moves forward); a camera's
        // canonical sample comes after its own exposure samples, and its frame-end work runs immediately after it.
        struct Sample {double t;int canonical;size_t camera;int j;};std::vector<Sample> samples;
        for(size_t k=0;k<cameras.size();k++){const auto& cam=*cameras[k];if(n>cam.last)continue;
            if(cam.observe)for(int j=0;j<3;j++)samples.push_back({std::max(0.,time-cam.options.exposure*(1.-j*.5)),j==2?1:0,k,j});
            else samples.push_back({time,1,k,0});}
        std::stable_sort(samples.begin(),samples.end(),[](const Sample& a,const Sample& b){return a.t!=b.t?a.t<b.t:a.canonical!=b.canonical?a.canonical<b.canonical:a.camera<b.camera;});
        for(const auto& s:samples){
            auto& cam=*cameras[s.camera];
            if(cam.observe){
                if(s.j==0)cam.observation->beginFrame(n,time);
                const float weights[3]{1.f/6,4.f/6,1.f/6};float fov=renderAt(s.t,s.canonical!=0,cam);
                {Scope z(profile,"observationAccumulate");cam.observation->accumulate({cam.sensor->radianceBuffer(),0,cam.sensor->radianceBytes()},linearRgb.info(),s.t,fov,weights[s.j]);}
            }else renderAt(s.t,true,cam);
            if(s.canonical)finishCamera(cam);
        }
        if(n>=timeline.first){
            // "logging" keeps its historical meaning (whole block); the sub-timers show where it goes.
            auto loggingScope=std::make_unique<Scope>(profile,"logging");
            {Scope z(profile,"loggingTransportStatistics");auto snapshot=transport.capture();
                statsWriter.submit([snapshot,&transportLog,&transportHeader,n,presentation,time,scaledTime]{Json stats=SceneTransport::summarize(*snapshot);stats["frame"]=n;stats["presentationTimeSeconds"]=presentation;if(scaledTime)stats["physicalTimeSeconds"]=time;csv(transportLog,stats,transportHeader);},size_t(transport.byteSize()));}
            for(auto& c:cameras)if(n<=c->last&&c->sensor)csv(c->sensorLog,c->sensorStats,c->sensorHeader);
            if(timeline.rigid()){Scope z(profile,"loggingRigidStatistics");auto all=timeline.rigidStatistics();Json numbers=Json::object();for(auto& item:all.items())if(item.value().is_number())numbers[item.key()]=item.value();numbers["frame"]=n;numbers["presentationTimeSeconds"]=presentation;if(scaledTime)numbers["physicalTimeSeconds"]=time;csv(rigidLog,numbers,rigidHeader);}
            for(auto& c:cameras)if(n<=c->last){
                c->frames<<n<<','<<std::setprecision(10)<<presentation<<',';if(scaledTime)c->frames<<time<<',';c->frames<<token(n,".png")<<','<<c->triangles+background.triangles.size()<<'\n';
                if(!c->frames)throw std::runtime_error("Cannot write sequence frame receipt");
            }
            std::cout<<"FRAME\t"<<n<<'/'<<timeline.count<<"\tt="<<presentation<<std::endl;
        }
        if(checkpointEvery&&n%checkpointEvery==0&&n<timeline.last){Scope z(profile,"checkpointWrite");const bool existed=std::filesystem::exists(checkpointPath(n));writeCheckpoint(n);checkpointsWritten+=!existed;}
    }
    {Scope z(profile,"pngDrain");writer.finish();}
    {Scope z(profile,"statisticsDrain");statsWriter.finish();}
    transportLog.flush();if(!transportLog)throw std::runtime_error("Cannot flush sequence diagnostics");
    for(auto& c:cameras){c->frames.flush();c->sensorLog.flush();if(!c->frames||!c->sensorLog)throw std::runtime_error("Cannot flush sequence diagnostics");
        if(c->observe){c->observationLog.flush();if(!c->observationLog)throw std::runtime_error("Cannot flush camera observation diagnostics");}
        if(c->rig.mounted){c->cameraLog.flush();if(!c->cameraLog)throw std::runtime_error("Cannot flush mounted camera poses");}}
    if(timeline.rigid()){rigidLog.flush();if(!rigidLog)throw std::runtime_error("Cannot flush rigid diagnostics");}
    timeline.flushRigidTrack();
    if(multiCamera){
        Json list=Json::array();
        for(size_t k=1;k<cameras.size();k++){const auto& c=*cameras[k];
            Json d={{"id",c.id},{"directory","cameras/"+c.id},{"camera",c.rig.camera.id},{"lastFrame",c.last},{"bodyMounted",c.rig.mounted},{"tracking",c.rig.tracking},{"maxRadiance",c.maxRadiance},{"sensor",c.sensor?c.sensor->description():Json{{"enabled",false}}}};
            if(c.observe)d["cameraObservation"]=c.observation->description();
            json(c.dir/"camera-manifest.json",d);list.push_back(d);}
        manifest["additionalCameras"]=list;manifest["multiCamera"]="one transport/rigid/timeline update per sample time shared by all cameras; per-camera sensor AGC and observation state";
    }
    if(!checkpointKey.empty())manifest["checkpoints"]={{"directory",pathText(checkpointDirectory)},{"key",checkpointKey},{"everyFrames",checkpointEvery},{"resumedFromFrame",resumedFrom},{"written",checkpointsWritten},
        {"state","transport cells and step, obstacle-mask switch, per-camera sensor AGC and detector histories; rigid poses are re-sampled deterministically (track cache or live solve)"},
        {"note","a resumed run renders frames resumedFromFrame+1..lastFrame; timeline diagnostics cover only those frames"}};
    if(event&&!heatClosure.is_null())manifest["impactHeatClosure"]=heatClosure;
    if(instanced)manifest["instancedParts"]=instancedParts;
    manifest["status"]="complete";manifest["timeline"]=timeline.receipt();manifest["warnings"]=warnings(manifest["timeline"]);manifest["wallSeconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();manifest["maxRadiance"]=cameras.front()->maxRadiance;{Json p=Json::object();for(const auto& item:profile.seconds)p[item.first]=item.second;p["rigidSample"]=timeline.rigidSampleSeconds;manifest["profileSeconds"]=p;manifest["outputThreads"]=outputThreads;manifest["geometryUpdate"]=instanced?"instanced: one BLAS per rigid part (static target, projectile, each fragment) built once per phase; per sample only instance transforms (TLAS rebuild) and world-space triangle records are uploaded; *_instanced shader variant":persistentGeometry?"persistent per-triangle-count adapter; BLAS/TLAS rebuilt in place (mode BUILD, prefer-fast-trace) each sample":"per-sample adapter rebuild";manifest["stripSubmission"]=batchedStrips?"32-row strips, one command buffer each, one queue submission and fence per sample":"32-row strips, submit and wait per strip";manifest["transportStatistics"]="GPU copy to host-cached staging on the render thread; CPU scan and transport.csv row on one ordered worker";}json(output/"timeline.json",timeline.receipt());json(output/"run-manifest.json",manifest);return 0;
}
}
