// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Multi path manipulator - a tool component that edits multiple paths at once
 */
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOL_MULTI_PATH_MANIPULATOR_H
#define INKSCAPE_UI_TOOL_MULTI_PATH_MANIPULATOR_H

#include <2geom/affine.h>

#include "commit-events.h"
#include "node-types.h"
#include "manipulator.h"
#include "modifier-tracker.h"
#include "shape-record.h"
#include "util-string/context-string.h"

namespace Geom {
class PathBuilder;
}

namespace Inkscape {
namespace UI {
enum class NodeDeleteMode;

class PathManipulator;
class MultiPathManipulator;
struct PathSharedData;

/**
 * Manipulator that manages multiple path manipulators active at the same time.
 */
class MultiPathManipulator : public PointManipulator
{
public:
    MultiPathManipulator(PathSharedData &data);
    ~MultiPathManipulator() override;

    bool event(Inkscape::UI::Tools::ToolBase *tool, CanvasEvent const &event) override;

    bool empty() const { return _mmap.empty(); }
    size_t size() const { return _mmap.size(); }
    void setItems(std::set<ShapeRecord> const &);
    void clear() { _mmap.clear(); }
    void cleanup();
    bool isReplacingItem() const { return _replacing_item; }

    void selectSubpaths();
    void shiftSelection(int dir);
    void invertSelectionInSubpaths();

    void setNodeType(NodeType t);
    void setSegmentType(SegmentType t);

    void insertNodesAtExtrema(ExtremumType extremum);
    void insertNodes();
    void insertNode(Geom::Point pt);
    void alertLPE();
    void duplicateNodes();
    void copySelectedPath(Geom::PathBuilder *builder);
    void simplifyInvisible(double epsilon);
    void joinNodes();
    void breakNodes();
    void deleteNodes();
    void deleteNodes(NodeDeleteMode mode);
    void joinSegments();
    void deleteSegments();
    void alignNodes(Geom::Dim2 d, AlignTargetNode target = AlignTargetNode::MID_NODE);
    void distributeNodes(Geom::Dim2 d);
    void reverseSubpaths();
    void move(Geom::Point const &delta);
    void scale(Geom::Point const &center, Geom::Point const &scale);

    void showOutline(bool show);
    void showHandles(bool show);
    void showPathDirection(bool show);
    void setLiveOutline(bool set);
    void setLiveObjects(bool set);
    void updateOutlineColors();
    void updateHandles();
    void updatePaths();

    /// Emitted whenever the coordinates shown in the status bar need updating.
    sigc::signal<void ()> signal_coords_changed;

private:
    using MapPair = std::pair<ShapeRecord, std::shared_ptr<PathManipulator>>;
    using MapType = std::map<ShapeRecord, std::shared_ptr<PathManipulator>>;

    template <typename R>
    void invokeForAll(R (PathManipulator::*method)()) {
        // With WriteXML(), a path may be removed during the loop. Thus, we operate
        // on a copy of the map (relying on i->second being a std::shared_ptr).
        auto temp(_mmap);
        for (auto & i : temp) {
            ((i.second.get())->*method)();
        }
    }
    template <typename R, typename A>
    void invokeForAll(R (PathManipulator::*method)(A), A a) {
        for (auto & i : _mmap) {
            ((i.second.get())->*method)(a);
        }
    }
    template <typename R, typename A>
    void invokeForAll(R (PathManipulator::*method)(A const &), A const &a) {
        for (auto & i : _mmap) {
            ((i.second.get())->*method)(a);
        }
    }
    template <typename R, typename A, typename B>
    void invokeForAll(R (PathManipulator::*method)(A,B), A a, B b) {
        for (auto & i : _mmap) {
            ((i.second.get())->*method)(a, b);
        }
    }

    void _commit(CommitEvent cps);
    void _done(Inkscape::Util::Internal::ContextString reason, bool alert_LPE = true);
    void _maybeDone(char const *key, Inkscape::Util::Internal::ContextString reason, bool alert_LPE = true);
    void _doneWithCleanup(Inkscape::Util::Internal::ContextString reason, bool alert_LPE = false);
    Colors::Color _getOutlineColor(ShapeRole role, SPObject *object);

    MapType _mmap;
    bool _replacing_item = false;

public:
    PathSharedData const &_path_data;

private:
    ModifierTracker _tracker;
    bool _show_handles;
    bool _show_outline;
    bool _show_path_direction;
    bool _live_outline;
    bool _live_objects;

    friend class PathManipulator;
};

} // namespace UI
} // namespace Inkscape

#endif // INKSCAPE_UI_TOOL_MULTI_PATH_MANIPULATOR_H

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
