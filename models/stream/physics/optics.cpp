#include "optics.h"
#include "constants.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace stream {
namespace {

std::string displayPath(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace

OptCoeff readOptCoeff(const std::filesystem::path& file)
{
    std::ifstream input(file);
    if (!input) throw std::runtime_error("找不到 PROSPECT/BSM 系数：" + displayPath(file));
    std::string line;
    std::getline(input, line);
    OptCoeff coeff;
    std::vector<float>* columns[] = {&coeff.wl, &coeff.nr, &coeff.kab, &coeff.kca, &coeff.ks, &coeff.kw, &coeff.kdm,
        &coeff.phiI, &coeff.phiII, &coeff.kcaV, &coeff.kcaZ, &coeff.kcant, &coeff.kcaV2, &coeff.phi,
        &coeff.gsv1, &coeff.gsv2, &coeff.gsv3, &coeff.nw};
    while (std::getline(input, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::vector<std::string> fields;
        std::stringstream stream(line);
        std::string field;
        while (std::getline(stream, field, ',')) fields.push_back(field);
        if (fields.size() < 18) throw std::runtime_error("PROSPECT/BSM 系数列数不足：" + displayPath(file));
        for (int column = 0; column < 18; ++column) columns[column]->push_back(std::stof(fields[column]));
    }
    if (coeff.wl.size() != kSpectralCount) throw std::runtime_error("PROSPECT/BSM 系数须覆盖 400–2400 nm 共 2001 行");
    return coeff;
}

float expint(float x)
{
    double sum = 0.0;
    for (int i = 1000; i < 100000; i++) {
        const double ii = i / 1000.0;
        sum = sum + std::exp(-x * ii) / ii * 0.001;
    }
    return static_cast<float>(sum);
}

float calctav(float alfa, float nr)
{
    const float pi = 3.1415926f;
    const float rd = pi / 180;
    const float n2 = nr * nr;
    const float np = n2 + 1;
    const float nm = n2 - 1;
    const float a = (nr + 1) * (nr + 1) / 2;
    const float k = -(n2 - 1) * (n2 - 1) / 4;
    const float sa = std::sin(alfa * rd);
    float b1 = 0;
    if (alfa != 90) b1 = std::sqrt((sa * sa - np / 2) * (sa * sa - np / 2) + k);
    const float b2 = sa * sa - np / 2;
    const float b = b1 - b2;
    const float b3 = b * b * b;
    const float a3 = a * a * a;
    const float ts = (k * k / (6 * b3) + k / b - b / 2) - (k * k / (6 * a3) + k / a - a / 2);
    const float tp1 = -2 * n2 * (b - a) / (np * np);
    const float tp2 = -2 * n2 * np * std::log(b / a) / (nm * nm);
    const float tp3 = n2 * (1 / b - 1 / a) / 2;
    const float tp4 = 16 * n2 * n2 * (n2 * n2 + 1) * std::log((2 * np * b - nm * nm) / (2 * np * a - nm * nm)) / (np * np * np * nm * nm);
    const float tp5 = 16 * n2 * n2 * n2 * (1 / (2 * np * b - nm * nm) - 1 / (2 * np * a - nm * nm)) / (np * np * np);
    const float tp = tp1 + tp2 + tp3 + tp4 + tp5;
    return (ts + tp) / (2 * sa * sa);
}

void prospect(const OptCoeff& coeff, const LeafOptics& leaf, std::vector<float>& reflectance, std::vector<float>& transmittance)
{
    reflectance.assign(kSpectralCount, 0.0f);
    transmittance.assign(kSpectralCount, 0.0f);
    const float N = leaf.n;
    for (int i = 0; i < kSpectralCount; i++) {
        const float nr = coeff.nr[i];
        const float Kall = (leaf.cab * coeff.kab[i] + leaf.cdm * coeff.kdm[i] + leaf.cw * coeff.kw[i] + leaf.cs * coeff.ks[i]) / N;

        const float t1 = (1 - Kall) * std::exp(-Kall);
        const float t2 = Kall * Kall * expint(Kall);
        float tau = 1;
        if (Kall > 0) tau = t1 + t2;

        const float talf = calctav(59, nr);
        const float ralf = 1 - talf;
        const float t12 = calctav(90, nr);
        const float r12 = 1 - t12;
        const float t21 = t12 / (nr * nr);
        const float r21 = 1 - t21;

        float denom = 1 - r21 * r21 * tau * tau;
        const float Ta = talf * tau * t21 / denom;
        const float Ra = ralf + r21 * tau * Ta;

        const float tt = t12 * tau * t21 / denom;
        const float r = r12 + r21 * tau * tt;

        const float D = std::sqrt((1 + r + tt) * (1 + r - tt) * (1 - r + tt) * (1 - r - tt));
        const float rq = r * r;
        const float tq = tt * tt;
        const float a = (1 + rq - tq + D) / (2 * r);
        const float b = (1 - rq + tq + D) / (2 * tt);

        const float bNm1 = std::pow(b, (N - 1));
        const float bN2 = bNm1 * bNm1;
        const float a2 = a * a;
        denom = a2 * bN2 - 1;
        float Rsub = a * (bN2 - 1) / denom;
        float Tsub = bNm1 * (a2 - 1) / denom;

        if (r + tt >= 1) {
            Tsub = tt / (tt + (1 - tt) * (N - 1));
            Rsub = 1 - Tsub;
        }

        denom = 1 - Rsub * r;
        transmittance[i] = Ta * Tsub / denom;
        reflectance[i] = Ra + Ta * Rsub * tt / denom;
    }
}

void bsm(const OptCoeff& coeff, const SoilOptics& soil, float moisture, std::vector<float>& reflectance)
{
    const float B = soil.brightness;
    const float SMCp = 0.25f;
    const float film = 0.015f;
    const float rd = static_cast<float>(3.1415926 / 180.0);
    const float f1 = B * std::sin(soil.latitude * rd);
    const float f2 = B * std::cos(soil.latitude * rd) * std::sin(soil.longitude * rd);
    const float f3 = B * std::cos(soil.latitude * rd) * std::cos(soil.longitude * rd);
    const size_t count = coeff.gsv1.size();
    reflectance.assign(count, 0.0f);
    for (size_t k = 0; k < count; k++) {
        const float rdry = coeff.gsv1[k] * f1 + coeff.gsv2[k] * f2 + coeff.gsv3[k] * f3;
        const float tw = std::exp(-coeff.kw[k] * film);
        const float rbac = 1 - (1 - rdry) * (rdry * calctav(90, static_cast<float>(2.0 / coeff.nw[k])) / calctav(90, 2.0f) + 1 - rdry);
        const float p = 1 - calctav(90, coeff.nw[k]) / coeff.nw[k] / coeff.nw[k];
        const float Rw = 1 - calctav(40, coeff.nw[k]);
        const float Radd = (1 - Rw) * (1 - p) * rbac / (1 - p * rbac);
        const float water = std::clamp(moisture, 0.05f, 0.55f);
        const float mu = static_cast<float>((water - 0.05) / SMCp);
        // SCOPE BSM: soil moisture at or below 5 % leaves the dry spectrum unchanged.
        if (mu <= 0) {
            reflectance[k] = rdry;
            continue;
        }
        const float fdry = std::exp(-mu);
        const float fmul = (std::exp(tw * mu) - 1) * fdry;
        reflectance[k] = rdry * fdry + Rw * (1 - fdry) + Radd * fmul;
    }
}

}  // namespace stream
