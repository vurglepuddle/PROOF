// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Group belonging to an SVG drawing element.
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2011 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "drawing-group.h"
#include "cairo-utils.h"
#include "drawing-context.h"
#include "drawing-surface.h"
#include "drawing-text.h"
#include "drawing.h"
#include "style.h"

namespace Inkscape {

DrawingGroup::DrawingGroup(Drawing &drawing)
    : DrawingItem(drawing) {}

/**
 * Set whether the group returns children from pick calls.
 * Previously this feature was called "transparent groups".
 */
void DrawingGroup::setPickChildren(bool pick_children)
{
    defer([=, this] {
        _pick_children = pick_children;
    });
}

/**
 * Set additional transform for the group.
 * This is applied after the normal transform and mainly useful for
 * markers, clipping paths, etc.
 */
void DrawingGroup::setChildTransform(Geom::Affine const &transform)
{
    defer([=, this] {
        auto constexpr EPS = 1e-18;
        auto current = _child_transform ? *_child_transform : Geom::identity();
        if (Geom::are_near(transform, current, EPS)) return;
        _markForRendering();
        _child_transform = transform.isIdentity(EPS) ? nullptr : std::make_unique<Geom::Affine>(transform);
        _markForUpdate(STATE_ALL, true);
    });
}

unsigned DrawingGroup::_updateItem(Geom::IntRect const &area, UpdateContext const &ctx, unsigned flags, unsigned reset)
{
    bool outline = _drawing.renderMode() == RenderMode::OUTLINE || _drawing.outlineOverlay();

    UpdateContext child_ctx(ctx);
    if (_child_transform) {
        child_ctx.ctm = *_child_transform * ctx.ctm;
        if (child_ctx.stroke) {
            child_ctx.stroke->transform = *_child_transform * child_ctx.stroke->transform;
        }
    }

    _bbox = {};

    for (auto &c : _children) {
        c.update(area, child_ctx, flags, reset);
        if (c.visible()) {
            _bbox.unionWith(outline ? c.bbox() : c.drawbox());
        }
        _update_complexity += c.getUpdateComplexity();
        _contains_unisolated_blend |= c.unisolatedBlend();
    }

    return STATE_ALL;
}

unsigned DrawingGroup::_renderItem(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, unsigned flags, DrawingItem const *stop_at) const
{
    if (!stop_at) {
        // normal rendering
        for (auto &i : _children) {
            i.render(dc, rc, area, flags, stop_at);
        }
    } else {
        // background rendering
        for (auto &i : _children) {
            if (&i == stop_at) {
                return RENDER_OK; // do not render the stop_at item at all
            }
            if (i.isAncestorOf(stop_at)) {
                // render its ancestors without masks, opacity or filters
                i.render(dc, rc, area, flags | RENDER_FILTER_BACKGROUND, stop_at);
                return RENDER_OK;
            } else {
                i.render(dc, rc, area, flags, stop_at);
            }
        }
    }
    return RENDER_OK;
}

void DrawingGroup::_clipItem(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area) const
{
    for (auto &i : _children) {
        i.clip(dc, rc, area);
    }
}

DrawingItem *DrawingGroup::_pickItem(Geom::Point const &p, double delta, Geom::OptIntRect const &area_world, unsigned flags)
{
    for (auto &i : _children) {
        DrawingItem *picked = i.pick(p, delta, area_world, flags);
        if (picked) {
            return _pick_children ? picked : this;
        }
    }
    return nullptr;
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
