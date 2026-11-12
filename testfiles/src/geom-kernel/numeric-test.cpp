// SPDX-License-Identifier: GPL-2.0-or-later
#include "reference.h"
#include "geom-kernel/dd.h"
#include <cfenv>
#include <cfloat>
#include <limits>
#include <thread>

using namespace Proof::GK;
using namespace GKTest;

TEST(GKNumeric, BuildAndRuntimeConditions) {
    static_assert(std::numeric_limits<double>::is_iec559);
    static_assert(FLT_EVAL_METHOD==0);
    EXPECT_TRUE(check_round_to_nearest()); EXPECT_TRUE(numeric_environment());
    // Restore even if a check fails; do not perform interval arithmetic in another mode.
    int old=std::fegetround();
    ASSERT_EQ(std::fesetround(FE_DOWNWARD),0);
    auto wrong=check_round_to_nearest();
    EXPECT_FALSE(wrong); EXPECT_EQ(wrong.status,Status::Unsupported);
    ASSERT_EQ(std::fesetround(old),0);
    std::thread other([] { EXPECT_TRUE(numeric_environment()); }); other.join();
}
TEST(GKNumeric, ExactInputsAndCertainPredicates) {
    for (double v:{0.0,-0.0,1.0,-1.0,0x1p-1074,0x1p20}) {
        Interval x(v); EXPECT_EQ(bits(x.lo),bits(v)); EXPECT_EQ(bits(x.hi),bits(v));
    }
    EXPECT_TRUE(Interval(1,2).positive()); EXPECT_TRUE(Interval(-2,-1).negative());
    EXPECT_FALSE(Interval(0,2).positive()); EXPECT_FALSE(Interval(-1,1).positive());
    EXPECT_TRUE(Interval(0,2).nonnegative()); EXPECT_TRUE(Interval(-2,0).nonpositive());
    EXPECT_TRUE(Interval(std::numeric_limits<double>::quiet_NaN()).is_empty());
    EXPECT_TRUE(intersection(Interval(1,2),Interval(3,4)).is_empty());
    same(hull(Interval(-2,1),Interval(0,4)),Interval(-2,4));
    same(intersection(Interval(-2,1),Interval(0,4)),Interval(0,1));
    same(min(Interval(-2,1),Interval(0,4)),Interval(-2,1));
    same(max(Interval(-2,1),Interval(0,4)),Interval(0,4));
    same(abs(Interval(-4,2)),Interval(0,4));
    EXPECT_FALSE(Interval::empty().positive());
}
TEST(GKNumeric, RandomAndAdversarialContainment) {
    std::mt19937_64 g(0x47ab19);
    for (int k=0;k<1800;++k) {
        double a=random_value(g,static_cast<int>(g()%2098)-1074);
        double b=random_value(g,static_cast<int>(g()%2098)-1074);
        if (!std::isfinite(a)||!std::isfinite(b)) continue;
        contains(Interval(a)+Interval(b),MP(a)+MP(b));
        contains(Interval(a)-Interval(b),MP(a)-MP(b));
        contains(Interval(a)*Interval(b),MP(a)*MP(b));
        contains(square(Interval(a)),MP(a)*MP(a));
        if (b!=0) { auto q=divide(Interval(a),Interval(b)); if(q) contains(*q.value,MP(a)/MP(b)); }
        auto r=sqrt(Interval(std::abs(a))); ASSERT_TRUE(r); contains(*r.value,sqrt(MP(std::abs(a))));
    }
    double tiny=std::numeric_limits<double>::denorm_min(),large=std::numeric_limits<double>::max();
    for (double a:{tiny,0x1p-1022,0x1p-24,0x1p-20,1.0,0x1p20,large}) {
        contains(Interval(a)+Interval(-a),MP(0));
        contains(Interval(a)-Interval(down(a)),MP(a)-MP(down(a)));
    }
    auto overflow=Interval(large)*Interval(2);
    EXPECT_EQ(overflow.lo,large); EXPECT_TRUE(std::isinf(overflow.hi));
    contains(overflow,MP(large)*2);
    auto negative=Interval(-large)*Interval(2);
    EXPECT_TRUE(std::isinf(negative.lo)); EXPECT_EQ(negative.hi,-large);
    EXPECT_FALSE(divide(Interval(large),Interval(tiny)));
    EXPECT_FALSE(divide(Interval(1),Interval(-tiny,tiny)));
    EXPECT_FALSE(sqrt(Interval(-tiny,1)));
    EXPECT_FALSE(sqrt(Interval::empty()));
    auto near_zero=divide(Interval(tiny),Interval(tiny)); ASSERT_TRUE(near_zero);
    contains(*near_zero.value,MP(1));
}
TEST(GKNumeric, IntervalCornersAndVectors) {
    std::mt19937_64 g(548);
    for (int k=0;k<400;++k) {
        double a=random_value(g,20),b=random_value(g,20);
        Interval x(std::min(a,b),std::max(a,b));
        double c=std::abs(random_value(g,-20)),d=std::abs(random_value(g,20));
        Interval y(std::min(c,d),std::max(c,d));
        auto q=divide(x,y); ASSERT_TRUE(q);
        for(double xx:{x.lo,x.hi}) for(double yy:{y.lo,y.hi}) {
            contains(x*y,MP(xx)*MP(yy)); contains(*q.value,MP(xx)/MP(yy));
        }
        IPoint v{x,y},w{Interval(.3),Interval(-2)};
        for(double xx:{x.lo,x.hi}) for(double yy:{y.lo,y.hi}) {
            contains(dot(v,w),MP(xx)*MP(.3)-2*MP(yy));
            contains(cross(v,w),-2*MP(xx)-MP(yy)*MP(.3));
            contains(norm_sq(v),MP(xx)*MP(xx)+MP(yy)*MP(yy));
            auto n=norm(v); ASSERT_TRUE(n); contains(*n.value,sqrt(MP(xx)*MP(xx)+MP(yy)*MP(yy)));
        }
    }
}
TEST(GKNumeric, ErrorFreeTransformsAndBoundedDD) {
    std::mt19937_64 g(94);
    for(int k=0;k<1200;++k) {
        double a=random_value(g,static_cast<int>(g()%801)-400);
        double b=random_value(g,static_cast<int>(g()%801)-400);
        auto s=two_sum(a,b); ASSERT_TRUE(s);
        EXPECT_EQ(MP(s.value->hi)+MP(s.value->lo),MP(a)+MP(b));
        auto p=two_product(a,b); ASSERT_TRUE(p);
        EXPECT_EQ(MP(p.value->hi)+MP(p.value->lo),MP(a)*MP(b));
        auto f=std::abs(a)>=std::abs(b)?fast_two_sum(a,b):fast_two_sum(b,a);
        ASSERT_TRUE(f); EXPECT_EQ(MP(f.value->hi)+MP(f.value->lo),MP(a)+MP(b));
        DD x{a,std::ldexp(a,-54)},y{b,std::ldexp(b,-54)};
        auto sum=dd_add(x,y),prod=dd_multiply(x,y);
        ASSERT_TRUE(sum); ASSERT_TRUE(prod);
        contains(enclosure(*sum.value),MP(x.hi)+MP(x.lo)+MP(y.hi)+MP(y.lo));
        contains(enclosure(*prod.value),(MP(x.hi)+MP(x.lo))*(MP(y.hi)+MP(y.lo)));
    }
    EXPECT_FALSE(fast_two_sum(1,2));
    EXPECT_FALSE(two_product(0x1p-1022,0x1p-1022));
    EXPECT_FALSE(two_product(std::numeric_limits<double>::max(),2));
    same(enclosure({1,0x1p-54,0}),Interval(1,up(1)));
    same(enclosure({1,-0x1p-54,0}),Interval(down(1),1));
    auto s=dd_add({1,0x1p-100,0},{-1,0,0}); ASSERT_TRUE(s);
    contains(enclosure(*s.value),MP(0x1p-100));
    auto ball=dd_multiply({1,0,.1},{-2,0,.2}); ASSERT_TRUE(ball);
    for(double a:{.9,1.1}) for(double b:{-2.2,-1.8}) contains(enclosure(*ball.value),MP(a)*MP(b));
}
TEST(GKNumeric, RepeatedBits) {
    auto reference=dd_multiply({1,0x1p-55,0},{.3,0x1p-58,0}); ASSERT_TRUE(reference);
    for(int k=0;k<40;++k) {
        auto r=dd_multiply({1,0x1p-55,0},{.3,0x1p-58,0}); ASSERT_TRUE(r);
        same(enclosure(*r.value),enclosure(*reference.value));
        same(Interval(1)-Interval(.3),Interval(1)-Interval(.3));
    }
}
