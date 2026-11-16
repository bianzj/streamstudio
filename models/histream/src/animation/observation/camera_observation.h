#pragma once
#include "animation/render/compute_pass.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace animation {
struct DetectorOptions {
    int width=384,height=216,adcBits=14;
    float psfSigma=.85f,aperture=.9f,noise=.06f,gainFpn=.003f,offset=.07f,rowNoise=.025f,response=.012f;
    glm::vec2 boresight{.37f,-.21f};float rollDegrees=.08f;
    double low=0,high=1;
};
struct ObservationOptions {
    int width=1280,height=720,seed=20261108;
    double wavelength=10,exposure=1./60,jitter=.003,jitterFast=.0012,rollDegrees=.06,fieldScale=.985;
    DetectorOptions ir,rgb;
    double impactTime=-1,impactShake=0;
    // Onset of the impact shake. "contact" (default) starts it at impactTime; "acoustic" delays
    // it by cameraDistance/soundSpeed (set by the caller from the camera-contact distance).
    // shakeStart<0 means impactTime. Regional blur keeps impactTime (it marks the event, not a load).
    std::string blastArrival="contact";double soundSpeed=343,cameraDistance=-1,shakeStart=-1;bool blastArrivalSupplied=false;
    // impact-v1 "blast": Kingery-Bulmash arrival time and incident overpressure at the camera (set by the caller).
    double blastOverpressurePa=-1,blastShakeScale=1;
    double shakeOnset()const{return shakeStart>=0?shakeStart:impactTime;}
    bool jitterOnImpact=false,irLinearPercent2=false;
    struct RegionalBlur {
        bool enabled=false,onImpact=true;
        glm::vec2 center{.5f},radius{.22f,.32f};
        glm::vec4 sigma{2.3f,3.2f,1.f,1.3f}; // IR/RGB centre, IR/RGB edge, source pixels
    } regional;
    ObservationOptions();
    // fps: frames per *physical* second (fps / presentationTimeScale for impact-v1).
    static ObservationOptions parse(const nlohmann::json&,int width,int height,double fps,double wavelength,double impactTime=-1);
};
// Generic GPU camera: exposure/PSF/aperture, response/noise, ADC, then display.
// No measured camera calibration or photon-response curve is implied.
class CameraObservation {
public:
    CameraObservation(AppSetting&,const std::filesystem::path& shaders,const std::filesystem::path& provenance,const ObservationOptions&);
    ~CameraObservation();
    void beginFrame(int frame,double time);
    void setRegionalCenter(glm::vec2 normalizedCenter);
    void accumulate(VkDescriptorBufferInfo infrared,VkDescriptorBufferInfo linearRgb,double sampleTime,float fovY,float weight);
    nlohmann::json finish(double interval);
    const uint32_t* infraredDisplay()const;
    const uint32_t* opticalDisplay()const;
    const std::vector<uint16_t>& infraredCodes()const;
    std::vector<glm::vec4> infraredLinear()const;
    std::vector<glm::vec4> infraredResponse()const;
    nlohmann::json description()const;
    // Temporal state (detector response histories, display AGC, frame counter) for checkpoints.
    std::vector<uint8_t> saveState()const;
    void loadState(const std::vector<uint8_t>&);
private:
    struct Impl;std::unique_ptr<Impl> m;
};
}
