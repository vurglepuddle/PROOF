// SPDX-License-Identifier: GPL-2.0-or-later
#include "det-math.h"
#include "dd.h"
#include <algorithm>
#include <cmath>

namespace Proof::GK {
namespace {
/* Error argument (round-to-nearest, gradual underflow, no contraction):
 * For |x| <= 16 choose integer n = round(x / pio2[0]). Subtract n times
 * each of three pio2 limbs separately in DD; do not round n*pi/2 first.
 * The sum of the limbs differs from pi/2 by < 2^-160. Its multiplication
 * by |n| <= 11 contributes < 11*2^-160. DD expansion compression records
 * the absolute sum of every discarded term, so cancellation loses no bound.
 * Refuse unless the resulting remainder enclosure is inside [-.786,.786].
 * Horner evaluates the degree-29 sine and degree-28 cosine Taylor polynomials
 * in bounded DD. Coefficients below enclose the exact rational 1/k!: the
 * stored radius covers hi+lo rounding (decimal generation used 130 digits).
 * Taylor remainders on this range are bounded by |r|*.786^30/31! and
 * .786^30/30!, respectively; both are below |r|*2^-116 and 2^-116.
 * Polynomial evaluation propagates the reduction radius as input uncertainty.
 * Final conversion encloses hi+lo and the accumulated radius, then intersects
 * with [-1,1]. These are enclosures, not estimates based on libc sin/cos.
 * A final width check refuses a scalar enclosure wider than four ulp at its
 * largest endpoint. Thus successful results have a checked small width too.
 * For |x| <= 2^-27, |sin(x)-x| <= |x|^3/6 is less than one predecessor
 * spacing (also in the subnormal binade); 0 <= 1-cos(x) <= x*x/2 < 2^-55.
 * The direct one-sided enclosures avoid squaring subnormal inputs in DD.
 */
constexpr double pio2[] = {0x1.921fb54442d18p+0,0x1.1a62633145c07p-54,-0x1.f1976b7ed8fbcp-110};
constexpr DD sine[] = {
    {0x1.0000000000000p+0,0x0.0p+0,0x0.0p+0},
    {-0x1.5555555555555p-3,-0x1.5555555555555p-57,0x1.5555555555556p-111},
    {0x1.1111111111111p-7,0x1.1111111111111p-63,0x1.1111111111112p-119},
    {-0x1.a01a01a01a01ap-13,-0x1.a01a01a01a01ap-73,0x1.a01a01a01a01bp-133},
    {0x1.71de3a556c734p-19,-0x1.c154f8ddc6c00p-73,0x1.71de3a556c735p-127},
    {-0x1.ae64567f544e4p-26,0x1.c062e06d1f209p-80,0x1.c7880adcbc46fp-136},
    {0x1.6124613a86d09p-33,0x1.f28e0cc748ebep-87,0x1.7b2c4c8a840bdp-141},
    {-0x1.ae7f3e733b81fp-41,-0x1.1d8656b0ee8cbp-97,0x1.6e142a138f826p-157},
    {0x1.952c77030ad4ap-49,0x1.ac981465ddc6cp-103,0x1.588b72e53bc60p-165},
    {-0x1.2f49b46814157p-57,-0x1.2650f61dbdcb4p-112,0x1.69502917cbf3cp-166},
    {0x1.71b8ef6dcf572p-66,-0x1.d043ae40c4647p-120,0x1.486121e81d5ffp-176},
    {-0x1.761b41316381ap-75,0x1.3423c7d91404fp-130,0x1.e6135bfc1194bp-185},
    {0x1.3f3ccdd165fa9p-84,-0x1.58ddadf344487p-139,0x1.e8ed8001ad67fp-193},
    {-0x1.d1ab1c2dccea3p-94,-0x1.054d0c78aea14p-149,0x1.196bf16c33a57p-203},
    {0x1.259f98b4358adp-103,0x1.eaf8c39dd9bc5p-157,0x1.6e29990a26fb7p-211},
};
constexpr DD cosine[] = {
    {0x1.0000000000000p+0,0x0.0p+0,0x0.0p+0},
    {-0x1.0000000000000p-1,0x0.0p+0,0x0.0p+0},
    {0x1.5555555555555p-5,0x1.5555555555555p-59,0x1.5555555555556p-113},
    {-0x1.6c16c16c16c17p-10,0x1.f49f49f49f49fp-65,0x1.27d27d27d27d3p-119},
    {0x1.a01a01a01a01ap-16,0x1.a01a01a01a01ap-76,0x1.a01a01a01a01bp-136},
    {-0x1.27e4fb7789f5cp-22,-0x1.cbbc05b4fa99ap-76,0x1.c6d278883e8f6p-132},
    {0x1.1eed8eff8d898p-29,-0x1.2aec959e14c06p-83,0x1.2fb0073dd2d9fp-139},
    {-0x1.93974a8c07c9dp-37,-0x1.05d6f8a2efd1fp-92,0x1.3aa3346236a5ep-147},
    {0x1.ae7f3e733b81fp-45,0x1.1d8656b0ee8cbp-101,0x1.6e142a138f826p-161},
    {-0x1.6827863b97d97p-53,-0x1.eec01221a8b0bp-107,0x1.568798662118cp-161},
    {0x1.e542ba4020225p-62,0x1.ea72b4afe3c2fp-120,0x1.44020dfd65c8dp-174},
    {-0x1.0ce396db7f853p-70,0x1.aebcdbd20331cp-124,0x1.38a88578b4d76p-178},
    {0x1.f2cf01972f578p-80,-0x1.9ada5fcc1ab14p-135,0x1.440ce7fd610ddp-189},
    {-0x1.88e85fc6a4e5ap-89,0x1.71c37ebd16540p-143,0x1.494676265a365p-197},
    {0x1.0a18a2635085dp-98,0x1.b9e2e28e1aa54p-153,0x1.a8549a9d99587p-207},
};

Result<DD> polynomial(DD z, DD const *c) {
    DD p=c[14];
    for (int j=13;j>=0;--j) {
        auto m=dd_multiply(p,z);
        if (!m) return m;
        auto a=dd_add(*m.value,c[j]);
        if (!a) return a;
        p=*a.value;
    }
    return Result<DD>::success(p);
}
Result<Interval> atan_scalar(double y, double x) {
    auto pi=pi_interval();
    if (y==0) return Result<Interval>::success(x<0?pi:Interval(0));
    if (x==0) return Result<Interval>::success(y<0?-pi*Interval(.5):pi*Interval(.5));
    double ax=std::abs(x),ay=std::abs(y);
    auto z=divide(Interval(std::min(ax,ay)),Interval(std::max(ax,ay)));
    if (!z) return z;
    auto root=sqrt(Interval(1)+square(*z.value));
    if (!root) return root;
    auto q=divide(*z.value,Interval(1)+*root.value);
    if (!q) return q;
    if (abs(*q.value).hi>.415) return Result<Interval>::failure(Status::NumericalFailure,"atan reduction");
    auto zz=square(*q.value);
    Interval p;
    for (int k=48;k>=0;--k) {
        auto c=divide(Interval(k%2?-1:1),Interval(2*k+1));
        if (!c) return c;
        p=*c.value+zz*p;
    }
    // atan z = 2 atan q. Alternating remainder <= 2*.415^99/99 < 2^-118.
    Interval a=Interval(2)*(*q.value)*p+Interval(-0x1p-118,0x1p-118);
    if (ay>ax) a=pi*Interval(.5)-a;
    if (x<0) a=pi-a;
    if (y<0) a=-a;
    return Result<Interval>::success(a);
}
}
Interval pi_interval() { return Interval(0x1.921fb54442d18p+1,up(0x1.921fb54442d18p+1)); }
Result<SinCos> det_sincos(double x) {
    if (auto env=numeric_environment(); !env) return Result<SinCos>::failure(env.status,env.detail);
    if (!std::isfinite(x)) return Result<SinCos>::failure(Status::InvalidInput,"nonfinite angle");
    if (std::abs(x)>supported_angle) return Result<SinCos>::failure(Status::Unsupported,"angle exceeds 16 radians");
    if (x==0) return Result<SinCos>::success({Interval(x),Interval(1)});
    if (std::abs(x)<=0x1p-27)
        return Result<SinCos>::success({x>0?Interval(down(x),x):Interval(x,up(x)),Interval(down(1),1)});
    int n=static_cast<int>(std::round(x/pio2[0]));
    DD r{x};
    for (double limb:pio2) {
        auto m=dd_multiply(DD{static_cast<double>(n)},DD{limb});
        if (!m) return Result<SinCos>::failure(m.status,m.detail);
        auto a=dd_add(r,negate(*m.value));
        if (!a) return Result<SinCos>::failure(a.status,a.detail);
        r=*a.value;
    }
    r.error=up(r.error+std::abs(n)*0x1p-160);
    if (abs(enclosure(r)).hi>.786) return Result<SinCos>::failure(Status::NumericalFailure,"sincos reduction");
    auto z=dd_multiply(r,r);
    if (!z) return Result<SinCos>::failure(z.status,z.detail);
    auto s=polynomial(*z.value,sine),c=polynomial(*z.value,cosine);
    if (!s || !c) return Result<SinCos>::failure(Status::NumericalFailure,"sincos polynomial");
    s=dd_multiply(r,*s.value);
    if (!s) return Result<SinCos>::failure(s.status,s.detail);
    s.value->error=up(s.value->error+abs(enclosure(r)).hi*0x1p-116);
    c.value->error=up(c.value->error+0x1p-116);
    Interval si=enclosure(*s.value),co=enclosure(*c.value);
    SinCos out;
    switch ((n%4+4)%4) {
        case 0: out={si,co}; break;
        case 1: out={co,-si}; break;
        case 2: out={-si,-co}; break;
        default: out={-co,si};
    }
    out.sin=intersection(out.sin,Interval(-1,1));
    out.cos=intersection(out.cos,Interval(-1,1));
    for (auto i:{out.sin,out.cos}) {
        double m=std::max(std::abs(i.lo),std::abs(i.hi));
        if (!i.finite() || i.width()>4*(up(m)-m))
            return Result<SinCos>::failure(Status::NumericalFailure,"sincos enclosure too wide");
    }
    return Result<SinCos>::success(out);
}
Result<SinCos> det_sincos(Interval x) {
    if (!x.finite()) return Result<SinCos>::failure(Status::InvalidInput,"nonfinite angle interval");
    auto a=det_sincos(x.lo),b=det_sincos(x.hi);
    if (!a || !b) return Result<SinCos>::failure(Status::Unsupported,"angle exceeds supported range");
    SinCos r{hull(a.value->sin,b.value->sin),hull(a.value->cos,b.value->cos)};
    for (int k=-10;k<=10;++k) {
        if (intersection(pi_interval()*Interval(k*.5),x).is_empty()) continue;
        int q=(k%4+4)%4;
        if (q==1 || q==3) r.sin=hull(r.sin,Interval(q==1?1:-1));
        else r.cos=hull(r.cos,Interval(q==0?1:-1));
    }
    return Result<SinCos>::success(r);
}
Result<Interval> det_atan2(Interval y, Interval x) {
    if (!x.finite() || !y.finite() || (x.contains(0)&&y.contains(0)))
        return Result<Interval>::failure(Status::Unsupported,"undefined argument enclosure");
    Interval r=Interval::empty();
    bool unwrap=x.hi<0 && y.contains(0);
    for (double xx:{x.lo,x.hi}) for (double yy:{y.lo,y.hi}) {
        auto a=atan_scalar(yy,xx);
        if (!a) return a;
        if (unwrap && yy<0) *a.value=*a.value+pi_interval()*Interval(2);
        r=hull(r,*a.value);
    }
    return Result<Interval>::success(r);
}
} // namespace Proof::GK
