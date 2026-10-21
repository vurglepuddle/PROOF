// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Canvas belonging to SVG pattern.
 *//*
 * Authors:
 *   Tomasz Boczkowski <penginsbacon@gmail.com>
 *
 * Copyright (C) 2014 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cairomm/region.h>
#include <cairo.h>
#include "cairo-utils.h"
#include "display/drawing-item.h"
#include "drawing-context.h"
#include "drawing-pattern.h"
#include "drawing-surface.h"
#include "drawing.h"
#include "helper/geom.h"
#include "ui/util.h"

namespace Inkscape {

auto constexpr PATTERN_MATRIX_EPSILON = 1e-18;

DrawingPattern::Surface::Surface(Geom::IntRect const &rect, int device_scale)
    : rect(rect)
    , surface(Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, rect.width() * device_scale, rect.height() * device_scale))
{
    cairo_surface_set_device_scale(surface->cobj(), device_scale, device_scale);
}

DrawingPattern::DrawingPattern(Drawing &drawing)
    : DrawingGroup(drawing)
    , _overflow_steps(1)
{
}

void DrawingPattern::setPatternToUserTransform(Geom::Affine const &transform)
{
    defer([=, this] {
        auto current = _pattern_to_user ? *_pattern_to_user : Geom::identity();
        if (Geom::are_near(transform, current, PATTERN_MATRIX_EPSILON)) {
            return;
        }
        _markForRendering();
        _pattern_to_user = transform.isIdentity(PATTERN_MATRIX_EPSILON) ? nullptr : std::make_unique<Geom::Affine>(transform);
        _markForUpdate(STATE_ALL, true);
    });
}

void DrawingPattern::setTileRect(Geom::Rect const &tile_rect)
{
    defer([=, this] {
        _tile_rect = tile_rect;
        _markForUpdate(STATE_ALL, true);
    });
}

void DrawingPattern::setOverflow(Geom::Affine const &initial_transform, int steps, Geom::Affine const &step_transform)
{
    defer([=, this] {
        _overflow_initial_transform = initial_transform;
        _overflow_steps = steps;
        _overflow_step_transform = step_transform;
    });
}

