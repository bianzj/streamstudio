//
// Created by admin on 2024/1/24.
//

#include "compo.h"

#include "soil_tir_emissivity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
float bounded(float value, float fallback)
{
    return std::clamp(std::isfinite(value) ? value : fallback, 0.0f, 1.0f);
}

float wavelengthMicrometres(float wavelength)
{
    return wavelength > 50.0f ? wavelength / 1000.0f : wavelength;
}

float wavelengthNanometres(float wavelength)
{
    return wavelength > 0.0f && wavelength <= 50.0f ? wavelength * 1000.0f : wavelength;
}

std::vector<float> parseSpectrumRow(std::string line)
{
    std::replace(line.begin(), line.end(), ',', ' ');
    std::replace(line.begin(), line.end(), ';', ' ');
    std::vector<float> values;
    std::istringstream stream(line);
    float value = 0.0f;
    while (stream >> value) if (std::isfinite(value)) values.push_back(value);
    return values;
}

void readSpectrumFile(const std::string& path, std::vector<float>& wavelengths,
                      std::vector<float>& reflectance, std::vector<float>& transmittance)
{
    // DEFERRED[中] 安全: std::ifstream 以窄字符 std::string 路径打开，在 Windows 上按 ANSI 代码页解析，UTF-8 编码或含代码页外字符的中文路径会打开失败；理由：需窄字符路径改 u8path 全链路（含 fileio/atmosphere_lut 各处 ifstream），超出本轮范围，未改。
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open imported spectrum: " + path);
    std::vector<std::vector<float>> rows;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        auto values = parseSpectrumRow(line);
        if (!values.empty()) rows.push_back(std::move(values));
    }
    if (rows.size() < 2U || rows.size() > 3U || rows[0].size() < 2U ||
        rows[1].size() != rows[0].size() ||
        (rows.size() == 3U && rows[2].size() != rows[0].size())) {
        throw std::runtime_error("Imported spectrum must contain wavelength and reflectance rows, plus optional transmittance row: " + path);
    }
    std::vector<std::pair<float, std::size_t>> order;
    for (std::size_t index = 0; index < rows[0].size(); ++index)
        order.emplace_back(wavelengthNanometres(rows[0][index]), index);
    std::sort(order.begin(), order.end());
    wavelengths.clear(); reflectance.clear(); transmittance.clear();
    for (const auto& entry : order) {
        if (!wavelengths.empty() && entry.first <= wavelengths.back())
            throw std::runtime_error("Imported spectrum wavelengths must be unique: " + path);
        wavelengths.push_back(entry.first);
        reflectance.push_back(rows[1][entry.second]);
        transmittance.push_back(rows.size() == 3U ? rows[2][entry.second] : 0.0f);
    }
}

float interpolateValue(const std::vector<float>& wavelengths,
                       const std::vector<float>& values, float wavelength, float fallback,
                       const std::string& label)
{
    if (values.empty()) return fallback;
    // FIXED[中] 错误处理: 自定义光谱的反射率/透射率个数与传感器波段数不一致时对所有波段静默返回第一个值（平坦错误光谱且无提示）；已修复：按 wavelengths/values 的较短长度插值，并对不一致组合用 std::call_once 打一次含材质名与两侧长度的告警（单值广播的既有用法保持原行为）。
    const std::size_t count = std::min(wavelengths.size(), values.size());
    if (values.size() > 1U && values.size() != wavelengths.size()) {
        static std::once_flag mismatchWarned;
        std::call_once(mismatchWarned, [&] {
            std::cerr << "WARNING: spectrum length mismatch for material '" << label << "': "
                      << wavelengths.size() << " wavelengths vs " << values.size()
                      << " values; interpolating over the shorter length" << std::endl;
        });
    }
    if (count == 0U) return fallback;
    if (count == 1U) return values.front();
    const float target = wavelengthNanometres(wavelength);
    if (target <= wavelengths.front()) return values.front();
    if (target >= wavelengths[count - 1U]) return values[count - 1U];
    const auto upper = std::upper_bound(wavelengths.begin(),
                                        wavelengths.begin() + static_cast<std::ptrdiff_t>(count), target);
    const std::size_t right = static_cast<std::size_t>(upper - wavelengths.begin());
    const std::size_t left = right - 1U;
    const float span = wavelengths[right] - wavelengths[left];
    const float weight = span > 0.0f ? (target - wavelengths[left]) / span : 0.0f;
    return values[left] + (values[right] - values[left]) * weight;
}
}


