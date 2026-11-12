// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_GK_DD_H
#define PROOF_GK_DD_H
#include "interval.h"
namespace Proof::GK {
struct Sum { double hi, lo; };
Result<Sum> two_sum(double a, double b);
Result<Sum> fast_two_sum(double a, double b); // Refuses unless |a| >= |b|.
Result<Sum> two_product(double a, double b); // Refuses unrepresentable residuals.
// The represented real number is enclosed by hi+lo +/- error. Expansion compression
// records discarded terms; DD operations are not silently assumed error-free.
struct DD { double hi = 0, lo = 0, error = 0; };
Result<DD> dd_add(DD a, DD b);
Result<DD> dd_multiply(DD a, DD b);
DD negate(DD a);
Interval enclosure(DD a);
} // namespace Proof::GK
#endif
