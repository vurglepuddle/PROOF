// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SELTRANS_H
#define SEEN_SELTRANS_H

/*
 * Helper object for transforming selected items
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Carl Hetherington <inkscape@carlh.net>
 *   Diederik van Lierop <mail@diedenrezi.nl>
 *
 * Copyright (C) 2006      Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 1999-2002 Lauris Kaplinski
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <array>
#include <map>
#include <string>
#include <vector>
#include <2geom/point.h>
#include <2geom/affine.h>
#include <2geom/rect.h>
#include <cstddef>
#include <sigc++/sigc++.h>
#include <glibmm/refptr.h>

#include "message-context.h"
#include "seltrans-handles.h"
#include "selcue.h"

#include "object/sp-item.h"
#include "ui/knot/knot.h"

class  SPDesktop;
class  SPRect;
struct SPCanvasItem;
struct SPSelTransHandle;

namespace Gdk {
class Cursor;
}

namespace Inkscape {

class CanvasItemCtrl;
class CanvasItemCurve;
class CanvasItemRect;

Geom::Scale calcScaleFactors(Geom::Point const &initial_point, Geom::Point const &new_point, Geom::Point const &origin, bool const skew = false);

namespace XML {
    class Node;
}

class SelTrans
{
public:
    SelTrans(SPDesktop *desktop);
    ~SelTrans();

    enum State {
        STATE_SCALE, //scale or stretch
        STATE_ROTATE, //rotate or skew
        STATE_ALIGN  //on canvas align
    };

    void increaseState();
    void resetState(State state = STATE_SCALE);
    void setCenter(Geom::Point const &p);
    void grab(Geom::Point const &p, double x, double y, bool show_handles, bool translating);
    void transform(Geom::Affine const &rel_affine, Geom::Point const &norm);
    void ungrab();
    void stamp(bool clone = false);
    bool moveTo(Geom::Point const &xy, unsigned int state);
    void commitAbsoluteAffine();
    void commitRelativeAffine();
    void align(guint state, SPSelTransHandle const &handle);
    int request(SPSelTransHandle const &handle, Geom::Point &pt, unsigned int state);
    int scaleRequest(Geom::Point &pt, unsigned int state);
    int stretchRequest(Geom::Point &pt, unsigned int state, bool is_horz);
    int skewRequest(Geom::Point &pt, unsigned int state, bool is_horz);
    int rotateRequest(Geom::Point &pt, unsigned int state);
    int centerRequest(Geom::Point &pt, unsigned int state);
    int originRequest(Geom::Point &pt, unsigned int state, bool around_center = false);

    // StKey transforms functionality
    enum class StickyTransform
    {
        None,
        Grab,
        Scale,
        Rotate
    };
private:
    StickyTransform _stkey = StickyTransform::None;
    bool _stkey_transformed = false;
    bool _stkey_paused = false;

public:
    void grab_stkey(Geom::Point const &p, StickyTransform type);
    void pause_stkey() { _stkey_paused = true; }
    bool is_stkey() const { return _stkey != StickyTransform::None; }
    bool request_stkey(Geom::Point const &p, int state);
    bool ungrab_stkey(bool cancel = false);

    int handleRequest(SPKnot *knot, Geom::Point *position, unsigned int state, SPSelTransHandle const &handle);
    void handleGrab(SPKnot *knot, unsigned int state, SPSelTransHandle const &handle);
    void handleClick(SPKnot *knot, unsigned int state, SPSelTransHandle const &handle);
    void handleNewEvent(Geom::Point *position, unsigned int state, SPSelTransHandle const &handle);

    enum Show
    {
        SHOW_CONTENT,
        SHOW_OUTLINE
    };

    void setShow(Show s) {
        _show = s;
    }
    bool isEmpty() {
        return _empty;
    }
    bool isGrabbed() {
        return _grabbed;
    }
    bool centerIsVisible() {
        return ( knots[0]->is_visible());
    }

    void getNextClosestPoint(bool reverse);
    SelCue &getSelCue() { return _selcue; }
    /// Hide or show the selection cue and the transform box together.
    void setBoxesHidden(bool hidden);
    /// The pointer moved, in desktop coordinates, with no button down: corner radius handles
    /// show while it is over the selection.
    void setPointer(Geom::Point const &p);

private:
    class BoundingBoxPrefsObserver: public Preferences::Observer
    {
    public:
        BoundingBoxPrefsObserver(SelTrans &sel_trans);

        void notify(Preferences::Entry const &val) override;

    private:
        SelTrans &_sel_trans;
    };

    friend class Inkscape::SelTrans::BoundingBoxPrefsObserver;
    void _clear_stamp();
    void _updateHandles();
    void _updateVolatileState();
    void _selChanged(Inkscape::Selection *selection);
    void _selModified(Inkscape::Selection *selection, unsigned int flags);
    void _boundingBoxPrefsChanged(int prefs_bbox);
    void _makeHandles();
    void _showHandles(SPSelTransType type);
    void _updateBox();
    void _setBoxHandleZone(int index, Geom::Point const &position);
    Glib::RefPtr<Gdk::Cursor> _boxCursor(std::string const &name);
    void _clickThrough(unsigned state);
    SPRect *_radiusRect() const;
    void _makeRadiusKnots();
    void _updateRadiusKnots();
    bool _radiusRequest(int corner, SPKnot *knot, Geom::Point *position);
    void _radiusGrab(int corner, SPKnot *knot);
    void _radiusUngrab();
    Geom::Point _getGeomHandlePos(Geom::Point const &visual_handle_pos);
    Geom::Point _calcAbsAffineDefault(Geom::Scale const default_scale);
    Geom::Point _calcAbsAffineGeom(Geom::Scale const geom_scale);
    void _keepClosestPointOnly(Geom::Point const &p);

    SPDesktop *_desktop;

    std::vector<SPItem *> _items;
    std::vector<SPObject const *> _objects_const;
    std::vector<Geom::Affine> _items_affines;
    std::vector<Geom::Point> _items_centers;

    std::vector<Inkscape::SnapCandidatePoint> _snap_points;
    std::vector<Inkscape::SnapCandidatePoint> _bbox_points;
    std::vector<Inkscape::SnapCandidatePoint> _all_snap_sources_sorted;
    std::vector<Inkscape::SnapCandidatePoint>::iterator _all_snap_sources_iter;
    Inkscape::SelCue _selcue;

    Inkscape::Selection *_selection;
    State _state;
    Show _show;

    bool _grabbed = false;
    bool _show_handles = true;
    bool _empty;
    bool _changed;

    SPItem::BBoxType _snap_bbox_type;

    Geom::OptRect _bbox;
    Geom::OptRect _stroked_bbox;
    Geom::OptRect _geometric_bbox;
    double _strokewidth;

    Geom::Affine _current_relative_affine;
    Geom::Affine _absolute_affine;
    Geom::Affine _relative_affine;
    /* According to Merriam - Webster's online dictionary
     * Affine: a transformation (as a translation, a rotation, or a uniform stretching) that carries straight
     * lines into straight lines and parallel lines into parallel lines but may alter distance between points
     * and angles between lines <affine geometry>
     */

    Geom::Point _opposite; ///< opposite point to where a scale is taking place
    Geom::Point _opposite_for_specpoints;
    Geom::Point _opposite_for_bboxpoints;
    Geom::Point _origin_for_specpoints;
    Geom::Point _origin_for_bboxpoints;

    double _handle_x;
    double _handle_y;

    std::optional<Geom::Point> _center;
    bool _center_is_set; ///< we've already set _center, no need to reread it from items

    SPKnot *knots[NUMHANDS];
    // PROOF transform box: a thin box and centre dot drawn around the selection.
    CanvasItemPtr<CanvasItemRect> _box;
    CanvasItemPtr<CanvasItemCtrl> _center_mark;
    bool _boxes_hidden = false;
    std::map<std::string, Glib::RefPtr<Gdk::Cursor>> _box_cursors;
    // PROOF: corner radius handles, inside each corner of a single selected rectangle.
    std::array<SPKnot *, 4> _radius_knots{};
    bool _radius_hover = false;
    int _radius_dragging = -1;    ///< The corner being dragged, or -1.
    double _radius_start = 0;     ///< Radius when the drag began, in the rectangle's units.
    double _radius_inset_start = 0;
    CanvasItemPtr<CanvasItemCtrl> _norm;
    CanvasItemPtr<CanvasItemCtrl> _grip;
    std::array<CanvasItemPtr<CanvasItemCurve>, 4> _l;
    std::vector<SPItem*> _stamp_cache;
    bool _stamped = false;
    Geom::Point _origin; ///< position of origin for transforms
    Geom::Point _point; ///< original position of the knot being used for the current transform
    Geom::Point _point_geom; ///< original position of the knot being used for the current transform
    Inkscape::MessageContext _message_context;
    sigc::connection _sel_changed_connection;
    sigc::connection _sel_modified_connection;
    BoundingBoxPrefsObserver _bounding_box_prefs_observer;
};

}

#endif // SEEN_SELTRANS_H


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
