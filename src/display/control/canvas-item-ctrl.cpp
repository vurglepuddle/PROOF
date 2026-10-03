// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * A class to represent a control node.
 */
/*
 * Authors:
 *   Tavmjong Bah
 *   Sanidhya Singh
 *
 * Copyright (C) 2020 Tavmjong Bah
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "canvas-item-ctrl.h"

#include <2geom/transforms.h>
#include <algorithm>
#include <array>
#include <cairomm/context.h>
#include <cmath>
#include <iostream>

#include "ctrl-handle-rendering.h"
#include "preferences.h" // Default size.
#include "ui/widget/canvas.h"

// Render handles at different sizes and save them to "handles.png".
constexpr bool DUMP_HANDLES = false;
constexpr bool DRAW_BOUNDS = true;

// size range
constexpr int MIN_INDEX = 1;
constexpr int MAX_INDEX = 15;

namespace Inkscape {

/**
 * Create a null control node.
 */
CanvasItemCtrl::CanvasItemCtrl(CanvasItemGroup *group)
    : CanvasItem(group)
{
    _name = "CanvasItemCtrl:Null";
    _pickable = true; // Everybody gets events from this class!
}

/**
 * Create a control with type.
 */
CanvasItemCtrl::CanvasItemCtrl(CanvasItemGroup *group, CanvasItemCtrlType type)
    : CanvasItem(group)
    , _handle{.type = type}
{
    _name = "CanvasItemCtrl:Type_" + std::to_string(_handle.type);
    _pickable = true; // Everybody gets events from this class!
    set_size_default();

    // for debugging
    _dump();
}

void CanvasItemCtrl::_dump()
{
    // Ensure dead code is not emitted if flag is off.
    if (!DUMP_HANDLES) {
        return;
    }

    // Note: Atomicity not required.
    static bool first_run = true;
    if (!first_run) return;
    first_run = false;

    constexpr int step = 60;
    constexpr int h = MAX_INDEX;
    constexpr bool iterate_all_types = true;
    std::vector<CanvasItemCtrlType> types = {
        CANVAS_ITEM_CTRL_TYPE_ADJ_HANDLE, CANVAS_ITEM_CTRL_TYPE_ADJ_SKEW, CANVAS_ITEM_CTRL_TYPE_ADJ_ROTATE,
        CANVAS_ITEM_CTRL_TYPE_ADJ_CENTER, CANVAS_ITEM_CTRL_TYPE_ADJ_SALIGN, CANVAS_ITEM_CTRL_TYPE_ADJ_CALIGN,
        CANVAS_ITEM_CTRL_TYPE_ADJ_MALIGN,
        CANVAS_ITEM_CTRL_TYPE_POINT, // dot-like handle, indicator
        CANVAS_ITEM_CTRL_TYPE_CENTER,
        CANVAS_ITEM_CTRL_TYPE_MARKER,
        CANVAS_ITEM_CTRL_TYPE_NODE_AUTO, CANVAS_ITEM_CTRL_TYPE_NODE_CUSP,
        CANVAS_ITEM_CTRL_TYPE_NODE_SMOOTH,
        CANVAS_ITEM_CTRL_TYPE_GUIDE_HANDLE,
        CANVAS_ITEM_CTRL_TYPE_POINTER, // pointy, triangular handle
    };
    // device scale to use; 1 - low res, 2 - high res, 3 - very high res
    constexpr int scale = 1;

    if (iterate_all_types) {
        types.clear();
        for (int i = 0; i < RUBBERBAND_RECT; i++) {
            types.push_back(static_cast<CanvasItemCtrlType>(i));
        }
    }

    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, (types.size() + 1) * step * scale, (h + 1) * step * scale);
    cairo_surface_set_device_scale(surface->cobj(), 1, 1);
    auto buf = CanvasItemBuffer{
        .rect = Geom::IntRect(0, 0, surface->get_width(), surface->get_height()),
        .device_scale = scale,
        .cr = Cairo::Context::create(surface),
        .outline_pass = false
    };

    auto ctx = buf.cr;
    ctx->set_source_rgb(1, 0.9, 0.9);
    ctx->paint();
    ctx->set_source_rgba(0, 0, 1, 0.2);
    ctx->set_line_width(scale);
    constexpr double pix = scale & 1 ? 0.5 : 0;
    for (int size = 1; size <= h; ++size) {
        double y = size * step * scale + pix;
        ctx->move_to(0, y);
        ctx->line_to(surface->get_width(), y);
        ctx->stroke();
    }
    for (int i = 1; i <= types.size(); i++) {
        double x = i * step * scale + pix;
        ctx->move_to(x, 0);
        ctx->line_to(x, surface->get_height());
        ctx->stroke();
    }

    cairo_surface_set_device_scale(surface->cobj(), scale, scale);

    set_hover();
    for (int size = 1; size <= h; ++size) {
        int i = 1;
        for (auto type : types) {
            set_type(type);
            set_size_via_index(size);
            if (type == CANVAS_ITEM_CTRL_TYPE_ADJ_HANDLE || type == CANVAS_ITEM_CTRL_TYPE_ADJ_ROTATE ||
                type == CANVAS_ITEM_CTRL_TYPE_ADJ_CALIGN) {
                _anchor = SP_ANCHOR_SOUTH_EAST;
            } else {
                _anchor = SP_ANCHOR_CENTER;
            }
            _position = Geom::IntPoint{i++, size} * step;
            _update(false);
            _render(buf);
        }
    }

    cairo_surface_set_device_scale(surface->cobj(), scale, scale);
    surface->write_to_png("handles.png");
}

