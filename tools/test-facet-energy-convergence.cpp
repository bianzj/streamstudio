// ==========================================================================
// 文件：test-facet-energy-convergence.cpp
// 功能：FacetEB 能量平衡收敛判据（energyphysics.h 的 facetEnergyViolation / facetEnergyConverged）的边界测试
// 检查项：
//   1. 残差阈值严格小于 2 W m-2（正负对称），非有限残差一律判为违规
//   2. 失败面元比例严格小于 0.1 %（整数运算，0 个面元视为未收敛），含原始玉米场景的阈值
//   3. 大面元数下的乘法不溢出（uint32 上限附近）
//   4. 等效温度残差上限 ≤ 0.5 K，NaN 不通过
// 构建：作为独立可执行文件编译（只依赖头文件，无需链接引擎）；通过输出 PASS/FAIL 与退出码报告结果。
// ==========================================================================

#include "../models/histream/src/facetrt/energyphysics.h"
#include <iostream>
#include <limits>

int main() try {
    using namespace facetphysics;
    if (facetEnergyViolation(1.999) || facetEnergyViolation(-1.999) ||
        !facetEnergyViolation(2) || !facetEnergyViolation(-2))
        throw std::runtime_error("Energy residual must be strictly below 2 W/m2");
    if (!facetEnergyViolation(std::numeric_limits<double>::quiet_NaN()))
        throw std::runtime_error("Nonfinite residual must not pass");
    if (!facetEnergyConverged(0, 1000) || facetEnergyConverged(1, 1000) ||
        !facetEnergyConverged(1, 1001) || facetEnergyConverged(0, 0))
        throw std::runtime_error("Failure fraction must be strictly below 0.1 percent");
    if (!facetEnergyConverged(141, 141152) || facetEnergyConverged(142, 141152))
        throw std::runtime_error("Wrong threshold for the original maize scene");
    if (!facetEnergyConverged(4000000, 4294967295U) ||
        facetEnergyConverged(5000000, 4294967295U))
        throw std::runtime_error("Facet-count multiplication overflow");
    if (!facetEnergyConverged(0, 1000, 0.5) || facetEnergyConverged(0, 1000, 0.5001) ||
        facetEnergyConverged(0, 1000, std::numeric_limits<double>::quiet_NaN()) ||
        facetEnergyConverged(1, 1000, 0.1))
        throw std::runtime_error("Temperature-equivalent residual cap must be <= 0.5 K");
    std::cout << "PASS: signed residuals, strict boundaries, zero scene, large counts, 0.5 K cap\n";
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
