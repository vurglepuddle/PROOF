// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_GK_TEST_REFERENCE_H
#define PROOF_GK_TEST_REFERENCE_H
#include <boost/multiprecision/cpp_bin_float.hpp>
#include <gtest/gtest.h>
#include "geom-kernel/interval.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

namespace GKTest {
using MP=boost::multiprecision::number<boost::multiprecision::cpp_bin_float<256,
          boost::multiprecision::digit_base_2>>;
inline void contains(Proof::GK::Interval i, MP const &x) {
    ASSERT_FALSE(i.is_empty());
    EXPECT_LE(MP(i.lo),x); EXPECT_GE(MP(i.hi),x);
}
inline uint64_t bits(double x) { uint64_t b; std::memcpy(&b,&x,sizeof b); return b; }
inline void hash(uint64_t &h, Proof::GK::Interval i) {
    for (auto word:{bits(i.lo),bits(i.hi)}) h=(h^word)*UINT64_C(1099511628211);
}
inline void same(Proof::GK::Interval a, Proof::GK::Interval b) {
    EXPECT_EQ(bits(a.lo),bits(b.lo)); EXPECT_EQ(bits(a.hi),bits(b.hi));
}
inline double random_value(std::mt19937_64 &g, int exponent) {
    double m=1+static_cast<double>(g()>>12)*0x1p-52;
    return std::ldexp(g()&1?m:-m,exponent);
}
}
#endif
