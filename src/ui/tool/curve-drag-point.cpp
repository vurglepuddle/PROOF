// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/tool/curve-drag-point.h"

#include <glib/gi18n.h>
#include <2geom/angle.h>

#include "control-point-selection.h"
#include "desktop.h"
#include "multi-path-manipulator.h"
#include "display/control/snap-indicator.h"
#include "object/sp-namedview.h"
#include "path-manipulator.h"
#include "ui/widget/events/canvas-event.h"
#include "util/join.h"

namespace Inkscape {
namespace UI {


bool CurveDragPoint::_drags_stroke = false;
bool CurveDragPoint::_segment_was_degenerate = false;
bool CurveDragPoint::_moves_segment = false;
Geom::Point CurveDragPoint::_segment_start[2];

CurveDragPoint::CurveDragPoint(PathManipulator &pm) :
    ControlPoint(pm._multi_path_manipulator._path_data.node_data.desktop, Geom::Point(), SP_ANCHOR_CENTER,
                 Inkscape::CANVAS_ITEM_CTRL_TYPE_INVISIPOINT,
                 pm._multi_path_manipulator._path_data.dragpoint_group),
      _pm(pm)
{
    _canvas_item_ctrl->set_name("CanvasItemCtrl:CurveDragPoint");
    setVisible(false);
}

bool CurveDragPoint::_eventHandler(Inkscape::UI::Tools::ToolBase *event_context, CanvasEvent const &event)
{
    // do not process any events when the manipulator is empty
    if (_pm.empty()) {
        setVisible(false);
        return false;
    }
    return ControlPoint::_eventHandler(event_context, event);
}

bool CurveDragPoint::grabbed(MotionEvent const &event)
{
    _pm._selection.hideTransformHandles();
    NodeList::iterator second = first.next();

    // PROOF: as with Illustrator's Direct Selection, dragging a straight segment moves it, both
    // of its anchors following the pointer. Alt bends it into a curve instead, as Inkscape did.
    bool const straight = first->front()->isDegenerate() && second->back()->isDegenerate();
    _moves_segment = straight && !_pm._isBSpline() && !(event.modifiers & GDK_ALT_MASK);
    if (_moves_segment) {
        _segment_was_degenerate = false;
        _segment_start[0] = first->position();
        _segment_start[1] = second->position();
        return false;
    }

    // move the handles to 1/3 the length of the segment for line segments
    if (straight) {
        _segment_was_degenerate = true;

        // delta is a vector equal 1/3 of distance from first to second
        Geom::Point delta = (second->position() - first->position()) / 3.0;
        // only update the nodes if the mode is bspline
        if (!_pm._isBSpline()) {
            first->front()->move(first->front()->position() + delta);
            second->back()->move(second->back()->position() - delta);
        }
        _pm.update();
    } else {
        _segment_was_degenerate = false;
    }
    return false;
}

void CurveDragPoint::dragged(Geom::Point &new_pos, MotionEvent const &event)
{
    if (!first || !first.next()) return;
    NodeList::iterator second = first.next();

    auto const bspline_handles = Modifiers::Modifier::get(Modifiers::Type::NODE_BSPLINE_HANDLES)->active(event.modifiers);
    auto const no_snap = Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING)->active(event.modifiers);

    if (_moves_segment) {
        _moveSegment(new_pos, event);
        return;
    }

    // special cancel handling - retract handles when if the segment was degenerate
    if (_is_drag_cancelled(event) && _segment_was_degenerate) {
        first->front()->retract();
        second->back()->retract();
        _pm.update();
        return;
    }

    if (_drag_initiated && !no_snap) {
        auto &m = _desktop->getNamedView()->snap_manager;
        SPItem *path = static_cast<SPItem *>(_pm._path);
        m.setup(_desktop, true, path); // We will not try to snap to "path" itself
        Inkscape::SnapCandidatePoint scp(new_pos, Inkscape::SNAPSOURCE_OTHER_HANDLE);
        Inkscape::SnappedPoint sp = m.freeSnap(scp, Geom::OptRect(), false);
        new_pos = sp.getPoint();
        m.unSetup();
    }

