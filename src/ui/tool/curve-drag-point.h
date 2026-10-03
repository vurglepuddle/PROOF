// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOL_CURVE_DRAG_POINT_H
#define INKSCAPE_UI_TOOL_CURVE_DRAG_POINT_H

#include "control-point.h"
#include "node.h"

class SPDesktop;
namespace Inkscape::UI {

class PathManipulator;
struct PathSharedData;

// This point should be invisible to the user
// TODO make some methods from path-manipulator.cpp public so that this point doesn't have
// to be declared as a friend
/**
 * An invisible point used to drag curves. This point is used by PathManipulator to allow editing
 * of path segments by dragging them. It is defined in a separate file so that the node tool
 * can check if the mouseovered control point is a curve drag point and update the cursor
 * accordingly, without the need to drag in the full PathManipulator header.
 */
class CurveDragPoint : public ControlPoint
{
public:
    explicit CurveDragPoint(PathManipulator &pm);
    void setSize(double sz) { _setSize(sz); }
    void setTimeValue(double t) { _t = t; }
    double getTimeValue() const { return _t; }
    void setIterator(NodeList::iterator i) { first = i; }
    NodeList::iterator getIterator() { return first; }
    bool _eventHandler(Inkscape::UI::Tools::ToolBase *event_context, CanvasEvent const &event) override;

protected:
    Glib::ustring _getTip(unsigned state) const override;
    void dragged(Geom::Point &, MotionEvent const &) override;
    bool grabbed(MotionEvent const &) override;
    void ungrabbed(ButtonReleaseEvent const *) override;
    bool clicked(ButtonReleaseEvent const &) override;
    bool doubleclicked(ButtonReleaseEvent const &) override;

private:
    double _t;
    PathManipulator &_pm;
    NodeList::iterator first;

    static bool _drags_stroke;
    static bool _segment_was_degenerate;
    // PROOF: a straight segment being moved whole, and where its ends started.
    static bool _moves_segment;
    static Geom::Point _segment_start[2];
    static Geom::Point _stroke_drag_origin;
    void _insertNode(bool take_selection);
    void _moveSegment(Geom::Point &new_pos, MotionEvent const &event);
};

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_TOOL_CURVE_DRAG_POINT_H

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
