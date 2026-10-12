// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Node selection - implementation.
 */
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "desktop.h"
#include "display/control/snap-indicator.h"
#include "ui/tool/control-point.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/transform-handle-set.h"
#include "ui/tool/node.h"
#include "ui/widget/events/canvas-event.h"
#include <algorithm>

namespace Inkscape {
namespace UI {

/**
 * @class ControlPointSelection
 * Group of selected control points.
 *
 * Some operations can be performed on all selected points regardless of their type, therefore
 * this class is also a Manipulator. It handles the transformations of points using
 * the keyboard.
 *
 * The exposed interface is similar to that of an STL set. Internally, a hash map is used.
 * @todo Correct iterators (that don't expose the connection list)
 */

/** @var ControlPointSelection::signal_update
 * Fires when the display needs to be updated to reflect changes.
 */
/** @var ControlPointSelection::signal_point_changed
 * Fires when a control point is added to or removed from the selection.
 * The first param contains a pointer to the control point that changed sel. state. 
 * The second says whether the point is currently selected.
 */
/** @var ControlPointSelection::signal_commit
 * Fires when a change that needs to be committed to XML happens.
 */

ControlPointSelection::ControlPointSelection(SPDesktop *d, Inkscape::CanvasItemGroup *th_group)
    : Manipulator(d)
    , _handles(new TransformHandleSet(d, th_group))
    , _dragging(false)
    , _handles_visible(true)
    , _one_node_handles(false)
{
    signal_update.connect( sigc::bind(
        sigc::mem_fun(*this, &ControlPointSelection::_updateTransformHandles),
        true));
    ControlPoint::signal_mouseover_change.connect(
        sigc::hide(
            sigc::mem_fun(*this, &ControlPointSelection::_mouseoverChanged)));
    _handles->signal_transform.connect(
        sigc::mem_fun(*this, &ControlPointSelection::transform));
    _handles->signal_commit.connect(
        sigc::mem_fun(*this, &ControlPointSelection::_commitHandlesTransform));
}

ControlPointSelection::~ControlPointSelection()
{
    clear();
    delete _handles;
}

/** Add a control point to the selection. */
std::pair<ControlPointSelection::iterator, bool> ControlPointSelection::insert(const value_type &x, bool notify, bool to_update)
{
    if (!_segment_mutation) _anchor_points.insert(x);
    iterator found = _points.find(x);
    if (found != _points.end()) {
        return std::pair<iterator, bool>(found, false);
    }

    found = _points.insert(x).first;
    _points_list.push_back(x);

    x->updateState();

    if (to_update) {
        _update();
    }
    if (notify) {
        signal_selection_changed.emit(std::vector<key_type>(1, x), true);
    }

    return std::pair<iterator, bool>(found, true);
}

/** Remove a point from the selection. */
void ControlPointSelection::erase(iterator pos, bool to_update)
{
    SelectableControlPoint *erased = *pos;
    _anchor_points.erase(erased);
    set_type candidates;
    bool changed = false;
    for (auto i = _segments.begin(); i != _segments.end();) {
        if (i->first == erased || i->second == erased) {
            candidates.insert(i->first); candidates.insert(i->second);
            i = _segments.erase(i);
            changed = true;
        } else ++i;
    }
    _erasePointRaw(erased);
    for (auto point : candidates) {
        if (point != erased && !individuallySelected(point) && !_segmentUses(point)) _erasePointRaw(point);
    }
    if (to_update) {
        _update();
    }
    if (changed) signal_segments_changed.emit();
}

void ControlPointSelection::_erasePointRaw(key_type point)
{
    if (!_points.erase(point)) return;
    _points_list.remove(point);
    point->updateState();
}

bool ControlPointSelection::_segmentUses(key_type point) const
{
    return std::any_of(_segments.begin(), _segments.end(), [point](auto const &segment) {
        return segment.first == point || segment.second == point;
    });
}

void ControlPointSelection::selectSegment(key_type first, key_type second, bool add)
{
    if (!first || !second || first == second) return;
    if (!add) clear();
    Segment segment{first, second};
    bool remove = add && _segments.erase(segment);
    std::vector<key_type> changed;
    if (remove) {
        for (auto point : {first, second}) {
            if (!individuallySelected(point) && !_segmentUses(point) && _points.contains(point)) {
                _erasePointRaw(point);
                changed.push_back(point);
            }
        }
    } else {
        _segments.insert(segment);
        _segment_mutation = true;
        for (auto point : {first, second}) {
            if (insert(point, false, false).second) changed.push_back(point);
        }
        _segment_mutation = false;
    }
    _update();
    if (!changed.empty()) signal_selection_changed.emit(changed, !remove);
    signal_segments_changed.emit();
}

void ControlPointSelection::splitSegment(key_type first, key_type second, key_type inserted)
{
    if (!segmentSelected(first, second)) return;
    selectSegment(first, second, true);
    selectSegment(first, inserted, true);
    selectSegment(inserted, second, true);
}

void ControlPointSelection::reverseSegments(std::vector<Segment> const &segments)
{
    for (auto const &segment : segments) _segments.erase(segment);
    for (auto const &segment : segments) _segments.insert({segment.second, segment.first});
    if (!segments.empty()) signal_segments_changed.emit();
}
ControlPointSelection::size_type ControlPointSelection::erase(const key_type &k, bool notify)
{
    iterator pos = _points.find(k);
    if (pos == _points.end()) return 0;
    erase(pos);

    if (notify) {
        signal_selection_changed.emit(std::vector<key_type>(1, k), false);
    }
    return 1;
}
void ControlPointSelection::erase(iterator first, iterator last)
{
    std::vector<SelectableControlPoint *> out(first, last);
    // Removing one segment endpoint can also remove an unused neighbouring
    // endpoint. Iterate a copy instead of invalidated set iterators.
    for (auto point : out) {
        auto found = _points.find(point);
        if (found != _points.end()) erase(found, false);
    }
    _update();
    signal_selection_changed.emit(out, false);
}

/** Remove all points from the selection, making it empty. */
void ControlPointSelection::clear()
{
    _anchor_points.clear();
    bool had_segments = !_segments.empty();
    _segments.clear();
    if (empty()) {
        if (had_segments) signal_segments_changed.emit();
        return;
    }

    std::vector<SelectableControlPoint *> out(begin(), end()); // begin() takes from _points
    _points.clear();
    _points_list.clear();
    for (auto erased : out) {
        erased->updateState();
    }

    _update();
    signal_selection_changed.emit(out, false);
    if (had_segments) signal_segments_changed.emit();
}

/** Select all points that this selection can contain. */
void ControlPointSelection::selectAll()
{
    for (auto _all_point : _all_points) {
        insert(_all_point, false, false);
    }
    std::vector<SelectableControlPoint *> out(_all_points.begin(), _all_points.end());
    if (!out.empty()) {
        _update();
        signal_selection_changed.emit(out, true);
    }
}
/** Select all points inside the given rectangle (in desktop coordinates). */
void ControlPointSelection::selectArea(Geom::Path const &path, bool invert)
{
    std::vector<SelectableControlPoint *> out;
    for (auto _all_point : _all_points) {
        if (path.winding(_all_point->position()) % 2 != 0) {
            if (invert) {
                erase(_all_point, false);
            } else {
                insert(_all_point, false, false);
            }
            out.push_back(_all_point);
        }
    }
    if (!out.empty()) {
        _update();
        signal_selection_changed.emit(out, true);
    }
}
/** Unselect all selected points and select all unselected points. */
void ControlPointSelection::invertSelection()
{
    std::vector<SelectableControlPoint *> in, out;
    for (auto _all_point : _all_points) {
        if (_all_point->selected()) {
            in.push_back(_all_point);
            erase(_all_point, false);
        }
        else {
            out.push_back(_all_point);
            insert(_all_point, false, false); 
        }
    }
    _update();
    if (!in.empty())
        signal_selection_changed.emit(in, false);
    if (!out.empty())
        signal_selection_changed.emit(out, true);
}
void ControlPointSelection::spatialGrow(SelectableControlPoint *origin, int dir)
{
    bool grow = (dir > 0);
    Geom::Point p = origin->position();
    double best_dist = grow ? HUGE_VAL : 0;
    SelectableControlPoint *match = nullptr;
    for (auto _all_point : _all_points) {
        bool selected = _all_point->selected();
        if (grow && !selected) {
            double dist = Geom::distance(_all_point->position(), p);
            if (dist < best_dist) {
                best_dist = dist;
                match = _all_point;
            }
        }
        if (!grow && selected) {
            double dist = Geom::distance(_all_point->position(), p);
            // use >= to also deselect the origin node when it's the last one selected
            if (dist >= best_dist) {
                best_dist = dist;
                match = _all_point;
            }
        }
    }
    if (match) {
        if (grow) insert(match);
        else erase(match);
        signal_selection_changed.emit(std::vector<value_type>(1, match), grow);
    }
}

/** Transform all selected control points by the given affine transformation. */
void ControlPointSelection::transform(Geom::Affine const &m)
{
    for (auto cur : _points) {
        cur->transform(m);
    }
    for (auto cur : _points) {
        cur->fixNeighbors();
    }

    _updateBounds();
    // TODO preserving the rotation radius needs some rethinking...
    if (_rot_radius) (*_rot_radius) *= m.descrim();
    if (_mouseover_rot_radius) (*_mouseover_rot_radius) *= m.descrim();
    signal_update.emit();
}

/** Align control points on the specified axis. */
void ControlPointSelection::align(Geom::Dim2 axis, AlignTargetNode target)
{
    if (empty()) return;
    Geom::Dim2 d = static_cast<Geom::Dim2>((axis + 1) % 2);

    Geom::OptInterval bound;
    for (auto _point : _points) {
        bound.unionWith(Geom::OptInterval(_point->position()[d]));
    }

    if (!bound) { return; }

    double new_coord;
    switch (target) {
        case AlignTargetNode::FIRST_NODE:
            new_coord=(_points_list.front())->position()[d];
            break;
        case AlignTargetNode::LAST_NODE:
            new_coord=(_points_list.back())->position()[d];
            break;
        case AlignTargetNode::MID_NODE:
            new_coord=bound->middle();
            break;
        case AlignTargetNode::MIN_NODE:
            new_coord=bound->min();
            break;
        case AlignTargetNode::MAX_NODE:
            new_coord=bound->max();
            break;
        default:
            return;
    }

    for (auto _point : _points) {
        Geom::Point pos = _point->position();
        pos[d] = new_coord;
        _point->move(pos);
    }
    for (auto point : _points) point->fixNeighbors();
    _update();
}

/** Equdistantly distribute control points by moving them in the specified dimension. */
void ControlPointSelection::distribute(Geom::Dim2 d)
{
    if (empty()) return;

    // this needs to be a multimap, otherwise it will fail when some points have the same coord
    typedef std::multimap<double, SelectableControlPoint*> SortMap;

    SortMap sm;
    Geom::OptInterval bound;
    // first we insert all points into a multimap keyed by the aligned coord to sort them
    // simultaneously we compute the extent of selection
    for (auto _point : _points) {
        Geom::Point pos = _point->position();
        sm.insert(std::make_pair(pos[d], _point));
        bound.unionWith(Geom::OptInterval(pos[d]));
    }

    if (!bound) { return; }

    // now we iterate over the multimap and set aligned positions.
    double step = size() == 1 ? 0 : bound->extent() / (size() - 1);
    double start = bound->min();
    unsigned num = 0;
    for (SortMap::iterator i = sm.begin(); i != sm.end(); ++i, ++num) {
        Geom::Point pos = i->second->position();
        pos[d] = start + num * step;
        i->second->move(pos);
    }
    for (auto point : _points) point->fixNeighbors();
    _update();
}

/** Get the bounds of the selection.
 * @return Smallest rectangle containing the positions of all selected points,
 *         or nothing if the selection is empty */
Geom::OptRect ControlPointSelection::pointwiseBounds()
{
    return _bounds;
}

Geom::OptRect ControlPointSelection::bounds()
{
    return size() == 1 ? (*_points.begin())->bounds() : _bounds;
}

/**
 * The first selected point is the first selection a user makes, but only
 * if they selected exactly one point. Selecting multiples at once does nothing.
 */
std::optional<Geom::Point> ControlPointSelection::firstSelectedPoint() const
{
    return _first_point;
}

void ControlPointSelection::showTransformHandles(bool v, bool one_node)
{
    _one_node_handles = one_node;
    _handles_visible = v;
    _updateTransformHandles(false);
}

void ControlPointSelection::hideTransformHandles()
{
    _handles->setVisible(false);
}
void ControlPointSelection::restoreTransformHandles()
{
    _updateTransformHandles(true);
}

void ControlPointSelection::toggleTransformHandlesMode()
{
    if (_handles->mode() == TransformHandleSet::MODE_SCALE) {
        _handles->setMode(TransformHandleSet::MODE_ROTATE_SKEW);
        if (size() == 1) {
            _handles->rotationCenter().setVisible(false);
        }
    } else {
        _handles->setMode(TransformHandleSet::MODE_SCALE);
    }
}

void ControlPointSelection::_pointGrabbed(SelectableControlPoint *point)
{
    hideTransformHandles();
    _dragging = true;
    _grabbed_point = point;
    _farthest_point = point;
    double maxdist = 0;
    Geom::Affine m;
    m.setIdentity();
    for (auto _point : _points) {
        _original_positions.insert(std::make_pair(_point, _point->position()));
        _last_trans.insert(std::make_pair(_point, m));
        double dist = Geom::distance(_grabbed_point->position(), _point->position());
        if (dist > maxdist) {
            maxdist = dist;
            _farthest_point = _point;
        }
    }
}

void ControlPointSelection::_pointDragged(Geom::Point &new_pos, MotionEvent const &event)
{
    if (!_grabbed_point) {
        return;
    }

    Geom::Point abs_delta = new_pos - _original_positions[_grabbed_point];
    double fdist = Geom::distance(_original_positions[_grabbed_point], _original_positions[_farthest_point]);
    if (mod_alt_only(event) && fdist > 0) {
        // Sculpting
        for (auto cur : _points) {
            Geom::Affine trans;
            trans.setIdentity();
            double dist = Geom::distance(_original_positions[cur], _original_positions[_grabbed_point]);
            double deltafrac = 0.5 + 0.5 * cos(M_PI * dist/fdist);
            if (dist != 0.0) {
                // The sculpting transformation is not affine, but it can be
                // locally approximated by one. Here we compute the local
                // affine approximation of the sculpting transformation near
                // the currently transformed point. We then transform the point
                // by this approximation. This gives us sensible behavior for node handles.
                // NOTE: probably it would be better to transform the node handles,
                // but ControlPointSelection is supposed to work for any
                // SelectableControlPoints, not only Nodes. We could create a specialized
                // NodeSelection class that inherits from this one and move sculpting there.
                Geom::Point origdx(Geom::EPSILON, 0);
                Geom::Point origdy(0, Geom::EPSILON);
                Geom::Point origp = _original_positions[cur];
                Geom::Point origpx = _original_positions[cur] + origdx;
                Geom::Point origpy = _original_positions[cur] + origdy;
                double distdx = Geom::distance(origpx, _original_positions[_grabbed_point]);
                double distdy = Geom::distance(origpy, _original_positions[_grabbed_point]);
                double deltafracdx = 0.5 + 0.5 * cos(M_PI * distdx/fdist);
                double deltafracdy = 0.5 + 0.5 * cos(M_PI * distdy/fdist);
                Geom::Point newp = origp + abs_delta * deltafrac;
                Geom::Point newpx = origpx + abs_delta * deltafracdx;
                Geom::Point newpy = origpy + abs_delta * deltafracdy;
                Geom::Point newdx = (newpx - newp) / Geom::EPSILON;
                Geom::Point newdy = (newpy - newp) / Geom::EPSILON;

                Geom::Affine itrans(newdx[Geom::X], newdx[Geom::Y], newdy[Geom::X], newdy[Geom::Y], 0, 0);
                if (itrans.isSingular())
                    itrans.setIdentity();

                trans *= Geom::Translate(-cur->position());
                trans *= _last_trans[cur].inverse();
                trans *= itrans;
                trans *= Geom::Translate(_original_positions[cur] + abs_delta * deltafrac);
                _last_trans[cur] = itrans;
            } else {
                trans *= Geom::Translate(-cur->position() + _original_positions[cur] + abs_delta * deltafrac);
            }
            cur->transform(trans);
            //cur->move(_original_positions[cur] + abs_delta * deltafrac);
        }
    } else {
        Geom::Point delta = new_pos - _grabbed_point->position();
        for (auto cur : _points) {
            cur->move(_original_positions[cur] + abs_delta);
        }
        _handles->rotationCenter().move(_handles->rotationCenter().position() + delta);
    }
    for (auto cur : _points) {
        cur->fixNeighbors();
    }
    signal_update.emit();
}

void ControlPointSelection::_pointUngrabbed()
{
    _desktop->getSnapIndicator()->remove_snaptarget();
    _original_positions.clear();
    _last_trans.clear();
    _dragging = false;
    _grabbed_point = _farthest_point = nullptr;
    _updateBounds();
    restoreTransformHandles();
    signal_commit.emit(COMMIT_MOUSE_MOVE);
}

bool ControlPointSelection::_pointClicked(SelectableControlPoint *p, ButtonReleaseEvent const &event)
{
    // clicking a selected node should toggle the transform handles between rotate and scale mode,
    // if they are visible
    if (mod_none(event) && _handles_visible && p->selected()) {
        toggleTransformHandlesMode();
        return true;
    }
    return false;
}

void ControlPointSelection::_mouseoverChanged()
{
    _mouseover_rot_radius = std::nullopt;
}

void ControlPointSelection::_update()
{
    _updateBounds();
    _updateTransformHandles(false);
    if (_bounds) {
        _handles->rotationCenter().move(_bounds->midpoint());
    }
    // This records the first node's position, ONLY if it was individually selected
    // Any clearing and this first position is cleared too. Any more and we remember it unchanged.
    if (empty()) {
        _first_point = {};
    } else if (size() == 1) {
        _first_point = (*begin())->position();
    }
}

void ControlPointSelection::_updateBounds()
{
    _rot_radius = std::nullopt;
    _bounds = Geom::OptRect();
    for (auto cur : _points) {
        Geom::Point p = cur->position();
        if (!_bounds) {
            _bounds = Geom::Rect(p, p);
        } else {
            _bounds->expandTo(p);
        }
    }
}

void ControlPointSelection::_updateTransformHandles(bool preserve_center)
{
    if (_dragging) return;

    if (_handles_visible && size() > 1) {
        _handles->setBounds(*bounds(), preserve_center);
        _handles->setVisible(true);
    } else if (_one_node_handles && size() == 1) { // only one control point in selection
        SelectableControlPoint *p = *begin();
        _handles->setBounds(p->bounds());
        _handles->rotationCenter().move(p->position());
        _handles->rotationCenter().setVisible(false);
        _handles->setVisible(true);
    } else {
        _handles->setVisible(false);
    }
}

/** Moves the selected points along the supplied unit vector according to
 * the modifier state of the supplied event. */
bool ControlPointSelection::_keyboardMove(KeyPressEvent const &event, Geom::Point const &dir)
{
    if (mod_ctrl(event)) return false;
    unsigned num = 1 + Tools::gobble_key_events(event.keyval, 0);

    auto prefs = Preferences::get();

    Geom::Point delta = dir * num; 
    if (mod_shift(event)) delta *= 10;
    if (mod_alt(event)) {
        delta /= _desktop->current_zoom();
    } else {
        double nudge = prefs->getDoubleLimited("/options/nudgedistance/value", 2, 0, 1000, "px");
        delta *= nudge;
    }

    bool const rotated = prefs->getBool("/options/moverotated/value", true);
    if (rotated) {
        delta *= _desktop->current_rotation().inverse();
    }

    transform(Geom::Translate(delta));
    signal_commit.emit(dir.x() != 0 ? COMMIT_KEYBOARD_MOVE_X : COMMIT_KEYBOARD_MOVE_Y);
    return true;
}

/**
 * Computes the distance to the farthest corner of the bounding box.
 * Used to determine what it means to "rotate by one pixel".
 */
double ControlPointSelection::_rotationRadius(Geom::Point const &rc)
{
    if (empty()) return 1.0; // some safe value
    Geom::Rect b = *bounds();
    double maxlen = 0;
    for (unsigned i = 0; i < 4; ++i) {
        double len = Geom::distance(b.corner(i), rc);
        if (len > maxlen) maxlen = len;
    }
    return maxlen;
}

/**
 * Rotates the selected points in the given direction according to the modifier state
 * from the supplied event.
 * @param event Key event to take modifier state from
 * @param dir   Direction of rotation (math convention: 1 = counterclockwise, -1 = clockwise)
 */
bool ControlPointSelection::_keyboardRotate(KeyPressEvent const &event, int dir)
{
    if (empty()) return false;

    Geom::Point rc;

    // rotate around the mouseovered point, or the selection's rotation center
    // if nothing is mouseovered
    double radius;
    SelectableControlPoint *scp =
        dynamic_cast<SelectableControlPoint*>(ControlPoint::mouseovered_point);
    if (scp) {
        rc = scp->position();
        if (!_mouseover_rot_radius) {
            _mouseover_rot_radius = _rotationRadius(rc);
        }
        radius = *_mouseover_rot_radius;
    } else {
        rc = _handles->rotationCenter().position();
        if (!_rot_radius) {
            _rot_radius = _rotationRadius(rc);
        }
        radius = *_rot_radius;
    }

    double angle;
    if (mod_alt(event)) {
        // Rotate by "one pixel". We interpret this as rotating by an angle that causes
        // the topmost point of a circle circumscribed about the selection's bounding box
        // to move on an arc 1 screen pixel long.
        angle = atan2(1.0 / _desktop->current_zoom(), radius) * dir;
    } else {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        double snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);
        angle = M_PI * dir / snaps;
    }