    // Magic Bezier Drag Equations follow!
    // "weight" describes how the influence of the drag should be distributed
    // among the handles; 0 = front handle only, 1 = back handle only.
    double weight, t = _t;
    if (t <= 1.0 / 6.0) weight = 0;
    else if (t <= 0.5) weight = (pow((6 * t - 1) / 2.0, 3)) / 2;
    else if (t <= 5.0 / 6.0) weight = (1 - pow((6 * (1-t) - 1) / 2.0, 3)) / 2 + 0.5;
    else weight = 1;

    Geom::Point delta = new_pos - position();
    Geom::Point offset0 = ((1-weight)/(3*t*(1-t)*(1-t))) * delta;
    Geom::Point offset1 = (weight/(3*t*t*(1-t))) * delta;

    //modified so that, if the trace is bspline, it only acts if the SHIFT key is pressed
    if (!_pm._isBSpline()) {
        first->front()->move(first->front()->position() + offset0);
        second->back()->move(second->back()->position() + offset1);
    } else if (weight >= 0.8) {
        if (bspline_handles) {
            second->back()->move(new_pos);
        } else {
            second->move(second->position() + delta);
        }
    } else if (weight <= 0.2) {
        if (bspline_handles) {
            first->back()->move(new_pos);
        } else {
            first->move(first->position() + delta);
        }
    } else {
        first->move(first->position() + delta);
        second->move(second->position() + delta);
    }
    _pm.update();
}

/**
 * PROOF: move a straight segment whole. Shift keeps the move horizontal, vertical or diagonal;
 * otherwise whichever end lands nearer a snap target snaps, and the other end follows.
 */
void CurveDragPoint::_moveSegment(Geom::Point &new_pos, MotionEvent const &event)
{
    NodeList::iterator second = first.next();
    Geom::Point const origin = _last_drag_origin();

    if (_is_drag_cancelled(event)) {
        first->move(_segment_start[0]);
        second->move(_segment_start[1]);
        new_pos = origin;
        _pm.update();
        return;
    }

    Geom::Point delta = new_pos - origin;
    if (event.modifiers & GDK_SHIFT_MASK) {
        double const step = M_PI / 4;
        double const angle = std::round(Geom::atan2(delta) / step) * step;
        delta = Geom::Point::polar(angle, Geom::dot(delta, Geom::Point::polar(angle)));
    } else if (!Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING)->active(event.modifiers)) {
        auto &m = _desktop->getNamedView()->snap_manager;
        m.setup(_desktop, true, static_cast<SPItem *>(_pm._path));
        std::optional<Inkscape::SnappedPoint> best;
        Geom::Point correction;
        for (auto const &start : _segment_start) {
            Inkscape::SnapCandidatePoint scp(start + delta, Inkscape::SNAPSOURCE_NODE_CUSP);
            auto sp = m.freeSnap(scp, Geom::OptRect(), false);
            if (sp.getSnapped() && (!best || sp.getSnapDistance() < best->getSnapDistance())) {
                best = sp;
                correction = sp.getPoint() - (start + delta);
            }
        }
        m.unSetup();
        if (best) {
            delta += correction;
            _desktop->getSnapIndicator()->set_new_snaptarget(*best);
        }
    }

    first->move(_segment_start[0] + delta);
    second->move(_segment_start[1] + delta);
    new_pos = origin + delta;
    _pm.update();
}

void CurveDragPoint::ungrabbed(ButtonReleaseEvent const *)
{
    _pm._updateDragPoint(_desktop->d2w(position()));
    _pm._commit(_moves_segment ? RC_("Undo", "Move segment") : RC_("Undo", "Drag curve"));
    _moves_segment = false;
    _pm._selection.restoreTransformHandles();
}

bool CurveDragPoint::clicked(ButtonReleaseEvent const &event)
{
    auto const insert = Modifiers::Modifier::get(Modifiers::Type::NODE_INSERT)->active(event.modifiers);
    auto const add_to = Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO)->active(event.modifiers);

    // This check is probably redundant
    if (!first || event.button != 1) return false;
    // the next iterator can be invalid if we click very near the end of path
    NodeList::iterator second = first.next();
    if (!second) return false;

    if (insert) {
        _insertNode(false);
        return true;
    }

    if (add_to) {
        // if both nodes of the segment are selected, deselect;
        // otherwise add to selection
        if (first->selected() && second->selected())  {
            _pm._selection.erase(first.ptr());
            _pm._selection.erase(second.ptr());
        } else {
            _pm._selection.insert(first.ptr());
            _pm._selection.insert(second.ptr());
        }
    } else {
        // without Shift, take selection
        _pm._selection.clear();
        _pm._selection.insert(first.ptr(), false, false);
        _pm._selection.insert(second.ptr());
    }
    return true;
}