cairo_pattern_t *DrawingPattern::renderPattern(RenderContext &rc, Geom::IntRect const &area, float opacity, int device_scale) const
{
    if (opacity < 1e-3) {
        // Invisible.
        return nullptr;
    }

    if (!_tile_rect || _tile_rect->hasZeroArea()) {
        // Empty.
        return nullptr;
    }

    // Calculate various transforms.
    auto const dt = Geom::Translate(-_tile_rect->min()) * Geom::Scale(_pattern_resolution / _tile_rect->dimensions()); // AKA user_to_tile.
    if (dt.isSingular(PATTERN_MATRIX_EPSILON)) {
        return nullptr;
    }
    auto const idt = dt.inverse();
    auto const pattern_to_tile = _pattern_to_user ? _pattern_to_user->inverse() * dt : dt;
    auto const screen_to_tile = _ctm.inverse() * pattern_to_tile;

    // Return a canonical choice of rectangle with the same periodic tiling as rect.
    auto canonicalised = [&, this] (Geom::IntRect rect) {
        for (int i = 0; i < 2; i++) {
            if (rect.dimensions()[i] >= _pattern_resolution[i]) {
                rect[i] = {0, _pattern_resolution[i]};
            } else {
                rect[i] -= Util::round_down(rect[i].min(), _pattern_resolution[i]);
            }
        }
        return rect;
    };

    // Return whether the periodic tiling of a contains the periodic tiling of b.
    auto wrapped_contains = [&] (Geom::IntRect const &a, Geom::IntRect const &b) {
        auto check = [&] (int i) {
            int const period = _pattern_resolution[i];
            if (a[i].extent() >= period) return true;
            if (b[i].extent() > a[i].extent()) return false;
            return Util::round_down(b[i].min() - a[i].min(), period) >= b[i].max() - a[i].max();
        };
        return check(0) && check(1);
    };

    // Return whether the periodic tiling of a intersects with or touches the periodic tiling of b.
    auto wrapped_touches = [&] (Geom::IntRect const &a, Geom::IntRect const &b) {
        auto check = [&] (int i) {
            int const period = _pattern_resolution[i];
            if (a[i].extent() >= period) return true;
            if (b[i].extent() >= period) return true;
            return Util::round_down(b[i].max() - a[i].min(), period) >= b[i].min() - a[i].max();
        };
        return check(0) && check(1);
    };

    // Calculate the minimum and maximum translates of a that overlap with b.
    auto overlapping_translates = [&, this] (Geom::IntRect const &a, Geom::IntRect const &b) {
        Geom::IntPoint min, max;
        for (int i = 0; i < 2; i++) {
            min[i] = Util::round_up  (b[i].min() - a[i].max() + 1, _pattern_resolution[i]);
            max[i] = Util::round_down(b[i].max() - a[i].min() - 1, _pattern_resolution[i]);
        }
        return std::make_pair(min, max);
    };

    // Paint the periodic tiling of a into b, and remove the painted region from dirty.
    auto wrapped_paint = [&, this] (Surface const &a, Geom::IntRect &b, Cairo::RefPtr<Cairo::Context> const &cr, Cairo::RefPtr<Cairo::Region> const &dirty) {
        auto const [min, max] = overlapping_translates(a.rect, b);
        for (int x = min.x(); x <= max.x(); x += _pattern_resolution.x()) {
            for (int y = min.y(); y <= max.y(); y += _pattern_resolution.y()) {
                auto const rect = a.rect + Geom::IntPoint(x, y);
                dirty->subtract(geom_to_cairo(rect));
                cr->set_source(a.surface, rect.left(), rect.top());
                cr->paint();
            }
        }
    };

    // Calculate the requested area to draw within tile rasterisation space.
    auto const area_orig = (Geom::Rect(area) * screen_to_tile).roundOutwards();
    auto const area_tile = canonicalised(area_orig);

    // Simplest solution for now to protecting pattern cache is a mutex. This makes all
    // pattern rendering single-threaded, however patterns are typically not the bottleneck.
    auto lock = std::lock_guard(mutables);

    auto get_surface = [&, this] () -> std::pair<Surface*, Cairo::RefPtr<Cairo::Region>> {
        // If there is a rectangle containing the requested area, just use that.
        for (auto &s : surfaces) {
            if (wrapped_contains(s.rect, area_tile)) {
                return { &s, {} };
            }
        }

        // Otherwise, recursively merge the requested area with all overlapping or touching rectangles, and paint the missing part.
        std::vector<Surface> merged;
        auto expanded = area_tile;

        while (true) {
            bool modified = false;

            for (auto it = surfaces.begin(); it != surfaces.end(); ) {
                if (wrapped_touches(expanded, it->rect)) {
                    expanded.unionWith(it->rect + round_down(expanded.max() - it->rect.min(), _pattern_resolution));
                    merged.emplace_back(std::move(*it));
                    *it = std::move(surfaces.back());
                    surfaces.pop_back();
                    modified = true;
                } else {
                    ++it;
                }
            }

            if (!modified) break;
        }

        // Canonicalise the expanded rectangle. (Stops Cairo's coordinates overflowing and the pattern disappearing.)
        expanded = canonicalised(expanded);

        // Create a new surface covering the expanded rectangle.
        auto surface = Surface(expanded, device_scale);
        auto cr = Cairo::Context::create(surface.surface);
        cr->translate(-surface.rect.left(), -surface.rect.top());

        // Paste all the old surfaces into the new surface, tracking the remaining dirty region.
        auto dirty = Cairo::Region::create(geom_to_cairo(expanded));

        for (auto &m : merged) {
            wrapped_paint(m, expanded, cr, dirty);
        }

        // Emplace the surface, and return it along with the remaining dirty region.
        surfaces.emplace_back(std::move(surface));
        return std::make_pair(&surfaces.back(), std::move(dirty));
    };

    // Find an already-drawn surface containing the requested area, or create if it none exists.
    auto [surface, dirty] = get_surface();

    // Draw the pattern contents to the dirty areas of the surface, taking care of possible wrapping.
    Inkscape::DrawingContext dc(surface->surface->cobj(), surface->rect.min());
    if (rc.antialiasing_override) {
        apply_antialias(dc, rc.antialiasing_override.value());
    }

    auto paint = [&, this] (Geom::IntRect const &rect) {
        if (_overflow_steps == 1) {
            render(dc, rc, rect);
        } else {
            // Overflow transforms need to be transformed to the old coordinate system
            // before stretching to the pattern resolution.
            auto const initial_transform = idt * _overflow_initial_transform * dt;
            auto const step_transform    = idt * _overflow_step_transform    * dt;
            dc.transform(initial_transform);
            for (int i = 0; i < _overflow_steps; i++) {
                // render() fails to handle transforms applied here when using cache.
                render(dc, rc, rect, RENDER_BYPASS_CACHE);
                dc.transform(step_transform);
                // auto raw = pattern_surface.raw();
                // auto filename = "drawing-pattern" + std::to_string(i) + ".png";
                // cairo_surface_write_to_png(pattern_surface.raw(), filename.c_str());
            }
        }
    };

    if (dirty) {
        for (int i = 0; i < dirty->get_num_rectangles(); i++) {
            auto const rect = cairo_to_geom(dirty->get_rectangle(i));
            for (int x = 0; x <= 1; x++) {
                for (int y = 0; y <= 1; y++) {
                    auto const wrap = _pattern_resolution * Geom::IntPoint(x, y);
                    auto const rect2 = rect & Geom::IntRect(wrap, wrap + _pattern_resolution);
                    if (!rect2) continue;
                    auto save = DrawingContext::Save(dc);
                    // Clip to rectangle to be drawn.
                    dc.rectangle(*rect2);
                    dc.clip();
                    // Draw the pattern.
                    dc.translate(wrap);
                    paint(*rect2 - wrap);
                    // Apply opacity, if necessary.
                    if (opacity < 1.0 - 1e-3) {
                        dc.setOperator(CAIRO_OPERATOR_DEST_IN);
                        dc.setSource(0.0, 0.0, 0.0, opacity);
                        dc.paint();
                    }
                }
            }
        }
        dirty.reset();
    }

    // Debug: Show pattern tile.
    // surface->surface->write_to_png("/tmp/patternsurface.png");

    // Create and return pattern.
    auto cp = cairo_pattern_create_for_surface(surface->surface->cobj());
    auto const shift = surface->rect.min() + round_down(area_orig.min() - surface->rect.min(), _pattern_resolution);
    ink_cairo_pattern_set_matrix(cp, pattern_to_tile * Geom::Translate(-shift));
    cairo_pattern_set_extend(cp, CAIRO_EXTEND_REPEAT);
    if (rc.antialiasing_override && rc.antialiasing_override.value() == Antialiasing::None) {
        cairo_pattern_set_filter(cp, CAIRO_FILTER_NEAREST);
    }
    return cp;
}

