// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_GK_INTERVAL_H
#define PROOF_GK_INTERVAL_H
#include "status.h"
#include <cfloat>
#include <limits>

namespace Proof::GK {
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8 &&
              std::numeric_limits<double>::digits == 53, "GK requires IEC 559 binary64");
static_assert(FLT_EVAL_METHOD == 0, "GK does not support x87 extended evaluation");

Result<bool> check_round_to_nearest();
// Called once per calling thread; the rounding mode must then remain unchanged.
// Low-level arithmetic assumes that the caller has obtained a successful check.
Result<bool> numeric_environment();
double down(double x);
double up(double x);
struct Interval {
    double lo = 0, hi = 0;
    Interval() = default;
    explicit Interval(double x);
    Interval(double lower, double upper);
    static Interval empty();
    bool is_empty() const { return lo > hi; }
    bool finite() const;
    bool contains(double x) const { return lo <= x && x <= hi; }
    bool positive() const { return lo > 0 && !is_empty(); }
    bool negative() const { return hi < 0 && !is_empty(); }
    bool nonnegative() const { return lo >= 0 && !is_empty(); }
    bool nonpositive() const { return hi <= 0 && !is_empty(); }
    double width() const;
};
// Extended bounds are allowed, but NaNs never enter a nonempty interval. Overflow
// enclosures remain visible as infinities. Checked operations refuse nonfinite results.
Interval operator+(Interval a, Interval b);
Interval operator-(Interval a, Interval b);
Interval operator-(Interval a);
Interval operator*(Interval a, Interval b);
Result<Interval> divide(Interval a, Interval b);
Interval square(Interval a);
Result<Interval> sqrt(Interval a);
Interval abs(Interval a);
Interval min(Interval a, Interval b);
Interval max(Interval a, Interval b);
Interval hull(Interval a, Interval b);
Interval intersection(Interval a, Interval b);
struct IPoint { Interval x, y; };
IPoint operator+(IPoint a, IPoint b);
IPoint operator-(IPoint a, IPoint b);
IPoint operator*(IPoint a, Interval b);
Interval dot(IPoint a, IPoint b);
Interval cross(IPoint a, IPoint b);
Interval norm_sq(IPoint a);
Result<Interval> norm(IPoint a); // lo is lo|v|, hi is hi|v|.
} // namespace Proof::GK
#endif