/**
 * Create a control ctrl. Point is in document coordinates.
 */
CanvasItemCtrl::CanvasItemCtrl(CanvasItemGroup *group, CanvasItemCtrlType type, Geom::Point const &p)
    : CanvasItemCtrl(group, type)
{
    _position = p;
    request_update();
}

/**
 * Set the position. Point is in document coordinates.
 */
void CanvasItemCtrl::set_position(Geom::Point const &position)
{
    defer([=, this] {
        if (_position == position) return;
        _position = position;
        request_update();
    });
}

/**
 * Returns distance between point in canvas units and position of ctrl.
 */
double CanvasItemCtrl::closest_distance_to(Geom::Point const &p) const
{
    // TODO: Different criteria for different shapes.
    return Geom::distance(p, _position * affine());
}

/**
 * If tolerance is zero, returns true if point p (in canvas units) is inside bounding box,
 * else returns true if p (in canvas units) is within tolerance (canvas units) distance of ctrl.
 * The latter assumes ctrl center anchored.
 */
bool CanvasItemCtrl::contains(Geom::Point const &p, double tolerance)
{
    if (_pick_zone) {
        return _pick_zone_contains(p);
    }
    // TODO: Different criteria for different shapes.
    if (!_bounds) {
        return false;
    }
    if (tolerance == 0) {
        return _bounds->interiorContains(p);
    } else {
        return closest_distance_to(p) <= tolerance;
    }
}

void CanvasItemCtrl::set_fill(uint32_t fill)
{
    defer([=, this] {
        _fill_set = true;
        if (_fill == fill) return;
        _fill = fill;
        _built.reset();
        request_redraw();
    });
}

void CanvasItemCtrl::set_stroke(uint32_t stroke)
{
    defer([=, this] {
        _stroke_set = true;
        if (_stroke == stroke) return;
        _stroke = stroke;
        _built.reset();
        request_redraw();
    });
}

void CanvasItemCtrl::set_shape(CanvasItemCtrlShape shape)
{
    defer([=, this] {
        _shape_set = true;
        if (_shape == shape) return;
        _shape = shape;
        _built.reset();
        request_update(); // Geometry could change
    });
}

void CanvasItemCtrl::_set_size(int size)
{
    defer([=, this] {
        if (_width == size) return;
        _width  = size;
        _built.reset();
        request_update(); // Geometry change
    });
}

static int get_size_default() {
    return Preferences::get()->getIntLimited("/options/grabsize/value", 3, MIN_INDEX, MAX_INDEX);
}

void CanvasItemCtrl::set_size(HandleSize rel_size) {
    _rel_size = rel_size;
    set_size_via_index(get_size_default());
}

