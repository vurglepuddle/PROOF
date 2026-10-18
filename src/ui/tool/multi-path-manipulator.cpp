// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Multi path manipulator - implementation.
 */
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *   Abhishek Sharma
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/tool/multi-path-manipulator.h"

#include <unordered_set>
#include <glibmm/i18n.h>

#include "control-point-selection.h"
#include "desktop.h"
#include "document-undo.h"
#include "live_effects/lpeobject.h"
#include "node.h"
#include "object/mesh-object.h"
#include "object/sp-path.h"
#include "path-manipulator.h"
#include "ui/icon-names.h"
#include "ui/widget/events/canvas-event.h"

namespace Inkscape {
namespace UI {

namespace {

struct hash_nodelist_iterator
{
    std::size_t operator()(NodeList::iterator i) const {
        return std::hash<NodeList::iterator::pointer>()(&*i);
    }
};

typedef std::pair<NodeList::iterator, NodeList::iterator> IterPair;
typedef std::vector<IterPair> IterPairList;
typedef std::unordered_set<NodeList::iterator, hash_nodelist_iterator> IterSet;
typedef std::multimap<double, IterPair> DistanceMap;
typedef std::pair<double, IterPair> DistanceMapItem;

/** Find pairs of selected endnodes suitable for joining. */
void find_join_iterators(ControlPointSelection &sel, IterPairList &pairs)
{
    IterSet join_iters;

    // find all endnodes in selection
    for (auto i : sel) {
        Node *node = dynamic_cast<Node*>(i);
        if (!node) continue;
        NodeList::iterator iter = NodeList::get_iterator(node);
        if (!iter.next() || !iter.prev()) join_iters.insert(iter);
    }

    if (join_iters.size() < 2) return;

    // Below we find the closest pairs. The algorithm is O(N^3).
    // We can go down to O(N^2 log N) by using O(N^2) memory, by putting all pairs
    // with their distances in a multimap (not worth it IMO).
    while (join_iters.size() >= 2) {
        double closest = DBL_MAX;
        IterPair closest_pair;
        for (IterSet::iterator i = join_iters.begin(); i != join_iters.end(); ++i) {
            for (IterSet::iterator j = join_iters.begin(); j != i; ++j) {
                double dist = Geom::distance((*i)->position(), (*j)->position());
                if (dist < closest) {
                    closest = dist;
                    closest_pair = std::make_pair(*i, *j);
                }
            }
        }
        pairs.push_back(closest_pair);
        join_iters.erase(closest_pair.first);
        join_iters.erase(closest_pair.second);
    }
}

/** After this function, first should be at the end of path and second at the beginning.
 * @returns True if the nodes are in the same subpath */
bool prepare_join(IterPair &join_iters)
{
    if (&NodeList::get(join_iters.first) == &NodeList::get(join_iters.second)) {
        if (join_iters.first.next()) // if first is begin, swap the iterators
            std::swap(join_iters.first, join_iters.second);
        return true;
    }

    NodeList &sp_first = NodeList::get(join_iters.first);
    NodeList &sp_second = NodeList::get(join_iters.second);
    if (join_iters.first.next()) { // first is begin
        if (join_iters.second.next()) { // second is begin
            sp_first.reverse();
        } else { // second is end
            std::swap(join_iters.first, join_iters.second);
        }
    } else { // first is end
        if (join_iters.second.next()) { // second is begin
            // do nothing
        } else { // second is end
            sp_second.reverse();
        }
    }
    return false;
}
} // anonymous namespace


MultiPathManipulator::MultiPathManipulator(PathSharedData &data)
    : PointManipulator(data.node_data.desktop, *data.node_data.selection)
    , _path_data(data)
{
    _selection.signal_commit.connect(
        sigc::mem_fun(*this, &MultiPathManipulator::_commit));
    _selection.signal_selection_changed.connect(
        sigc::hide( sigc::hide(
            signal_coords_changed.make_slot())));
}

MultiPathManipulator::~MultiPathManipulator()
{
    _mmap.clear();
}

/** Remove empty manipulators. */
void MultiPathManipulator::cleanup()
{
    std::erase_if(_mmap, [] (auto const &i) {
        return i.second->empty();
    });
}

/**
 * Change the set of items to edit.
 *
 * This method attempts to preserve as much of the state as possible.
 */
void MultiPathManipulator::setItems(std::set<ShapeRecord> const &s)
{
    std::set<ShapeRecord> shapes(s);

    // iterate over currently edited items, modifying / removing them as necessary
    for (MapType::iterator i = _mmap.begin(); i != _mmap.end();) {
        std::set<ShapeRecord>::iterator si = shapes.find(i->first);
        if (si == shapes.end()) {
            // This item is no longer supposed to be edited - remove its manipulator
            i = _mmap.erase(i);
        } else {
            ShapeRecord const &sr = i->first;
            ShapeRecord const &sr_new = *si;
            // if the shape record differs, replace the key only and modify other values
            if (sr.edit_transform != sr_new.edit_transform ||
                sr.role != sr_new.role)
            {
                std::shared_ptr<PathManipulator> hold(i->second);
                if (sr.edit_transform != sr_new.edit_transform)
                    hold->setControlsTransform(sr_new.edit_transform);
                if (sr.role != sr_new.role) {
                    //hold->setOutlineColor(_getOutlineColor(sr_new.role));
                }
                i = _mmap.erase(i);
                _mmap.insert(std::make_pair(sr_new, hold));
            } else {
                ++i;
            }
            shapes.erase(si); // remove the processed record
        }
    }

    // add newly selected items
    for (auto const &r : shapes) {
        if (!(is<SPPath>(r.object) || is<LivePathEffectObject>(r.object) || PathManipulator::canEditShape(r.object))) {
            continue;
        }
        // A mesh object's outline follows its mesh: Direct Selection shows its
        // mesh points (the gradient dragger), as Illustrator does, not path anchors
        // or Live Corners that would move the outline away from the mesh.
        if (r.role == SHAPE_ROLE_NORMAL && MeshObject::is_mesh_object(r.object)) {
            continue;
        }
        auto newpm = std::make_shared<PathManipulator>(*this, r.object,
            r.edit_transform, _getOutlineColor(r.role, r.object).toRGBA(), r.lpe_key);
        newpm->showHandles(_show_handles);
        // always show outlines for clips and masks
        newpm->showOutline(_show_outline || r.role != SHAPE_ROLE_NORMAL);
        newpm->showPathDirection(_show_path_direction);
        newpm->setLiveOutline(_live_outline);
        newpm->setLiveObjects(_live_objects);
        _mmap.emplace(r, std::move(newpm));
    }
}

bool MultiPathManipulator::pressSegment(SPObject *item, Tools::ToolBase *tool, ButtonPressEvent const &event)
{
    for (auto const &[record, pm] : _mmap) {
        if (record.object == item && pm->pressSegment(tool, event)) return true;
    }
    return false;
}

void MultiPathManipulator::selectSubpaths()
{
    if (_selection.empty()) {
        _selection.selectAll();
    } else {
        invokeForAll(&PathManipulator::selectSubpaths);
    }
}

void MultiPathManipulator::shiftSelection(int dir)
{
    if (empty()) return;

    // 1. find last selected node
    // 2. select the next node; if the last node or nothing is selected,
    //    select first node
    MapType::iterator last_i;
    SubpathList::iterator last_j;
    NodeList::iterator last_k;
    bool anything_found = false;
    bool anynode_found = false;

    for (MapType::iterator i = _mmap.begin(); i != _mmap.end(); ++i) {
        SubpathList &sp = i->second->subpathList();
        for (SubpathList::iterator j = sp.begin(); j != sp.end(); ++j) {
            anynode_found = true;
            for (NodeList::iterator k = (*j)->begin(); k != (*j)->end(); ++k) {
                if (k->selected()) {
                    last_i = i;
                    last_j = j;
                    last_k = k;
                    anything_found = true;
                    // when tabbing backwards, we want the first node
                    if (dir == -1) goto exit_loop;
                }
            }
        }
    }
    exit_loop:

    // NOTE: we should not assume the _selection contains only nodes
    // in future it might also contain handles and other types of control points
    // this is why we use a flag instead in the loop above, instead of calling
    // selection.empty()
    if (!anything_found) {
        // select first / last node
        // this should never fail because there must be at least 1 non-empty manipulator
        if (anynode_found) {
          if (dir == 1) {
            _selection.insert((*_mmap.begin()->second->subpathList().begin())->begin().ptr());
          } else {
            _selection.insert((--(*--(--_mmap.end())->second->subpathList().end())->end()).ptr());
          }
        }
        return;
    }

    // three levels deep - w00t!
    if (dir == 1) {
        if (++last_k == (*last_j)->end()) {
            // here, last_k points to the node to be selected
            ++last_j;
            if (last_j == last_i->second->subpathList().end()) {
                ++last_i;
                if (last_i == _mmap.end()) {
                    last_i = _mmap.begin();
                }
                last_j = last_i->second->subpathList().begin();
            }
            last_k = (*last_j)->begin();
        }
    } else {
        if (!last_k || last_k == (*last_j)->begin()) {
            if (last_j == last_i->second->subpathList().begin()) {
                if (last_i == _mmap.begin()) {
                    last_i = _mmap.end();
                }
                --last_i;
                last_j = last_i->second->subpathList().end();
            }
            --last_j;
            last_k = (*last_j)->end();
        }
        --last_k;
    }
    _selection.clear();
    _selection.insert(last_k.ptr());
}

void MultiPathManipulator::invertSelectionInSubpaths()
{
    invokeForAll(&PathManipulator::invertSelectionInSubpaths);
}

void MultiPathManipulator::setNodeType(NodeType type)
{
    if (_selection.empty()) return;

    // When all selected nodes are already cusp, retract their handles
    bool retract_handles = (type == NODE_CUSP);

    for (auto i : _selection) {
        Node *node = dynamic_cast<Node*>(i);
        if (node) {
            retract_handles &= (node->type() == NODE_CUSP);
            node->setType(type);
        }
    }

    if (retract_handles) {
        for (auto i : _selection) {
            Node *node = dynamic_cast<Node*>(i);
            if (node) {
                node->front()->retract();
                node->back()->retract();
            }
        }
    }

    _done(retract_handles ? RC_("Undo", "Retract handles") : RC_("Undo", "Change node type"));
}

void MultiPathManipulator::setSegmentType(SegmentType type)
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::setSegmentType, type);
    if (type == SEGMENT_STRAIGHT) {
        _done(RC_("Undo", "Straighten segments"));
    } else {
        _done(RC_("Undo", "Make segments curves"));
    }
}