bool Compo::createCompOptical(std::shared_ptr<FileIO> &fileio, std::shared_ptr<RaytracingIO> &raytracingio)
{
    auto &meshio = raytracingio->m_meshio;
    meshio->spectrals.clear();
    meshio->spectralNames.clear();
    int materialId = 0;
    for (const auto& material : fileio->m_pRaytracingXml->spectralxmls) {
        meshio->spectralNames[material.spectralName] = materialId++;
        const auto values = resolveSpectrals(material, fileio->m_pRaytracingXml->sensorxml.waves,
                                             fileio->m_pRaytracingXml->sensorxml.waves,
                                             raytracingio->m_defined->m_fluspectCoeff);
        meshio->spectrals.insert(meshio->spectrals.end(), values.begin(), values.end());
    }

    meshio->thermals.clear();
    meshio->thermalNames.clear();
    int thermalId = 0;
    for (const auto &thermalxml : fileio->m_pRaytracingXml->thermalxmls) {
        meshio->thermals.push_back(Thermal{thermalxml.sunlitTemperature, thermalxml.shadedTemperature});
        meshio->thermalNames[thermalxml.thermalName] = thermalId++;
    }
    // DEFERRED[低] Bug: 仅在非温度传感器时追加 "None" 占位热属性；温度传感器且未配置任何 thermal 时 thermals 为空，后续按固定描述符绑定上传空缓冲，VoxelRT 版本则完全没有占位；理由：需与 VoxelRT 描述符/上传侧一并修改，且新增默认温度属物理常量，不属本轮允许项。
    if (!fileio->m_pRaytracingXml->sensorxml.isTemperature) {
        meshio->thermals.push_back(Thermal{300.0f, 300.0f});
        meshio->thermalNames["None"] = thermalId;
    }
    return true;
}

bool Compo::createCompOptical(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelrtIO> &modelio)
{
    auto &meshio = modelio->m_meshio;
    meshio->spectrals.clear();
    meshio->spectralNames.clear();
    int materialId = 0;
    for (const auto& material : fileio->m_pVoxelrtXml->spectralxmls) {
        meshio->spectralNames[material.spectralName] = materialId++;
        const auto values = resolveSpectrals(material, fileio->m_pVoxelrtXml->sensorxml.waves,
                                             fileio->m_pVoxelrtXml->sensorxml.waves,
                                             modelio->m_defined->m_fluspectCoeff);
        meshio->spectrals.insert(meshio->spectrals.end(), values.begin(), values.end());
    }

    meshio->thermals.clear();
    meshio->thermalNames.clear();
    int thermalId = 0;
    for (const auto &thermalxml : fileio->m_pVoxelrtXml->thermalxmls) {
        meshio->thermals.push_back(Thermal{thermalxml.sunlitTemperature, thermalxml.shadedTemperature});
        meshio->thermalNames[thermalxml.thermalName] = thermalId++;
    }

    meshio->canopies.clear();
    meshio->canopyNames.clear();
    int canopyId = 0;
    for (const auto &canopyxml : fileio->m_pVoxelrtXml->canopyxmls) {
        meshio->canopies.push_back(canopyxml.canopy);
        meshio->canopyNames[canopyxml.canopyName] = canopyId++;
    }
    return true;
}

float Compo::calctav(float alfa,float nr)
{
    float rd,pi,n2,np,nm,a,k,sa,b1,b2,b,a3,b3,tp1,tp2,tp3,tp4,ts,tp5,tp,tav;
    pi=3.1415926;
    rd          = pi/180;
    n2          = nr*nr;
    np          = n2+1;
    nm          = n2-1;
    a           = (nr+1)*(nr+1)/2;
    k           = -(n2-1)*(n2-1)/4;
    sa          = sin(alfa*rd);
    b1 = 0;
    if(alfa !=90) b1          = sqrt((sa*sa-np/2)*(sa*sa-np/2)+k);
    b2          = sa*sa-np/2;
    b           = b1-b2;
    b3          = b*b*b;
    a3          = a*a*a;
    ts          = (k*k/(6*b3)+k/b-b/2)-(k*k/(6*a3)+k/a-a/2);

    tp1         = -2*n2*(b-a)/(np*np);
    tp2         = -2*n2*np*log(b/a)/(nm*nm);
    tp3         = n2*(1/b-1/a)/2;
    tp4         = 16*n2*n2*(n2*n2+1)*log((2*np*b-nm*nm)/(2*np*a-nm*nm))/(np*np*np*nm*nm);
    tp5         = 16*n2*n2*n2*(1/(2*np*b-nm*nm)-1/(2*np*a-nm*nm))/(np*np*np);
    tp          = tp1+tp2+tp3+tp4+tp5;
    tav         = (ts+tp)/(2*sa*sa);

    return tav;
}

