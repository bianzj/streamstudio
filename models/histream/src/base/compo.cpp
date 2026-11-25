// ==========================================================================
// 文件：compo.cpp
// 功能：把工程 XML 中的光学/热学/冠层属性（材质光谱、热属性、冠层、叶/土/水参数）整理成各模式
//       meshio 里按名称索引的数组，并实现叶片 PROSPECT（fluspect）与土壤 BSM 两个光谱模型。
// 主要模块：
//   1. 匿名命名空间内部辅助：withLeafAngles（SAIL 叶倾角频率与 diffuseG）、bounded、
//      波长单位转换、光谱文本文件读取（readSpectrumFile）、按波长线性插值（interpolateValue）
//   2. createCompOptical（RayTracing / VoxelRT 两个重载）：材质光谱 + 热属性（+ VoxelRT 冠层）
//   3. calctav / fluspect：PROSPECT 叶片反射率/透射率
//   4. resolveSpectrals：把 PROSPECT/BSM/导入文件/面板数值统一重采样到目标波长，并做限幅与
//      热红外平坦值处理
//   5. createCompProperty（VoxelEB）：热属性、传感器波长与固定波长两套光谱、冠层与叶/土/水属性
//   6. bsm：土壤 BSM（Brightness-Shape-Moisture）干/湿反射率
// 数据流：FileIO 解析出的 *Xml 结构（spectralxmls、thermalxmls、canopyxmls、propxmls）
//       → resolveSpectrals 等 → 写入 meshio->spectrals / thermals / canopies / leafbios ...
//       → 由各模式把这些数组上传到 GPU 缓冲。名称到下标的映射存于 *Names 字典。
// 与其他文件关系：compo.h 声明；structs.h/structs_cg.h 定义 Spectral、Thermal、OptCoeff 等；
//       soil_tir_emissivity.h 提供土壤热红外反射率；shader/lidf_common.h 提供叶倾角分布函数。
// 约定：光谱数组按“材质主序、波长次序”扁平存放（每个材质占 sensorxml.waves.size() 个元素）；
//       波长 >50 视为 nm，<=50 视为 μm；热红外波段指波长 >2.5 μm；
//       N1(=2001) 是 PROSPECT/BSM 系数表的波长点数（见 structs.h）。
// ==========================================================================

//
// Created by admin on 2024/1/24.
//

#include "compo.h"

#include "soil_tir_emissivity.h"
#include "../../shader/lidf_common.h"

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