void CanvasItemCtrl::set_size_via_index(int size_index)
{
    // Size must always be an odd number to center on pixel.
    if (size_index < MIN_INDEX || size_index > MAX_INDEX) {
        std::cerr << "CanvasItemCtrl::set_size_via_index: size_index out of range!" << std::endl;
        size_index = 3;
    }

    auto size = std::clamp(size_index + static_cast<int>(_rel_size), MIN_INDEX, MAX_INDEX);
    _set_size(size);
}

float CanvasItemCtrl::get_width() const {
    auto const &style = _context->handlesCss()->style_map.at(_handle);
    auto size = _width * style.scale() + style.size_extra();
    return size;
}

float CanvasItemCtrl::get_total_width() const {
    const auto& style = _context->handlesCss()->style_map.at(_handle);
    auto width = get_width() + get_stroke_width() + 2 * style.outline_width();
    return width;
}

void CanvasItemCtrl::set_size_default()
{
    int size = get_size_default();
    set_size_via_index(size);
}

void CanvasItemCtrl::set_type(CanvasItemCtrlType type)
{
    defer([=, this] {
        if (_handle.type == type) return;
        _handle.type = type;
        set_size_default();
        _built.reset();
        request_update(); // Possible geometry change
    });
}

void CanvasItemCtrl::set_selected(bool selected)
{
    defer([=, this] {
        _handle.selected = selected;
        _built.reset();
        request_update();
    });
}

void CanvasItemCtrl::set_click(bool click)
{
    defer([=, this] {
        _handle.click = click;
        _built.reset();
        request_update();
    });
}

void CanvasItemCtrl::set_hover(bool hover)
{
    defer([=, this] {
        _handle.hover = hover;
        _built.reset();
        request_update();
    });
}

/**
 * Reset the state to normal or normal selected
 */
void CanvasItemCtrl::set_normal(bool selected)
{
    defer([=, this] {
        _handle.selected = selected;
        _handle.hover = false;
        _handle.click = false;
        _built.reset();
        request_update();
    });
}

void CanvasItemCtrl::set_preferred_size_parity(int value)
{
    defer([=, this] {
        if (_size_parity == value) {
            return;
        }
        _size_parity = value;
        request_redraw();
    });
}

void CanvasItemCtrl::set_pick_zone(std::optional<PickZone> zone)
{
    defer([=, this] {
        _pick_zone = zone;
        request_update(); // The pick zone is part of the bounds.
    });
}

bool CanvasItemCtrl::_pick_zone_contains(Geom::Point const &p) const
{
    auto const &z = *_pick_zone;
    auto const a = z.a * affine();
    auto const b = z.b * affine();

    if (z.outward != Geom::Point(0, 0)) {
        // Work in the corner's own frame: u and v grow away from the box.
        auto const out = (z.a + z.outward) * affine() - a;
        double const sx = out.x() < 0 ? -1 : 1;
        double const sy = out.y() < 0 ? -1 : 1;
        double const u = (p.x() - a.x()) * sx;
        double const v = (p.y() - a.y()) * sy;
        return std::max(u, v) >= 0 && u >= -z.overlap && v >= -z.overlap && u <= z.reach && v <= z.reach;
    }

    auto const ab = b - a;
    double const len2 = Geom::dot(ab, ab);
    double t = len2 > 0 ? Geom::dot(p - a, ab) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    auto const d = p - (a + ab * t);
    if (Geom::L2(d) > z.radius) {
        return false;
    }
    if (z.inner) {
        // Measure inward across a segment, or straight toward the centre from a point.
        auto const in = *z.inner * affine();
        Geom::Point toward;
        double room;
        if (len2 > 0) {
            toward = Geom::rot90(ab) / std::sqrt(len2);
            room = Geom::dot(in - a, toward);
            if (room < 0) {
                toward = -toward;
                room = -room;
            }
        } else {
            toward = in - a;
            room = Geom::L2(toward);
            if (room > 0) {
                toward /= room;
            }
        }
        if (room < z.min_room) {
            return false;
        }
        if (room > 0 && Geom::dot(d, toward) > std::min(z.radius, room / 4)) {
            return false;
        }
    }
    return true;
}