unsigned DrawingPattern::_updateItem(Geom::IntRect const &area, UpdateContext const &ctx, unsigned flags, unsigned reset)
{
    _dropPatternCache();

    if (!_tile_rect || _tile_rect->hasZeroArea()) {
        return STATE_NONE;
    }

    // Calculate the desired resolution of a pattern tile.
    double const det_ctm = ctx.ctm.det();
    double const det_ps2user = _pattern_to_user ? _pattern_to_user->det() : 1.0;
    double scale = std::sqrt(std::abs(det_ctm * det_ps2user));
    // Fixme: When scale is too big (zooming in a pattern), Cairo doesn't render the pattern.
    // More precisely it fails when setting pattern matrix in DrawingPattern::renderPattern.
    // Correct solution should make use of visible area and change pattern tile rect accordingly.
    auto const c = _tile_rect->dimensions() * scale;
    // Rounded up, but not for what is a whole number of pixels up to the last digits: a tile of
    // 36.00000002 pixels (a pattern read back from a file's decimals) would be drawn at 37 and
    // resampled, softer than the same tile at 36.
    auto const whole = [](double v) { return v > 1 ? std::ceil(v - 1e-4) : std::ceil(v); };
    _pattern_resolution = Geom::IntPoint(whole(c.x()), whole(c.y()));

    // Map tile rect to the origin and stretch it to the desired resolution.
    auto const dt = Geom::Translate(-_tile_rect->min()) * Geom::Scale(_pattern_resolution / _tile_rect->dimensions());

    // Apply this transform to the actual pattern tree.
    return DrawingGroup::_updateItem(Geom::IntRect::infinite(), { dt }, flags, reset);
}

void DrawingPattern::_dropPatternCache()
{
    surfaces.clear();
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
