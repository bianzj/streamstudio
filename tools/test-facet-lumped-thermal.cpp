// ==========================================================================
// 文件：test-facet-lumped-thermal.cpp
// 功能：FacetEB 集总热体（facetrt/lumpedthermal.h 的 solveLumpedCore / averageLumpedPower）的解析解、
//       能量守恒与输入校验测试；CMake 目标 histream_lumped_thermal_tests（EXCLUDE_FROM_ALL，
//       包含目录为 models/histream/src）
// 检查项：
//   1. 隐式加热/冷却与环境平衡的解析解，传导功率与能量闭合残差
//   2. 时间步与加热窗口部分重叠时的平均功率
//   3. 含空气导热时各功率项闭合（P − 传导 − 空气损失 − 储热 = 0）
//   4. 多表皮面元的分布式传导与集总传导互易（面积×权重加权）
//   5. 非法热容（负、0、NaN）必须被拒绝
// 约定：LumpedThermalParameters 依次为 热容 J/K、表皮导热 W/K、空气导热 W/K、功率 W、
//       加热起止时刻 s、初始温度 K；温度 K，时间 s。
// ==========================================================================

#include "facetrt/lumpedthermal.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void near(double a, double b, double tolerance, const char* message) { check(std::abs(a-b)<=tolerance,message); }

int main() {
    using namespace facetphysics;
    try {
        // 基准参数：C=1000 J/K，G=10 W/K，Ga=0，P=500 W，加热窗 0–100 s，初温 300 K。
        // 隐式欧拉 dt=1：ΔT = P/(C/dt + G) = 500/1010 K（表皮与气温均为 300 K）。
        LumpedThermalParameters p{1000,10,0,500,0,100,300};
        auto r=solveLumpedCore(p,300,300,300,1,0);
        near(r.temperatureK,300+500./1010,1e-11,"implicit heating analytic solution");
        near(r.conductedW,10*(r.temperatureK-300),1e-10,"conductive heat flow");
        near(r.residualW,0,1e-8,"core energy closure");
        auto q=p;q.powerW=0;
        r=solveLumpedCore(q,300,300,300,1,0);
        near(r.temperatureK,300,1e-12,"ambient equilibrium");
        r=solveLumpedCore(q,340,300,300,1,0);
        near(r.temperatureK,300+40*100./101,1e-11,"implicit cooling analytic solution");
        r=solveLumpedCore(q,300,320,300,10,0);
        check(r.temperatureK>300 && r.conductedW<0,"surface heats core bidirectionally");
        p.startSeconds=10;p.stopSeconds=20;
        // 加热窗改为 [10,20] s 后测平均功率：步 [5,15] 与 [15,25] 都只有一半时间重叠，平均 250 W。
        near(averageLumpedPower(p,5,10),250,1e-12,"partial power-on interval");
        near(averageLumpedPower(p,15,10),250,1e-12,"partial power-off interval");
        near(averageLumpedPower(p,20,10),0,1e-12,"power fully off");
        p.airConductanceWPerK=3;
        r=solveLumpedCore(p,330,310,300,7,12);
        near(r.powerW-r.conductedW-r.airLossW-r.storageW,0,1e-8,"all core terms close");
        // 互易检验：两块表皮（面积 2、3，权重 0.2、0.8，温度 305、315 K）；
        // 集总热体看到的是面积×权重加权平均温度，分块传导之和必须等于集总传导功率。
        const double area1=2,area2=3,w1=.2,w2=.8;
        const double mean=(area1*w1*305+area2*w2*315)/(area1*w1+area2*w2);
        r=solveLumpedCore(p,330,mean,300,7,12);
        const double scale=p.conductanceWPerK/(area1*w1+area2*w2);
        near(area1*scale*w1*(r.temperatureK-305)+area2*scale*w2*(r.temperatureK-315),r.conductedW,1e-9,"distributed surface/core reciprocity");
        // 非法热容 -1、0、NaN 都必须抛 std::runtime_error。
        int rejected=0;
        for (double invalid : {-1.,0.,std::numeric_limits<double>::quiet_NaN()}) {
            auto bad=p;bad.capacityJPerK=invalid;
            try { (void)solveLumpedCore(bad,300,300,300,1,0); } catch(const std::runtime_error&) { ++rejected; }
        }
        check(rejected==3,"invalid parameters rejected");
        std::cout<<"PASS 9 lumped thermal analytical/conservation/input checks\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
