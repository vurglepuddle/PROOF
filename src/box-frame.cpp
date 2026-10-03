// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the orientation of an object's bounding box. See box-frame.h.
 */
#include "box-frame.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

#include <2geom/angle.h>
#include <2geom/transforms.h>

#include "object/sp-ellipse.h"
#include "object/sp-item-group.h"
#include "object/sp-rect.h"
#include "object/sp-star.h"
#include "object/sp-use.h"
#include "xml/node.h"

namespace Inkscape {
namespace {

/// Angles closer than this are the same, in radians (about 0.0006 degrees).
constexpr double SAME_ANGLE = 1e-5;
/// Box axes further from perpendicular than this, as the cosine between them, are sheared.
constexpr double SHEARED = 1e-6;

double local_angle(SPItem const &item)
{
    auto repr = item.getRepr();
    return repr ? Geom::rad_from_deg(repr->getAttributeDouble(BOX_ANGLE_ATTRIBUTE, 0.0)) : 0.0;
}

/// A box repeats every quarter turn: its angle in (-pi/4, pi/4].
double quarter(double angle)
{
    double a = std::remainder(angle, M_PI / 2);
    return a <= -M_PI / 4 ? a + M_PI / 2 : a;
}

/// An angle in (-pi, pi].
double half(double angle)
{
    double a = std::remainder(angle, 2 * M_PI);
    return a <= -M_PI ? a + 2 * M_PI : a;
}

bool same_box(double a, double b)
{
    return std::abs(std::remainder(a - b, M_PI / 2)) < SAME_ANGLE;
}

/// The angle of a box at `angle` after `affine`, or nothing when the affine shears the box so
/// that it is no longer a rectangle. A mirrored box could be read as turned either way round;
/// it is read as turned by less than a quarter turn either side, so flipping an upright object
/// leaves it at 0 and flipping one at 30 degrees gives -30.
std::optional<double> map_angle(double angle, Geom::Affine const &affine)
{
    auto const linear = affine.withoutTranslation();
    Geom::Point const axis(std::cos(angle), std::sin(angle));
    auto const u = axis * linear;
    auto const v = Geom::rot90(axis) * linear;
    double const lu = Geom::L2(u), lv = Geom::L2(v);
    if (lu < 1e-12 || lv < 1e-12 || std::abs(Geom::dot(u, v)) > SHEARED * lu * lv) {
        return {};
    }
    double const result = std::atan2(u.y(), u.x());
    if (linear.det() < 0) {
        double const a = half(result);
        return a > M_PI / 2 ? a - M_PI : a <= -M_PI / 2 ? a + M_PI : a;
    }
    return half(result);
}

void write_local_angle(SPItem &item, double angle)
{
    auto repr = item.getRepr();
    if (!repr) {
        return;
    }
    angle = half(angle);
    if (std::abs(angle) < SAME_ANGLE) {
        if (repr->attribute(BOX_ANGLE_ATTRIBUTE)) {
            repr->removeAttribute(BOX_ANGLE_ATTRIBUTE);
        }
        return;
    }
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(10) << Geom::deg_from_rad(angle);
    repr->setAttribute(BOX_ANGLE_ATTRIBUTE, os.str());
}

/**
 * Visit the objects whose boxes make up a box: the object itself, or the visible objects in a
 * group, and the original shown by a clone, with the transform of each to desktop coordinates.
 * The visitor returns false to stop.
 */
template <typename Visit>
bool visit_objects(SPItem &item, Geom::Affine const &i2dt, bool into_clones, Visit &&visit)
{
    if (item.isHidden()) {
        return true;
    }
    if (auto group = cast<SPGroup>(&item)) {
        for (auto &child : group->children) {
            if (auto child_item = cast<SPItem>(&child)) {
                if (!visit_objects(*child_item, child_item->transform * i2dt, into_clones, visit)) {
                    return false;
                }
            }
        }
        return true;
    }
    if (auto use = cast<SPUse>(&item)) {
        if (into_clones && use->child) {
            return visit_objects(*use->child, use->child->transform * use->get_xy_offset() * i2dt, into_clones, visit);
        }
        return true;
    }
    return visit(item, i2dt);
}

} // namespace

double box_angle(std::vector<SPItem *> const &items)
{
    std::optional<double> shared;
    bool mixed = false;
    for (auto item : items) {
        visit_objects(*item, item->i2dt_affine(), true, [&](SPItem &object, Geom::Affine const &i2dt) {
            // A sheared object has no rectangular box of its own, so it counts as upright.
            double const angle = quarter(map_angle(local_angle(object), i2dt).value_or(0));
            if (!shared) {
                shared = angle;
            } else if (!same_box(*shared, angle)) {
                mixed = true;
            }
            return !mixed;
        });
        if (mixed) {
            return 0;
        }
    }
    return shared.value_or(0);
}

std::optional<double> box_rotation(std::vector<SPItem *> const &items)
{
    std::optional<double> first;
    bool mixed = false;
    for (auto item : items) {
        visit_objects(*item, item->i2dt_affine(), true, [&](SPItem &object, Geom::Affine const &i2dt) {
            double const angle = map_angle(local_angle(object), i2dt).value_or(0);
            if (!first) {
                first = angle;
            } else if (!same_box(*first, angle)) {
                mixed = true;
            }
            return !mixed;
        });
        if (mixed) {
            return {};
        }
    }
    return first;
}

std::optional<Geom::Point> box_middle(std::vector<SPItem *> const &items, SPItem::BBoxType type)
{
    double const angle = box_angle(items);
    if (auto bounds = frame_bounds(items, angle, type)) {
        return bounds->midpoint() * Geom::Rotate(angle);
    }
    return {};
}

Geom::OptRect frame_bounds(std::vector<SPItem *> const &items, double angle, SPItem::BBoxType type, bool stroked)
{
    Geom::Affine const to_frame = Geom::Rotate(-angle);
    Geom::OptRect bounds;
    for (auto item : items) {
        auto const transform = item->i2dt_affine() * to_frame;
        if (stroked) {
            bounds.unionWith(item->visualBounds(transform, false, true, true));
        } else {
            bounds.unionWith(item->bounds(type, transform));
        }
    }
    return bounds;
}

void box_angle_embed(SPItem &item, Geom::Affine const &embedded)
{
    auto const linear = embedded.withoutTranslation();
    if (linear.isIdentity(1e-12)) {
        return; // Moves don't turn anything.
    }
    auto repr = item.getRepr();
    if (!repr) {
        return;
    }
    double const before = local_angle(item);
    if (auto after = map_angle(before, linear)) {
        if (std::abs(half(*after - before)) >= SAME_ANGLE) {
            write_local_angle(item, *after);
        }
    } else if (repr->attribute(BOX_ANGLE_ATTRIBUTE)) {
        repr->removeAttribute(BOX_ANGLE_ATTRIBUTE);
    }
}

bool reset_box_angle(SPItem &item)
{
    bool changed = false;
    visit_objects(item, item.i2dt_affine(), false, [&](SPItem &object, Geom::Affine const &i2dt) {
        auto const was = object.getRepr()->attribute(BOX_ANGLE_ATTRIBUTE);
        std::string const before = was ? was : "";
        // The angle in the object's own coordinates that points along the desktop's x axis.
        auto const linear = i2dt.withoutTranslation();
        if (linear.isSingular()) {
            return true;
        }
        auto const axis = Geom::Point(1, 0) * linear.inverse();
        double const upright = std::atan2(axis.y(), axis.x());
        if (map_angle(upright, linear)) {
            write_local_angle(object, upright);
        } else if (was) {
            object.getRepr()->removeAttribute(BOX_ANGLE_ATTRIBUTE);
        }
        auto const now = object.getRepr()->attribute(BOX_ANGLE_ATTRIBUTE);
        changed = changed || before != (now ? now : "");
        return true;
    });
    return changed;
}

std::vector<Geom::Point> shape_centers(std::vector<SPItem *> const &items, std::size_t limit)
{
    std::vector<Geom::Point> centers;
    bool too_many = false;
    for (auto item : items) {
        visit_objects(*item, item->i2dt_affine(), true, [&](SPItem &object, Geom::Affine const &i2dt) {
            std::optional<Geom::Point> center;
            if (auto rect = cast<SPRect>(&object)) {
                center = Geom::Point(rect->x.computed + rect->width.computed / 2,
                                     rect->y.computed + rect->height.computed / 2);
            } else if (auto ellipse = cast<SPGenericEllipse>(&object)) {
                center = Geom::Point(ellipse->cx.computed, ellipse->cy.computed);
            } else if (auto star = cast<SPStar>(&object)) {
                center = star->center;
            }
            if (center) {
                too_many = centers.size() >= limit;
                if (!too_many) {
                    centers.push_back(*center * i2dt);
                }
            }
            return !too_many;
        });
        if (too_many) {
            return {};
        }
    }
    return centers;
}

} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