void MultiPathManipulator::insertNodes()
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::insertNodes);
    _done(RC_("Undo", "Add nodes"));
}
void MultiPathManipulator::insertNodesAtExtrema(ExtremumType extremum)
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::insertNodeAtExtremum, extremum);
    _done(RC_("Undo", "Add extremum nodes"));
}

void MultiPathManipulator::insertNode(Geom::Point pt)
{
    // When double clicking to insert nodes, we might not have a selection of nodes (and we don't need one)
    // so don't check for "_selection.empty()" here, contrary to the other methods above and below this one
    invokeForAll(&PathManipulator::insertNode, pt);
    _done(RC_("Undo", "Add nodes"));
}

void MultiPathManipulator::duplicateNodes()
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::duplicateNodes);
    _done(RC_("Undo", "Duplicate nodes"));
}

void MultiPathManipulator::copySelectedPath(Geom::PathBuilder *builder)
{
    if (_selection.empty())
        return;
    invokeForAll(&PathManipulator::copySelectedPath, builder);
    _done(RC_("Undo", "Copy nodes"));
}

void MultiPathManipulator::simplifyInvisible(double epsilon)
{
    if (_selection.empty())
        return;
    invokeForAll(&PathManipulator::simplifyInvisible, epsilon);
    _doneWithCleanup(RC_("Undo", "Simplify invisible nodes"), true);
}

