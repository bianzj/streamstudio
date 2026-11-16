#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace stream {

// Precision from a finite symmetric positive-definite observation covariance.
// Row-major arrays; rejected covariances must not fall back to independent errors.
inline bool observationPrecision(const std::vector<double>& covariance, int n, std::vector<double>& precision)
{
    std::vector<double> l(n * n, 0.0);
    for (int i = 0; i < n; ++i) for (int j = 0; j <= i; ++j) {
        const double a = covariance[i * n + j], b = covariance[j * n + i];
        if (!std::isfinite(a) || !std::isfinite(b) ||
            std::abs(a - b) > 1e-10 * std::max({1.0, std::abs(a), std::abs(b)})) return false;
        double value = a;
        for (int k = 0; k < j; ++k) value -= l[i * n + k] * l[j * n + k];
        if (i == j) {
            if (!(value > 0) || !std::isfinite(value)) return false;
            l[i * n + j] = std::sqrt(value);
        } else l[i * n + j] = value / l[j * n + j];
    }
    precision.assign(n * n, 0.0);
    for (int column = 0; column < n; ++column) {
        std::vector<double> y(n), x(n);
        for (int i = 0; i < n; ++i) {
            double value = i == column ? 1.0 : 0.0;
            for (int j = 0; j < i; ++j) value -= l[i * n + j] * y[j];
            y[i] = value / l[i * n + i];
        }
        for (int i = n - 1; i >= 0; --i) {
            double value = y[i];
            for (int j = i + 1; j < n; ++j) value -= l[j * n + i] * x[j];
            x[i] = value / l[i * n + i];
            if (!std::isfinite(x[i])) return false;
            precision[i * n + column] = x[i];
        }
    }
    return true;
}

}  // namespace stream
