// SPDX-License-Identifier: GPL-2.0-or-later
#include "interval.h"
#include <algorithm>
#include <cfenv>
#include <cmath>

namespace Proof::GK {
namespace {
constexpr double inf = std::numeric_limits<double>::infinity();
Interval rounded(double lo, double hi) {
    if (std::isnan(lo) || std::isnan(hi)) return Interval(-inf, inf);
    return Interval(down(lo), up(hi));
}
double product(double a, double b) { return a == 0 || b == 0 ? 0 : a * b; }
}
Result<bool> check_round_to_nearest() {
    if (std::fegetround() != FE_TONEAREST)
        return Result<bool>::failure(Status::Unsupported, "round-to-nearest is required");
    // Reject FTZ/DAZ as well: IEC 559 compilation alone does not ensure gradual underflow.
    volatile double tiny = std::numeric_limits<double>::min();
    volatile double denorm = std::numeric_limits<double>::denorm_min();
    if (tiny * 0.5 == 0 || denorm == 0)
        return Result<bool>::failure(Status::Unsupported, "gradual underflow is required");
    return Result<bool>::success(true);
}
Result<bool> numeric_environment() {
    static thread_local auto const result = check_round_to_nearest();
    return result;
}
double down(double x) { return std::nextafter(x, -inf); }
double up(double x) { return std::nextafter(x, inf); }
Interval::Interval(double x) : Interval(x, x) {}
Interval::Interval(double lower, double upper) : lo(lower), hi(upper) {
    if (std::isnan(lo) || std::isnan(hi) || lo > hi) { lo = inf; hi = -inf; }
}
Interval Interval::empty() { return Interval(inf, -inf); }
bool Interval::finite() const { return !is_empty() && std::isfinite(lo) && std::isfinite(hi); }
double Interval::width() const { return is_empty() ? 0 : (lo == hi ? 0 : up(hi - lo)); }
Interval operator+(Interval a, Interval b) {
    return a.is_empty() || b.is_empty() ? Interval::empty() : rounded(a.lo + b.lo, a.hi + b.hi);
}
Interval operator-(Interval a, Interval b) { return a + (-b); }
Interval operator-(Interval a) { return a.is_empty() ? a : Interval(-a.hi, -a.lo); }
Interval operator*(Interval a, Interval b) {
    if (a.is_empty() || b.is_empty()) return Interval::empty();
    double p[] = {product(a.lo,b.lo), product(a.lo,b.hi), product(a.hi,b.lo), product(a.hi,b.hi)};
    return rounded(*std::min_element(p,p+4), *std::max_element(p,p+4));
}
Result<Interval> divide(Interval a, Interval b) {
    if (!a.finite() || !b.finite()) return Result<Interval>::failure(Status::InvalidInput, "nonfinite division");
    if (b.contains(0)) return Result<Interval>::failure(Status::NumericalFailure, "divisor contains zero");
    double p[] = {a.lo/b.lo, a.lo/b.hi, a.hi/b.lo, a.hi/b.hi};
    auto r = rounded(*std::min_element(p,p+4), *std::max_element(p,p+4));
    if (!r.finite()) return Result<Interval>::failure(Status::NumericalFailure, "division overflow");
    return Result<Interval>::success(r);
}
Interval square(Interval a) {
    if (a.is_empty()) return a;
    double lo = a.contains(0) ? 0 : std::min(a.lo*a.lo, a.hi*a.hi);
    double hi = std::max(a.lo*a.lo, a.hi*a.hi);
    return Interval(std::max(0.0, down(lo)), up(hi));
}
Result<Interval> sqrt(Interval a) {
    if (!a.finite()) return Result<Interval>::failure(Status::InvalidInput, "nonfinite square root");
    if (a.lo < 0) return Result<Interval>::failure(Status::NumericalFailure, "negative square root interval");
    return Result<Interval>::success(Interval(std::max(0.0, down(std::sqrt(a.lo))), up(std::sqrt(a.hi))));
}
Interval abs(Interval a) {
    if (a.is_empty()) return a;
    return Interval(a.contains(0) ? 0 : std::min(std::abs(a.lo),std::abs(a.hi)),
                    std::max(std::abs(a.lo),std::abs(a.hi)));
}
Interval min(Interval a, Interval b) {
    return a.is_empty() || b.is_empty() ? Interval::empty() : Interval(std::min(a.lo,b.lo),std::min(a.hi,b.hi));
}
Interval max(Interval a, Interval b) {
    return a.is_empty() || b.is_empty() ? Interval::empty() : Interval(std::max(a.lo,b.lo),std::max(a.hi,b.hi));
}
Interval hull(Interval a, Interval b) {
    if (a.is_empty()) return b;
    if (b.is_empty()) return a;
    return Interval(std::min(a.lo,b.lo),std::max(a.hi,b.hi));
}
Interval intersection(Interval a, Interval b) { return Interval(std::max(a.lo,b.lo),std::min(a.hi,b.hi)); }
IPoint operator+(IPoint a, IPoint b) { return {a.x+b.x,a.y+b.y}; }
IPoint operator-(IPoint a, IPoint b) { return {a.x-b.x,a.y-b.y}; }
IPoint operator*(IPoint a, Interval b) { return {a.x*b,a.y*b}; }
Interval dot(IPoint a, IPoint b) { return a.x*b.x+a.y*b.y; }
Interval cross(IPoint a, IPoint b) { return a.x*b.y-a.y*b.x; }
Interval norm_sq(IPoint a) { return square(a.x)+square(a.y); }
Result<Interval> norm(IPoint a) {
    auto n = norm_sq(a);
    return sqrt(intersection(n,Interval(0,inf)));
}
} // namespace Proof::GK