Geom::OptRect CanvasItemCtrl::_pick_zone_bounds() const
{
    if (!_pick_zone) {
        return {};
    }
    auto const &z = *_pick_zone;
    auto const a = z.a * affine();
    if (z.outward != Geom::Point(0, 0)) {
        double const r = std::max(z.reach, z.overlap);
        return Geom::Rect(a - Geom::Point(r, r), a + Geom::Point(r, r));
    }
    Geom::Rect r(a, z.b * affine());
    r.expandBy(z.radius);
    return r;
}

void CanvasItemCtrl::set_angle(double angle)
{
    defer([=, this] {
        if (_angle == angle) return;
        _angle = angle;
        _built.reset();
        request_update(); // Geometry change
    });
}

void CanvasItemCtrl::set_anchor(SPAnchorType anchor)
{
    defer([=, this] {
        if (_anchor == anchor) return;
        _anchor = anchor;
        request_update(); // Geometry change
    });
}

static double angle_of(Geom::Affine const &affine)
{
    return std::atan2(affine[1], affine[0]);
}

/**
 * Update and redraw control ctrl.
 */
void CanvasItemCtrl::_update(bool)
{
    // Queue redraw of old area (erase previous content).
    request_redraw();

    // Setting the position to (inf, inf) to hide it is a pervasive hack we need to support.
    if (!_position.isFinite()) {
        _bounds = {};
        return;
    }

    const auto width = static_cast<double>(get_total_width());

    // Get half width, rounded down.
    double const w_half = width / 2;

    // Set _angle, and compute adjustment for anchor.
    double dx = 0;
    double dy = 0;

    CanvasItemCtrlShape shape = _shape;
    if (!_shape_set) {
        auto const &style = _context->handlesCss()->style_map.at(_handle);
        shape = style.shape();
    }

    switch (shape) {
    case CANVAS_ITEM_CTRL_SHAPE_DARROW:
    case CANVAS_ITEM_CTRL_SHAPE_SARROW:
    case CANVAS_ITEM_CTRL_SHAPE_CARROW:
    case CANVAS_ITEM_CTRL_SHAPE_SALIGN:
    case CANVAS_ITEM_CTRL_SHAPE_CALIGN: {
        double angle = int{_anchor} * M_PI_4;
        // Affine flips if view orientation has been altered (horizontal or vertical flip).
        // But it also flips when Y axis is pointing up. We need to take both into account.
        if (affine().flips() == _context->yaxisdown()) {
            angle = -angle;
        }
        angle += angle_of(affine());
        double const half = width / 2.0;

        dx = -(half + 2) * cos(angle); // Add a bit to prevent tip from overlapping due to rounding errors.
        dy = -(half + 2) * sin(angle);

        switch (shape) {
        case CANVAS_ITEM_CTRL_SHAPE_CARROW:
            angle += 5 * M_PI_4;
            break;

        case CANVAS_ITEM_CTRL_SHAPE_SARROW:
            angle += M_PI_2;
            break;

        case CANVAS_ITEM_CTRL_SHAPE_SALIGN:
            dx = -(half / 2 + 2) * cos(angle);
            dy = -(half / 2 + 2) * sin(angle);
            angle -= M_PI_2;
            break;

        case CANVAS_ITEM_CTRL_SHAPE_CALIGN:
            angle -= M_PI_4;
            dx = (half / 2 + 2) * (sin(angle) - cos(angle));
            dy = (half / 2 + 2) * (-sin(angle) - cos(angle));
            break;

        default:
            break;
        }

        if (_angle != angle) {
            _angle = angle;
            _built.reset();
        }

        break;
    }

    case CANVAS_ITEM_CTRL_SHAPE_PIVOT:
    case CANVAS_ITEM_CTRL_SHAPE_MALIGN: {
        double const angle = angle_of(affine());
        if (_angle != angle) {
            _angle = angle;
            _built.reset();
        }
        break;
    }

    default:
        switch (_anchor) {
        case SP_ANCHOR_N:
        case SP_ANCHOR_CENTER:
        case SP_ANCHOR_S:
            break;

        case SP_ANCHOR_NW:
        case SP_ANCHOR_W:
        case SP_ANCHOR_SW:
            dx = w_half;
            break;

        case SP_ANCHOR_NE:
        case SP_ANCHOR_E:
        case SP_ANCHOR_SE:
            dx = -w_half;
            break;
        }

        switch (_anchor) {
        case SP_ANCHOR_W:
        case SP_ANCHOR_CENTER:
        case SP_ANCHOR_E:
            break;

        case SP_ANCHOR_NW:
        case SP_ANCHOR_N:
        case SP_ANCHOR_NE:
            dy = w_half;
            break;

        case SP_ANCHOR_SW:
        case SP_ANCHOR_S:
        case SP_ANCHOR_SE:
            dy = -w_half;
            break;
        }
        break;
    }

    // The location we want to place our anchor/ctrl point
    _pos = Geom::Point(dx, dy) + _position * affine();

    // The bounding box we want to invalidate in cairo, rounded out to catch any stray pixels
    _bounds = Geom::Rect::from_xywh(_pos - Geom::Point(w_half, w_half), {width, width}).roundOutwards();

    // Parent groups only pick within their children's bounds, so those must cover the pick zone.
    if (auto zone = _pick_zone_bounds()) {
        _bounds.unionWith(*zone);
        _bounds = _bounds->roundOutwards();
    }

    // Queue redraw of new area
    request_redraw();
}