void MultiPathManipulator::joinNodes()
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::hideDragPoint);
    // Node join has two parts. In the first one we join two subpaths by fusing endpoints
    // into one. In the second we fuse nodes in each subpath.
    IterPairList joins;
    NodeList::iterator preserve_pos;
    Node *mouseover_node = dynamic_cast<Node*>(ControlPoint::mouseovered_point);
    if (mouseover_node) {
        preserve_pos = NodeList::get_iterator(mouseover_node);
    }
    find_join_iterators(_selection, joins);

    for (auto & join : joins) {
        bool same_path = prepare_join(join);
        NodeList &sp_first = NodeList::get(join.first);
        NodeList &sp_second = NodeList::get(join.second);
        join.first->setType(NODE_CUSP, false);

        Geom::Point joined_pos, pos_handle_front, pos_handle_back;
        pos_handle_front = join.second->front()->position();
        pos_handle_back = join.first->back()->position();

        // When we encounter the mouseover node, we unset the iterator - it will be invalidated
        if (join.first == preserve_pos) {
            joined_pos = join.first->position();
            preserve_pos = NodeList::iterator();
        } else if (join.second == preserve_pos) {
            joined_pos = join.second->position();
            preserve_pos = NodeList::iterator();
        } else {
            joined_pos = Geom::middle_point(join.first->position(), join.second->position());
        }

        // if the handles aren't degenerate, don't move them
        join.first->move(joined_pos);
        Node *joined_node = join.first.ptr();
        if (!join.second->front()->isDegenerate()) {
            joined_node->front()->setPosition(pos_handle_front);
        }
        if (!join.first->back()->isDegenerate()) {
            joined_node->back()->setPosition(pos_handle_back);
        }
        sp_second.erase(join.second);

        if (same_path) {
            sp_first.setClosed(true);
        } else {
            sp_first.splice(sp_first.end(), sp_second);
            sp_second.kill();
        }
        _selection.insert(join.first.ptr());
    }

    if (joins.empty()) {
        // Second part replaces contiguous selections of nodes with single nodes
        invokeForAll(&PathManipulator::weldNodes, preserve_pos);
    }

    _doneWithCleanup(RC_("Undo", "Join nodes"), true);
}

