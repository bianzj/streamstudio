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


def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


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
test = ROOT/'build/test-facet-collatz-generated.cpp'
test.write_text(harness, encoding='utf-8')
exe = ROOT/'build/test-facet-collatz.exe'
subprocess.run(['cl','/nologo','/utf-8','/EHsc','/std:c++17',str(test),f'/Fo:{test.with_suffix(".obj")}',f'/Fe:{exe}'],check=True)
subprocess.run([str(exe)],check=True)
