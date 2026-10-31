#pragma once
// Forward-mode automatic differentiation for the differentiable STREAM operators.
//
// Dual<N> carries a value and N partial derivatives. The canopy, hom3 radiosity, soil emissivity, shortwave and
// energy-balance kernels are templates on their scalar type: the forward model runs them with double, the
// assimilation runs the same code with Ad and obtains exact derivatives (no finite differences, no second copy of
// the physics). Branches and clamps follow the value, so derivatives are those of the active branch.
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace stream {

template <int N>
struct Dual {
    double v = 0.0;
    std::array<double, N> d{};

    Dual() = default;
    Dual(double value) : v(value) {}  // constants promote implicitly
    static Dual seed(double value, int index)
    {
        Dual result(value);
        if (index >= 0 && index < N) result.d[index] = 1.0;
        return result;
    }

    Dual& operator+=(const Dual& o) { v += o.v; for (int i = 0; i < N; ++i) d[i] += o.d[i]; return *this; }
    Dual& operator-=(const Dual& o) { v -= o.v; for (int i = 0; i < N; ++i) d[i] -= o.d[i]; return *this; }
    Dual& operator*=(const Dual& o)
    {
        for (int i = 0; i < N; ++i) d[i] = d[i] * o.v + v * o.d[i];
        v *= o.v;
        return *this;
    }
    Dual& operator/=(const Dual& o)
    {
        const double q = v / o.v;
        for (int i = 0; i < N; ++i) d[i] = (d[i] - q * o.d[i]) / o.v;
        v = q;
        return *this;
    }
    friend Dual operator+(Dual a, const Dual& b) { return a += b; }
    friend Dual operator-(Dual a, const Dual& b) { return a -= b; }
    friend Dual operator*(Dual a, const Dual& b) { return a *= b; }
    friend Dual operator/(Dual a, const Dual& b) { return a /= b; }
    friend Dual operator-(Dual a) { a.v = -a.v; for (int i = 0; i < N; ++i) a.d[i] = -a.d[i]; return a; }
    friend Dual operator+(const Dual& a) { return a; }
    friend bool operator<(const Dual& a, const Dual& b) { return a.v < b.v; }
    friend bool operator>(const Dual& a, const Dual& b) { return a.v > b.v; }
    friend bool operator<=(const Dual& a, const Dual& b) { return a.v <= b.v; }
    friend bool operator>=(const Dual& a, const Dual& b) { return a.v >= b.v; }
    friend bool operator==(const Dual& a, const Dual& b) { return a.v == b.v; }
    friend bool operator!=(const Dual& a, const Dual& b) { return a.v != b.v; }
};

// Controls of one pixel never exceed 8 (four component temperatures + soil moisture + Vcmax + LAI).
constexpr int kAdSize = 8;
using Ad = Dual<kAdSize>;

namespace ad {

inline double value(double x) { return x; }
template <int N> double value(const Dual<N>& x) { return x.v; }

template <int N>
Dual<N> chain(const Dual<N>& x, double f, double df)
{
    Dual<N> result(f);
    for (int i = 0; i < N; ++i) result.d[i] = df * x.d[i];
    return result;
}

inline double exp(double x) { return std::exp(x); }
template <int N> Dual<N> exp(const Dual<N>& x) { const double e = std::exp(x.v); return chain(x, e, e); }
inline double expm1(double x) { return std::expm1(x); }
template <int N> Dual<N> expm1(const Dual<N>& x) { return chain(x, std::expm1(x.v), std::exp(x.v)); }
inline double log(double x) { return std::log(x); }
template <int N> Dual<N> log(const Dual<N>& x) { return chain(x, std::log(x.v), 1.0 / x.v); }
inline double log1p(double x) { return std::log1p(x); }
template <int N> Dual<N> log1p(const Dual<N>& x) { return chain(x, std::log1p(x.v), 1.0 / (1.0 + x.v)); }
inline double log10(double x) { return std::log10(x); }
template <int N> Dual<N> log10(const Dual<N>& x) { return chain(x, std::log10(x.v), 1.0 / (x.v * std::log(10.0))); }
inline double sqrt(double x) { return std::sqrt(x); }
template <int N> Dual<N> sqrt(const Dual<N>& x)
{
    const double s = std::sqrt(x.v);
    return chain(x, s, s > 0.0 ? 0.5 / s : 0.0);
}
inline double cbrt(double x) { return std::cbrt(x); }
template <int N> Dual<N> cbrt(const Dual<N>& x)
{
    const double c = std::cbrt(x.v);
    return chain(x, c, c != 0.0 ? 1.0 / (3.0 * c * c) : 0.0);
}
inline double sinh(double x) { return std::sinh(x); }
template <int N> Dual<N> sinh(const Dual<N>& x) { return chain(x, std::sinh(x.v), std::cosh(x.v)); }
inline double atan(double x) { return std::atan(x); }
template <int N> Dual<N> atan(const Dual<N>& x) { return chain(x, std::atan(x.v), 1.0 / (1.0 + x.v * x.v)); }
inline double abs(double x) { return std::abs(x); }
template <int N> Dual<N> abs(const Dual<N>& x) { return x.v < 0.0 ? -x : x; }
inline double pow(double x, double p) { return std::pow(x, p); }
template <int N> Dual<N> pow(const Dual<N>& x, double p)
{
    const double f = std::pow(x.v, p);
    return chain(x, f, x.v != 0.0 ? p * f / x.v : 0.0);
}
template <int N> Dual<N> pow(double base, const Dual<N>& e)
{
    const double f = std::pow(base, e.v);
    return chain(e, f, f * std::log(base));
}

inline bool isfinite(double x) { return std::isfinite(x); }
template <int N> bool isfinite(const Dual<N>& x) { return std::isfinite(x.v); }

// Same selection rules as std::min / std::max / std::clamp (first argument kept on ties, NaN passes clamp),
// so the double instantiation reproduces the original forward code exactly.
inline double min(double a, double b) { return std::min(a, b); }
inline double max(double a, double b) { return std::max(a, b); }
inline double clamp(double x, double low, double high) { return std::clamp(x, low, high); }
template <int N> Dual<N> min(const Dual<N>& a, const Dual<N>& b) { return b.v < a.v ? b : a; }
template <int N> Dual<N> min(const Dual<N>& a, double b) { return b < a.v ? Dual<N>(b) : a; }
template <int N> Dual<N> min(double a, const Dual<N>& b) { return b.v < a ? b : Dual<N>(a); }
template <int N> Dual<N> max(const Dual<N>& a, const Dual<N>& b) { return a.v < b.v ? b : a; }
template <int N> Dual<N> max(const Dual<N>& a, double b) { return a.v < b ? Dual<N>(b) : a; }
template <int N> Dual<N> max(double a, const Dual<N>& b) { return a < b.v ? b : Dual<N>(a); }
template <int N> Dual<N> clamp(const Dual<N>& x, double low, double high)
{
    return x.v < low ? Dual<N>(low) : high < x.v ? Dual<N>(high) : x;
}
template <int N> Dual<N> clamp(const Dual<N>& x, const Dual<N>& low, const Dual<N>& high)
{
    return x.v < low.v ? low : high.v < x.v ? high : x;
}
template <int N> Dual<N> clamp(const Dual<N>& x, double low, const Dual<N>& high)
{
    return x.v < low ? Dual<N>(low) : high.v < x.v ? high : x;
}
template <int N> Dual<N> clamp(double x, double low, const Dual<N>& high)
{
    return x < low ? Dual<N>(low) : high.v < x ? high : Dual<N>(x);
}

}  // namespace ad
}  // namespace stream
