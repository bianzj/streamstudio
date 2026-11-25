// ==========================================================================
// 文件：observation_errors.h
// 功能：由观测误差协方差矩阵求精度矩阵（逆协方差），供反演/同化的 R^-1 加权使用
// 主要模块：
//   1. observationPrecision：Cholesky 分解 + 前/后代求逆，同时检验对称性与正定性
// 数据流：同化/反演（app/assimilation.cpp 等）把多波段观测误差协方差交入，
//         得到的精度矩阵用于 MAP 代价 (y-h)^T R^-1 (y-h)
// 约定：矩阵按行主序存放（行 i、列 j 为 [i*n + j]）；仅依赖标准库，无自动微分
// ==========================================================================

#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace stream {

// 作用：求 n×n 观测协方差的逆（精度矩阵），写入 precision；成功返回 true。
// 步骤 1：检验对称（容差 1e-10 × max(1, |a|, |b|)）与有限性，同时做 Cholesky 分解 C = L L^T；
//         对角元 <= 0 或非有限视为非正定，返回 false。
// 步骤 2：逐列解 L y = e_k（前代）与 L^T x = y（回代），得到逆矩阵的第 k 列。
// 说明：调用方不得在失败时退回“独立误差”假设（被拒绝的协方差必须向上报错）。
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