void Compo::fluspect(OptCoeff fluspectCoeff, FluspectParam fluspectParam, std::vector<Spectral>& spectrals)
{
    float Cab         = fluspectParam.Cab;
    float Cw          = fluspectParam.Cw;
    float Cdm         = fluspectParam.Cdm;
    float Cs          = fluspectParam.Cs;
    float N           = fluspectParam.N;
    std::vector<float> &nr_         = fluspectCoeff.nr_;
    std::vector<float> &kdm_        = fluspectCoeff.kdm_;
    std::vector<float> &kab_         = fluspectCoeff.kab_;
    std::vector<float> &kw_         = fluspectCoeff.kw_;
    std::vector<float> &ks_         = fluspectCoeff.ks_;
    std::vector<float> &phiI_       = fluspectCoeff.phiI_;
    std::vector<float> &phiII_      = fluspectCoeff.phiII_;

    float nr, Kdm, Kab, Kw, Ks, phiII, phiI, Kall, t1, t2, tau, kChlrel,
            t12, r12, t21, r21, denom, Ra, r, D, rq, tq, a, b, bNm1, bN2_g, a2, Rsub, Tsub,
            s, j, talf, ralf, tt, Ta;

    // FIXED[中] 数值: 未校验 PROSPECT 参数，N<=0 除零、N<1 使 pow(b,N-1)/Tsub 非物理、浓度为负使 Kall<0 令 expint 发散并产生被 bounded() 静默替换的 NaN；已修复：入口校验 N>=1 且 Cab/Cw/Cdm/Cs 为有限非负值，非法时抛 std::runtime_error 并报出各参数（OptCoeff 仍按值传递，改 const 引用属接口改动，未做）。
    if (!std::isfinite(N) || N < 1.0f
        || !std::isfinite(Cab) || Cab < 0.0f
        || !std::isfinite(Cw) || Cw < 0.0f
        || !std::isfinite(Cdm) || Cdm < 0.0f
        || !std::isfinite(Cs) || Cs < 0.0f) {
        throw std::runtime_error("Invalid PROSPECT parameters: N=" + std::to_string(N)
            + ", Cab=" + std::to_string(Cab) + ", Cw=" + std::to_string(Cw)
            + ", Cdm=" + std::to_string(Cdm) + ", Cs=" + std::to_string(Cs));
    }
    spectrals.clear();
    for(int i=0; i<N1; i++)
    {
        nr          = nr_[i];
        //
        Kdm         = kdm_[i];
        Kab         = kab_[i];
        Kw          = kw_[i];
        Ks          = ks_[i];
        phiI        = phiI_[i];
        phiII       = phiII_[i];

        // PROSPECT calculations
        Kall        = (Cab*Kab + Cdm*Kdm + Cw*Kw  + Cs*Ks)/N;

        // Non-conservative scattering (normal case)
        t1          = (1-Kall)*exp(-Kall);
        t2          = Kall*Kall*Utils::expint(Kall);
        tau         = 1;
        if(Kall > 0) tau      = t1+t2;
        kChlrel     = 0;
        if(Kall > 0) kChlrel  = Cab*Kab/(Kall*N);

        talf        = calctav(59,nr);
        ralf        = 1-talf;
        t12         = calctav(90,nr);
        r12         = 1-t12;
        t21         = t12/(nr*nr);
        r21         = 1-t21;

        // top layer
        denom       = 1-r21*r21*tau*tau;
        Ta          = talf*tau*t21/denom;
        Ra          = ralf+r21*tau*Ta;

        // deeper layers
        tt           = t12*tau*t21/denom;
        r           = r12+r21*tau*tt;

        // Stokes equations to compute properties of next N-1 layers (N real)
        // Normal case

        D           = sqrt((1+r+tt)*(1+r-tt)*(1-r+tt)*(1-r-tt));
        rq          = r*r;
        tq          = tt*tt;
        a           = (1+rq-tq+D)/(2*r);
        b           = (1-rq+tq+D)/(2*tt);

        bNm1        = pow(b,(N-1));
        bN2_g         = bNm1*bNm1;
        a2          = a*a;
        denom       = a2*bN2_g-1;
        Rsub        = a*(bN2_g-1)/denom;
        Tsub        = bNm1*(a2-1)/denom;

        s           = r/tt;                             // Conservative scattering (CS)
        if(Kall>0) s        = 2*a/(a*a-1)*log(b);   // Normal case overwrites CS case

        //			Case of zero absorption

        if(r+tt>=1)
        {
            Tsub     = tt/(tt+(1-tt)*(N-1));
            Rsub	    = 1-Tsub;
        }
        // Reflectance and transmittance of the leaf: combine top layer with next N-1 layers
        denom       = 1-Rsub*r;
        Spectral spectral;
        spectral.transmittance        = Ta*Tsub/denom;
        spectral.reflectance      = Ra+Ta*Rsub*tt/denom;
        spectrals.push_back(spectral);
    }
}

