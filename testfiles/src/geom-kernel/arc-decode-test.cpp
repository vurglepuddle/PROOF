// SPDX-License-Identifier: GPL-2.0-or-later
#include "reference.h"
#include "geom-kernel/arc-decode.h"
#include <2geom/elliptical-arc.h>
#include <array>
#include <iostream>
using namespace Proof::GK;
using namespace GKTest;
namespace {
struct ReferenceArc { MP cx,cy,ux,uy,vx,vy,t0,delta; bool scaled; };
ReferenceArc reference(ArcInput const &a) {
    MP rx=abs(MP(a.rx)),ry=abs(MP(a.ry)),cp=cos(MP(a.rotation)),sp=sin(MP(a.rotation));
    MP dx=(MP(a.start[0])-MP(a.end[0]))/2,dy=(MP(a.start[1])-MP(a.end[1]))/2;
    MP x=cp*dx+sp*dy,y=-sp*dx+cp*dy;
    MP lambda=x*x/(rx*rx)+y*y/(ry*ry);
    bool scaled=lambda>1;
    if(scaled) { MP scale=sqrt(lambda);rx*=scale;ry*=scale; }
    MP q=rx*rx*ry*ry-ry*ry*x*x-rx*rx*y*y;
    if(a.rx==a.ry&&!scaled) q=rx*rx*(rx*rx-dx*dx-dy*dy);
    MP coeff=scaled||q==0?MP(0):sqrt(q/(ry*ry*x*x+rx*rx*y*y));
    if(a.large_arc==a.sweep) coeff=-coeff;
    MP px=coeff*rx*y/ry,py=-coeff*ry*x/rx;
    MP cx=cp*px-sp*py+(MP(a.start[0])+MP(a.end[0]))/2;
    MP cy=sp*px+cp*py+(MP(a.start[1])+MP(a.end[1]))/2;
    MP sx=(x-px)/rx,sy=(y-py)/ry,ex=(-x-px)/rx,ey=(-y-py)/ry;
    MP t0=atan2(sy,sx),delta=atan2(sx*ey-sy*ex,sx*ex+sy*ey),pi=acos(MP(-1));
    if(a.sweep&&delta<0) delta+=2*pi;
    if(!a.sweep&&delta>0) delta-=2*pi;
    MP ux=rx*cp,uy=rx*sp,vx=-ry*sp,vy=ry*cp;
    auto const &m=a.to_kernel;
    return {MP(m[0])*cx+MP(m[2])*cy+MP(m[4]),MP(m[1])*cx+MP(m[3])*cy+MP(m[5]),
            MP(m[0])*ux+MP(m[2])*uy,MP(m[1])*ux+MP(m[3])*uy,
            MP(m[0])*vx+MP(m[2])*vy,MP(m[1])*vx+MP(m[3])*vy,t0,delta,scaled};
}
void verify(ArcInput const &input,double eps) {
    auto decoded=decode_arc(input,eps); ASSERT_TRUE(decoded) << decoded.detail;
    auto const &a=*decoded.value;
    auto ref=reference(input);
    contains(a.c.x,ref.cx);contains(a.c.y,ref.cy);
    contains(a.u.x,ref.ux);contains(a.u.y,ref.uy);
    contains(a.v.x,ref.vx);contains(a.v.y,ref.vy);
    EXPECT_EQ(a.radii_scaled,ref.scaled);
    for(auto p:{a.c,a.u,a.v}) { EXPECT_LE(p.x.width(),eps/64);EXPECT_LE(p.y.width(),eps/64); }
    for(double t:{0.0,.01,.25,.5,.75,.99,1.0}) {
        auto point=point_at(a,t); ASSERT_TRUE(point) << point.detail;
        MP theta=ref.t0+MP(t)*ref.delta;
        contains(point.value->x,ref.cx+ref.ux*cos(theta)+ref.vx*sin(theta));
        contains(point.value->y,ref.cy+ref.uy*cos(theta)+ref.vy*sin(theta));
        auto angle=angle_at(a,t); ASSERT_TRUE(angle);
        auto inverse=parameter_at(a,*angle.value); ASSERT_TRUE(inverse);
        contains(*inverse.value,MP(t));
    }
    auto again=decode_arc(input,eps); ASSERT_TRUE(again);
    for(auto pair:{std::pair{a.c,again.value->c},std::pair{a.u,again.value->u},std::pair{a.v,again.value->v}}) {
        same(pair.first.x,pair.second.x);same(pair.first.y,pair.second.y);
    }
    same(a.theta_a,again.value->theta_a);same(a.theta_b,again.value->theta_b);
    EXPECT_FALSE(angle_at(a,-.001));EXPECT_FALSE(angle_at(a,1.001));
}
ArcInput circle(double radius=1) {
    return {Geom::Point(-radius,0),Geom::Point(radius,0),radius,radius,0,false,true,Geom::Affine()};
}
}
TEST(GKArc, NumericFloor) {
    EXPECT_EQ(*numeric_floor(0).value,0x1p-24);
    EXPECT_EQ(*numeric_floor(0x1p16).value,0x1p-24);
    EXPECT_EQ(*numeric_floor(up(0x1p16)).value,0x1p-23);
    EXPECT_EQ(*numeric_floor(0x1p20).value,0x1p-20);
    EXPECT_EQ(*numeric_floor(up(0x1p20)).value,0x1p-19);
    EXPECT_EQ(*numeric_floor(std::numeric_limits<double>::max()).value,0x1p984);
    EXPECT_FALSE(numeric_floor(-1));EXPECT_FALSE(numeric_floor(std::numeric_limits<double>::infinity()));
}
TEST(GKArc, SemicirclesAndNearSemicircles) {
    for(bool large:{false,true}) for(bool sweep:{false,true}) {
        auto a=circle(50);a.large_arc=large;a.sweep=sweep;
        verify(a,0x1p-24);
        for(double shortfall:{0x1p-10,0x1p-30,0x1p-45}) {
            a=circle();a.end=Geom::Point(1-shortfall,0);a.large_arc=large;a.sweep=sweep;
            verify(a,0x1p-24);
            auto d=decode_arc(a,0x1p-24);ASSERT_TRUE(d);
            Geom::EllipticalArc lib(a.start,a.rx,a.ry,a.rotation,a.large_arc,a.sweep,a.end);
            // Characterization only. The high-precision formulas above are the oracle.
            // Near a half circle, sqrt(2*rho*u) is the relevant conditioning scale.
            double bound=8*std::sqrt(2*a.rx*std::numeric_limits<double>::epsilon());
            auto centre=lib.center();
            EXPECT_LE(std::abs(centre[0]-(d.value->c.x.lo+d.value->c.x.hi)*.5),bound);
            EXPECT_LE(std::abs(centre[1]-(d.value->c.y.lo+d.value->c.y.hi)*.5),bound);
        }
    }
}
TEST(GKArc, EllipsesFlagsTransformsAndScaling) {
    for(bool large:{false,true}) for(bool sweep:{false,true}) {
        ArcInput a{Geom::Point(0,0),Geom::Point(100,5),80,30,.4,large,sweep,Geom::Affine()};
        verify(a,0x1p-24);
        a.to_kernel=Geom::Affine(2,.25,.5,1,0x1p20,-0x1p20);
        verify(a,0x1p-20);
        a={Geom::Point(0,0),Geom::Point(4,0),1,1,0,large,sweep,Geom::Affine()};
        verify(a,0x1p-24);
        a.rx=1;a.ry=2;a.end=Geom::Point(8,2);verify(a,0x1p-24);
    }
    auto small=circle(1e-20);verify(small,0x1p-24);
    auto tiny=circle(1e-100);verify(tiny,0x1p-24);
    auto huge=circle(1e8);verify(huge,.01);
    auto circular=circle();circular.rotation=.3;
    circular.to_kernel=Geom::Affine(2,0,0,2,100,200);
    verify(circular,0x1p-24);
    auto d=decode_arc(circular,0x1p-24);ASSERT_TRUE(d);EXPECT_TRUE(d.value->circular);
    same(d.value->v.x,-d.value->u.y);same(d.value->v.y,d.value->u.x);
    circular.to_kernel[3]=up(2);
    d=decode_arc(circular,0x1p-24);ASSERT_TRUE(d);EXPECT_FALSE(d.value->circular);
}
TEST(GKArc, RandomCoordinateRange) {
    std::mt19937_64 g(8275);
    for(int j=0;j<100;++j) {
        double x=(static_cast<double>(g()>>12)*0x1p-52)*0x1p21-0x1p20;
        double y=(static_cast<double>(g()>>12)*0x1p-52)*0x1p21-0x1p20;
        ArcInput a{Geom::Point(x,y),Geom::Point(x+50,y+10),100,80,.2*(j%20),bool(j&1),bool(j&2),Geom::Affine()};
        verify(a,*numeric_floor(std::max(std::abs(x)+50,std::abs(y)+10)).value);
    }
}
TEST(GKArc, ExplicitRefusals) {
    auto a=circle();a.rx=0;
    auto r=decode_arc(a,0x1p-24);EXPECT_EQ(r.status,Status::Unsupported);EXPECT_EQ(r.detail,"zero-radius arc is a line");
    a=circle();a.end=a.start;r=decode_arc(a,0x1p-24);EXPECT_FALSE(r);EXPECT_EQ(r.detail,"coincident arc endpoints");
    a=circle();a.rotation=.3;r=decode_arc(a,1e-30);EXPECT_FALSE(r);EXPECT_EQ(r.detail,"ill-conditioned arc");
    a=circle();a.rotation=17;r=decode_arc(a,0x1p-24);EXPECT_FALSE(r);EXPECT_EQ(r.status,Status::Unsupported);
    a=circle();a.to_kernel=Geom::Affine(1,0,1,0,0,0);EXPECT_FALSE(decode_arc(a,1));
    a=circle(1e-160);EXPECT_FALSE(decode_arc(a,0x1p-24));
    a=circle(1e100);EXPECT_FALSE(decode_arc(a,0x1p-24));
    a=circle();a.rx=std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(decode_arc(a,1).status,Status::InvalidInput);
    EXPECT_EQ(decode_arc(circle(),0).status,Status::InvalidInput);
}
TEST(GKArc, ProcessDigest) {
    uint64_t digest=UINT64_C(14695981039346656037);
    for(bool large:{false,true}) for(bool sweep:{false,true}) {
        ArcInput input{Geom::Point(1,2),Geom::Point(100,5),80,30,.4,large,sweep,Geom::Affine()};
        auto r=decode_arc(input,0x1p-24);ASSERT_TRUE(r);
        auto const &a=*r.value;
        for(auto p:{a.c,a.u,a.v}) {GKTest::hash(digest,p.x);GKTest::hash(digest,p.y);}
        GKTest::hash(digest,a.theta_a);GKTest::hash(digest,a.theta_b);
        for(double t:{0.0,1.0/3,1.0}) {
            auto p=point_at(a,t);ASSERT_TRUE(p);
            GKTest::hash(digest,p.value->x);GKTest::hash(digest,p.value->y);
        }
    }
    std::cout << "Arc process digest: " << std::hex << digest << std::dec << '\n';
}