// 模块：本文件内部辅助函数（匿名命名空间，不对外暴露）。
namespace {
// 作用：由冠层的 LIDFa/LIDFb 填写 13 个 SAIL 倾角类频率 lidf 与 32 个投影值 diffuseG。
// 输入：冠层参数副本 canopy、冠层名 name（仅用于警告信息）；输出：更新后的 Canopy（按值返回）。
// 说明：diffuseG[k] 是 G 在 |cosθ|=(2k+1)/64（把 [0,1] 等分 32 段的段中心）处的取值，
//       供 GPU 做漫射项积分；下面几种“旧行为”分支保证旧工程结果不变。
// Derive the SAIL leaf inclination class frequencies of a turbid canopy.
// They define G(Omega) and the area scattering phase function on the GPU.
// lidfValid stays 0 (constant G, spherical-shaped Gamma scaled by G/0.5) for
//   * rigid bodies and participating media (structureType != 0),
//   * LIDFa = LIDFb = 0, the "not specified" placeholder of older projects
//     and presets: the user G is used as before,
//   * LIDFa > 1, the SCOPE convention for the exact spherical distribution:
//     G = 0.5 and the Ross closed form, i.e. exactly the former model,
//   * parameters outside the SAIL domain |a| + |b| <= 1 (warning).
Canopy withLeafAngles(Canopy canopy, const std::string& name)
{
    // 步骤 1：先复位为“常数 G”状态（lidfValid=0，lidf 清零，diffuseG 全取用户 G）。
    canopy.lidfValid = 0;
    for (float& value : canopy.lidf) value = 0.0f;
    if (canopy.structureType == 0 && std::isfinite(canopy.LIDFa) && canopy.LIDFa > 1.0f)
        canopy.G = 0.5f;
    for (float& value : canopy.diffuseG) value = canopy.G;
    // 步骤 2：刚体/参与介质，以及 LIDFa=LIDFb=0 或 LIDFa>1 的情形，保留常数 G，直接返回。
    if (canopy.structureType != 0) return canopy;
    if (canopy.LIDFa == 0.0f && canopy.LIDFb == 0.0f) return canopy;
    if (std::isfinite(canopy.LIDFa) && canopy.LIDFa > 1.0f) return canopy;
    // 步骤 3：参数在 SAIL 域内则启用 LIDF，并按 G(mu) 预先算好 32 个 diffuseG；否则警告并回退常数 G。
    if (lidf::sailLeafAngleFrequencies(canopy.LIDFa, canopy.LIDFb, canopy.lidf)) {
        canopy.lidfValid = 1;
        for (int k = 0; k < 32; ++k)
            canopy.diffuseG[k] = lidf::lidfProjection(canopy.lidf, (2.0f * k + 1.0f) / 64.0f);
    } else {
        std::cerr << "Warning: canopy '" << name << "' LIDFa=" << canopy.LIDFa
                  << " LIDFb=" << canopy.LIDFb
                  << " is outside the SAIL domain (|a|+|b|<=1); using constant G="
                  << canopy.G << " with a spherical phase function." << std::endl;
    }
    return canopy;
}

// 作用：把值限幅到 [0,1]；非有限值（NaN/Inf）先替换为 fallback。用于反射率/透射率。
float bounded(float value, float fallback)
{
    return std::clamp(std::isfinite(value) ? value : fallback, 0.0f, 1.0f);
}

// 作用：波长单位归一：>50 视为 nm 转 μm，否则认为已是 μm（兼容旧工程用 μm 写波长）。
float wavelengthMicrometres(float wavelength)
{
    return wavelength > 50.0f ? wavelength / 1000.0f : wavelength;
}

// 作用：波长单位归一：(0,50] 视为 μm 转 nm，否则认为已是 nm。
float wavelengthNanometres(float wavelength)
{
    return wavelength > 0.0f && wavelength <= 50.0f ? wavelength * 1000.0f : wavelength;
}

// 作用：把一行文本按逗号/分号/空白切分成浮点数；忽略非有限值，遇到非数字停止。
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

// 作用：读取导入的光谱 TXT 文件。格式：# 开头为注释；第 1 行波长，第 2 行反射率，
//       可选第 3 行透射率（各行元素个数必须相同，至少 2 个波长）。
// 输出：按波长升序排好的 wavelengths（nm）、reflectance、transmittance（无第 3 行时为 0）。
// 异常：文件打不开、行数/长度不符、波长重复时抛 std::runtime_error。
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

// 作用：在 (wavelengths[nm], values) 上对目标波长做线性插值。
// 边界：目标波长超出范围取端点值（不外推）；values 为空返回 fallback；
//       仅 1 个点则返回该点；两数组长度不一致时取较短长度并只警告一次。
float interpolateValue(const std::vector<float>& wavelengths,
                       const std::vector<float>& values, float wavelength, float fallback,
                       const std::string& label)
{
    if (values.empty()) return fallback;
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


// 模块：RayTracing 模式的光学与热属性准备。
// 输入：fileio 里的 RaytracingXml（spectralxmls、thermalxmls、sensorxml.waves）。
// 输出：raytracingio->m_meshio 的 spectrals（材质主序、每材质 waves.size() 个）、spectralNames、
//       thermals、thermalNames。
// 说明：非温度传感器时额外追加名为 "None" 的 300 K 占位热属性，保证描述符绑定的缓冲非空。
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

// 模块：VoxelRT 模式的光学、热属性与冠层准备。
// 输入：VoxelrtXml；输出：modelio->m_meshio 的 spectrals/thermals/canopies 及对应 Names 字典。
// 说明：与 RayTracing 重载相比，多了冠层（经 withLeafAngles 填写叶倾角分布）、且无 "None" 占位热属性。
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
        meshio->canopies.push_back(withLeafAngles(canopyxml.canopy, canopyxml.canopyName));
        meshio->canopyNames[canopyxml.canopyName] = canopyId++;
    }
    return true;
}