std::vector<Spectral> Compo::resolveSpectrals(const SpectralXml& material,
                                              const std::vector<float>& outputWavelengths,
                                              const std::vector<float>& customWavelengths,
                                              const OptCoeff& coefficients)
{
    std::vector<float> sourceWavelengths;
    std::vector<float> sourceReflectance;
    std::vector<float> sourceTransmittance;

    if (material.type == spectralType::PROSPECT || material.type == spectralType::BSM) {
        const bool prospectReady = coefficients.wl_.size() >= N1 &&
            coefficients.nr_.size() >= N1 && coefficients.kdm_.size() >= N1 &&
            coefficients.kab_.size() >= N1 && coefficients.kw_.size() >= N1 &&
            coefficients.ks_.size() >= N1 && coefficients.phiI_.size() >= N1 &&
            coefficients.phiII_.size() >= N1;
        const bool bsmReady = coefficients.wl_.size() >= N1 &&
            coefficients.gsv1_.size() >= N1 && coefficients.gsv2_.size() >= N1 &&
            coefficients.gsv3_.size() >= N1 && coefficients.kw_.size() >= N1 &&
            coefficients.nw_.size() >= N1;
        if ((material.type == spectralType::PROSPECT && !prospectReady) ||
            (material.type == spectralType::BSM && !bsmReady)) {
            throw std::runtime_error("Optical model coefficients are unavailable for material: " + material.spectralName);
        }
        std::vector<Spectral> modeled;
        if (material.type == spectralType::PROSPECT)
            fluspect(coefficients, material.fp, modeled);
        else
            bsm(coefficients, material.bsm, modeled);
        const std::size_t count = std::min(modeled.size(), coefficients.wl_.size());
        sourceWavelengths.reserve(count);
        sourceReflectance.reserve(count);
        sourceTransmittance.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            sourceWavelengths.push_back(wavelengthNanometres(coefficients.wl_[index]));
            sourceReflectance.push_back(modeled[index].reflectance);
            sourceTransmittance.push_back(modeled[index].transmittance);
        }
    } else if (material.type == spectralType::OTHER) {
        if (material.path.empty()) throw std::runtime_error("Imported spectrum path is empty for material: " + material.spectralName);
        readSpectrumFile(material.path, sourceWavelengths, sourceReflectance, sourceTransmittance);
    } else {
        sourceWavelengths.reserve(customWavelengths.size());
        for (float wavelength : customWavelengths)
            sourceWavelengths.push_back(wavelengthNanometres(wavelength));
        sourceReflectance = material.reflectances;
        sourceTransmittance = material.transmittance;
    }

    std::vector<Spectral> resolved;
    resolved.reserve(outputWavelengths.size());
    for (float wavelength : outputWavelengths) {
        if (wavelengthMicrometres(wavelength) > 2.5f) {
            // Soil TIR optics follow the material water content: the reflectance
            // falls and the emissivity rises as the soil gets wetter. Only the
            // BSM model carries a soil water content, so leaves, custom panels
            // and imported spectra keep their flat TIR value.
            const float reflectance = material.type == spectralType::BSM
                ? soiloptics::tirReflectance(material.refl_tir, material.tau_tir,
                                             material.refl_tir_wet, material.bsm.SMC)
                : bounded(material.refl_tir, 0.05f);
            resolved.push_back(Spectral{reflectance,
                std::min(bounded(material.tau_tir, 0.0f), 1.0f - reflectance)});
            continue;
        }
        const float reflectance = bounded(interpolateValue(
            sourceWavelengths, sourceReflectance, wavelength, 0.2f, material.spectralName), 0.2f);
        const float transmittance = std::min(bounded(interpolateValue(
            sourceWavelengths, sourceTransmittance, wavelength, 0.0f, material.spectralName), 0.0f),
            1.0f - reflectance);
        resolved.push_back(Spectral{reflectance, transmittance,
            material.specularEnabled ? material.specularFraction : 0.0f,
            material.specularRoughness});
    }
    return resolved;
}