void MultiPathManipulator::breakNodes()
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::breakNodes);
    _done(RC_("Undo", "Break nodes"), true);
}

/**
 * Delete nodes, use the preference to decide which mode to use.
 */
void MultiPathManipulator::deleteNodes() {
    auto prefs = Inkscape::Preferences::get();
    deleteNodes((NodeDeleteMode)prefs->getInt("/tools/nodes/delete_mode", 0));
}

void MultiPathManipulator::deleteNodes(NodeDeleteMode mode)
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::deleteNodes, mode);
    _doneWithCleanup(RC_("Undo", "Delete nodes"), true);
}

/** Join selected endpoints to create segments. */
void MultiPathManipulator::joinSegments()
{
    if (_selection.empty()) return;
    IterPairList joins;
    find_join_iterators(_selection, joins);

    for (auto & join : joins) {
        bool same_path = prepare_join(join);
        NodeList &sp_first = NodeList::get(join.first);
        NodeList &sp_second = NodeList::get(join.second);
        join.first->setType(NODE_CUSP, false);
        join.second->setType(NODE_CUSP, false);
        if (same_path) {
            sp_first.setClosed(true);
        } else {
            sp_first.splice(sp_first.end(), sp_second);
            sp_second.kill();
        }
    }

    if (joins.empty()) {
        invokeForAll(&PathManipulator::weldSegments);
    }
    _doneWithCleanup(RC_("Undo", "Join segments"), true);
}

void MultiPathManipulator::deleteSegments()
{
    if (_selection.empty()) return;
    invokeForAll(&PathManipulator::deleteSegments);
    _doneWithCleanup(RC_("Undo", "Delete segments"), true);
}

void MultiPathManipulator::alignNodes(Geom::Dim2 d, AlignTargetNode target)
{
    if (_selection.empty()) return;
    _selection.align(d, target);
    if (d == Geom::X) {
        _done(RC_("Undo", "Align nodes to a horizontal line"));
    } else {
        _done(RC_("Undo", "Align nodes to a vertical line"));
    }
}

void MultiPathManipulator::distributeNodes(Geom::Dim2 d)
{
    if (_selection.empty()) return;
    _selection.distribute(d);
    if (d == Geom::X) {
        _done(RC_("Undo", "Distribute nodes horizontally"));
    } else {
        _done(RC_("Undo", "Distribute nodes vertically"));
    }
}

void MultiPathManipulator::reverseSubpaths()
{
    if (_selection.empty()) {
        invokeForAll(&PathManipulator::reverseSubpaths, false);
        _done(RC_("Undo", "Reverse subpaths"));
    } else {
        invokeForAll(&PathManipulator::reverseSubpaths, true);
        _done(RC_("Undo", "Reverse selected subpaths"));
    }
}

