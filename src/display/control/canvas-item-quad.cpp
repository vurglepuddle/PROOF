// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * A class to represent a control quadrilateral. Used to highlight selected text.
 */

/*
 * Author:
 *   Tavmjong Bah
 *
 * Copyright (C) 2020 Tavmjong Bah
 *
 * Rewrite of SPCtrlQuadr
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cassert>

#include "canvas-item-quad.h"

#include "display/cairo-utils.h"
#include "helper/geom.h"

namespace Inkscape {

/**
 * Create an null control quad.
 */
CanvasItemQuad::CanvasItemQuad(CanvasItemGroup *group)
    : CanvasItem(group)
{
    _name = "CanvasItemQuad:Null";
}

/**
 * Create a control quad. Points are in document coordinates.
 */
CanvasItemQuad::CanvasItemQuad(CanvasItemGroup *group,
                               Geom::Point const &p0, Geom::Point const &p1,
                               Geom::Point const &p2, Geom::Point const &p3)
    : CanvasItem(group)
    , _p0(p0)
    , _p1(p1)
    , _p2(p2)
    , _p3(p3)
{
    _name = "CanvasItemQuad";
}

/**
 * Set a control quad. Points are in document coordinates.
 */
void CanvasItemQuad::set_coords(Geom::Point const &p0, Geom::Point const &p1, Geom::Point const &p2, Geom::Point const &p3)
{
    defer([=, this] {
        _p0 = p0;
        _p1 = p1;
        _p2 = p2;
        _p3 = p3;
        request_update();
    });
}

/**
 * Returns true if point p (in canvas units) is within tolerance (canvas units) distance of quad.
 */
bool CanvasItemQuad::contains(Geom::Point const &p, double tolerance)
{
    if (tolerance != 0) {
        std::cerr << "CanvasItemQuad::contains: Non-zero tolerance not implemented!" << std::endl;
    }

    Geom::Point p0 = _p0 * affine();
    Geom::Point p1 = _p1 * affine();
    Geom::Point p2 = _p2 * affine();
    Geom::Point p3 = _p3 * affine();

    // From 2geom rotated-rect.cpp
    return
        Geom::cross(p1 - p0, p - p0) >= 0 &&
        Geom::cross(p2 - p1, p - p1) >= 0 &&
        Geom::cross(p3 - p2, p - p2) >= 0 &&
        Geom::cross(p0 - p3, p - p3) >= 0;
}

/**
 * Update and redraw control quad.
 */
void CanvasItemQuad::_update(bool)
{
    if (_p0 == _p1 || _p1 == _p2 || _p2 == _p3 || _p3 == _p0) {
        _bounds = {};
        return; // Not quad or not initialized.
    }

    // Queue redraw of old area (erase previous content).
    request_redraw(); // This is actually never useful as quads are always deleted
    // and recreated when a node is moved! But keep it in case we change that.

    _bounds = expandedBy(bounds_of(_p0, _p1, _p2, _p3) * affine(), 2); // Room for anti-aliasing effects.

    // Queue redraw of new area
    request_redraw();
}

/**
 * Render quad to screen via Cairo.
 */
void CanvasItemQuad::_render(Inkscape::CanvasItemBuffer &buf) const
{
    // Document to canvas
    Geom::Point p0 = _p0 * affine();
    Geom::Point p1 = _p1 * affine();
    Geom::Point p2 = _p2 * affine();
    Geom::Point p3 = _p3 * affine();

    // Canvas to screen
    p0 *= Geom::Translate(-buf.rect.min());
    p1 *= Geom::Translate(-buf.rect.min());
    p2 *= Geom::Translate(-buf.rect.min());
    p3 *= Geom::Translate(-buf.rect.min());

    buf.cr->save();

    buf.cr->begin_new_path();

    buf.cr->move_to(p0.x(), p0.y());
    buf.cr->line_to(p1.x(), p1.y());
    buf.cr->line_to(p2.x(), p2.y());
    buf.cr->line_to(p3.x(), p3.y());
    buf.cr->close_path();

    if (_inverted) {
        cairo_set_operator(buf.cr->cobj(), CAIRO_OPERATOR_DIFFERENCE);
    } else if (_highlighter) {
        // A light wash first, so the mark shows on a dark desk too; then the ink, which
        // tints what is light and leaves what is dark.
        auto wash = Colors::Color(_fill);
        wash.setOpacity(wash.getOpacity() * 0.25);
        ink_cairo_set_source_color(buf.cr, wash);
        buf.cr->fill_preserve();
        cairo_set_operator(buf.cr->cobj(), CAIRO_OPERATOR_MULTIPLY);
    }

    ink_cairo_set_source_color(buf.cr, Colors::Color(_fill));
    buf.cr->fill_preserve();

    if (!_highlighter) {
        buf.cr->set_line_width(1);
        ink_cairo_set_source_color(buf.cr, Colors::Color(_stroke));
        buf.cr->stroke_preserve();
    }
    buf.cr->begin_new_path();

    buf.cr->restore();
}

void CanvasItemQuad::set_inverted(bool inverted)
{
    defer([=, this] {
        if (_inverted == inverted) return;
        _inverted = inverted;
        request_redraw();
    });
}

void CanvasItemQuad::set_highlighter(bool highlighter)
{
    defer([=, this] {
        if (_highlighter == highlighter) return;
        _highlighter = highlighter;
        request_redraw();
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
