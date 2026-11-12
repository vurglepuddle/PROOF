// SPDX-License-Identifier: GPL-2.0-or-later
#include "dd.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace Proof::GK {
Result<Sum> two_sum(double a, double b) {
    double s = a+b;
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(s))
        return Result<Sum>::failure(Status::NumericalFailure, "two-sum overflow or nonfinite input");
    double bv = s-a;
    return Result<Sum>::success({s,(a-(s-bv))+(b-bv)});
}
Result<Sum> fast_two_sum(double a, double b) {
    if (std::abs(a) < std::abs(b))
        return Result<Sum>::failure(Status::InvalidInput, "fast-two-sum magnitude order");
    auto r = two_sum(a,b);
    if (!r) return r;
    double s = a+b;
    return Result<Sum>::success({s,b-(s-a)});
}
Result<Sum> two_product(double a, double b) {
    double p = a*b;
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(p))
        return Result<Sum>::failure(Status::NumericalFailure, "two-product overflow or nonfinite input");
    if (a == 0 || b == 0) return Result<Sum>::success({p,0});
    int ea,eb;
    std::frexp(a,&ea); std::frexp(b,&eb);
    // A product of two 53-bit significands can have a bit at ea+eb-106.
    // Below -1074 its residual cannot in general be represented by any double.
    if (ea+eb < -968)
        return Result<Sum>::failure(Status::Unsupported, "two-product residual may underflow");
    // Explicit fma has IEEE single rounding even with -ffp-contract=off. UCRT64
    // x86-64 supplies fma without requiring a global -mfma or CPU-specific build.
    return Result<Sum>::success({p,std::fma(a,b,-p)});
}
namespace {
bool valid(DD a) { return std::isfinite(a.hi) && std::isfinite(a.lo) && std::isfinite(a.error) && a.error >= 0; }
double add_error(double a, double b) { return a == 0 && b == 0 ? 0 : up(a+b); }
Result<DD> compress(std::array<double,8> terms, unsigned count, double error) {
    // Grow an exact, nonoverlapping expansion using error-free sums; retaining the
    // largest two terms leaves a remainder bounded by the sum of the omitted terms.
    std::array<double,9> e{},next{};
    unsigned size = 0;
    for (unsigned j=0;j<count;++j) {
        double q = terms[j]; unsigned n = 0;
        for (unsigned i=0;i<size;++i) {
            auto s = two_sum(q,e[i]);
            if (!s) return Result<DD>::failure(s.status,s.detail);
            if (s.value->lo != 0) next[n++] = s.value->lo;
            q = s.value->hi;
        }
        next[n++] = q; e = next; size = n;
    }
    for (unsigned i=0;i+2<size;++i) error = add_error(error,std::abs(e[i]));
    DD r{e[size-1],size>1?e[size-2]:0,error};
    if (!valid(r)) return Result<DD>::failure(Status::NumericalFailure,"DD overflow");
    return Result<DD>::success(r);
}
}
DD negate(DD a) { return {-a.hi,-a.lo,a.error}; }
Result<DD> dd_add(DD a, DD b) {
    if (!valid(a) || !valid(b)) return Result<DD>::failure(Status::InvalidInput,"invalid DD");
    return compress({a.lo,a.hi,b.lo,b.hi},4,add_error(a.error,b.error));
}
Result<DD> dd_multiply(DD a, DD b) {
    if (!valid(a) || !valid(b)) return Result<DD>::failure(Status::InvalidInput,"invalid DD");
    double error = 0;
    if (a.error || b.error) {
        error = (abs(enclosure({a.hi,a.lo,0}))*Interval(b.error) +
                 abs(enclosure({b.hi,b.lo,0}))*Interval(a.error) +
                 Interval(a.error)*Interval(b.error)).hi;
    }
    std::array<double,8> terms{}; unsigned n = 0;
    for (double x : {a.lo,a.hi}) for (double y : {b.lo,b.hi}) {
        auto p = two_product(x,y);
        if (!p) return Result<DD>::failure(p.status,p.detail);
        terms[n++] = p.value->lo; terms[n++] = p.value->hi;
    }
    return compress(terms,n,error);
}
Interval enclosure(DD a) {
    if (!valid(a)) return Interval::empty();
    auto s = two_sum(a.hi,a.lo);
    if (!s) return Interval::empty();
    double x=s.value->hi,e=s.value->lo;
    Interval r(e<0?down(x):x,e>0?up(x):x);
    return a.error == 0 ? r : r+Interval(-a.error,a.error);
}
} // namespace Proof::GK