void MultiPathManipulator::move(Geom::Point const &delta)
{
    if (_selection.empty()) return;
    _selection.transform(Geom::Translate(delta));
    _maybeDone("manipulator:move", RC_("Undo", "Move nodes"));
}

void MultiPathManipulator::scale(Geom::Point const &center, Geom::Point const &scale)
{
    if (_selection.empty()) return;

    Geom::Translate const n2d(-center);
    Geom::Translate const d2n(center);
    _selection.transform(n2d * Geom::Scale(scale) * d2n);

    _maybeDone("manipulator:scale", RC_("Undo", "Scale nodes"));
}

void MultiPathManipulator::showOutline(bool show)
{
    for (auto & i : _mmap) {
        // always show outlines for clipping paths and masks
        i.second->showOutline(show || i.first.role != SHAPE_ROLE_NORMAL);
    }
    _show_outline = show;
}

void MultiPathManipulator::showHandles(bool show)
{
    invokeForAll(&PathManipulator::showHandles, show);
    _show_handles = show;
}

void MultiPathManipulator::showPathDirection(bool show)
{
    invokeForAll(&PathManipulator::showPathDirection, show);
    _show_path_direction = show;
}

/**
 * Set live outline update status.
 * When set to true, outline will be updated continuously when dragging
 * or transforming nodes. Otherwise it will only update when changes are committed
 * to XML.
 */
void MultiPathManipulator::setLiveOutline(bool set)
{
    invokeForAll(&PathManipulator::setLiveOutline, set);
    _live_outline = set;
}

/**
 * Set live object update status.
 * When set to true, objects will be updated continuously when dragging
 * or transforming nodes. Otherwise they will only update when changes are committed
 * to XML.
 */
void MultiPathManipulator::setLiveObjects(bool set)
{
    invokeForAll(&PathManipulator::setLiveObjects, set);
    _live_objects = set;
}

void MultiPathManipulator::updateOutlineColors()
{
    //for (MapType::iterator i = _mmap.begin(); i != _mmap.end(); ++i) {
    //    i->second->setOutlineColor(_getOutlineColor(i->first.role));
    //}
}

void MultiPathManipulator::updateHandles()
{
    invokeForAll(&PathManipulator::updateHandles);
}

void MultiPathManipulator::updatePaths()
{
    invokeForAll(&PathManipulator::updatePath);
}