bool Compo::createCompProperty(std::shared_ptr<FileIO> &fileio, std::shared_ptr<VoxelebIO> &modelio) {

    auto & meshio = modelio->m_meshio;
    auto & definedio = modelio->m_defined;
    meshio->thermals.clear();
    meshio->thermalNames.clear();
    int thermalId = 0;
    for (const auto &thermalxml : fileio->m_pVoxelebXml->thermalxmls) {
        meshio->thermals.push_back(Thermal{
            thermalxml.sunlitTemperature,
            thermalxml.shadedTemperature
        });
        meshio->thermalNames.insert({thermalxml.thermalName, thermalId++});
    }
    if (meshio->thermals.empty()) {
        meshio->thermals.push_back(Thermal{300.0f, 300.0f});
        meshio->thermalNames.insert({"__default_thermal", 0});
    }

    meshio->spectrals.clear();
    meshio->fixedSpectrals.clear();
    meshio->spectralNames.clear();
    std::vector<float> fixedWavelengths;
    fixedWavelengths.reserve(N1 + 1);
    for (int index = 0; index < N1; ++index) fixedWavelengths.push_back(400.0f + index);
    fixedWavelengths.push_back(10500.0f);
    int id = 0;
    for (const auto& spectralxml : fileio->m_pVoxelebXml->spectralxmls) {
        const auto sensorValues = resolveSpectrals(
            spectralxml, fileio->m_pVoxelebXml->sensorxml.waves,
            fileio->m_pVoxelebXml->sensorxml.waves, definedio->m_fluspectCoeff);
        const auto fixedValues = resolveSpectrals(
            spectralxml, fixedWavelengths, fileio->m_pVoxelebXml->sensorxml.waves,
            definedio->m_fluspectCoeff);
        meshio->spectrals.insert(meshio->spectrals.end(), sensorValues.begin(), sensorValues.end());
        meshio->fixedSpectrals.insert(meshio->fixedSpectrals.end(), fixedValues.begin(), fixedValues.end());
        meshio->spectralNames.insert({spectralxml.spectralName, id++});
    }

    // DEFERRED[低] Bug: canopies/leafbios/soilsets/watersets 及其 Names 映射在追加前未 clear（thermals/spectrals 却清了），重复调用会产生重复条目且 map.insert 不覆盖旧 ID；理由：属 createCompProperty 生命周期/初始化重构，不属本轮允许的缺陷类别，未改。
    id = 0;
    for(auto &canopyxml: fileio->m_pVoxelebXml->canopyxmls){
        meshio->canopies.push_back(canopyxml.canopy);
        meshio->canopyNames.insert({canopyxml.canopyName,id});
        id++;
    }

    // The VoxelEB descriptor layout is fixed, so water-only scenes still need
    // valid placeholder buffers for resources unused by water shaders.
    if (meshio->canopies.empty()) {
        meshio->canopies.push_back(Canopy{});
        meshio->canopyNames.insert({"__default_canopy", 0});
    }

    int id1 = 0,id2 = 0,id3 = 0;
    for(auto &propxml: fileio->m_pVoxelebXml->propxmls){

        if(propxml.type == Type::VEGETATION) {
            meshio->leafbios.push_back(propxml.leafbio);
            meshio->leafbioNames.insert({propxml.name, id1});
            id1++;
        }
        else if(propxml.type == Type::SOIL || propxml.type == Type::BUILDING){
            meshio->soilsets.push_back(propxml.soilset);
            meshio->soilsetNames.insert({propxml.name, id2});
            id2++;
        }
        else if(propxml.type == Type::WATER){
            meshio->watersets.push_back(propxml.waterset);
            meshio->watersetNames.insert({propxml.name, id3});
            id3++;
        }
    }

    if (meshio->leafbios.empty()) {
        meshio->leafbios.push_back(LeafBio{});
        meshio->leafbioNames.insert({"__default_leaf", 0});
    }
    if (meshio->soilsets.empty()) {
        meshio->soilsets.push_back(SoilSet{});
        meshio->soilsetNames.insert({"__default_soil", 0});
    }

    // Descriptor bindings are fixed; keep compatibility with scenes that have no water.
    if (meshio->watersets.empty()) {
        meshio->watersets.push_back(WaterSet{0.0f, 4.186e6f, 1.0f, 1.0f});
        meshio->watersetNames.insert({"water", 0});
    }

    // DEFERRED[低] 可维护性: 成功路径返回 false，与 createCompOptical 返回 true 相反；理由：唯一调用点 voxeleb.cpp:124 未检查返回值，改返回值属接口语义调整，不属本轮允许项，未改。
    return false;
}