/**
 * Render ctrl to screen via Cairo.
 */
void CanvasItemCtrl::_render(CanvasItemBuffer &buf) const
{
    _built.init([&, this] { build_cache(buf.device_scale); });

    if (!_cache) {
        return;
    }

    // Round to the device pixel at the very last minute so we get less bluring

    auto cache_size = Geom::Point(_cache->get_width(), _cache->get_height());
    auto center_offset = (cache_size * 0.5 / _cache->get_device_scale());

    auto const [x, y] =
        CanvasItem::align_to_pixels05(_pos, _cache->get_width(), buf.device_scale) - center_offset - buf.rect.min();

    if constexpr (DUMP_HANDLES && DRAW_BOUNDS) {
        // draw bitmap bounds
        buf.cr->rectangle(x, y, cache_size.x() / _cache->get_device_scale(),
                          cache_size.y() / _cache->get_device_scale());
        buf.cr->set_source_rgba(0, 1, 0.5, 0.5);
        buf.cr->fill();
    }

    cairo_set_source_surface(buf.cr->cobj(), const_cast<cairo_surface_t *>(_cache->cobj()), x, y); // C API is const-incorrect.
    buf.cr->paint();
}

void CanvasItemCtrl::_invalidate_ctrl_handles()
{
    assert(!_context->snapshotted()); // precondition
    _built.reset();
    request_update();
}

float CanvasItemCtrl::get_stroke_width() const {
    const auto& style = _context->handlesCss()->style_map.at(_handle);
    // growing stroke width with handle size, if style enables it
    auto stroke_width = style.stroke_width() * (1.0f + _width * style.stroke_scale());
    return stroke_width;
}

/**
 * Build object-specific cache.
 */
void CanvasItemCtrl::build_cache(int device_scale) const
{
    auto width = get_width();
    if (width < 1) {
        return; // Nothing to render
    }

    // take size in logical pixels and make it fit physical pixel grid
    auto pixel_fit = [=](float v) { return std::round(v * device_scale) / device_scale; };

    auto const &style = _context->handlesCss()->style_map.at(_handle);
    // effective stroke width
    auto stroke_width = pixel_fit(get_stroke_width());
    // fixed-size outline
    auto outline_width = pixel_fit(style.outline_width());
    // handle size
    auto size = width; // final rounding and pixel alignment done by renderer based on shape other parameters

    auto preferred_parity = -1;
    if (_size_parity >= 0 && size >= _size_parity) {
        preferred_parity = (_size_parity * device_scale) & 1;
    }

    _cache = Handles::draw({
        .shape = _shape_set ? _shape : style.shape(),
        .fill = _fill_set ? _fill : style.getFill(),
        .stroke = _stroke_set ? _stroke : style.getStroke(),
        .outline = style.getOutline(),
        .stroke_width = stroke_width,
        .outline_width = outline_width,
        .size = size,
        .angle = _angle,
        .device_scale = device_scale,
        .size_parity = preferred_parity
    });
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
