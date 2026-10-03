// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Helper object for showing selected items
 *
 * Authors:
 *   bulia byak <bulia@users.sf.net>
 *   Carl Hetherington <inkscape@carlh.net>
 *   Abhishek Sharma
 *
 * Copyright (C) 2004 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "selcue.h"

#include <memory>

#include "box-frame.h"
#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-ctrl.h"
#include "display/control/canvas-item-guideline.h"
#include "display/control/canvas-item-rect.h"
#include "object/sp-flowtext.h"
#include "object/sp-item-group.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "selection.h"
#include "text-editing.h"

namespace Inkscape {

SelCue::BoundingBoxPrefsObserver::BoundingBoxPrefsObserver(SelCue &sel_cue)
    : Observer("/tools/bounding_box")
    , _sel_cue(sel_cue)
{}

void SelCue::BoundingBoxPrefsObserver::notify(Preferences::Entry const &val)
{
    _sel_cue._boundingBoxPrefsChanged(static_cast<int>(val.getBool()));
}

SelCue::SelCue(SPDesktop *desktop)
    : _desktop(desktop)
    , _bounding_box_prefs_observer(*this)
{
    _selection = _desktop->getSelection();

    _sel_changed_connection = _selection->connectChanged(sigc::hide(sigc::mem_fun(*this, &SelCue::_newItemBboxes)));

    {
        void (SelCue::*modifiedSignal)() = &SelCue::_updateItemBboxes;
        _sel_modified_connection =
            _selection->connectModified(sigc::hide(sigc::hide(sigc::mem_fun(*this, modifiedSignal))));
    }

    Preferences *prefs = Preferences::get();
    _updateItemBboxes(prefs);
    prefs->addObserver(_bounding_box_prefs_observer);
}

SelCue::~SelCue()
{
    _sel_changed_connection.disconnect();
    _sel_modified_connection.disconnect();
}

void SelCue::_updateItemBboxes()
{
    _updateItemBboxes(Preferences::get());
}

void SelCue::_updateItemBboxes(Preferences *prefs)
{
    gint mode = prefs->getInt("/options/selcue/value", MARK);
    if (mode == NONE) {
        return;
    }

    g_return_if_fail(_selection != nullptr);

    int prefs_bbox = prefs->getBool("/tools/bounding_box");

    _updateItemBboxes(mode, prefs_bbox);
}

void SelCue::_updateItemBboxes(gint mode, int prefs_bbox)
{
    // PROOF: nothing shows while the selection is dragged, and the cue is drawn again at the
    // result, so don't follow every step of the drag.
    if (_transform_box && _transforming) {
        for (auto *items : {&_item_bboxes, &_item_lines, &_text_baselines, &_item_centers}) {
            for (auto &canvas_item : *items) {
                canvas_item->set_visible(false);
            }
        }
        return;
    }

    auto items = _selection->items();
    if (_item_bboxes.size() != std::ranges::distance(items)) {
        _newItemBboxes();
        return;
    }

    int bcount = 0;
    for (auto item : items) {
        auto canvas_item = _item_bboxes[bcount++].get();

        if (canvas_item) {
            if (_transform_box && mode == BBOX) {
                if (!_setItemBox(*canvas_item, *item, prefs_bbox)) {
                    _newItemBboxes(); // The box has turned, or turned upright.
                    return;
                }
                continue;
            }

            Geom::OptRect const b = (prefs_bbox == 0) ? item->desktopVisualBounds() : item->desktopGeometricBounds();

            if (b) {
                if (auto ctrl = dynamic_cast<CanvasItemCtrl *>(canvas_item)) {
                    ctrl->set_position(Geom::Point(b->min().x(), b->max().y()));
                } else if (auto rect = dynamic_cast<CanvasItemRect *>(canvas_item)) {
                    rect->set_rect(*b);
                }
                canvas_item->set_visible(_visible());
            } else { // no bbox
                canvas_item->set_visible(false);
            }
        }
    }

    _newItemLines();
    _newTextBaselines();
    _newItemCenters();
}

/**
 * PROOF: the thin box around one of several selected objects, turned with the object as the
 * transform box is. Returns false when the canvas item is of the wrong kind for the box: a
 * rectangle for an upright box, a path for a turned one.
 */
bool SelCue::_setItemBox(CanvasItem &canvas_item, SPItem &item, int prefs_bbox) const
{
    auto const type = prefs_bbox == 0 ? SPItem::VISUAL_BBOX : SPItem::GEOMETRIC_BBOX;
    std::vector<SPItem *> const one{&item};
    double const angle = box_angle(one);
    auto const rect = dynamic_cast<CanvasItemRect *>(&canvas_item);
    auto const path = dynamic_cast<CanvasItemBpath *>(&canvas_item);
    if (angle != 0 ? !path : !rect) {
        return false;
    }

    auto const b = angle != 0 ? frame_bounds(one, angle, type) : item.desktopBounds(type);
    if (!b) {
        canvas_item.set_visible(false);
        return true;
    }
    if (rect) {
        rect->set_rect(*b);
    } else {
        Geom::Rotate const frame(angle);
        Geom::Path outline(b->corner(0) * frame);
        for (int i = 1; i < 4; i++) {
            outline.appendNew<Geom::LineSegment>(b->corner(i) * frame);
        }
        outline.close();
        path->set_bpath(Geom::PathVector(outline));
    }
    canvas_item.set_visible(_visible());
    return true;
}

void SelCue::_newItemBboxes()
{
    _item_bboxes.clear();
    _item_centers.clear();

    Preferences *prefs = Preferences::get();
    gint mode = prefs->getInt("/options/selcue/value", MARK);
    if (mode == NONE) {
        return;
    }

    g_return_if_fail(_selection != nullptr);

    int prefs_bbox = prefs->getBool("/tools/bounding_box");

    auto items = _selection->items();
    bool const box_shows_it = _transform_box && std::ranges::distance(items) == 1;
    for (auto item : items) {
        Geom::OptRect const bbox = (prefs_bbox == 0) ? item->desktopVisualBounds() : item->desktopGeometricBounds();

        if (bbox && !box_shows_it) {
            CanvasItemPtr<CanvasItem> canvas_item;

            if (mode == MARK) {
                auto ctrl = make_canvasitem<CanvasItemCtrl>(_desktop->getCanvasControls(), CANVAS_ITEM_CTRL_TYPE_SHAPER,
                                                            Geom::Point(bbox->min().x(), bbox->max().y()));
                canvas_item = std::move(ctrl);
            } else if (mode == BBOX && _transform_box) {
                // PROOF: thin and solid, and turned with the object as the transform box is.
                std::vector<SPItem *> const one{item};
                if (box_angle(one) != 0) {
                    auto path = make_canvasitem<CanvasItemBpath>(_desktop->getCanvasControls());
                    path->set_stroke(0x277fffff);
                    path->set_fill(0x0, SP_WIND_RULE_NONZERO);
                    canvas_item = std::move(path);
                } else {
                    auto rect = make_canvasitem<CanvasItemRect>(_desktop->getCanvasControls(), *bbox);
                    rect->set_stroke(0x277fffff);
                    canvas_item = std::move(rect);
                }
                _setItemBox(*canvas_item, *item, prefs_bbox);
            } else if (mode == BBOX) {
                auto rect = make_canvasitem<CanvasItemRect>(_desktop->getCanvasControls(), *bbox);
                rect->set_stroke(0xffffffa0);
                rect->set_shadow(0x0000c0a0, 1);
                rect->set_dashed(true);
                rect->set_inverted(false);
                canvas_item = std::move(rect);
            }

            if (canvas_item) {
                canvas_item->set_pickable(false);
                canvas_item->lower_to_bottom(); // Just low enough to not get in the way of other draggable knots.
                canvas_item->set_visible(_visible());
                _item_bboxes.emplace_back(std::move(canvas_item));
            }
        }
    }

    _newItemLines();
    _newTextBaselines();
    _newItemCenters();
}

/**
 * PROOF: the centres of the shapes (rectangles, ellipses, polygons and stars) in a group or
 * among several selected objects, as Illustrator shows them. A single selected shape needs none:
 * the transform box marks its centre.
 */
void SelCue::_newItemCenters()
{
    _item_centers.clear();
    if (!_transform_box) {
        return;
    }

    auto const items = _selection->items_vector();
    if (items.size() == 1 && !is<SPGroup>(items.front()) && !is<SPUse>(items.front())) {
        return;
    }
    // Too many centres would only clutter the canvas and slow selection down.
    constexpr std::size_t MAX_CENTERS = 200;
    for (auto const &center : shape_centers(items, MAX_CENTERS)) {
        auto ctrl = make_canvasitem<CanvasItemCtrl>(_desktop->getCanvasControls(), CANVAS_ITEM_CTRL_TYPE_BOX_CENTER, center);
        ctrl->set_pickable(false);
        ctrl->lower_to_bottom();
        ctrl->set_visible(_visible());
        _item_centers.emplace_back(std::move(ctrl));
    }
}

/**
 * Create any required visual-only guide lines related to the selection.
 */
void SelCue::_newItemLines()
{
    _item_lines.clear();

    auto bbox = _selection->preferredBounds();

    // Show a set of lines where the anchor is.
    if (_selection->has_anchor && bbox) {
        auto anchor = Geom::Scale(_selection->anchor);
        auto point = bbox->min() + (bbox->dimensions() * anchor);
        for (bool horz : {false, true}) {
            auto line = make_canvasitem<CanvasItemGuideLine>(_desktop->getCanvasGuides(), "", point, Geom::Point(!horz, horz));
            line->lower_to_bottom();
            line->set_visible(!_transforming);
            line->set_stroke(0xddddaa11);
            line->set_inverted(true);
            _item_lines.emplace_back(std::move(line));
        }
    }
}

void SelCue::_newTextBaselines()
{
    _text_baselines.clear();

    auto items = _selection->items();
    for (auto item : items) {
        std::optional<Geom::Point> pt;
        if (auto text = cast<SPText>(item)) {
            pt = text->getBaselinePoint();
        } else if (auto flow = cast<SPFlowtext>(item)) {
            pt = flow->getBaselinePoint();
        }
        if (pt) {
            auto canvas_item = make_canvasitem<CanvasItemCtrl>(_desktop->getCanvasControls(), CANVAS_ITEM_CTRL_TYPE_SIZER, (*pt) * item->i2dt_affine());
            canvas_item->set_size(Inkscape::HandleSize::XTINY);
            canvas_item->lower_to_bottom();
            canvas_item->set_visible(!_transforming);
            _text_baselines.emplace_back(std::move(canvas_item));
        }
    }
}

void SelCue::_boundingBoxPrefsChanged(int prefs_bbox)
{
    Preferences *prefs = Preferences::get();
    gint mode = prefs->getInt("/options/selcue/value", MARK);
    if (mode == NONE) {
        return;
    }

    g_return_if_fail(_selection != nullptr);

    _updateItemBboxes(mode, prefs_bbox);
}

void SelCue::setBboxesVisible(bool visible)
{
    _bboxes_visible = visible;
    _updateItemBboxes();
}

void SelCue::setTransformBox(bool transform_box)
{
    _transform_box = transform_box;
    _newItemBboxes();
}

void SelCue::setTransforming(bool transforming)
{
    if (_transforming == transforming) {
        return;
    }
    _transforming = transforming;
    _updateItemBboxes();
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
