// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Control point selection - stores a set of control points and applies transformations
 * to them
 */
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOL_CONTROL_POINT_SELECTION_H
#define INKSCAPE_UI_TOOL_CONTROL_POINT_SELECTION_H

#include <unordered_set>
#include <set>
#include <functional>
#include <utility>

#include "node-types.h"
#include "snap-candidate.h"
#include "ui/tool/commit-events.h"
#include "ui/tool/manipulator.h"

class SPDesktop;

namespace Inkscape {
class CanvasItemGroup;
struct KeyPressEvent;
struct ButtonReleaseEvent;

namespace UI {
class TransformHandleSet;
class SelectableControlPoint;
class CurveDragPoint;

class ControlPointSelection
    : public Manipulator
    , public sigc::trackable
{
    friend class CurveDragPoint;
public:
    ControlPointSelection(SPDesktop *d, Inkscape::CanvasItemGroup *th_group);
    ~ControlPointSelection() override;
    using set_type = std::unordered_set<SelectableControlPoint*>;
    using Set = set_type; // convenience alias

    using iterator = set_type::iterator;
    using const_iterator = set_type::const_iterator;
    using size_type = set_type::size_type;
    using value_type = SelectableControlPoint*;
    using key_type = SelectableControlPoint*;
    using Segment = std::pair<key_type, key_type>;
    struct SegmentLess {
        bool operator()(Segment const &a, Segment const &b) const {
            auto less = std::less<key_type>{};
            return less(a.first, b.first) || (a.first == b.first && less(a.second, b.second));
        }
    };

    // size
    bool empty() const { return _points.empty(); }
    size_type size() const { return _points.size(); }

    // iterators
    iterator begin() { return _points.begin(); }
    const_iterator begin() const { return _points.begin(); }
    iterator end() { return _points.end(); }
    const_iterator end() const { return _points.end(); }

    // insert
    std::pair<iterator, bool> insert(const value_type& x, bool notify = true, bool to_update = true);
    template <class InputIterator>
    void insert(InputIterator first, InputIterator last) {
        for (; first != last; ++first) {
            insert(*first, false, false);
        }
        _update();
        signal_selection_changed.emit(std::vector<key_type>(first, last), true);
    }

    // erase
    void clear();
    void erase(iterator pos, bool to_update = true);
    size_type erase(const key_type& k, bool notify = true);
    void erase(iterator first, iterator last);

    // find
    iterator find(const key_type &k) {
        return _points.find(k);
    }

    // Sometimes it is very useful to keep a list of all selectable points.
    set_type const &allPoints() const { return _all_points; }
    set_type &allPoints() { return _all_points; }
    // ...for example in these methods. Another useful case is snapping.
    void selectAll();
    void selectArea(Geom::Path const &, bool invert = false);
    void invertSelection();
    void spatialGrow(SelectableControlPoint *origin, int dir);

    // A segment has its own identity. Its endpoints participate in ordinary
    // node movement, but do not replace independently selected anchors.
    void selectSegment(key_type first, key_type second, bool add);
    bool segmentSelected(key_type first, key_type second) const { return _segments.contains({first, second}); }
    bool individuallySelected(key_type point) const { return _anchor_points.contains(point); }
    bool segmentEditable(key_type first, key_type second) const {
        return segmentSelected(first, second) || (individuallySelected(first) && individuallySelected(second));
    }
    bool hasSegments() const { return !_segments.empty(); }
    void splitSegment(key_type first, key_type second, key_type inserted);
    void reverseSegments(std::vector<Segment> const &segments);

    bool event(Inkscape::UI::Tools::ToolBase *tool, CanvasEvent const &event) override;

    void transform(Geom::Affine const &m);
    void align(Geom::Dim2 d, AlignTargetNode target = AlignTargetNode::MID_NODE);
    void distribute(Geom::Dim2 d);

    Geom::OptRect pointwiseBounds();
    Geom::OptRect bounds();
    std::optional<Geom::Point> firstSelectedPoint() const;

    bool transformHandlesEnabled() { return _handles_visible; }
    void showTransformHandles(bool v, bool one_node);
    // the two methods below do not modify the state; they are for use in manipulators
    // that need to temporarily hide the handles, for example when moving a node
    void hideTransformHandles();
    void restoreTransformHandles();
    void toggleTransformHandlesMode();

    sigc::signal<void ()> signal_update;
    // It turns out that emitting a signal after every point is selected or deselected is not too efficient,
    // so this can be done in a massive group once the selection is finally changed.
    sigc::signal<void (std::vector<SelectableControlPoint *>, bool)> signal_selection_changed;
    sigc::signal<void (CommitEvent)> signal_commit;
    sigc::signal<void ()> signal_segments_changed;

    void getOriginalPoints(std::vector<Inkscape::SnapCandidatePoint> &pts);
    void getUnselectedPoints(std::vector<Inkscape::SnapCandidatePoint> &pts);
    void setOriginalPoints();
    //the purpose of this list is to keep track of first and last selected
    std::list<SelectableControlPoint *> _points_list;

private:
    // The functions below are invoked from SelectableControlPoint.
    // Previously they were connected to handlers when selecting, but this
    // creates problems when dragging a point that was not selected.
    void _pointGrabbed(SelectableControlPoint *);
    bool _segmentUses(key_type point) const;
    void _erasePointRaw(key_type point);
    void _pointDragged(Geom::Point &, MotionEvent const &);
    void _pointUngrabbed();
    bool _pointClicked(SelectableControlPoint *, ButtonReleaseEvent const &);
    void _mouseoverChanged();

    void _update();
    void _updateTransformHandles(bool preserve_center);
    void _updateBounds();
    bool _keyboardMove(KeyPressEvent const &, Geom::Point const &);
    bool _keyboardRotate(KeyPressEvent const &, int);
    bool _keyboardScale(KeyPressEvent const &, int);
    bool _keyboardFlip(Geom::Dim2);
    void _keyboardTransform(Geom::Affine const &);
    void _commitHandlesTransform(CommitEvent ce);
    double _rotationRadius(Geom::Point const &);

    set_type _points;
    set_type _anchor_points;
    std::set<Segment, SegmentLess> _segments;
    bool _segment_mutation = false;

    set_type _all_points;
    std::unordered_map<SelectableControlPoint *, Geom::Point> _original_positions;
    std::unordered_map<SelectableControlPoint *, Geom::Affine> _last_trans;
    std::optional<double> _rot_radius;
    std::optional<double> _mouseover_rot_radius;
    std::optional<Geom::Point> _first_point;
    Geom::OptRect _bounds;
    TransformHandleSet *_handles;
    SelectableControlPoint *_grabbed_point, *_farthest_point;
    unsigned _dragging         : 1;
    unsigned _handles_visible  : 1;
    unsigned _one_node_handles : 1;

    friend class SelectableControlPoint;
};

} // namespace UI
} // namespace Inkscape

#endif // INKSCAPE_UI_TOOL_CONTROL_POINT_SELECTION_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8 :
