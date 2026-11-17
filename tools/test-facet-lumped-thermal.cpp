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
        near(averageLumpedPower(p,5,10),250,1e-12,"partial power-on interval");
        near(averageLumpedPower(p,15,10),250,1e-12,"partial power-off interval");
        near(averageLumpedPower(p,20,10),0,1e-12,"power fully off");
        p.airConductanceWPerK=3;
        r=solveLumpedCore(p,330,310,300,7,12);
        near(r.powerW-r.conductedW-r.airLossW-r.storageW,0,1e-8,"all core terms close");
        const double area1=2,area2=3,w1=.2,w2=.8;
        const double mean=(area1*w1*305+area2*w2*315)/(area1*w1+area2*w2);
        r=solveLumpedCore(p,330,mean,300,7,12);
        const double scale=p.conductanceWPerK/(area1*w1+area2*w2);
        near(area1*scale*w1*(r.temperatureK-305)+area2*scale*w2*(r.temperatureK-315),r.conductedW,1e-9,"distributed surface/core reciprocity");
        int rejected=0;
        for (double invalid : {-1.,0.,std::numeric_limits<double>::quiet_NaN()}) {
            auto bad=p;bad.capacityJPerK=invalid;
            try { (void)solveLumpedCore(bad,300,300,300,1,0); } catch(const std::runtime_error&) { ++rejected; }
        }
        check(rejected==3,"invalid parameters rejected");
        std::cout<<"PASS 9 lumped thermal analytical/conservation/input checks\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