bool MultiPathManipulator::event(Inkscape::UI::Tools::ToolBase *tool, CanvasEvent const &event)
{
    _tracker.event(event);
    unsigned key = 0;
    if (event.type() == EventType::KEY_PRESS) {
        key = static_cast<KeyPressEvent const &>(event).keyval;
    }

    // Single handle adjustments go here.
    if (_selection.size() == 1 && event.type() == EventType::KEY_PRESS) {
        do {
            auto n = dynamic_cast<Node*>(*_selection.begin());
            if (!n) break;

            auto &pm = n->nodeList().subpathList().pm();

            int which = 0;
            if (_tracker.rightAlt() || _tracker.rightControl()) {
                which = 1;
            }
            if (_tracker.leftAlt() || _tracker.leftControl()) {
                if (which != 0) break; // ambiguous
                which = -1;
            }
            if (which == 0) break; // no handle chosen
            bool one_pixel = _tracker.leftAlt() || _tracker.rightAlt();

            switch (key) {
            // single handle functions
            // rotation
            case GDK_KEY_bracketleft:
            case GDK_KEY_braceleft:
                pm.rotateHandle(n, which, -_desktop->yaxisdir(), one_pixel);
                return true;
            case GDK_KEY_bracketright:
            case GDK_KEY_braceright:
                pm.rotateHandle(n, which, _desktop->yaxisdir(), one_pixel);
                return true;
            // adjust length
            case GDK_KEY_period:
            case GDK_KEY_greater:
                pm.scaleHandle(n, which, 1, one_pixel);
                return true;
            case GDK_KEY_comma:
            case GDK_KEY_less:
                pm.scaleHandle(n, which, -1, one_pixel);
                return true;
            default:
                break;
            }
        } while (false);
    }

    bool ret = false;

    inspect_event(event,
    [&] (KeyPressEvent const &event) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();

        switch (key) {
        case GDK_KEY_Insert:
        case GDK_KEY_KP_Insert:
            // Insert - insert nodes in the middle of selected segments
            insertNodes();
            ret = true;
            return;
        case GDK_KEY_i:
        case GDK_KEY_I:
            if (mod_shift_only(event)) {
                // Shift+I - insert nodes (alternate keybinding for Mac keyboards
                //           that don't have the Insert key)
                insertNodes();
                ret = true;
                return;
            }
            break;
        case GDK_KEY_d:
        case GDK_KEY_D:
            if (mod_shift_only(event)) {
                duplicateNodes();
                ret = true;
                return;
            }
            break;
        case GDK_KEY_j:
        case GDK_KEY_J:
            if (mod_shift_only(event)) {
                // Shift+J - join nodes
                joinNodes();
                ret = true;
                return;
            }
            if (mod_alt_only(event)) {
                // Alt+J - join segments
                joinSegments();
                ret = true;
                return;
            }
            break;
        case GDK_KEY_b:
        case GDK_KEY_B:
            if (mod_shift_only(event)) {
                // Shift+B - break nodes
                breakNodes();
                ret = true;
                return;
            }
            break;
        case GDK_KEY_c:
        case GDK_KEY_C:
            if (mod_shift_only(event)) {
                // Shift+C - make nodes cusp
                setNodeType(NODE_CUSP);
                ret = true;
                return;
            }
            break;
        case GDK_KEY_s:
        case GDK_KEY_S:
            if (mod_shift_only(event)) {
                // Shift+S - make nodes smooth
                setNodeType(NODE_SMOOTH);
                ret = true;
                return;
            }
            break;
        case GDK_KEY_a:
        case GDK_KEY_A:
            if (mod_shift_only(event)) {
                // Shift+A - make nodes auto-smooth
                setNodeType(NODE_AUTO);
                ret = true;
                return;
            }
            break;
        case GDK_KEY_y:
        case GDK_KEY_Y:
            if (mod_shift_only(event)) {
                // Shift+Y - make nodes symmetric
                setNodeType(NODE_SYMMETRIC);
                ret = true;
                return;
            }
            break;
        case GDK_KEY_r:
        case GDK_KEY_R:
            if (mod_shift_only(event)) {
                // Shift+R - reverse subpaths
                reverseSubpaths();
                ret = true;
                return;
            }
            break;
        case GDK_KEY_l:
        case GDK_KEY_L:
            if (mod_shift_only(event)) {
                // Shift+L - make segments linear
                setSegmentType(SEGMENT_STRAIGHT);
                ret = true;
                return;
            }
            break;
        case GDK_KEY_u:
        case GDK_KEY_U:
            if (mod_shift_only(event)) {
                // Shift+U - make segments curves
                setSegmentType(SEGMENT_CUBIC_BEZIER);
                ret = true;
                return;
            }
            break;
        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:
        case GDK_KEY_BackSpace:
            if (mod_shift(event)) {
                deleteNodes((NodeDeleteMode)prefs->getInt("/tools/node/delete-mode-shift", (int)NodeDeleteMode::inverse_auto));
            } else if (mod_alt(event)) {
                deleteNodes((NodeDeleteMode)prefs->getInt("/tools/node/delete-mode-alt", (int)NodeDeleteMode::gap_nodes));
            } else if (mod_ctrl(event)) {
                deleteNodes((NodeDeleteMode)prefs->getInt("/tools/node/delete-mode-ctrl", (int)NodeDeleteMode::line_segment));
            } else {
                deleteNodes((NodeDeleteMode)prefs->getInt("/tools/node/delete-mode-default", (int)NodeDeleteMode::automatic));
            }

            // Delete any selected gradient nodes as well
            tool->deleteSelectedDrag(mod_ctrl(event));

            ret = true;
            return;
        default:
            break;
        }
    },
    [&] (MotionEvent const &event) {
        for (auto &it : _mmap) {
            if (it.second->event(tool, event)) {
                ret = true;
                return;
            }
        }
    },
    [&] (CanvasEvent const &event) {}
    );

    return ret;
}

/** Commit changes to XML and add undo stack entry based on the action that was done. Invoked
 * by sub-manipulators, for example TransformHandleSet and ControlPointSelection. */
