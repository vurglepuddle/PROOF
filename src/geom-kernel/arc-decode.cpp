// SPDX-License-Identifier: GPL-2.0-or-later
#include "arc-decode.h"
#include "dd.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Proof::GK {
namespace {
struct Refusal { Status status; std::string detail; };
// Checked arithmetic failures leave this routine through one catch, never through
// an unchecked optional or a floating-point invalid operation.
template <typename T> T take(Result<T> r) {
    if (!r) throw Refusal{r.status,r.detail};
    return std::move(*r.value);
}
[[noreturn]] void ill() { throw Refusal{Status::Unsupported,"ill-conditioned arc"}; }
bool bits(double a, double b) { return std::memcmp(&a,&b,sizeof a)==0; }
DD from_interval(Interval x) {
    if (!x.finite()) ill();
    if (x.lo==x.hi) return {x.lo};
    double m=x.lo*.5+x.hi*.5;
    return {m,0,std::max((Interval(m)-Interval(x.lo)).hi,(Interval(x.hi)-Interval(m)).hi)};
}
DD add(DD a, DD b) { return take(dd_add(a,b)); }
DD mul(DD a, DD b) { return take(dd_multiply(a,b)); }
IPoint transform(IPoint p, Geom::Affine const &a, bool translate) {
    IPoint r{p.x*Interval(a[0])+p.y*Interval(a[2]),p.x*Interval(a[1])+p.y*Interval(a[3])};
    if (translate) r=r+IPoint{Interval(a[4]),Interval(a[5])};
    return r;
}
bool similarity(Geom::Affine const &a) {
    // Signed zeros are the same exact zero matrix coefficient. All nonzero
    // coefficients must match bit for bit; there is no near-similarity test.
    return bits(a[0],a[3]) && ((a[1]==0&&a[2]==0)||bits(a[1],-a[2]));
}
}
Result<double> numeric_floor(double m) {
    if (!std::isfinite(m)||m<0) return Result<double>::failure(Status::InvalidInput,"invalid coordinate magnitude");
    int e; double f=std::frexp(std::max(m,0x1p16),&e);
    int exponent=f==.5?e-1:e;
    // Compute the scaled power directly, even when M2 itself would overflow.
    return Result<double>::success(std::ldexp(1.0,exponent-40));
}
Result<DecodedArc> decode_arc(ArcInput const &a, double eps) {
    if (auto env=numeric_environment(); !env) return Result<DecodedArc>::failure(env.status,env.detail);
    if (!std::isfinite(eps)||eps<=0) return Result<DecodedArc>::failure(Status::InvalidInput,"invalid arc epsilon");
    for (double x:{a.start[0],a.start[1],a.end[0],a.end[1],a.rx,a.ry,a.rotation,
                   a.to_kernel[0],a.to_kernel[1],a.to_kernel[2],a.to_kernel[3],a.to_kernel[4],a.to_kernel[5]})
        if (!std::isfinite(x)) return Result<DecodedArc>::failure(Status::InvalidInput,"nonfinite arc input");
    if (a.rx==0 || a.ry==0)
        return Result<DecodedArc>::failure(Status::Unsupported,"zero-radius arc is a line");
    if (a.start==a.end)
        return Result<DecodedArc>::failure(Status::Unsupported,"coincident arc endpoints");
    try {
        double rx=std::abs(a.rx),ry=std::abs(a.ry);
        auto trig=take(det_sincos(a.rotation));
        auto cp=trig.cos,sp=trig.sin;
        DD dx=mul(add(DD{a.start[0]},DD{-a.end[0]}),DD{.5});
        DD dy=mul(add(DD{a.start[1]},DD{-a.end[1]}),DD{.5});
        DD xp=add(mul(from_interval(cp),dx),mul(from_interval(sp),dy));
        DD yp=add(mul(negate(from_interval(sp)),dx),mul(from_interval(cp),dy));
        DD xx=mul(DD{rx},DD{rx}),yy=mul(DD{ry},DD{ry});
        DD q;
        if (rx==ry) {
            // Rotation invariance avoids interval dependency in x'^2+y'^2.
            q=mul(xx,add(add(xx,negate(mul(dx,dx))),negate(mul(dy,dy))));
        } else {
            q=add(add(mul(xx,yy),negate(mul(yy,mul(xp,xp)))),negate(mul(xx,mul(yp,yp))));
        }
        auto rad=enclosure(q),x=enclosure(xp),y=enclosure(yp);
        if (!rad.finite()||!x.finite()||!y.finite()) ill();
        Interval rxi(rx),ryi(ry),coef(0);
        bool scaled=rad.hi<0;
        if (scaled) {
            auto scale=take(sqrt(take(divide(square(x),square(rxi)))+take(divide(square(y),square(ryi)))));
            rxi=rxi*scale; ryi=ryi*scale;
            rad=Interval(0); // The corrected SVG ellipse has antipodal endpoints.
        } else if (rad.lo<0) {
            ill(); // The correction branch cannot be certified.
        }
        if (rad.hi>0) {
            auto denominator=square(ryi)*square(x)+square(rxi)*square(y);
            coef=take(sqrt(take(divide(rad,denominator))));
            if (a.large_arc==a.sweep) coef=-coef;
        }
        IPoint prime{coef*take(divide(rxi,ryi))*y,-coef*take(divide(ryi,rxi))*x};
        IPoint mid{(Interval(a.start[0])+Interval(a.end[0]))*Interval(.5),
                   (Interval(a.start[1])+Interval(a.end[1]))*Interval(.5)};
        IPoint centre{cp*prime.x-sp*prime.y,sp*prime.x+cp*prime.y};
        centre=centre+mid;
        DecodedArc out;
        out.c=transform(centre,a.to_kernel,true);
        out.u=transform({rxi*cp,rxi*sp},a.to_kernel,false);
        out.v=transform({-ryi*sp,ryi*cp},a.to_kernel,false);
        out.circular=bits(a.rx,a.ry)&&similarity(a.to_kernel);
        if (out.circular) out.v={-out.u.y,out.u.x}; // One shared radius and a quarter-turn.
        out.radii_scaled=scaled;
        auto determinant=enclosure(add(mul(DD{a.to_kernel[0]},DD{a.to_kernel[3]}),
                                       negate(mul(DD{a.to_kernel[1]},DD{a.to_kernel[2]}))));
        if (determinant.contains(0)) ill();
        for (auto p:{out.c,out.u,out.v}) for (auto t:{p.x,p.y})
            if (!t.finite()||t.width()>eps/64) ill();
        if (abs(cross(out.u,out.v)).hi==0) ill();
        IPoint start{take(divide(x-prime.x,rxi)),take(divide(y-prime.y,ryi))};
        IPoint end{take(divide(-x-prime.x,rxi)),take(divide(-y-prime.y,ryi))};
        out.theta_a=take(det_atan2(start.y,start.x));
        Interval delta;
        if (rad.lo==0 && rad.hi==0) delta=pi_interval();
        else {
            delta=take(det_atan2(abs(cross(start,end)),dot(start,end)));
            if (a.large_arc) delta=pi_interval()*Interval(2)-delta;
        }
        if (!a.sweep) delta=-delta;
        out.theta_b=out.theta_a+delta;
        return Result<DecodedArc>::success(out);
    } catch (Refusal const &r) {
        if (r.status==Status::Unsupported && r.detail=="angle exceeds 16 radians")
            return Result<DecodedArc>::failure(r.status,r.detail);
        return Result<DecodedArc>::failure(Status::Unsupported,"ill-conditioned arc");
    }
}
Result<Interval> angle_at(DecodedArc const &a, double lambda) {
    if (!std::isfinite(lambda)||lambda<0||lambda>1)
        return Result<Interval>::failure(Status::InvalidInput,"arc parameter outside [0,1]");
    if (lambda==0) return Result<Interval>::success(a.theta_a);
    if (lambda==1) return Result<Interval>::success(a.theta_b);
    return Result<Interval>::success(a.theta_a*(Interval(1)-Interval(lambda))+a.theta_b*Interval(lambda));
}
Result<Interval> parameter_at(DecodedArc const &a, Interval theta) {
    return divide(theta-a.theta_a,a.theta_b-a.theta_a);
}
Result<IPoint> point_at(DecodedArc const &a, double lambda) {
    auto theta=angle_at(a,lambda);
    if (!theta) return Result<IPoint>::failure(theta.status,theta.detail);
    auto sc=det_sincos(*theta.value);
    if (!sc) return Result<IPoint>::failure(sc.status,sc.detail);
    return Result<IPoint>::success(a.c+a.u*sc.value->cos+a.v*sc.value->sin);
}
} // namespace Proof::GK
