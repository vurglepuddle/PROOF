// SPDX-License-Identifier: GPL-2.0-or-later
//

#include "transform-objects.h"

#include <glibmm/i18n.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

#include "box-frame.h"
#include "desktop.h"
#include "preferences.h"
#include "selection.h"
#include "object/algorithms/bboxsort.h"
#include "object/sp-item-transform.h"

using namespace Inkscape;

void transform_move(Inkscape::Selection* selection,
                    double x, double y,
                    bool relative,
                    bool apply_separately,
                    double yaxisdir)
{
    if (!selection || selection->isEmpty()) return;

    // If relative, Y is specified in screen coordinates (UI); adapt to document y-axis direction
    if (relative) {
        y *= yaxisdir;
    }

    if (!apply_separately) {
        if (relative) {
            selection->moveRelative(x, y);
        } else {
            Geom::OptRect bbox = selection->preferredBounds();
            if (bbox) {
                selection->moveRelative(x - bbox->min()[Geom::X], y - bbox->min()[Geom::Y]);
            }
        }
    } else {
        if (relative) {
            // shift each object relatively to the previous one
            auto selected = selection->items_vector();
            if (selected.empty()) return;

            if (fabs(x) > 1e-6) {
                std::vector<BBoxSort> sorted;
                for (auto item : selected) {
                    Geom::OptRect bbox = item->desktopPreferredBounds();
                    if (bbox) {
                        sorted.emplace_back(item, *bbox, Geom::X, x > 0 ? 1. : 0., x > 0 ? 0. : 1.);
                    }
                }
                std::stable_sort(sorted.begin(), sorted.end());

                double move = x;
                for (auto it = sorted.begin(); it < sorted.end(); ++it) {
                    it->item->move_rel(Geom::Translate(move, 0));
                    move += x;
                }
            }
            if (fabs(y) > 1e-6) {
                std::vector<BBoxSort> sorted;
                for (auto item : selected) {
                    Geom::OptRect bbox = item->desktopPreferredBounds();
                    if (bbox) {
                        sorted.emplace_back(item, *bbox, Geom::Y, y > 0 ? 1. : 0., y > 0 ? 0. : 1.);
                    }
                }
                std::stable_sort(sorted.begin(), sorted.end());

                double move = y;
                for (auto it = sorted.begin(); it < sorted.end(); ++it) {
                    it->item->move_rel(Geom::Translate(0, move));
                    move += y;
                }
            }
        } else {
            // absolute positioning per selection bbox (preserve previous behavior)
            Geom::OptRect bbox = selection->preferredBounds();
            if (bbox) {
                selection->moveRelative(x - bbox->min()[Geom::X], y - bbox->min()[Geom::Y]);
            }
        }
    }
}

void transform_scale(Inkscape::Selection* selection,
                     double sx, double sy,
                     bool is_percent,
                     bool apply_separately,
                     bool transform_stroke,
                     bool preserve)
{
    if (!selection || selection->isEmpty()) return;

    if (apply_separately) {
        auto tmp = selection->items();
        for (auto item : tmp) {
            Geom::OptRect bbox_pref = item->desktopPreferredBounds();
            Geom::OptRect bbox_geom = item->desktopGeometricBounds();
            if (bbox_pref && bbox_geom) {
                double new_width = sx;
                double new_height = sy;
                if (is_percent) {
                    new_width = sx / 100 * bbox_pref->width();
                    new_height = sy / 100 * bbox_pref->height();
                }
                if (fabs(new_width) < 1e-6) new_width = 1e-6;
                if (fabs(new_height) < 1e-6) new_height = 1e-6;

                double x0 = bbox_pref->midpoint()[Geom::X] - new_width / 2;
                double y0 = bbox_pref->midpoint()[Geom::Y] - new_height / 2;
                double x1 = bbox_pref->midpoint()[Geom::X] + new_width / 2;
                double y1 = bbox_pref->midpoint()[Geom::Y] + new_height / 2;

                Geom::Affine scaler = get_scale_transform_for_variable_stroke(*bbox_pref, *bbox_geom, transform_stroke, preserve, x0, y0, x1, y1);
                item->set_i2d_affine(item->i2dt_affine() * scaler);
                item->doWriteTransform(item->transform);
            }
        }
    } else {
        Geom::OptRect bbox_pref = selection->preferredBounds();
        Geom::OptRect bbox_geom = selection->geometricBounds();
        if (bbox_pref && bbox_geom) {
            double new_width = sx;
            double new_height = sy;
            if (is_percent) {
                new_width = sx / 100 * bbox_pref->width();
                new_height = sy / 100 * bbox_pref->height();
            }
            if (fabs(new_width) < 1e-6) new_width = 1e-6;
            if (fabs(new_height) < 1e-6) new_height = 1e-6;

            double x0 = bbox_pref->midpoint()[Geom::X] - new_width / 2;
            double y0 = bbox_pref->midpoint()[Geom::Y] - new_height / 2;
            double x1 = bbox_pref->midpoint()[Geom::X] + new_width / 2;
            double y1 = bbox_pref->midpoint()[Geom::Y] + new_height / 2;
            Geom::Affine scaler = get_scale_transform_for_variable_stroke(*bbox_pref, *bbox_geom, transform_stroke, preserve, x0, y0, x1, y1);
            selection->applyAffine(scaler);
        }
    }
}

