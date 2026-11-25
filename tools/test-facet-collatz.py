# ==========================================================================
# 文件：test-facet-collatz.py
# 功能：FacetRT 叶片气孔阻抗（Collatz C3/C4 + Ball-Berry）的聚焦测试：从 facetrtcore.cpp 中按函数签名
#       抽取真实实现（saturationVaporPressure、quadraticRoot、leafStomatalResistance）拼成测试程序，
#       避免链接 Vulkan 应用，也不复制生理学实现
# 运行：在 VS x64 开发者命令行中执行（需要 cl）；生成 build/test-facet-collatz-generated.cpp 并编译运行
# 主要步骤：
#   1. function()：按花括号配平从 facetrtcore.cpp 截取指定函数全文
#   2. 拼出 C++ 测试程序（含冻结的 C4 参考值，来自 diagnose-maize-hot-leaf.py 的独立 CO2/能量闭合）
#   3. 检查：C4 参考值、夜间截距的局地摩尔体积转换、零截距解析解、C3/C4 阻抗范围 [1, 625000] s m-1
#   4. 调用 cl 编译并运行，失败即抛出非零退出码
# 与其他文件：依赖 models/histream/src/facetrt/facetrtcore.cpp 与 leafbiochemistry.h。
# ==========================================================================

"""Compile the actual anonymous-namespace Collatz function for focused tests.

Extraction avoids linking the Vulkan application; no physiology implementation
is copied into this test. Frozen C4 reference values come from independent
CO2/energy closure in diagnose-maize-hot-leaf.py.
Run from a VS x64 developer environment.
"""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT/'models/histream/src/facetrt/facetrtcore.cpp').read_text(encoding='utf-8')


# 作用：返回 facetrtcore.cpp 中以 signature 开头的函数全文（从签名到匹配的右花括号）。
# 局限：按花括号计数，不处理字符串/注释内的花括号。
def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# 测试程序头部：包含生物化学头文件并拼接被测函数（保持原实现不变）。
harness = r'''
#include "../models/histream/src/facetrt/leafbiochemistry.h"
#include <limits>
#include <iostream>
#include <string>
using facetbio::LeafBiochemistry;
'''
for signature in ['float saturationVaporPressure(', 'double quadraticRoot(', 'float leafStomatalResistance(']:
    harness += function(signature) + '\n'
harness += r'''
void near(double actual, double expected, double tolerance, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>tolerance)
        throw std::runtime_error(std::string(message)+": "+std::to_string(actual)+" vs "+std::to_string(expected));
}
int main() try {
    LeafBiochemistry leaf;
    leaf.type=4; leaf.vcmax25=60; leaf.ballBerrySlope=4;
    leaf.ballBerry0=.01f; leaf.rdParam=.012f; leaf.kV=.7f;
    const float t=310.2758280046f;
    const float vapor=.6486143940961258f*saturationVaporPressure(t);
    near(leafStomatalResistance(leaf,t,1706.2763443f,vapor,839,-.6481104262f,380),
         134.5861879943,.04,"C4 diffusion and Ball-Berry closure");
    // In darkness only the intercept remains; its velocity conversion must use
    // the same local molar volume as the assimilation-dependent conductance.
    for (float pressure : {700.f, 1013.f})
        near(leafStomatalResistance(leaf,300,0,15,pressure,-.5,380),
             pressure*100/(.01*8.314*300),.01,"Night intercept conversion");
    // Zero intercept has an analytic Ci; independently evaluate its C4 A.
    leaf.ballBerry0=0;
    const double temp=300, rh=.7, apar=1700, depth=-.5, pressure=839;
    const double vmax=60*std::exp(.7*depth), ci=380*(1-1.6/(4*rh));
    const double ve=.5*4/(.05+std::max(.8738,.0301*(temp-273.15)+.0773)+4)*apar/6;
    const double v=quadraticRoot(.8,-(vmax+ve),vmax*ve,false);
    const double vs=vmax/56*1e6*ci*1e-6*pressure*.001;
    const double a=quadraticRoot(.98,-(v+vs),v*vs,false)-.012*vmax/(1+std::exp(1.3*(temp-328)));
    const double expected=pressure*100/(8.314*temp)/(4*a*rh/380);
    near(leafStomatalResistance(leaf,temp,apar,rh*saturationVaporPressure(temp),pressure,depth,380),
         expected,.02,"Zero intercept uses local molar density");
    for (int type : {3,4}) for (float rhTest : {.05f,.4f,.9f}) {
        leaf.type=type; leaf.ballBerry0=.01f;
        const float r=leafStomatalResistance(leaf,310,1500,rhTest*saturationVaporPressure(310),839,-.5,380);
        if (!std::isfinite(r)||r<1||r>625000) throw std::runtime_error("Invalid C3/C4 resistance");
    }
    std::cout << "PASS: Collatz nonzero/zero intercept, local gas conversion, darkness and C3/C4 bounds\n";
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
'''
# 步骤 4：写出测试源码，用 MSVC cl（C++17）编译并运行；任一步失败即抛 CalledProcessError。
test = ROOT/'build/test-facet-collatz-generated.cpp'
test.write_text(harness, encoding='utf-8')
exe = ROOT/'build/test-facet-collatz.exe'
subprocess.run(['cl','/nologo','/utf-8','/EHsc','/std:c++17',str(test),f'/Fo:{test.with_suffix(".obj")}',f'/Fe:{exe}'],check=True)
subprocess.run([str(exe)],check=True)
