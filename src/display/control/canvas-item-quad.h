// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_CANVAS_ITEM_QUAD_H
#define SEEN_CANVAS_ITEM_QUAD_H

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

#include <2geom/point.h>
#include <2geom/transforms.h>

#include "canvas-item.h"

namespace Inkscape {

class CanvasItemQuad final : public CanvasItem
{
public:
    CanvasItemQuad(CanvasItemGroup *group);
    CanvasItemQuad(CanvasItemGroup *group, Geom::Point const &p0, Geom::Point const &p1,
                                           Geom::Point const &p2, Geom::Point const &p3);

    // Geometry
    void set_coords(Geom::Point const &p0, Geom::Point const &p1, Geom::Point const &p2, Geom::Point const &p3);

    // Selection
    bool contains(Geom::Point const &p, double tolerance = 0) override;

    void set_inverted(bool inverted);
    /// PROOF: a highlighter's mark. The fill darkens what is under it and doesn't cover
    /// it (dark type stays dark), and no edge is drawn.
    void set_highlighter(bool highlighter);

protected:
    ~CanvasItemQuad() override = default;

    void _update(bool propagate) override;
    void _render(Inkscape::CanvasItemBuffer &buf) const override;

    Geom::Point _p0;
    Geom::Point _p1;
    Geom::Point _p2;
    Geom::Point _p3;

    bool _inverted = false;
    bool _highlighter = false;
};

} // namespace Inkscape

#endif // SEEN_CANVAS_ITEM_QUAD_H

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
