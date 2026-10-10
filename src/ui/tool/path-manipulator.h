// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Path manipulator - a component that edits a single path on-canvas
 */
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOL_PATH_MANIPULATOR_H
#define INKSCAPE_UI_TOOL_PATH_MANIPULATOR_H

#include <memory>
#include <optional>
#include <2geom/path-sink.h>
#include "manipulator.h"
#include "node.h"
#include "object/sp-object.h"
#include "util-string/context-string.h"

class SPPath;

namespace Inkscape {

class CanvasItemBpath;

namespace XML { class Node; }

namespace UI {

class PathManipulator;
class ControlPointSelection;
class PathManipulatorObserver;
class CurveDragPoint;
class PathCanvasGroups;
class MultiPathManipulator;
class Node;
class Handle;

struct PathSharedData
{
    NodeSharedData node_data;
    Inkscape::CanvasItemGroup *outline_group;
    Inkscape::CanvasItemGroup *dragpoint_group;
};

enum class NodeDeleteMode
{
    automatic,    // try to preserve shape if deleted nodes do not form sharp corners
    inverse_auto, // opposite of what automatic mode would do
    curve_fit,    // preserve shape
    line_segment, // do not preserve shape; delete nodes and connect subpaths with a line segment
    gap_nodes,    // Remove the connection between the selected nodes, leaving a gap
    gap_lines     // Remove the connection between the selected lines, leaving a gap
};

/**
 * Manipulator that edits a single path using nodes with handles.
 * Currently only cubic bezier and linear segments are supported, but this might change
 * some time in the future.
 */
class PathManipulator
    : public PointManipulator
    , public std::enable_shared_from_this<PathManipulator>
{
public:
    using ItemType = SPPath*;

    PathManipulator(MultiPathManipulator &mpm, SPObject *path, Geom::Affine const &edit_trans,
        uint32_t outline_color, Glib::ustring lpe_key);
    ~PathManipulator() override;
    bool event(Inkscape::UI::Tools::ToolBase *tool, CanvasEvent const &event) override;

    bool empty();
    void writeXML();
    void update(bool alert_LPE = false); // update display, but don't commit
    void clear(); // remove all nodes from manipulator

    SPObject const *item() const { return _path; }
    SPObject       *item()       { return _path; }

    void selectSubpaths();
    void invertSelectionInSubpaths();

    void insertNodeAtExtremum(ExtremumType extremum);
    void insertNodes();
    void insertNode(Geom::Point);
    void insertNode(NodeList::iterator first, double t, bool take_selection);
    void duplicateNodes();
    void copySelectedPath(Geom::PathBuilder *builder);
    void simplifyInvisible(double threshold);
    void weldNodes(NodeList::iterator preserve_pos = NodeList::iterator());
    void weldSegments();
    void breakNodes() { breakNodes(true); }
    void breakNodes(bool new_nodes);
    void deleteNodes(NodeDeleteMode mode);
    void deleteSegments();
    void reverseSubpaths(bool selected_only);
    void setSegmentType(SegmentType);

    void scaleHandle(Node *n, int which, int dir, bool pixel);
    void rotateHandle(Node *n, int which, int dir, bool pixel);

    void showOutline(bool show);
    void showHandles(bool show);
    void showPathDirection(bool show);
    void setLiveOutline(bool set);
    void setLiveObjects(bool set);
    void updateHandles();
    void updatePath();
    void setControlsTransform(Geom::Affine const &);
    void hideDragPoint();
    MultiPathManipulator &mpm() { return _multi_path_manipulator; }

    NodeList::iterator subdivideSegment(NodeList::iterator after, double t);
    NodeList::iterator extremeNode(NodeList::iterator origin, bool search_selected,
    bool search_unselected, bool closest);

    int _bsplineGetSteps() const;
    // this is necessary for Tab-selection in MultiPathManipulator
    SubpathList &subpathList() { return _subpaths; }

    static bool is_item_type(void *item);
    static bool canEditShape(SPObject const *item);

private:
    using Subpath = NodeList;
    using SubpathPtr = std::shared_ptr<NodeList>;

    void _createControlPointsFromGeometry();
    void _deleteSegments(bool delete_singles);

    void _recalculateIsBSpline();
    bool _isBSpline() const;
    double _bsplineHandlePosition(Handle *h, bool check_other = true);
    Geom::Point _bsplineHandleReposition(Handle *h, bool check_other = true);
    Geom::Point _bsplineHandleReposition(Handle *h, double pos);
    void _createGeometryFromControlPoints(bool alert_LPE = false);
    void _normalizeBsplineHandles(Node *n);
    unsigned _deleteStretch(NodeList::iterator first, NodeList::iterator last, NodeDeleteMode mode);
    std::string _createTypeString() const;
    void _updateOutline();
    //void _setOutline(Geom::PathVector const &);
    void _getGeometry();
    void _setGeometry();
    bool _shapeChanged() const;
    void _convertShape();
    void _restoreShapePreview();
    Glib::ustring _nodetypesKey();
    Inkscape::XML::Node *_getXMLNode();
    Geom::Affine _getTransform() const;

    void _selectionChangedM(std::vector<SelectableControlPoint *> pvec, bool selected);
    void _selectionChanged(SelectableControlPoint * p, bool selected);
    bool _nodeClicked(Node *, ButtonReleaseEvent const &, bool skip_auto);
    void _handleGrabbed();
    bool _handleClicked(Handle *, ButtonReleaseEvent const &);
    void _handleUngrabbed();

    void _externalChange(unsigned type);
    void _removeNodesFromSelection();
    void _commit(Inkscape::Util::Internal::ContextString annotation);
    void _commit(Inkscape::Util::Internal::ContextString annotation, gchar const *key);
    Geom::Coord _updateDragPoint(Geom::Point const &);
    void _updateOutlineOnZoomChange();
    double _getStrokeTolerance();
    Handle *_chooseHandle(Node *n, int which);

    SubpathList _subpaths;
    MultiPathManipulator &_multi_path_manipulator;
    SPObject *_path; ///< can be an SPPath or an Inkscape::LivePathEffect::Effect  !!!
    Geom::PathVector _spcurve; // in item coordinates
    std::optional<Geom::PathVector> _shape_geometry;
    std::string _shape_types;
    CanvasItemPtr<Inkscape::CanvasItemBpath> _outline;
    CurveDragPoint *_dragpoint; // an invisible control point hovering over curve
    PathManipulatorObserver *_observer;
    Geom::Affine _d2i_transform; ///< desktop-to-item transform
    Geom::Affine _i2d_transform; ///< item-to-desktop transform, inverse of _d2i_transform
    Geom::Affine _edit_transform; ///< additional transform to apply to editing controls
    bool _show_handles = true;
    bool _show_outline = false;
    bool _show_path_direction = false;
    bool _live_outline = true;
    bool _live_objects = true;
    bool _is_bspline = false;
    Glib::ustring _lpe_key;

    friend class PathManipulatorObserver;
    friend class CurveDragPoint;
    friend class Node;
    friend class Handle;
};

} // namespace UI
} // namespace Inkscape

#endif // INKSCAPE_UI_TOOL_PATH_MANIPULATOR_H

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
