// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_GK_DET_MATH_H
#define PROOF_GK_DET_MATH_H
#include "interval.h"
namespace Proof::GK {
constexpr double supported_angle = 16.0; // Radians, including the endpoints.
struct SinCos { Interval sin, cos; };
Result<SinCos> det_sincos(double angle);
Result<SinCos> det_sincos(Interval angle);
Interval pi_interval();
// A continuous choice of argument for a rectangle excluding the origin. A box
// crossing the negative x axis is unwrapped about +pi, rather than widened to 2pi.
Result<Interval> det_atan2(Interval y, Interval x);
} // namespace Proof::GK
#endif