// 作用：PROSPECT 的 tav 函数——折射率为 nr 的介质平面界面，对入射角 0~alfa（度）范围内
//       各向同性入射光的平均透射率（Allen 等；PROSPECT 源码中的同名函数，公式照搬）。
// 输入：alfa（度，90 表示半球各向同性）、nr（折射率，无量纲）；输出：平均透射率 tav。
// 说明：alfa==90 时 b1 取 0，避免平方根内出现数值误差；pi 使用 7 位近似值，与原 PROSPECT 代码一致。
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

// 作用：PROSPECT 叶片光谱模型，计算 N1 个波长点的叶片反射率与透射率。
// 输入：系数表 fluspectCoeff（折射率 nr_，吸收系数 kab_/kdm_/kw_/ks_）；
//       FluspectParam：Cab（叶绿素）、Cw（水）、Cdm（干物质）、Cs（棕色素）、N（叶片结构参数）。
// 输出：spectrals（先清空，再逐波长追加 {reflectance, transmittance}）。
// 异常：N<1 或任一参数为负/非有限时抛 std::runtime_error。
// 说明：荧光系数 phiI_/phiII_ 虽被读取，但本函数没有计算荧光（kChlrel、s 等中间量也未输出）。
void Compo::fluspect(const OptCoeff& fluspectCoeff, FluspectParam fluspectParam, std::vector<Spectral>& spectrals)
{
    float Cab         = fluspectParam.Cab;
    float Cw          = fluspectParam.Cw;
    float Cdm         = fluspectParam.Cdm;
    float Cs          = fluspectParam.Cs;
    float N           = fluspectParam.N;
    const std::vector<float> &nr_         = fluspectCoeff.nr_;
    const std::vector<float> &kdm_        = fluspectCoeff.kdm_;
    const std::vector<float> &kab_         = fluspectCoeff.kab_;
    const std::vector<float> &kw_         = fluspectCoeff.kw_;
    const std::vector<float> &ks_         = fluspectCoeff.ks_;
    const std::vector<float> &phiI_       = fluspectCoeff.phiI_;
    const std::vector<float> &phiII_      = fluspectCoeff.phiII_;

    float nr, Kdm, Kab, Kw, Ks, phiII, phiI, Kall, t1, t2, tau, kChlrel,
            t12, r12, t21, r21, denom, Ra, r, D, rq, tq, a, b, bNm1, bN2_g, a2, Rsub, Tsub,
            s, j, talf, ralf, tt, Ta;

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
        // 步骤 1：总吸收系数 Kall（对 N 层平均），并求单层透射 tau（含指数积分项 expint）；
        //         Kall<=0 时 tau=1（无吸收）。
        Kall        = (Cab*Kab + Cdm*Kdm + Cw*Kw  + Cs*Ks)/N;

        // Non-conservative scattering (normal case)
        t1          = (1-Kall)*exp(-Kall);
        t2          = Kall*Kall*Utils::expint(Kall);
        tau         = 1;
        if(Kall > 0) tau      = t1+t2;
        kChlrel     = 0;
        if(Kall > 0) kChlrel  = Cab*Kab/(Kall*N);

        // 步骤 2：界面透射/反射：talf 为 59° 入射（近似太阳光方向）、t12/r12 为 90° 半球入射（外→内）、
        //         t21/r21 为内→外（t12/n^2）。
        talf        = calctav(59,nr);
        ralf        = 1-talf;
        t12         = calctav(90,nr);
        r12         = 1-t12;
        t21         = t12/(nr*nr);
        r21         = 1-t21;

        // top layer
        // 步骤 3：第一层（顶层）的反射 Ra/透射 Ta，以及后续层的单层反射 r、透射 tt（含层内多次反射）。
        denom       = 1-r21*r21*tau*tau;
        Ta          = talf*tau*t21/denom;
        Ra          = ralf+r21*tau*Ta;

        // deeper layers
        tt           = t12*tau*t21/denom;
        r           = r12+r21*tau*tt;

        // Stokes equations to compute properties of next N-1 layers (N real)
        // Normal case

        // 步骤 4：用 Stokes 方程求余下 N-1 层叠加的反射 Rsub 与透射 Tsub（N 可为实数）。
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

        // 步骤 5：零吸收（保守散射）极限：公式在 r+tt>=1 时奇异，改用解析极限式。
        if(r+tt>=1)
        {
            Tsub     = tt/(tt+(1-tt)*(N-1));
            Rsub	    = 1-Tsub;
        }
        // Reflectance and transmittance of the leaf: combine top layer with next N-1 layers
        // 步骤 6：顶层与其余 N-1 层合成，得到整片叶的反射率与透射率。
        denom       = 1-Rsub*r;
        Spectral spectral;
        spectral.transmittance        = Ta*Tsub/denom;
        spectral.reflectance      = Ra+Ta*Rsub*tt/denom;
        spectrals.push_back(spectral);
    }
}