    // translate to origin, rotate, translate back to original position
    Geom::Affine m = Geom::Translate(-rc)
        * Geom::Rotate(angle) * Geom::Translate(rc);
    transform(m);
    signal_commit.emit(COMMIT_KEYBOARD_ROTATE);
    return true;
}

bool ControlPointSelection::_keyboardScale(KeyPressEvent const &event, int dir)
{
    if (empty()) return false;

    double maxext = bounds()->maxExtent();
    if (Geom::are_near(maxext, 0)) return false;

    Geom::Point center;
    SelectableControlPoint *scp =
        dynamic_cast<SelectableControlPoint*>(ControlPoint::mouseovered_point);
    if (scp) {
        center = scp->position();
    } else {
        center = _handles->rotationCenter().position();
    }

    double length_change;
    if (mod_alt(event)) {
        // Scale by "one pixel". It means shrink/grow 1px for the larger dimension
        // of the bounding box.
        length_change = 1.0 / _desktop->current_zoom() * dir;
    } else {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        length_change = prefs->getDoubleLimited("/options/defaultscale/value", 2, 1, 1000, "px");
        length_change *= dir;
    }
    double scale = (maxext + length_change) / maxext;
    
    Geom::Affine m = Geom::Translate(-center) * Geom::Scale(scale) * Geom::Translate(center);
    transform(m);
    signal_commit.emit(COMMIT_KEYBOARD_SCALE_UNIFORM);
    return true;
}

bool ControlPointSelection::_keyboardFlip(Geom::Dim2 d)
{
    if (empty()) return false;

    Geom::Scale scale_transform(1, 1);
    if (d == Geom::X) {
        scale_transform = Geom::Scale(-1, 1);
    } else {
        scale_transform = Geom::Scale(1, -1);
    }

    SelectableControlPoint *scp =
        dynamic_cast<SelectableControlPoint*>(ControlPoint::mouseovered_point);
    Geom::Point center = scp ? scp->position() : _handles->rotationCenter().position();

    Geom::Affine m = Geom::Translate(-center) * scale_transform * Geom::Translate(center);
    transform(m);
    signal_commit.emit(d == Geom::X ? COMMIT_FLIP_X : COMMIT_FLIP_Y);
    return true;
}

void ControlPointSelection::_commitHandlesTransform(CommitEvent ce)
{
    _updateBounds();
    _updateTransformHandles(true);
    signal_commit.emit(ce);
}

bool ControlPointSelection::event(Inkscape::UI::Tools::ToolBase *, CanvasEvent const &event)
{
    // implement generic event handling that should apply for all control point selections here;
    // for example, keyboard moves and transformations. This way this functionality doesn't need
    // to be duplicated in many places
    // Later split out so that it can be reused in object selection

    if (event.type() != EventType::KEY_PRESS || empty()) {
        return false;
    }

    auto &keyevent = static_cast<KeyPressEvent const &>(event);

    switch (keyevent.keyval) {
        // moves
        case GDK_KEY_Up:
        case GDK_KEY_KP_Up:
        case GDK_KEY_KP_8:
            return _keyboardMove(keyevent, Geom::Point(0, -_desktop->yaxisdir()));
        case GDK_KEY_Down:
        case GDK_KEY_KP_Down:
        case GDK_KEY_KP_2:
            return _keyboardMove(keyevent, Geom::Point(0, _desktop->yaxisdir()));
        case GDK_KEY_Right:
        case GDK_KEY_KP_Right:
        case GDK_KEY_KP_6:
            return _keyboardMove(keyevent, Geom::Point(1, 0));
        case GDK_KEY_Left:
        case GDK_KEY_KP_Left:
        case GDK_KEY_KP_4:
            return _keyboardMove(keyevent, Geom::Point(-1, 0));

        // rotates
        case GDK_KEY_bracketleft:
            return _keyboardRotate(keyevent, -_desktop->yaxisdir());
        case GDK_KEY_bracketright:
            return _keyboardRotate(keyevent, _desktop->yaxisdir());

        // scaling
        case GDK_KEY_less:
        case GDK_KEY_comma:
            return _keyboardScale(keyevent, -1);
        case GDK_KEY_greater:
        case GDK_KEY_period:
            return _keyboardScale(keyevent, 1);

        // TODO: skewing

        // flipping
        // NOTE: H is horizontal flip, while Shift+H switches transform handle mode!
        case GDK_KEY_h:
        case GDK_KEY_H:
            if (mod_shift(keyevent)) {
                toggleTransformHandlesMode();
                return true;
            }
            // any modifiers except shift should cause no action
            if (mod_any(keyevent)) break;
            return _keyboardFlip(Geom::X);
        case GDK_KEY_v:
        case GDK_KEY_V:
            if (mod_any(keyevent)) break;
            return _keyboardFlip(Geom::Y);
        default:
            break;
    }

    return false;
}

void ControlPointSelection::getOriginalPoints(std::vector<Inkscape::SnapCandidatePoint> &pts)
{
    pts.clear();
    for (auto _point : _points) {
        pts.emplace_back(_original_positions[_point], SNAPSOURCE_NODE_HANDLE);
    }
}

void ControlPointSelection::getUnselectedPoints(std::vector<Inkscape::SnapCandidatePoint> &pts)
{
    pts.clear();
    ControlPointSelection::Set &nodes = this->allPoints();
    for (auto node : nodes) {
        if (!node->selected()) {
            Node *n = static_cast<Node*>(node);
            pts.push_back(n->snapCandidatePoint());
        }
    }
}

void ControlPointSelection::setOriginalPoints()
{
    _original_positions.clear();
    for (auto _point : _points) {
        _original_positions.insert(std::make_pair(_point, _point->position()));
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