void MultiPathManipulator::_commit(CommitEvent cps)
{
    std::optional<Inkscape::Util::Internal::ContextString> reason;
    gchar const *key = nullptr;

    switch(cps) {
    case COMMIT_MOUSE_MOVE:
        reason = RC_("Undo", "Move nodes");
        break;
    case COMMIT_KEYBOARD_MOVE_X:
        reason = RC_("Undo", "Move nodes horizontally");
        key = "node:move:x";
        break;
    case COMMIT_KEYBOARD_MOVE_Y:
        reason = RC_("Undo", "Move nodes vertically");
        key = "node:move:y";
        break;
    case COMMIT_MOUSE_ROTATE:
        reason = RC_("Undo", "Rotate nodes");
        break;
    case COMMIT_KEYBOARD_ROTATE:
        reason = RC_("Undo", "Rotate nodes");
        key = "node:rotate";
        break;
    case COMMIT_MOUSE_SCALE_UNIFORM:
        reason = RC_("Undo", "Scale nodes uniformly");
        break;
    case COMMIT_MOUSE_SCALE:
        reason = RC_("Undo", "Scale nodes");
        break;
    case COMMIT_KEYBOARD_SCALE_UNIFORM:
        reason = RC_("Undo", "Scale nodes uniformly");
        key = "node:scale:uniform";
        break;
    case COMMIT_KEYBOARD_SCALE_X:
        reason = RC_("Undo", "Scale nodes horizontally");
        key = "node:scale:x";
        break;
    case COMMIT_KEYBOARD_SCALE_Y:
        reason = RC_("Undo", "Scale nodes vertically");
        key = "node:scale:y";
        break;
    case COMMIT_MOUSE_SKEW_X:
        reason = RC_("Undo", "Skew nodes horizontally");
        key = "node:skew:x";
        break;
    case COMMIT_MOUSE_SKEW_Y:
        reason = RC_("Undo", "Skew nodes vertically");
        key = "node:skew:y";
        break;
    case COMMIT_FLIP_X:
        reason = RC_("Undo", "Flip nodes horizontally");
        break;
    case COMMIT_FLIP_Y:
        reason = RC_("Undo", "Flip nodes vertically");
        break;
    default: return;
    }

    _selection.signal_update.emit();
    invokeForAll(&PathManipulator::writeXML);
    if (key) {
        DocumentUndo::maybeDone(_desktop->getDocument(), key, *reason, INKSCAPE_ICON("tool-node-editor"));
    } else {
        DocumentUndo::done(_desktop->getDocument(), *reason, INKSCAPE_ICON("tool-node-editor"));
    }
    signal_coords_changed.emit();
}

/** Commits changes to XML and adds undo stack entry. */
void MultiPathManipulator::_done(Inkscape::Util::Internal::ContextString reason, bool alert_LPE) {
    invokeForAll(&PathManipulator::update, alert_LPE);
    invokeForAll(&PathManipulator::writeXML);
    DocumentUndo::done(_desktop->getDocument(), reason, INKSCAPE_ICON("tool-node-editor"));
    signal_coords_changed.emit();
}

void MultiPathManipulator::_maybeDone(char const *key, Inkscape::Util::Internal::ContextString reason, bool alert_LPE)
{
    invokeForAll(&PathManipulator::update, alert_LPE);
    invokeForAll(&PathManipulator::writeXML);
    DocumentUndo::maybeDone(_desktop->getDocument(), key, reason, INKSCAPE_ICON("tool-node-editor"));
    signal_coords_changed.emit();
}

/** Commits changes to XML, adds undo stack entry and removes empty manipulators. */
void MultiPathManipulator::_doneWithCleanup(Inkscape::Util::Internal::ContextString reason, bool alert_LPE) {
    _done(reason, alert_LPE);
    cleanup();
}

/** Get an outline color based on the shape's role (normal, mask, LPE parameter, etc.). */
Colors::Color MultiPathManipulator::_getOutlineColor(ShapeRole role, SPObject *object)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    switch(role) {
    case SHAPE_ROLE_CLIPPING_PATH:
        return prefs->getColor("/tools/nodes/clipping_path_color", "#00ff00ff");
    case SHAPE_ROLE_MASK:
        return prefs->getColor("/tools/nodes/mask_color", "#0000ffff");
    case SHAPE_ROLE_LPE_PARAM:
        return prefs->getColor("/tools/nodes/lpe_param_color", "#009000ff");
    case SHAPE_ROLE_NORMAL:
    default:
        return prefs->getColor("/tools/nodes/highlight_color", "#ff0000ff");;
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
