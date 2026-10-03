// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_SELCUE_H
#define SEEN_SP_SELCUE_H

/*
 * Helper object for showing selected items
 *
 * Authors:
 *   bulia byak <bulia@users.sf.net>
 *   Carl Hetherington <inkscape@carlh.net>
 *
 * Copyright (C) 2004 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstddef>
#include <vector>
#include <memory>

#include <sigc++/sigc++.h>

#include "display/control/canvas-item-ptr.h"
#include "preferences.h"

class  SPDesktop;
class  SPItem;

namespace Inkscape {

class CanvasItem;

class Selection;

class SelCue
{
public:
    SelCue(SPDesktop *desktop);
    ~SelCue();

    enum Type {
        NONE,
        MARK,
        BBOX
    };

    void setBboxesVisible(bool visible);
    /// PROOF: hide the cue while the selection is being dragged, as Illustrator does. It would
    /// otherwise trail behind the moving objects; it reappears at the result on release.
    void setTransforming(bool transforming);
    /// PROOF: the select tool draws its own transform box around the selection, so a single
    /// object needs no cue of its own, and the boxes of several objects are thin and solid.
    void setTransformBox(bool transform_box);

private:
    class BoundingBoxPrefsObserver: public Preferences::Observer
    {
    public:
        BoundingBoxPrefsObserver(SelCue &sel_cue);

        void notify(Preferences::Entry const &val) override;

    private:
        SelCue &_sel_cue;
    };

    friend class Inkscape::SelCue::BoundingBoxPrefsObserver;

    void _updateItemBboxes();
    void _updateItemBboxes(Inkscape::Preferences *prefs);
    void _updateItemBboxes(int mode, int prefs_bbox);
    void _newItemBboxes();
    void _newItemLines();
    void _newTextBaselines();
    void _newItemCenters();
    bool _setItemBox(CanvasItem &canvas_item, SPItem &item, int prefs_bbox) const;
    void _boundingBoxPrefsChanged(int prefs_bbox);

    bool _bboxes_visible = true;
    bool _transforming = false;
    bool _transform_box = false;
    bool _visible() const { return _bboxes_visible && !_transforming; }
    SPDesktop *_desktop;
    Selection *_selection;
    sigc::connection _sel_changed_connection;
    sigc::connection _sel_modified_connection;
    std::vector<CanvasItemPtr<CanvasItem>> _item_bboxes;
    std::vector<CanvasItemPtr<CanvasItem>> _text_baselines;
    std::vector<CanvasItemPtr<CanvasItem>> _item_lines;
    std::vector<CanvasItemPtr<CanvasItem>> _item_centers; ///< PROOF: centres of shapes in the selection

    BoundingBoxPrefsObserver _bounding_box_prefs_observer;
};

}
  
#endif


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