bool CurveDragPoint::doubleclicked(ButtonReleaseEvent const &event)
{
    if (event.button != 1 || !first || !first.next()) return false;

    auto const delete_segment = Modifiers::Modifier::get(Modifiers::Type::NODE_DELETE_SEGMENT)->active(event.modifiers);
    auto const straighten = Modifiers::Modifier::get(Modifiers::Type::NODE_STRAIGHTEN_SEGMENT)->active(event.modifiers);

    if (delete_segment) {
        auto ref = _pm.shared_from_this(); // hold ref during possible deletion of _pm.
        _pm.deleteSegments();
        _pm.update(true);
        _pm._commit(RC_("Undo", "Remove segment"));
    } else if (straighten) {
        _pm.setSegmentType(Inkscape::UI::SEGMENT_STRAIGHT);
        _pm.update(true);
        _pm._commit(RC_("Undo", "Straighten segments"));
    } else {
        _pm._updateDragPoint(_desktop->d2w(position()));
        _insertNode(true);
    }
    return true;
}

void CurveDragPoint::_insertNode(bool take_selection)
{
    // The purpose of this call is to make way for the just created node.
    // Otherwise clicks on the new node would only work after the user moves the mouse a bit.
    // PathManipulator will restore visibility when necessary.
    setVisible(false);

    _pm.insertNode(first, _t, take_selection);
}

Glib::ustring CurveDragPoint::_getTip(unsigned state) const
{
    if (_pm.empty()) return "";
    if (!first || !first.next()) return "";
    bool linear = first->front()->isDegenerate() && first.next()->back()->isDegenerate();

    auto const mod_add_to = Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO);
    auto const mod_bspline_handles = Modifiers::Modifier::get(Modifiers::Type::NODE_BSPLINE_HANDLES);
    auto const mod_insert = Modifiers::Modifier::get(Modifiers::Type::NODE_INSERT);
    auto const mod_straighten = Modifiers::Modifier::get(Modifiers::Type::NODE_STRAIGHTEN_SEGMENT);

    if (mod_bspline_handles->active(state) && _pm._isBSpline()) {
        return Glib::ustring::compose(C_("Path segment tip", "<b>%1</b>: drag to open or move BSpline handles"),
                                      mod_bspline_handles->get_label());
    }
    if (mod_add_to->active(state)) {
        return Glib::ustring::compose(C_("Path segment tip", "<b>%1</b>: click to toggle segment selection"),
                                      mod_add_to->get_label());
    }
    if (mod_insert->active(state)) {
        return Glib::ustring::compose(C_("Path segment tip", "<b>%1</b>: click to insert a node"),
                                      mod_insert->get_label());
    }
    if (mod_straighten->active(state)) {
        return Glib::ustring::compose(C_("Path segment tip", "<b>%1</b>: double click to change line type"),
                                      mod_straighten->get_label());
    }

    std::set<Glib::ustring> labels;
    if (_pm._isBSpline()) {
        labels.insert(mod_bspline_handles->get_label());
    }
    labels.insert(mod_add_to->get_label());
    labels.insert(mod_insert->get_label());
    labels.insert(mod_straighten->get_label());
    auto const more_labels = Inkscape::Util::join_with_separator(labels);

    if (_pm._isBSpline()) {
        return Glib::ustring::compose(C_("Path segment tip", "<b>BSpline segment</b>: drag to shape the segment, doubleclick to insert node, "
                                         "click to select (more: %1)"), more_labels);
    }
    if (linear) {
        return Glib::ustring::compose(C_("Path segment tip", "<b>Linear segment</b>: drag to move it, Alt+drag to bend it, "
                                         "doubleclick to insert node, click to select (more: %1)"), more_labels);
    } else {
        return Glib::ustring::compose(C_("Path segment tip", "<b>Bezier segment</b>: drag to shape the segment, doubleclick to insert node, "
                                         "click to select (more: %1)"), more_labels);
    }
}

} // namespace UI
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
