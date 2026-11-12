// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_GK_ARC_DECODE_H
#define PROOF_GK_ARC_DECODE_H
#include "det-math.h"
#include <2geom/affine.h>
#include <2geom/point.h>

namespace Proof::GK {
struct ArcInput {
    Geom::Point start, end;
    double rx, ry, rotation; // Rotation in radians, not SVG attribute degrees.
    bool large_arc, sweep;
    Geom::Affine to_kernel;
};
struct DecodedArc {
    IPoint c,u,v;
    Interval theta_a,theta_b;
    bool circular = false;
    bool radii_scaled = false;
};
Result<double> numeric_floor(double max_absolute_coordinate);
Result<DecodedArc> decode_arc(ArcInput const &input, double eps);
Result<Interval> angle_at(DecodedArc const &arc, double lambda);
Result<Interval> parameter_at(DecodedArc const &arc, Interval theta);
Result<IPoint> point_at(DecodedArc const &arc, double lambda);
} // namespace Proof::GK
#endif