void transform_rotate(Inkscape::Selection* selection,
                      double angle_degrees,
                      bool apply_separately)
{
    if (!selection || selection->isEmpty()) return;

    auto const type = Preferences::get()->getInt("/tools/bounding_box") == 0
                        ? SPItem::VISUAL_BBOX : SPItem::GEOMETRIC_BBOX;
    if (apply_separately) {
        auto tmp = selection->items();
        for (auto item : tmp) {
            if (auto const center = box_rotation_center({item}, type)) {
                // Keep SPItem::rotate_rel's per-item transform handling, using the turned box pivot.
                bool const explicit_center = item->isCenterSet();
                auto const affine = Geom::Translate(-*center) * Geom::Rotate::from_degrees(angle_degrees)
                                  * Geom::Translate(*center);
                item->set_i2d_affine(item->i2dt_affine() * affine);
                item->doWriteTransform(item->transform);
                if (explicit_center) {
                    item->setCenter(*center);
                    item->updateRepr();
                }
            }
        }
    } else {
        auto const center = box_rotation_center(selection->items_vector(), type);
        if (center) {
            selection->rotateRelative(*center, angle_degrees);
        }
    }
}

void transform_skew(Inkscape::Selection* selection,
                    double hx, double hy,
                    SkewUnits units,
                    bool apply_separately,
                    double yaxisdir)
{
    if (!selection || selection->isEmpty()) return;

    if (units == SkewUnits::Percent) {
        hy *= yaxisdir; // match UI behavior for vertical direction
        if (fabs(0.01 * hx * 0.01 * hy - 1.0) < Geom::EPSILON) {
            selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
            return;
        }
        if (apply_separately) {
            auto items = selection->items();
            for (auto item : items) {
                item->skew_rel(0.01 * hx, 0.01 * hy);
            }
        } else {
            std::optional<Geom::Point> center = selection->center();
            if (center) {
                selection->skewRelative(*center, 0.01 * hx, 0.01 * hy);
            }
        }
    } else if (units == SkewUnits::AngleRadians) {
        double angleX = hx;
        double angleY = hy;
        if ((fabs(angleX - angleY + M_PI / 2) < Geom::EPSILON)
            || (fabs(angleX - angleY - M_PI / 2) < Geom::EPSILON)
            || (fabs((angleX - angleY) / 3 + M_PI / 2) < Geom::EPSILON)
            || (fabs((angleX - angleY) / 3 - M_PI / 2) < Geom::EPSILON)) {
            selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
            return;
        }
        double skewX = tan(angleX);
        double skewY = tan(angleY);
        if (apply_separately) {
            auto items = selection->items();
            for (auto item : items) {
                item->skew_rel(skewX, skewY);
            }
        } else {
            std::optional<Geom::Point> center = selection->center();
            if (center) {
                selection->skewRelative(*center, skewX, skewY);
            }
        }
    } else { // Absolute (linear) displacement inputs
        double x = hx;
        double y = hy * yaxisdir;
        if (apply_separately) {
            auto items = selection->items();
            for (auto item : items) {
                Geom::OptRect bbox = item->desktopPreferredBounds();
                if (!bbox) continue;
                double height = bbox->height();
                double width = bbox->width();
                if (fabs(x / height * y / width - 1.0) < Geom::EPSILON) {
                    selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
                    return;
                }
                item->skew_rel(x / height, y / width);
            }
        } else {
            std::optional<Geom::Point> center = selection->center();
            Geom::OptRect bbox = selection->preferredBounds();
            if (center && bbox) {
                double height = bbox->height();
                double width = bbox->width();
                if (fabs(x / height * y / width - 1.0) < Geom::EPSILON) {
                    selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
                    return;
                }
                selection->skewRelative(*center, x / height, y / width);
            }
        }
    }
}

void transform_apply_matrix(Inkscape::Selection* selection,
                            const Geom::Affine& affine,
                            bool replace_matrix)
{
    if (!selection || selection->isEmpty()) return;

    if (affine.isSingular()) {
        selection->desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Transform matrix is singular, <b>not used</b>."));
        return;
    }

    if (replace_matrix) {
        auto tmp = selection->items();
        for (auto item : tmp) {
            item->set_item_transform(affine);
            item->updateRepr();
        }
    } else {
        // post-multiply each object's transform
        selection->applyAffine(affine);
    }
}