// 作用：把一个材质的光谱来源统一解析并重采样到 outputWavelengths，得到每个波长的 Spectral。
// 输入：material（type 为 PROSPECT/BSM/OTHER（导入文件）/面板数值）、outputWavelengths（目标波长，
//       nm 或 μm）、customWavelengths（面板数值对应的波长）、coefficients（PROSPECT/BSM 系数表）。
// 输出：与 outputWavelengths 等长的 vector<Spectral>；系数缺失或导入路径为空时抛异常。
// 约定：反射率缺省 0.2、透射率缺省 0（兼容旧工程缺失光谱）；透射率限制为不超过 1-反射率。
std::vector<Spectral> Compo::resolveSpectrals(const SpectralXml& material,
                                              const std::vector<float>& outputWavelengths,
                                              const std::vector<float>& customWavelengths,
                                              const OptCoeff& coefficients)
{
    // 步骤 1：根据材质类型取得“源光谱”（波长 nm、反射率、透射率三个数组）。
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

    // 步骤 2：逐目标波长重采样。
    std::vector<Spectral> resolved;
    resolved.reserve(outputWavelengths.size());
    for (float wavelength : outputWavelengths) {
        // 热红外（>2.5 μm）不做光谱插值，用材质的平坦热红外反射率/透射率；
        // BSM 土壤的反射率随含水量变化（tirReflectance），其余材质用 refl_tir（缺省 0.05）。
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
        // 太阳反射波段：线性插值并限幅；高光比例/粗糙度仅在材质启用镜面时传入。
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


// 模块：VoxelEB 模式的属性准备（热属性、光谱、冠层、叶/土/水参数）。
// 输入：VoxelebXml；输出：modelio->m_meshio 的 thermals、spectrals（传感器波长）、
//       fixedSpectrals（固定波长 400..2400 nm 每 1 nm + 10500 nm，用于能量平衡的短波/长波，待核对用途）、
//       canopies、leafbios、soilsets、watersets 及对应 Names 字典。
// 说明：VoxelEB 描述符布局固定，各类属性为空时追加占位项，避免空缓冲。
// 返回：成功路径返回 false（与 createCompOptical 相反，调用方不检查，见下方 DEFERRED 备注）。
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
    // 步骤 1：热属性；没有任何热属性时放一个 300 K 的占位。
    if (meshio->thermals.empty()) {
        meshio->thermals.push_back(Thermal{300.0f, 300.0f});
        meshio->thermalNames.insert({"__default_thermal", 0});
    }

    meshio->spectrals.clear();
    meshio->fixedSpectrals.clear();
    meshio->spectralNames.clear();
    // 步骤 2：光谱——同一材质分别在传感器波长与固定波长（400..2400 nm 逐 1 nm，加 10500 nm）上解析。
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
    // 步骤 3：冠层（含叶倾角分布）。
    for(auto &canopyxml: fileio->m_pVoxelebXml->canopyxmls){
        meshio->canopies.push_back(withLeafAngles(canopyxml.canopy, canopyxml.canopyName));
        meshio->canopyNames.insert({canopyxml.canopyName,id});
        id++;
    }

    // The VoxelEB descriptor layout is fixed, so water-only scenes still need
    // valid placeholder buffers for resources unused by water shaders.
    if (meshio->canopies.empty()) {
        meshio->canopies.push_back(Canopy{});
        meshio->canopyNames.insert({"__default_canopy", 0});
    }

    // 步骤 4：按类型把属性分到三套数组：植被→leafbios，土壤/建筑→soilsets，水体→watersets；
    //         id1/id2/id3 分别是各数组内的下标。
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



// 作用：土壤 BSM（Brightness-Shape-Moisture，Verhoef 等，参考 SCOPE 实现）湿土反射率。
// 输入：bsmCoeff（GSV 基函数 gsv1/2/3_、水吸收 kw_、水折射率 nw_）；
//       BSMParam：BSMBrightness、BSMlat/BSMlon（光谱形状的球坐标，度）、SMC（土壤含水量）。
// 输出：spectrals 末尾追加各波长 {rwet, 0}（土壤不透射）；调用方负责事先清空。
// 方法：干土光谱 = GSV 基函数的加权和；湿土用水膜（film 厚度）的 Lekner–Dorf 型模型（待核对）。
void Compo::bsm(const OptCoeff& bsmCoeff,BSMParam bsm,std::vector<Spectral>& spectrals){

    const std::vector<float> &gsv1_ = bsmCoeff.gsv1_;
    const std::vector<float> &gsv2_ = bsmCoeff.gsv2_;
    const std::vector<float> &gsv3_ = bsmCoeff.gsv3_;
    const std::vector<float> &kw_ = bsmCoeff.kw_;
    const std::vector<float> &nw_ = bsmCoeff.nw_;

    float B = bsm.BSMBrightness;
    float lat = bsm.BSMlat;
    float lon = bsm.BSMlon;
    // Accept both percentage input (25) and fractional input (0.25).
    // 步骤 1：SMC 同时接受百分数与小数，统一成 [0,1] 小数。
    float SMC = bsm.SMC > 1.0f ? bsm.SMC / 100.0f : bsm.SMC;
    SMC = std::clamp(SMC, 0.0f, 1.0f);
    float SMCp = 0.25;
    float film = 0.015;
    float rd = 3.1415926/180.0;

    // 步骤 2：由 (B, lat, lon) 得到三个 GSV 权重 f1/f2/f3，再逐波长合成干土反射率 rdry。
    float f1 = B * sin(lat*rd);
    float f2 = B * cos(lat*rd)* sin(lon*rd);
    float f3 = B * cos(lat*rd) * cos(lon*rd);

    const std::size_t count = std::min({gsv1_.size(), gsv2_.size(), gsv3_.size(),
                                        kw_.size(), nw_.size()});
    std::vector<float> rdry_,rwet_;
    for(std::size_t k =0;k<count;k++){
        rdry_.push_back(gsv1_[k]*f1+gsv2_[k]*f2+gsv3_[k]*f3);
    }

    for(std::size_t k =0;k<count;k++) {
        float rdry = rdry_[k];
        // 步骤 3：逐波长求湿土反射率：tw 为水膜透过率，rbac/p/Rw/Radd 为水–土壤界面多次反射项，
        //         mu 为含水量相对 SMCp(=0.25) 的归一化量，fdry/fmul 为干/湿混合权重。
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
