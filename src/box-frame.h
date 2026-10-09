// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the orientation of an object's bounding box.
 *
 * As in Illustrator, a rotated object keeps a rotated box until Object > Transform > Reset
 * Bounding Box. Shapes, text, images and clones keep their rotation in their transform, so their
 * box follows it. Paths take transforms into their nodes; they keep the angle in the
 * proof:box-angle attribute instead, the counterpart of Illustrator's BBAccumRotation art tag.
 * A group, or several objects, share a rotated box only when all their objects have the same
 * angle; otherwise the box is upright.
 */
#ifndef SEEN_PROOF_BOX_FRAME_H
#define SEEN_PROOF_BOX_FRAME_H

#include <cstddef>
#include <optional>
#include <vector>

#include <2geom/affine.h>
#include <2geom/point.h>
#include <2geom/rect.h>

#include "object/sp-item.h"

namespace Inkscape {

/// The angle of an object's box in its own coordinates, in degrees from -180 to 180, when it is
/// not upright there.
inline constexpr char const *BOX_ANGLE_ATTRIBUTE = "proof:box-angle";

/// The angle of the box these objects share, in desktop coordinates: radians in (-pi/4, pi/4],
/// and 0 when their angles differ. A box repeats every quarter turn, so a rectangle turned by
/// 30 degrees and one turned by 120 share a box.
double box_angle(std::vector<SPItem *> const &items);

/// The angle these objects are turned by, as Illustrator shows it in Transform: radians in
/// desktop coordinates, from -pi to pi, of the first object when all share a box (a rectangle
/// turned by 120 degrees shows 120, not 30), and nothing when the box is upright because their
/// angles differ.
std::optional<double> box_rotation(std::vector<SPItem *> const &items);

/// The middle of the box these objects share, in desktop coordinates.
std::optional<Geom::Point> box_middle(std::vector<SPItem *> const &items, SPItem::BBoxType type);

/// The rotation pivot: the selection's explicit centre, or the middle of the box.
std::optional<Geom::Point> box_rotation_center(std::vector<SPItem *> const &items, SPItem::BBoxType type);

/// The bounds of the objects in the frame of a box at `angle` (desktop coordinates turned by
/// -angle), where that box is upright. `stroked` gives visual bounds without filters.
Geom::OptRect frame_bounds(std::vector<SPItem *> const &items, double angle, SPItem::BBoxType type,
                           bool stroked = false);

/// Keep an object's box angle when `embedded` goes into its own coordinates, as when a path
/// takes a transform into its nodes. A transform that shears the box removes the angle.
void box_angle_embed(SPItem &item, Geom::Affine const &embedded);

/// Object > Transform > Reset Bounding Box: make the box of the object, or of each object in a
/// group, upright again without moving anything. Clones keep their box. Returns whether
/// anything changed.
bool reset_box_angle(SPItem &item);

/// The centres Illustrator shows for the shapes (rectangles, ellipses, polygons and stars)
/// among these objects and in their groups, in desktop coordinates. Empty when there are more
/// than `limit` of them.
std::vector<Geom::Point> shape_centers(std::vector<SPItem *> const &items, std::size_t limit);

} // namespace Inkscape

#endif // SEEN_PROOF_BOX_FRAME_H
