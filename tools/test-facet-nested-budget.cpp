// ==========================================================================
// 文件：test-facet-nested-budget.cpp
// 功能：FacetEB 叶片耦合求解（energyphysics.h 的 coupledLeaf / temperatureRoot）工作量预算与早停测试，
//       直接执行生产求解器并统计回调次数，而不是检查源码文本
// 检查项：
//   1. coupledLeaf：外层 ln r_s 二分不超过 24 次，且结果满足 0.01 的 ln 容差；
//      已收敛的根立即退出（各回调仅 1 次）；无法收敛时刚好用满 24 次预算
//   2. temperatureRoot：不收敛时最多 32 步；正常收敛时提前退出；
//      小于 0.01 K 的修正在退出前被施加，容差边界两侧结果不跳变（连续性）
// 构建：独立可执行文件（只依赖头文件）；通过输出 PASS/FAIL 与退出码报告结果。
// ==========================================================================

#include "../models/histream/src/facetrt/energyphysics.h"
#include <iostream>

int main() try {
    // Verify work limits by executing the production solver, not inspecting text.
    // 固定的热/生理回调：叶温恒为 300 K、目标阻抗恒为 1000 s m-1，统计求解器调用回调的次数。
    int thermalCalls = 0, physiologyCalls = 0;
    const auto leaf = facetphysics::coupledLeaf(
        [&](double) { ++thermalCalls; return 300.0; },
        [&](double, double) { ++physiologyCalls; return 1000.0; });
    if (thermalCalls > 25 || physiologyCalls > 24)
        throw std::runtime_error("Exceeded the twenty-four resistance trial budget");
    if (std::abs(std::log(leaf.resistance / 1000.0)) > .01)
        throw std::runtime_error("Resistance did not meet the inner tolerance");
    // 目标阻抗取 ln 区间 [0, ln 625000] 的中点 exp(ln(625000)/2)=790.569…，第一次试探即命中，应当 1 次回调就退出。
    int exactThermal = 0, exactPhysiology = 0;
    const auto exact = facetphysics::coupledLeaf(
        [&](double) { ++exactThermal; return 300.0; },
        [&](double, double) { ++exactPhysiology; return 790.5694150420948; });
    if (exactThermal != 1 || exactPhysiology != 1 ||
        std::abs(exact.resistance - 790.5694150420948) > 1e-8)
        throw std::runtime_error("An already converged root did not exit immediately");
    // 目标阻抗 1e9 超出括号，永远不满足容差，应恰好用完 24 次预算。
    int budgetCalls = 0;
    (void)facetphysics::coupledLeaf([](double) { return 300.0; },
        [&](double, double) { ++budgetCalls; return 1e9; });
    if (budgetCalls != 24) throw std::runtime_error("Unconverged search did not stop at its budget");
    int energyCalls = 0;
    (void)facetphysics::temperatureRoot(300, [&](double) {
        ++energyCalls;
        return std::array<double, 2>{1, 1}; // Deliberately never reaches zero.
    });
    if (energyCalls != 32) throw std::runtime_error("Thermal work exceeded the thirty-two-step budget");
    int convergedCalls = 0;
    const auto t = facetphysics::temperatureRoot(300, [&](double temperature) {
        ++convergedCalls;
        return std::array<double, 2>{310 - temperature, 1};
    });
    if (std::abs(t - 310) > 1e-9 || convergedCalls != 2)
        throw std::runtime_error("Early convergence no longer works");
    int subToleranceCalls = 0;
    const auto close = facetphysics::temperatureRoot(300, [&](double temperature) {
        ++subToleranceCalls;
        return std::array<double, 2>{100 * (300.005 - temperature), 100};
    });
    if (subToleranceCalls != 1 || std::abs(close - 300.005) > .01)
        throw std::runtime_error("Temperature correction below 0.01 K did not terminate");
    // Float roundoff on either side of the 0.01 K exit threshold must not
    // create a 0.01 K discontinuity in the returned temperature.
    // 容差边界连续性：根位于 300.010001 与 300.009999（相差 2e-6）时，返回温度之差不得超过 1e-5。
    const auto nearExit = [](double root) {
        return facetphysics::temperatureRoot(300, [&](double t) {
            return std::array<double, 2>{100 * (root - t), 100};
        });
    };
    if (std::abs(nearExit(300.010001) - nearExit(300.009999)) > .00001)
        throw std::runtime_error("Thermal early exit jumps across its tolerance boundary");
    std::cout << "PASS: at most 24 resistance trials, 32 thermal cap, both early exits; log mismatch="
              << std::abs(std::log(leaf.resistance / 1000.0)) << '\n';
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