void Compo::bsm(OptCoeff bsmCoeff,BSMParam bsm,std::vector<Spectral>& spectrals){

    std::vector<float> &gsv1_ = bsmCoeff.gsv1_;
    std::vector<float> &gsv2_ = bsmCoeff.gsv2_;
    std::vector<float> &gsv3_ = bsmCoeff.gsv3_;
    std::vector<float> &kw_ = bsmCoeff.kw_;
    std::vector<float> &nw_ = bsmCoeff.nw_;

    float B = bsm.BSMBrightness;
    float lat = bsm.BSMlat;
    float lon = bsm.BSMlon;
    // Accept both percentage input (25) and fractional input (0.25).
    float SMC = bsm.SMC > 1.0f ? bsm.SMC / 100.0f : bsm.SMC;
    SMC = std::clamp(SMC, 0.0f, 1.0f);
    float SMCp = 0.25;
    float film = 0.015;
    float rd = 3.1415926/180.0;

    float f1 = B * sin(lat*rd);
    float f2 = B * cos(lat*rd)* sin(lon*rd);
    float f3 = B * cos(lat*rd) * cos(lon*rd);

    // FIXED[中] Bug: 循环以 gsv1_.size() 为界同时索引 gsv2_/gsv3_/kw_/nw_，gsv1_ 更长时越界读；已修复：上界改为 gsv1_/gsv2_/gsv3_/kw_/nw_ 长度的最小值，两个循环共用该上界。
    const std::size_t count = std::min({gsv1_.size(), gsv2_.size(), gsv3_.size(),
                                        kw_.size(), nw_.size()});
    std::vector<float> rdry_,rwet_;
    for(std::size_t k =0;k<count;k++){
        rdry_.push_back(gsv1_[k]*f1+gsv2_[k]*f2+gsv3_[k]*f3);
    }

    for(std::size_t k =0;k<count;k++) {
        float rdry = rdry_[k];
        float tw = exp(-kw_[k] * film);
        float rbac = 1 - (1-rdry_[k]) * (rdry_[k] * calctav(90,2.0/nw_[k]) / calctav(90,2.0) + 1-rdry_[k]);
        float p = 1-calctav(90,nw_[k])/nw_[k]/nw_[k];
        float Rw = 1-calctav(40,nw_[k]);
        float   Radd   = (1-Rw) * (1-p) * rbac /(1 - p * rbac);
        // DEFERRED[中] 物理: SCOPE BSM 在 mu<=0（土壤含水量 <5%）时直接取 rwet=rdry；这里未处理，SMC<0.05 时 mu<0 → fdry=exp(-mu)>1、1-fdry<0，湿土反射率被外推成非物理值；理由：mu<=0 时取 rwet=rdry 属物理语义变更，明确禁止本轮修改。
        float     mu  = (SMC - 0.05)/ SMCp;
        float fdry = exp(-mu);
        float  fmul = (exp(tw * mu) - 1) * fdry;
        float rwet = rdry * fdry + Rw * (1 - fdry) + Radd * fmul;
     //   rwet_.push_back(rwet);
         Spectral spectral{rwet,0};
         spectrals.push_back(spectral);
    }

}
