// SPDX-License-Identifier: GPL-2.0-or-later
#include "reference.h"
#include "geom-kernel/det-math.h"
#include <algorithm>
#include <iostream>
#include <vector>
using namespace Proof::GK;
using namespace GKTest;

TEST(GKDetMath, ScalarContainmentWidthAndRepeatedBits) {
    std::vector<double> angles{0,-0.0,16,-16,0x1p-1074,-0x1p-1074};
    MP pi=acos(MP(-1));
    for(int k=-10;k<=10;++k) {
        double x=static_cast<double>(MP(k)*pi/2);
        for(int j=0;j<25;++j) {
            if(std::abs(x)<=16) angles.push_back(x);
            x=down(x);
        }
        x=static_cast<double>(MP(k)*pi/2);
        for(int j=0;j<25;++j) {
            if(std::abs(x)<=16) angles.push_back(x);
            x=up(x);
        }
    }
    for(int e=-1074;e<0;e+=13) {angles.push_back(std::ldexp(1.0,e));angles.push_back(-std::ldexp(1.0,e));}
    std::mt19937_64 g(0x99341);
    for(int j=0;j<1400;++j) angles.push_back((static_cast<double>(g()>>12)*0x1p-52)*32-16);
    double max_ulps=0;
    uint64_t digest=UINT64_C(14695981039346656037);
    for(double x:angles) {
        SCOPED_TRACE(x);
        auto r=det_sincos(x); ASSERT_TRUE(r) << r.detail;
        contains(r.value->sin,sin(MP(x))); contains(r.value->cos,cos(MP(x)));
        GKTest::hash(digest,r.value->sin);GKTest::hash(digest,r.value->cos);
        for(auto i:{r.value->sin,r.value->cos}) {
            double m=std::max(std::abs(i.lo),std::abs(i.hi));
            double ulp=up(m)-m;
            double width=(i.hi-i.lo)/ulp;
            max_ulps=std::max(max_ulps,width);
            EXPECT_LE(width,4);
        }
        auto again=det_sincos(x); ASSERT_TRUE(again);
        same(r.value->sin,again.value->sin);same(r.value->cos,again.value->cos);
    }
    std::cout << "Maximum sampled sin/cos enclosure width: " << max_ulps << " ulp\n";
    std::cout << "Sincos process digest: " << std::hex << digest << std::dec << '\n';
    EXPECT_FALSE(det_sincos(up(16))); EXPECT_FALSE(det_sincos(down(-16)));
    EXPECT_FALSE(det_sincos(std::numeric_limits<double>::infinity()));
}
TEST(GKDetMath, CertificateConstants) {
    MP pi=acos(MP(-1));
    MP split=MP(0x1.921fb54442d18p+0)+MP(0x1.1a62633145c07p-54)-MP(0x1.f1976b7ed8fbcp-110);
    EXPECT_LT(abs(split-pi/2),MP(0x1p-160));
    MP factorial=1;
    for(int k=2;k<=30;++k) factorial*=k;
    EXPECT_LT(pow(MP(.786),30)/factorial,MP(0x1p-116));
    EXPECT_LT(pow(MP(.415),99)*2/99,MP(0x1p-118));
}
TEST(GKDetMath, IntervalExtremaAndArguments) {
    MP pi=acos(MP(-1));
    for(Interval x:{Interval(-16,16),Interval(1.5,1.6),Interval(-3.2,-3.1),Interval(-0x1p-1074,0x1p-1074)}) {
        auto r=det_sincos(x); ASSERT_TRUE(r);
        for(int j=0;j<=80;++j) {
            MP t=MP(x.lo)+(MP(x.hi)-MP(x.lo))*j/80;
            contains(r.value->sin,sin(t));contains(r.value->cos,cos(t));
        }
    }
    for(Interval x:{Interval(1,2),Interval(-2,-1),Interval(-1,1)})
        for(Interval y:{Interval(1,2),Interval(-2,-1),Interval(-1,1)}) {
            auto a=det_atan2(y,x);
            if(x.contains(0)&&y.contains(0)) { EXPECT_FALSE(a);continue; }
            ASSERT_TRUE(a);
            for(int j=0;j<=10;++j) for(int k=0;k<=10;++k) {
                MP xx=MP(x.lo)+(MP(x.hi)-MP(x.lo))*j/10;
                MP yy=MP(y.lo)+(MP(y.hi)-MP(y.lo))*k/10;
                MP t=atan2(yy,xx);
                if(x.hi<0&&y.contains(0)&&yy<0) t+=2*pi;
                contains(*a.value,t);
            }
        }
}
