// FP64 elementary functions: GLSL's transcendental intrinsics are float-only.
// Range reduction and convergent series retain the CPU solver's double roots.
double dexp(double x) {
    if (isnan(x)) return x;
    if (x > 709.782712893384lf) return double(uintBitsToFloat(0x7f800000u));
    if (x < -745.133219101941lf) return 0.0lf;
    int k = int(floor(x / 0.693147180559945309417lf + 0.5lf));
    double r = x - double(k) * 0.693147180559945309417lf;
    double term = 1.0lf, sum = 1.0lf;
    for (int i = 1; i <= 16; ++i) { term *= r / double(i); sum += term; }
    return ldexp(sum, k);
}
double dlog(double x) {
    if (isnan(x) || isinf(x)) return x;
    if (x <= 0.0lf) return double(uintBitsToFloat(x == 0.0lf ? 0xff800000u : 0x7fc00000u));
    int k; double m = frexp(x, k);
    if (m < 0.7071067811865475244lf) { m *= 2.0lf; --k; }
    double z = (m - 1.0lf) / (m + 1.0lf), zz = z * z, term = z, sum = z;
    for (int i = 3; i <= 31; i += 2) { term *= zz; sum += term / double(i); }
    return 2.0lf * sum + double(k) * 0.693147180559945309417lf;
}
double dpow(double x, double y) { return dexp(y * dlog(x)); }
double fourth(double x) { return x*x*x*x; }
double quadratic(double a, double b, double c, bool larger) {
    if (a == 0.0lf) return b != 0.0lf ? -c/b : double(uintBitsToFloat(0x7fc00000u));
    double d=b*b-4.0lf*a*c;
    if (d < -1.e-12lf*(b*b+abs(4.0lf*a*c))) return double(uintBitsToFloat(0x7fc00000u));
    double r=sqrt(max(d,0.0lf));
    if(r==0.0lf) return -b/(2.0lf*a);
    double q=-0.5lf*(b+(b<0.0lf?-r:r));
    return larger==(b<0.0lf)?q/a:c/q;
}
float saturation(float t) { float c=clamp(t-273.15,-80.0,80.0); return 6.107*pow(10.0,7.5*c/(237.3+c)); }
float saturationSlope(float t) { float c=clamp(t-273.15,-80.0,80.0); return saturation(t)*2.3026*7.5*237.3/pow(237.3+c,2.0); }
float latent(float t,float rb,float rs) {
    float L=(2.501-0.002361*(t-273.15))*1.e6;
    float deficit=0.622*max(0.0,saturation(t)-pc.vapor)/max(100.0,pc.pressure);
    return pc.rho*L*deficit/max(1.0,rb+rs);
}
float latentSlope(float t,float rb,float rs) {
    float L=(2.501-0.002361*(t-273.15))*1.e6;
    float deficit=0.622*max(0.0,saturation(t)-pc.vapor)/max(100.0,pc.pressure);
    return max(0.0,pc.rho/max(1.0,rb+rs)*(-2361.0*deficit+L*0.622*saturationSlope(t)/max(100.0,pc.pressure)));
}
float collatz(uint f,float t,float par,float vapor,float depth) {
    uint p=f*32u;
    bool c4=P[p+16u]>3.5;
    double tc=double(t), corr=double(P[p+17u]);
    double vmax=double(P[p+10u])*dexp(double(P[p+14u]*clamp(depth,-1.0,0.0)));
    double ppm=1.e-6lf*double(pc.pressure*1.e-3), cs=double(pc.co2)*ppm;
    double o=c4?0.0lf:209.0lf*1.e-3lf*double(pc.pressure*1.e-3);
    double qt=.1lf*(tc-298.15lf)*corr;
    double th=1.0lf+corr*dexp(double(P[p+19u])*(tc-double(P[p+21u])));
    double tl=1.0lf+corr*dexp(double(P[p+18u])*(double(P[p+20u])-tc));
    double kc=350.e-6lf*dexp(dlog(2.1lf)*qt), ko=450.e-3lf*dexp(dlog(1.2lf)*qt);
    double kpep=vmax/56.0lf*1.e6lf*dexp(dlog(1.8lf)*qt);
    double vcmax=vmax*dexp(dlog(2.1lf)*qt)/(c4?th*tl:th)*double(P[p+15u]);
    double specificity=2600.0lf*dexp(dlog(.75lf)*qt);
    double rd=double(P[p+13u])*vmax*dexp(dlog(1.8lf)*qt)/(1.0lf+dexp(1.3lf*(tc-double(P[p+22u]))));
    double kd=max(.8738lf,.0301lf*(tc-273.15lf)+.0773lf);
    double je=.5lf*4.0lf/(.05lf+kd+4.0lf)*double(max(0.0,par));
    double gamma=.5lf*o/specificity, rh=clamp(double(vapor)/double(saturation(t)),0.0lf,1.0lf);
    double product=double(P[p+11u])*rh, minCi=(c4?.1lf:.3lf)*cs;
    double ci=product>0.0lf?max(minCi,cs*(1.0lf-1.6lf/product)):minCi;
    double vc,ve,vs;
    if(c4) {vc=vcmax; vs=kpep*ci; ve=je/6.0lf;}
    else {double mm=kc*(1.0lf+o/ko); vc=mm+ci>0.0lf?vcmax*(ci-gamma)/(mm+ci):0.0lf;
        vs=vmax/2.0lf*dexp(dlog(1.8lf)*qt); ve=ci+2.0lf*gamma>0.0lf?je*(ci-gamma)/(ci+2.0lf*gamma)/5.0lf:0.0lf;}
    double v=quadratic(.8lf,-(vc+ve),vc*ve,vc<0.0lf);
    double ag=quadratic(.98lf,-(v+vs),v*vs,false);
    if(par<=0.0||product<=0.0lf||P[p+15u]<=0.0) ag=0.0lf;
    double a=ag-rd, conductance=double(max(0.0,P[p+12u]))*8.314lf*tc/double(pc.pressure*100.0);
    if(!isnan(a)&&!isinf(a)&&a>0.0lf) {
        double resistance=.625lf*max(cs-ci,0.0lf)/a*1.2047lf/28.96lf*1.e3lf*1.e6lf/double(pc.pressure*100.0)*1.e5lf;
        if(!isnan(resistance)&&!isinf(resistance)&&resistance>0.0lf) conductance+=1.0lf/resistance;
    }
    return conductance>0.0lf?float(clamp(1.0lf/conductance,1.0lf,625000.0lf)):625000.0;
}
double arrhenius(double activation,double t) { return dexp(activation*(t-298.15lf)/(298.15lf*(8.31lf*t/1000.0lf))); }
double peaked(double activation,double deactivation,double entropy,double t) {
    return arrhenius(activation,t)*(1.0lf+dexp((298.15lf*entropy-deactivation)/(8.31lf*298.15lf/1000.0lf)))/(1.0lf+dexp((t*entropy-deactivation)/(8.31lf*t/1000.0lf)));
}
double smallerRoot(double b,double c) {
    double disc=b*b-4.0lf*c;
    if(disc < -1.e-12lf*(b*b+abs(4.0lf*c))) return double(uintBitsToFloat(0x7fc00000u));
    double r=sqrt(max(0.0lf,disc)); return b<0.0lf?(b==r?0.0lf:2.0lf*c/(-b+r)):(-b-r)/2.0lf;
}
float farquhar(uint f,double t,float par,double rh,float depth) {
    uint p=f*32u; bool c4=P[p+16u]>3.5;
    double pressure=double(pc.pressure), pa=pressure*100.0lf, ppm=pa*1.e-11lf, cs=double(pc.co2)*ppm, oxygen=209.0lf*pa*1.e-8lf;
    double vmax=double(P[p+10u])*dexp(double(P[p+14u])*clamp(double(depth),-1.0lf,0.0lf));
    double rd=double(P[p+13u])*vmax*arrhenius(46.39lf,t), specificity=2862.0lf/arrhenius(37.83lf,t);
    double vcmax=vmax*(c4?peaked(67.29lf,144.57lf,.472lf,t):peaked(71.51lf,200.0lf,(-1.07lf*double(P[p+23u])+668.0lf)/1000.0lf,t));
    double jmax=vmax*(c4?(40.0lf/6.0lf)*peaked(77.9lf,191.9lf,.627lf,t):2.68lf*peaked(49.88lf,200.0lf,(-.75lf*double(P[p+23u])+660.0lf)/1000.0lf,t));
    double qt=(t-298.15lf)/10.0lf;
    double kc=(c4?944.0lf*dpow(2.1lf,qt):404.9lf*arrhenius(79.43lf,t))*ppm;
    double ko=(c4?633.0lf*dpow(1.2lf,qt):278.4lf*arrhenius(36.38lf,t))*pa*1.e-8lf;
    double kps=(1.e8lf+3.e7lf)*.88lf/.12lf*double(P[p+25u]);
    double denominator=kps+3.e7lf+1.e8lf+double(P[p+26u])*(3.e7lf+1.e8lf);
    double theta=(kps-1.95e8lf*double(P[p+25u]))/denominator;
    double excitation=double(P[p+24u])*double(par)*kps/denominator, capacity=jmax*double(P[p+25u]), sum=excitation+capacity;
    double electron=sum>0.0lf?2.0lf*excitation*capacity/(sum+sqrt(max(0.0lf,sum*sum-4.0lf*theta*excitation*capacity))):0.0lf;
    double gamma=.5lf*oxygen/specificity, lower=c4?9.9lf*ppm:gamma;
    double product=double(P[p+11u])*rh*double(P[p+15u]);
    double ci=product>0.0lf?max(lower,cs*(1.0lf-1.6lf/product)):lower, gross=0.0lf;
    if(par>0.0&&electron>0.0lf&&product>0.0lf&&cs>0.0lf) {
        if(!c4) gross=(1.0lf-gamma/ci)*min(vcmax*ci/(ci+kc*(1.0lf+oxygen/ko)),electron*ci/(4.5lf*ci+10.5lf*gamma));
        else {
            double vpmax=vmax*2.33lf*peaked(70.37lf,117.93lf,.376lf,t), kp=82.0lf*dpow(2.1lf,qt)*ppm;
            double vp=min(vpmax*ci/(ci+kp),80.0lf), gbs=(.0207lf*vmax+.4806lf)*1000.0lf, gam=.5lf/specificity;
            double supply=vp-.5lf*rd+gbs*ci, carbox=vcmax-rd, mm=gbs*kc*(1.0lf+oxygen/ko);
            double ls=.2lf*electron-.5lf*rd+gbs*ci, lc=.2lf*electron;
            double ac=smallerRoot(-(supply+carbox+mm),carbox*supply-gam*vcmax*gbs*oxygen+rd*mm);
            double aj=smallerRoot(-(ls+lc-rd+gbs*gam*oxygen*7.0lf/3.0lf),ls*(lc-rd)-gbs*gam*oxygen*(lc+rd*7.0lf/3.0lf));
            gross=min(ac,aj)+rd;
        }
    }
    double net=gross-rd, g=double(P[p+12u])*8.314lf*t/pa;
    if(net>0.0lf&&cs>ci) {double r=.625lf*(cs-ci)/net*1.2047lf/28.96lf*1.e3lf*1.e6lf/pa*1.e5lf; g+=1.0lf/r;}
    // MD12 rejects invalid assimilation; do not turn a NaN root into g0-only exchange.
    if (isnan(gross)||isinf(gross)||isnan(net)||isinf(net)||isnan(ci)||isinf(ci)||isnan(g)||isinf(g)||g<0.0lf)
        return uintBitsToFloat(0x7fc00000u);
    return g>0.0lf?float(clamp(1.0lf/g,1.0lf,625000.0lf)):625000.0;
}
float physiology(uint f,double t,double rs,float par,float rb,float depth) {
    float sat=saturation(float(t));
    double vapor=double(pc.vapor)+double(max(0.0,sat-pc.vapor))*double(rb)/(double(rb)+rs);
    return pc.method==0u?collatz(f,float(t),par,float(vapor),depth):farquhar(f,t,par,clamp(vapor/double(sat),0.0lf,1.0lf),depth);
}
double thermal(float initial,double rs,float rb,float net,double eps,float conductance,float storage,float previous) {
    double lower=220.0lf,upper=360.0lf,t=clamp(double(initial),lower,upper);
    for(int i=0;i<32;++i) {
        double residual=double(net)-eps*double(SIGMA)*(fourth(t)-fourth(double(initial)))-double(conductance)*(t-double(pc.air))-
            double(latent(float(t),rb,float(rs)))-double(storage)*(t-double(previous));
        double response=4.0lf*eps*double(SIGMA)*t*t*t+double(conductance)+double(storage)+double(latentSlope(float(t),rb,float(rs)));
        if(abs(residual)<.0005lf||upper-lower<.00001lf) break;
        if(residual>0.0lf) lower=t; else upper=t;
        double trial=t+residual/max(1.0lf,response);
        t=trial>lower&&trial<upper?trial:.5lf*(lower+upper);
    }
    return t;
}
