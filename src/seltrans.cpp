// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Helper object for transforming selected items.
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Carl Hetherington <inkscape@carlh.net>
 *   Diederik van Lierop <mail@diedenrezi.nl>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 1999-2014 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstring>
#include <string>

#include <gdk/gdk.h>
#include <glibmm/i18n.h>
#include <giomm/application.h>

#include <2geom/transforms.h>

#include "seltrans.h"

#include "desktop-style.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "message-stack.h"
#include "mod360.h"
#include "pure-transform.h"
#include "selection.h"
#include "seltrans-handles.h"

#include "display/control/snap-indicator.h"
#include "display/control/canvas-item-ctrl.h"
#include "display/control/canvas-item-curve.h"
#include "display/control/canvas-item-enums.h"
#include "display/control/canvas-item-group.h"
#include "display/control/canvas-item-rect.h"
#include "live_effects/effect-enum.h"
#include "live_effects/effect.h"

#include "object/sp-item-transform.h"
#include "object/sp-namedview.h"
#include "object/sp-rect.h"
#include "object/sp-root.h"

#include "ui/cursor-utils.h"
#include "ui/icon-names.h"
#include "ui/modifiers.h"
#include "ui/knot/knot.h"
#include "ui/tools/select-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::DocumentUndo;

static void sp_sel_trans_handle_grab(SPKnot *knot, guint state, SPSelTransHandle const* data);
static void sp_sel_trans_handle_ungrab(SPKnot *knot, guint state, SPSelTransHandle const* data);
static void sp_sel_trans_handle_click(SPKnot *knot, guint state, SPSelTransHandle const* data);
static void sp_sel_trans_handle_new_event(SPKnot *knot, Geom::Point const &position, guint32 state, SPSelTransHandle const* data);
static gboolean sp_sel_trans_handle_request(SPKnot *knot, Geom::Point *p, guint state, SPSelTransHandle const *data);

// TODO Remove
#define SP_SELECT_CONTEXT(obj) (dynamic_cast<Inkscape::UI::Tools::SelectTool*>((Inkscape::UI::Tools::ToolBase*)obj))

static bool sp_sel_trans_handle_event(SPKnot *knot, Inkscape::CanvasEvent const &event, SPSelTransHandle const*)
{
    bool ret = false;

    inspect_event(event,
        [&] (Inkscape::KeyPressEvent const &event) {
            if (Inkscape::UI::Tools::get_latin_keyval(event) == GDK_KEY_space) {
                // Stamping mode: both mode (show content and outline) operation with knot.
                if (!knot->is_grabbed()) {
                    return;
                }
                auto desktop = knot->desktop;
                auto seltrans = SP_SELECT_CONTEXT(desktop->getTool())->_seltrans;
                // This stamp can't produce clones without requiring extra support of "undoing"
                // the cascaded transform from this knot's changes.
                seltrans->stamp();
                ret = true;
            }
        },
        [&] (Inkscape::CanvasEvent const &event) {}
    );

    return ret;
}

Inkscape::SelTrans::BoundingBoxPrefsObserver::BoundingBoxPrefsObserver(SelTrans &sel_trans) :
    Observer("/tools/bounding_box"),
    _sel_trans(sel_trans)
{
}

void Inkscape::SelTrans::BoundingBoxPrefsObserver::notify(Preferences::Entry const &val)
{
    _sel_trans._boundingBoxPrefsChanged(static_cast<int>(val.getBool()));
}

Inkscape::SelTrans::SelTrans(SPDesktop *desktop) :
    _desktop(desktop),
    _selcue(desktop),
    _state(STATE_SCALE),
    _show(SHOW_CONTENT),
    _bbox(),
    _stroked_bbox(),
    _message_context(*desktop->messageStack()),
    _bounding_box_prefs_observer(*this)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    int prefs_bbox = prefs->getBool("/tools/bounding_box");
    _snap_bbox_type = !prefs_bbox ?
        SPItem::VISUAL_BBOX : SPItem::GEOMETRIC_BBOX;

    g_return_if_fail(desktop != nullptr);

    _updateVolatileState();
    _current_relative_affine.setIdentity();

    _center_is_set = false; // reread _center from items, or set to bbox midpoint

    _box = make_canvasitem<CanvasItemRect>(desktop->getCanvasControls());
    _box->set_stroke(0x277fffff);
    _box->set_pickable(false);
    _box->set_visible(false);
    _center_mark = make_canvasitem<CanvasItemCtrl>(desktop->getCanvasControls(), CANVAS_ITEM_CTRL_TYPE_BOX_CENTER);
    _center_mark->set_pickable(false);
    _center_mark->set_visible(false);
    _selcue.setTransformBox(true);

    _makeHandles();
    _makeRadiusKnots();
    _updateHandles();

    _selection = desktop->getSelection();

    _norm = make_canvasitem<CanvasItemCtrl>(desktop->getCanvasControls(), Inkscape::CANVAS_ITEM_CTRL_TYPE_CENTER);
    _norm->set_visible(false);

    _grip = make_canvasitem<CanvasItemCtrl>(desktop->getCanvasControls(), Inkscape::CANVAS_ITEM_CTRL_TYPE_MARKER);
    _grip->set_visible(false);

    for (auto &i : _l) {
        i = make_canvasitem<CanvasItemCurve>(desktop->getCanvasControls());
        i->set_visible(false);
    }

    _sel_changed_connection = _selection->connectChanged(
        sigc::mem_fun(*this, &Inkscape::SelTrans::_selChanged)
        );

    _sel_modified_connection = _selection->connectModified(
        sigc::mem_fun(*this, &Inkscape::SelTrans::_selModified)
        );

    _all_snap_sources_iter = _all_snap_sources_sorted.end();

    prefs->addObserver(_bounding_box_prefs_observer);
}

Inkscape::SelTrans::~SelTrans()
{
    _sel_changed_connection.disconnect();
    _sel_modified_connection.disconnect();

    for (auto &knot : knots) {
        SPKnot::unref(knot);
        knot = nullptr;
    }
    for (auto &knot : _radius_knots) {
        SPKnot::unref(knot);
        knot = nullptr;
    }

    _norm.reset();
    _grip.reset();
    _box.reset();
    _center_mark.reset();
    for (auto &i : _l) {
        i.reset();
    }

    _clear_stamp();

    for (auto &_item : _items) {
        sp_object_unref(_item, nullptr);
    }

    _items.clear();
    _objects_const.clear();
    _items_affines.clear();
    _items_centers.clear();
}

void
Inkscape::SelTrans::_clear_stamp() {
    _stamped = false;
    for (auto old_obj :_stamp_cache) {
        auto oldLPEObj = cast<SPLPEItem>(old_obj);
        if (oldLPEObj) {
            sp_lpe_item_enable_path_effects(oldLPEObj, true);
        }
    }
    if(!_stamp_cache.empty()){
        _stamp_cache.clear();
    }
}

void Inkscape::SelTrans::resetState(Inkscape::SelTrans::State state)
{
    _state = state;
}

void Inkscape::SelTrans::increaseState()
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool show_align = prefs->getBool("/dialogs/align/oncanvas", false);
        
    if (_state == STATE_SCALE) {
        _state = STATE_ROTATE;
    } else if (_state == STATE_ROTATE && show_align) {
        _state = STATE_ALIGN;
    } else {
        _state = STATE_SCALE;
    }

    _center_is_set = true; // no need to reread center

    _updateHandles();
}

void Inkscape::SelTrans::setCenter(Geom::Point const &p)
{
    _center = p;
    _center_is_set = true;

    // Write the new center position into all selected items
    auto items= _desktop->getSelection()->items();
    for (auto it : items) {
        it->setCenter(p);
        // only set the value; updating repr and document_done will be done once, on ungrab
    }

    _updateHandles();
}

void Inkscape::SelTrans::grab(Geom::Point const &p, gdouble x, gdouble y, bool show_handles, bool translating)
{
    // While dragging a handle, we will either scale, skew, or rotate and the "translating" parameter will be false
    // When dragging the selected item itself however, we will translate the selection and that parameter will be true
    Inkscape::Selection *selection = _desktop->getSelection();
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    g_return_if_fail(!_grabbed);

    _grabbed = true;
    _show_handles = show_handles;
    _selcue.setTransforming(true);
    _updateVolatileState();
    _current_relative_affine.setIdentity();

    _changed = false;

    if (_empty) {
        return;
    }

    auto items= _desktop->getSelection()->items();
    for (auto item : items) {
        SPItem *it = static_cast<SPItem*>(sp_object_ref(item, nullptr));
        _items.push_back(it);
        _objects_const.push_back(it);
        _items_affines.push_back(it->i2dt_affine());
        _items_centers.push_back(it->getCenter()); // for content-dragging, we need to remember original centers
    }

    if (y != -1 && _desktop->yaxisdown()) {
        y = 1 - y;
    }

    _handle_x = x;
    _handle_y = y;

    // The selector tool should snap the bbox, special snappoints, and path nodes
    // (The special points are the handles, center, rotation axis, font baseline, ends of spiral, etc.)

    // First, determine the bounding box
    _bbox = selection->bounds(_snap_bbox_type);
    _stroked_bbox = selection->strokedBounds(); // Used for correctly scaling the strokewidth
    _geometric_bbox = selection->geometricBounds();

    _point = p;
    if (_geometric_bbox) {
        _point_geom = _geometric_bbox->min() + _geometric_bbox->dimensions() * Geom::Scale(x, y);
    } else {
        _point_geom = p;
    }

    // Next, get all points to consider for snapping
    SnapManager const &m = _desktop->getNamedView()->snap_manager;
    _snap_points.clear();
    if (m.someSnapperMightSnap(false)) { // Only search for snap sources when really needed, to avoid unnecessary delays
        _snap_points = selection->getSnapPoints(&m.snapprefs); // This might take some time!
    }
    if (_snap_points.size() > 200 && !(prefs->getBool("/options/snapclosestonly/value", false))) {
        /* Snapping a huge number of nodes will take way too long, so limit the number of snappable nodes
        A typical user would rarely ever try to snap such a large number of nodes anyway, because
        (s)he would hardly be able to discern which node would be snapping */
        std::cerr << "Warning: limit of 200 snap sources reached, some will be ignored" << std::endl;
        _snap_points.resize(200);
        // Unfortunately, by now we will have lost the font-baseline snappoints :-(
    }

    // Find bbox hulling all special points, which excludes stroke width. Here we need to include the
    // path nodes, for example because a rectangle which has been converted to a path doesn't have
    // any other special points
    Geom::OptRect snap_points_bbox = selection->bounds(SPItem::GEOMETRIC_BBOX);

    _bbox_points.clear();
    // Collect the bounding box's corners and midpoints for each selected item
    if (m.snapprefs.isTargetSnappable(SNAPTARGET_BBOX_CATEGORY, SNAPTARGET_ALIGNMENT_CATEGORY, SNAPTARGET_DISTRIBUTION_CATEGORY)) {
        bool c = m.snapprefs.isTargetSnappable(SNAPTARGET_BBOX_CORNER, SNAPTARGET_ALIGNMENT_CATEGORY, SNAPTARGET_DISTRIBUTION_CATEGORY);
        bool mp = m.snapprefs.isTargetSnappable(SNAPTARGET_BBOX_MIDPOINT, SNAPTARGET_ALIGNMENT_CATEGORY, SNAPTARGET_DISTRIBUTION_CATEGORY);
        bool emp = m.snapprefs.isTargetSnappable(SNAPTARGET_BBOX_EDGE_MIDPOINT);
        // Preferably we'd use the bbox of each selected item, but for example 50 items will produce at least 200 bbox points,
        // which might make Inkscape crawl(see the comment a few lines above). In that case we will use the bbox of the selection as a whole
        bool c1 = (_items.size() > 0) && (_items.size() < 50);
        bool c2 = prefs->getBool("/options/snapclosestonly/value", false);
        if (translating && (c1 || c2)) {
            // Get the bounding box points for each item in the selection
            for (auto & _item : _items) {
                Geom::OptRect b = _item->desktopBounds(_snap_bbox_type);
                getBBoxPoints(b, &_bbox_points, false, c, emp, mp);
            }
        } else {
            // Only get the bounding box points of the selection as a whole
            getBBoxPoints(selection->bounds(_snap_bbox_type), &_bbox_points, false, c, emp, mp);
        }
    }

    if (_bbox) {
        // There are two separate "opposites" (i.e. opposite w.r.t. the handle being dragged):
        //  - one for snapping the boundingbox, which can be either visual or geometric
        //  - one for snapping the special points
        // The "opposite" in case of a geometric boundingbox always coincides with the "opposite" for the special points
        // These distinct "opposites" are needed in the snapmanager to avoid bugs such as LP167905 (in which
        // a box is caught between two guides)
        _opposite_for_bboxpoints = _bbox->min() + _bbox->dimensions() * Geom::Scale(1-x, 1-y);
        if (snap_points_bbox) {
            _opposite_for_specpoints = (*snap_points_bbox).min() + (*snap_points_bbox).dimensions() * Geom::Scale(1-x, 1-y);
        } else {
            _opposite_for_specpoints = _opposite_for_bboxpoints;
        }
        _opposite = _opposite_for_bboxpoints;
    }

    // When snapping the node closest to the mouse pointer is absolutely preferred over the closest snap
    // (i.e. when weight == 1), then we will not even try to snap to other points and disregard those other points

    if (prefs->getBool("/options/snapclosestonly/value", false)) {
        _keepClosestPointOnly(p);
    }

    if ((x != -1) && (y != -1)) {
        _norm->set_visible(true);
        _grip->set_visible(true);
    }

    if (_show == SHOW_OUTLINE) {
        for (auto & i : _l)
            i->set_visible(true);
    }

    _updateHandles();
    g_return_if_fail(_stamp_cache.empty());
}

void Inkscape::SelTrans::transform(Geom::Affine const &rel_affine, Geom::Point const &norm)
{
    g_return_if_fail(_grabbed);
    g_return_if_fail(!_empty);

    Geom::Affine const affine( Geom::Translate(-norm) * rel_affine * Geom::Translate(norm) );

    if (_show == SHOW_CONTENT) {
        auto selection = _desktop->getSelection();
        // update the content
        for (unsigned i = 0; i < _items.size(); i++) {
            SPItem &item = *_items[i];
            if( is<SPRoot>(&item) ) {
                _desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Cannot transform an embedded SVG."));
                break;
            }

            SiblingState sibling_state = selection->getSiblingState(&item);

            /**
             * Need checks for each SiblingState
             * Outside of SIBLING_TEXT_SHAPE_INSIDE and SIBLING_TEXT_PATH,
             * the rest of them need testing
             * This just skips the transformation
             */
            if (sibling_state == SiblingState::SIBLING_TEXT_SHAPE_INSIDE || sibling_state == SiblingState::SIBLING_TEXT_PATH) {
                continue;
            }

            Geom::Affine const &prev_transform = _items_affines[i];
            item.set_i2d_affine(prev_transform * affine);
            auto lpeitem = cast<SPLPEItem>(item.parent);
            if (lpeitem && lpeitem->hasPathEffectRecursive()) {
                sp_lpe_item_update_patheffect(lpeitem, true, false);
            }
            // The new affine will only have been applied if the transformation is different from the previous one, see SPItem::set_item_transform
        }
    } else {
        if (_bbox) {
            Geom::Point p[4];
            /* update the outline */
            for (unsigned i = 0 ; i < 4 ; i++) {
                p[i] = _bbox->corner(i) * affine;
            }
            for (unsigned i = 0 ; i < 4 ; i++) {
                _l[i]->set_coords(p[i], p[(i+1)%4]);
            }
        }
    }

    _current_relative_affine = affine;
    _changed = true;
    _updateHandles();
}

void Inkscape::SelTrans::ungrab()
{
    g_return_if_fail(_grabbed);
    _grabbed = false;
    _show_handles = true;

    _desktop->getSnapIndicator()->remove_snapsource();

    Inkscape::Selection *selection = _desktop->getSelection();
    _updateVolatileState();

    for (auto & _item : _items) {
        sp_object_unref(_item, nullptr);
    }

    _norm->set_visible(false);
    _grip->set_visible(false);

    if (_show == SHOW_OUTLINE) {
        for (auto & i : _l)
            i->set_visible(false);
    }
    if (_stamped) {
        _clear_stamp();
    }

    _message_context.clear();

    if (!_empty && _changed) {
        if (!_current_relative_affine.isIdentity()) { // we can have a identity affine
            // when trying to stretch a perfectly vertical line in horizontal direction, which will not be allowed by the handles;

            selection->applyAffine(_current_relative_affine, (_show == SHOW_OUTLINE) ? true : false);
            if (_center) {
                *_center *= _current_relative_affine;
                _center_is_set = true;
            }

            // If dragging showed content live, sp_selection_apply_affine cannot change the centers
            // appropriately - it does not know the original positions of the centers (all objects already have
            // the new bboxes). So we need to reset the centers from our saved array.
            if (_show != SHOW_OUTLINE && !_current_relative_affine.isTranslation()) {
                for (unsigned i = 0; i < _items_centers.size(); i++) {
                    SPItem *currentItem = _items[i];
                    if (currentItem->isCenterSet()) { // only if it's already set
                        currentItem->setCenter (_items_centers[i] * _current_relative_affine);
                        currentItem->updateRepr();
                    }
                }
            }
            for (unsigned i = 0; i < _items_centers.size(); i++) {
                auto currentItem = cast<SPLPEItem>(_items[i]);
                if (currentItem) {
                    sp_lpe_item_update_patheffect(currentItem, true, true);
                }
            }
        } 
        _items.clear();
        _objects_const.clear();
        _items_affines.clear();
        _items_centers.clear();

        if (!_current_relative_affine.isIdentity()) { // we can have a identity affine
            // when trying to stretch a perfectly vertical line in horizontal direction, which will not be allowed
            // by the handles; this would be identified as a (zero) translation by isTranslation()
            if (_current_relative_affine.isTranslation()) {
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Move"), INKSCAPE_ICON("tool-pointer"));
            } else if (_current_relative_affine.withoutTranslation().isScale()) {
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Scale"), INKSCAPE_ICON("tool-pointer"));
            } else if (_current_relative_affine.withoutTranslation().isRotation()) {
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Rotate"), INKSCAPE_ICON("tool-pointer"));
            } else {
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Skew"), INKSCAPE_ICON("tool-pointer"));
            }
        } else {
            _updateHandles();
        }

    } else {
        if (_stamped) {
            _clear_stamp();
        }
        if (_center_is_set) {
            // we were dragging center; update reprs and commit undoable action
        	auto items= _desktop->getSelection()->items();
            for (auto item : items) {
                SPItem *it = item;
                it->updateRepr();
            }
            DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Set center"), INKSCAPE_ICON("tool-pointer"));
        }

        _items.clear();
        _objects_const.clear();
        _items_affines.clear();
        _items_centers.clear();
        _updateHandles();
    }

    _desktop->getSnapIndicator()->remove_snaptarget();
    _selcue.setTransforming(false);
}

/* fixme: This is really bad, as we compare positions for each stamp (Lauris) */
/* fixme: IMHO the best way to keep sort cache would be to implement timestamping at last */

void Inkscape::SelTrans::stamp(bool clone)
{
    Inkscape::Selection *selection = _desktop->getSelection();

    bool fixup = !_grabbed;
    if ( fixup && !_stamp_cache.empty() ) {
        // TODO - give a proper fix. Simple temporary work-around for the grab() issue
        _stamp_cache.clear();
    }

    /* stamping mode */
    if (!_empty) {
        _stamped = true;
    	std::vector<SPItem*> l;
        if (!_stamp_cache.empty()) {
            l = _stamp_cache;
        } else {
            /* Build cache */
            l = selection->items_vector();
            std::sort(l.begin(), l.end(), sp_object_compare_position_bool);
            _stamp_cache = l;
            // we disable LPE while stamping and reenable on ungrab with _stamped bool
            for (auto old_obj : l) {
                if (auto oldLPEObj = cast<SPLPEItem>(old_obj)) {
                    sp_lpe_item_enable_path_effects(oldLPEObj, false);
                }
            }
        }
        std::vector<SPObject *> copies;
        // special case on clones when dragging a clone without its original
        // we check if its satellite is selected. if it has a clone original
        // to allow perform the write statement on line:616
        bool lpewritetransforms = true;
        for (auto old_obj : l) {
            auto oldLPEObj = cast<SPLPEItem>(old_obj);
            if (oldLPEObj) {
                auto effect = oldLPEObj->getFirstPathEffectOfType(Inkscape::LivePathEffect::CLONE_ORIGINAL);
                if (effect) {
                    std::vector<SPObject *> satellites = effect->effect_get_satellites();
                    for (auto obj : satellites) {
                        if (!selection->includes(obj)) {
                            lpewritetransforms = false;
                        }
                    }
                }
                effect = oldLPEObj->getFirstPathEffectOfType(Inkscape::LivePathEffect::BEND_PATH);
                if (effect) {
                    if (!oldLPEObj->optimizeTransforms() && selection->includes(oldLPEObj)) {
                        lpewritetransforms = false;
                        std::vector<SPObject *> satellites = effect->effect_get_satellites();
                        for (auto obj : satellites) {
                            if (selection->includes(obj)) {
                                lpewritetransforms = true;
                            }
                        }
                    }
                }
            }
        }

        for(auto &original_item : l) {
            Inkscape::XML::Node *original_repr = original_item->getRepr();

            // remember parent
            Inkscape::XML::Node *parent = original_repr->parent();

            Inkscape::XML::Node *copy_repr = nullptr;

            if (clone) {
                copy_repr = parent->document()->createElement("svg:use");
                copy_repr->setAttribute("x", "0");
                copy_repr->setAttribute("y", "0");
                copy_repr->setAttribute("xlink:href", std::string("#") + original_item->getId());
                copy_repr->setAttribute("inkscape:transform-center-x", original_repr->attribute("inkscape:transform-center-x"));
                copy_repr->setAttribute("inkscape:transform-center-y", original_repr->attribute("inkscape:transform-center-y"));
            } else {
                copy_repr = original_repr->duplicate(parent->document());
            }

            // add the new repr to the parent
            parent->addChild(copy_repr, original_repr->prev());

            SPItem *copy_item = (SPItem *) _desktop->getDocument()->getObjectByRepr(copy_repr);
            Geom::Affine new_affine = Geom::identity();
            if (_show == SHOW_OUTLINE || clone) {
                Geom::Affine const i2d(original_item->i2dt_affine());
                Geom::Affine const i2dnew( i2d * _current_relative_affine );
                copy_item->set_i2d_affine(i2dnew);
                new_affine = copy_item->transform;
                if (clone) {
                    new_affine = original_item->transform.inverse() * new_affine;
                }
            } else {
                new_affine = original_item->transform;
            }
            if (original_item && copy_item && !clone) {
                original_item->setTmpSuccessor(copy_item);
            }
            auto newLPEObj = cast<SPLPEItem>(copy_item);
            if (newLPEObj) {
                // disable LPE bool on dowrite to prevent move of selection original satellite
                // when unselected (lpe performs a transform function that moves satellite and
                // on unselect, goes to the wrong place)
                if (newLPEObj->hasPathEffectOfType(Inkscape::LivePathEffect::BOOL_OP)) {
                    sp_lpe_item_enable_path_effects(newLPEObj,false);
                }
            }
            if (!newLPEObj || 
                !lpewritetransforms || 
                (!newLPEObj->hasPathEffectOfType(Inkscape::LivePathEffect::CLONE_ORIGINAL) &&
                 !newLPEObj->hasPathEffectOfType(Inkscape::LivePathEffect::BEND_PATH))) 
            {
                copy_item->doWriteTransform(new_affine);
                if ( copy_item->isCenterSet() && _center ) {
                    copy_item->setCenter(*_center * _current_relative_affine);
                }
            }
            Inkscape::GC::release(copy_repr);
            copies.push_back(copy_item);
        }
        for (auto new_obj : copies) {
            auto newLPEObj = cast<SPLPEItem>(new_obj);
            if (newLPEObj && !clone) {
                sp_lpe_item_enable_path_effects(newLPEObj,true);
                newLPEObj->forkPathEffectsIfNecessary(1, true, true);
                sp_lpe_item_update_patheffect(newLPEObj, false, true, true);
            }
        }
        for(auto original_item : l) {
            // unrefering tmp _successor (not needed anymore) used on fork to keep new satellite 
            // items forked along the LPEs
            if (original_item && !clone) {
                original_item->fixTmpSuccessors();
                original_item->unsetTmpSuccessor();
            }
        }
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Stamp"), INKSCAPE_ICON("tool-pointer"));
    }

    if ( fixup && _stamped ) {
        // TODO - give a proper fix. Simple temporary work-around for the grab() issue
        _clear_stamp();
    }
}

void Inkscape::SelTrans::_updateHandles()
{
    for (auto & knot : knots)
        knot->hide();

    if (!_center_is_set && !_empty) {
        _center = _desktop->getSelection()->center();
        _center_is_set = true;
    }

    if ( !_show_handles || _empty ) {
        _desktop->getSelection()->setAnchor(0.0, 0.0, false);
        _updateBox();
        _updateRadiusKnots();
        return;
    }

    if ( _state == STATE_SCALE ) {
        _showHandles(HANDLE_STRETCH);
        _showHandles(HANDLE_SCALE);
        // PROOF: rotate from just outside the corners at any time, with no separate mode.
        _showHandles(HANDLE_ROTATE);
    } else if(_state == STATE_ALIGN) {
       _showHandles(HANDLE_SIDE_ALIGN);
       _showHandles(HANDLE_CORNER_ALIGN);
       _showHandles(HANDLE_CENTER_ALIGN);
    } else {
        _showHandles(HANDLE_SKEW);
        _showHandles(HANDLE_ROTATE);
        _showHandles(HANDLE_CENTER);
    }
    _updateBox();
    _updateRadiusKnots();

    // Set anchor point, 0.0 is always set if nothing is selected (top/left).
    bool set = false;
    for (int i = 0; i < NUMHANDS; i++) {
        if (knots[i]->is_selected()) {
            double anchor_x, anchor_y = 0.0;
            if (hands[i].type == HANDLE_CENTER) {
                anchor_x = (_center->x() - _bbox->min()[Geom::X]) / _bbox->dimensions()[Geom::X];
                anchor_y = (_center->y() - _bbox->min()[Geom::Y]) / _bbox->dimensions()[Geom::Y];
            } else {
                anchor_x = hands[i].x;
                anchor_y = (hands[i].y - 0.5) * (-_desktop->yaxisdir()) + 0.5;
            }
            set = true;
            _desktop->getSelection()->setAnchor(anchor_x, anchor_y);
        }
    }
    if (!set)
        _desktop->getSelection()->setAnchor(0.0, 0.0, false);
}

void Inkscape::SelTrans::_updateVolatileState()
{
    Inkscape::Selection *selection = _desktop->getSelection();
    _empty = selection->isEmpty();

    if (_empty) {
        return;
    }

    //Update the bboxes
    _bbox = selection->bounds(_snap_bbox_type);
    _stroked_bbox = selection->strokedBounds();

    if (!_bbox) {
        _empty = true;
        return;
    }

    _strokewidth = stroke_average_width(selection->items_vector());
}

void Inkscape::SelTrans::_showHandles(SPSelTransType type)
{
    // shouldn't have nullary bbox, but knots
    g_assert(_bbox);

    auto const y_dir = _desktop->yaxisdir();

    for (int i = 0; i < NUMHANDS; i++) {
        if (hands[i].type != type)
            continue;

        // Position knots to scale the selection bbox
        Geom::Point const bpos(hands[i].x, (hands[i].y - 0.5) * (-y_dir) + 0.5);
        Geom::Point p(_bbox->min() + (_bbox->dimensions() * Geom::Scale(bpos)));
        knots[i]->moveto(p);
        _setBoxHandleZone(i, p);
        knots[i]->show();

        // This controls the center handle's position, because the default can
        // be moved and needs to be remembered.
        if( type == HANDLE_CENTER && _center )
            knots[i]->moveto(*_center);
    }
}

/**
 * PROOF: draw the transform box, a thin line around the selection with a dot at the centre of
 * rotation. Like the handles it is hidden while transforming and shown again at the result.
 */
void Inkscape::SelTrans::_updateBox()
{
    bool const visible = _show_handles && !_empty && _bbox && !_boxes_hidden;
    _box->set_visible(visible);
    // The draggable centre handle replaces the dot while it is shown.
    _center_mark->set_visible(visible && _center && _state != STATE_ROTATE);
    if (visible) {
        _box->set_rect(*_bbox);
        if (_center) {
            _center_mark->set_position(*_center);
        }
    }
}

void Inkscape::SelTrans::setBoxesHidden(bool hidden)
{
    _boxes_hidden = hidden;
    _selcue.setBboxesVisible(!hidden);
    _updateBox();
    _updateRadiusKnots();
}

Glib::RefPtr<Gdk::Cursor> Inkscape::SelTrans::_boxCursor(std::string const &name)
{
    auto &cursor = _box_cursors[name];
    if (!cursor) {
        if (name.ends_with(".svg")) {
            cursor = load_svg_cursor(*_desktop->getCanvas(), name);
        } else {
            cursor = Gdk::Cursor::create(name);
        }
    }
    return cursor;
}

/**
 * PROOF: pick zones and cursors of the transform box. Corners pick a little beyond their
 * squares, the invisible edge handles along the whole edge, and the invisible rotation handles
 * just outside each corner. Zones reach only a short way into the box, so the object itself can
 * still be dragged when it is small on screen; edges of a very thin box are not picked at all.
 */
void Inkscape::SelTrans::_setBoxHandleZone(int i, Geom::Point const &p)
{
    auto const &handle = hands[i];
    auto const mid = _bbox->midpoint();
    auto const dir = _desktop->d2w(p) - _desktop->d2w(mid); // On screen, y down.

    CanvasItemCtrl::PickZone zone;
    zone.a = zone.b = p;
    zone.inner = mid;
    std::string cursor;

    switch (handle.type) {
        case HANDLE_SCALE:
            zone.radius = 6;
            cursor = (dir.x() < 0) == (dir.y() < 0) ? "nwse-resize" : "nesw-resize";
            break;
        case HANDLE_STRETCH: {
            // The edge runs between the two corners either side of its midpoint.
            auto const along = handle.x == 0.5 ? Geom::Point(_bbox->width() / 2, 0) : Geom::Point(0, _bbox->height() / 2);
            zone.a = p - along;
            zone.b = p + along;
            zone.radius = 5;
            zone.min_room = 6;
            cursor = std::abs(dir.x()) > std::abs(dir.y()) ? "ew-resize" : "ns-resize";
            break;
        }
        case HANDLE_ROTATE:
            zone.inner.reset();
            zone.outward = p - mid;
            zone.reach = 20;
            zone.overlap = 8;
            cursor = std::string("proof-rotate-") + (dir.y() < 0 ? "n" : "s") + (dir.x() < 0 ? "w" : "e") + ".svg";
            break;
        default:
            return;
    }

    knots[i]->ctrl->set_pick_zone(zone);
    if (auto c = _boxCursor(cursor)) {
        knots[i]->setCursor(SP_KNOT_STATE_MOUSEOVER, c);
        knots[i]->setCursor(SP_KNOT_STATE_DRAGGING, c);
    }
}

namespace {

/// A corner of a rectangle in its own coordinates, and the direction into the rectangle from it.
struct RectCorner
{
    Geom::Point corner;
    Geom::Point inward;
};

RectCorner rect_corner(SPRect const &rect, int i)
{
    double const x = rect.x.computed, y = rect.y.computed, w = rect.width.computed, h = rect.height.computed;
    switch (i) {
        case 0: return {{x, y}, {1, 1}};
        case 1: return {{x + w, y}, {-1, 1}};
        case 2: return {{x + w, y + h}, {-1, -1}};
        default: return {{x, y + h}, {1, -1}};
    }
}

double rect_radius(SPRect const &rect)
{
    return rect.rx._set ? rect.rx.computed : rect.ry._set ? rect.ry.computed : 0;
}

/// How far into the corner a radius handle sits: at the radius, or a little in when it is small.
constexpr double RADIUS_HANDLE_MIN_INSET = 12; // screen pixels
/// Rectangles narrower than this on screen show no radius handles, leaving room to move them.
constexpr double RADIUS_HANDLES_MIN_SIZE = 48;

} // namespace

/**
 * PROOF: the rectangle whose corners can be rounded on canvas: a single selected rectangle
 * without path effects (the Corners effect has its own handles in the node tool).
 */
SPRect *Inkscape::SelTrans::_radiusRect() const
{
    auto rect = cast<SPRect>(_desktop->getSelection()->singleItem());
    if (!rect || rect->hasPathEffectRecursive()) {
        return nullptr;
    }
    return rect;
}

void Inkscape::SelTrans::_makeRadiusKnots()
{
    for (int i = 0; i < 4; i++) {
        auto knot = new SPKnot(_desktop, _("<b>Round the corners</b>: drag into the shape for a larger radius"),
                               CANVAS_ITEM_CTRL_TYPE_BOX_RADIUS, "SelTrans:radius");
        knot->updateCtrl();
        knot->request_signal.connect([this, i](SPKnot *knot, Geom::Point *position, unsigned) {
            return _radiusRequest(i, knot, position);
        });
        knot->grabbed_signal.connect([this, i](SPKnot *knot, unsigned) { _radiusGrab(i, knot); });
        knot->ungrabbed_signal.connect([this](SPKnot *, unsigned) { _radiusUngrab(); });
        knot->hide();
        _radius_knots[i] = knot;
    }
}

void Inkscape::SelTrans::setPointer(Geom::Point const &p)
{
    bool const hover = _bbox && _bbox->contains(p);
    if (hover != _radius_hover) {
        _radius_hover = hover;
        _updateRadiusKnots();
    }
}

/**
 * PROOF: corner radius handles, as in Figma: small circles inside the corners of a selected
 * rectangle, shown while the pointer is over it. They sit at the radius, or a fixed distance in
 * from a sharp corner, and round all four corners together.
 */
void Inkscape::SelTrans::_updateRadiusKnots()
{
    bool const dragging = _radius_dragging >= 0;
    auto rect = _radiusRect();
    bool show = rect && (_radius_hover || dragging) && (_show_handles || dragging) && !_empty &&
                _state == STATE_SCALE && !_boxes_hidden;

    Geom::Affine i2dt;
    double inset = 0;
    if (show) {
        i2dt = rect->i2dt_affine();
        double const scale = _desktop->current_zoom() * i2dt.descrim(); // Screen pixels per unit.
        double const size = std::min(rect->width.computed, rect->height.computed);
        show = scale > 0 && size * scale >= RADIUS_HANDLES_MIN_SIZE;
        if (show) {
            inset = std::min(std::max(rect_radius(*rect), RADIUS_HANDLE_MIN_INSET / scale), size / 2);
        }
    }

    for (int i = 0; i < 4; i++) {
        auto knot = _radius_knots[i];
        if (!show) {
            knot->hide();
        } else if (i != _radius_dragging) { // The dragged handle follows the pointer.
            auto const c = rect_corner(*rect, i);
            knot->moveto((c.corner + c.inward * inset) * i2dt);
            knot->show();
        }
    }
}

void Inkscape::SelTrans::_radiusGrab(int corner, SPKnot *knot)
{
    auto rect = _radiusRect();
    if (!rect) {
        return;
    }
    _radius_dragging = corner;
    _radius_start = rect_radius(*rect);
    auto const c = rect_corner(*rect, corner);
    _radius_inset_start = Geom::dot(knot->position() * rect->i2dt_affine().inverse() - c.corner, c.inward) / 2;
}

bool Inkscape::SelTrans::_radiusRequest(int corner, SPKnot *knot, Geom::Point *position)
{
    auto rect = _radiusRect();
    if (!rect || corner != _radius_dragging) {
        return true;
    }

    // Slide along the corner's diagonal, in the rectangle's own coordinates; the radius changes by
    // as much as the handle moves, so that it does not jump when a sharp corner is first dragged.
    auto const i2dt = rect->i2dt_affine();
    auto const c = rect_corner(*rect, corner);
    double const max = std::min(rect->width.computed, rect->height.computed) / 2;
    double const inset = std::clamp(Geom::dot(*position * i2dt.inverse() - c.corner, c.inward) / 2, 0.0, max);
    double const radius = std::clamp(_radius_start + inset - _radius_inset_start, 0.0, max);
    rect->setRx(radius > 0, radius);
    rect->setRy(radius > 0, radius);

    *position = (c.corner + c.inward * inset) * i2dt;
    knot->moveto(*position);

    auto const shown = Inkscape::Util::Quantity(rect->getVisibleRx(), "px");
    _message_context.setF(Inkscape::IMMEDIATE_MESSAGE, _("<b>Corner radius</b>: %s"),
                          shown.string(_desktop->getNamedView()->display_units).c_str());
    return true;
}

void Inkscape::SelTrans::_radiusUngrab()
{
    _radius_dragging = -1;
    _message_context.clear();
    if (auto rect = _radiusRect()) {
        rect->updateRepr();
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Round corners"), INKSCAPE_ICON("draw-rectangle"));
    }
    _updateRadiusKnots();
}

/**
 * PROOF: a click without a drag on an invisible zone acts as a click on the canvas there, so
 * that clicking just beside the selection still selects what is under the pointer, or deselects.
 */
void Inkscape::SelTrans::_clickThrough(unsigned state)
{
    using namespace Inkscape::Modifiers;
    auto selection = _desktop->getSelection();
    auto const w = _desktop->d2w(_desktop->point());
    bool const in_groups = Modifier::get(Type::SELECT_IN_GROUPS)->active(state);
    bool const add = Modifier::get(Type::SELECT_ADD_TO)->active(state);
    auto item = UI::Tools::sp_event_context_find_item(_desktop, w, false, in_groups);
    if (!item) {
        if (!add) {
            selection->clear();
        }
    } else if (add) {
        selection->toggle(item);
    } else if (!selection->includes(item)) {
        selection->set(item);
    }
}

void Inkscape::SelTrans::_makeHandles()
{
    for (int i = 0; i < NUMHANDS; i++) {
        using Inkscape::Modifiers::Type;
        using Inkscape::Modifiers::Modifier;

        auto confine_mod = Modifier::get(Type::TRANS_CONFINE)->get_label();
        auto center_mod = Modifier::get(Type::TRANS_OFF_CENTER)->get_label();
        auto increment_mod = Modifier::get(Type::TRANS_INCREMENT)->get_label();

        switch (hands[i].type) {
            case HANDLE_STRETCH:
            case HANDLE_SCALE:
            {
                auto tip = Glib::ustring::compose(_("<b>Scale</b> selection; with <b>%1</b> to scale uniformly; with <b>%2</b> to scale around rotation center"), confine_mod, center_mod);
                // PROOF: square corners only; each edge is an invisible zone along its length.
                auto const type = hands[i].type == HANDLE_SCALE ? CANVAS_ITEM_CTRL_TYPE_BOX_CORNER : CANVAS_ITEM_CTRL_TYPE_INVISIPOINT;
                knots[i] = new SPKnot(_desktop, tip.c_str(), type, "SelTrans");
                break;
            }
            case HANDLE_SKEW:
            {
                auto tip = Glib::ustring::compose(_("<b>Skew</b> selection; with <b>%1</b> to snap angle; with <b>%2</b> to skew around the opposite side"), increment_mod, center_mod);
                knots[i] = new SPKnot(_desktop, tip.c_str(), CANVAS_ITEM_CTRL_TYPE_ADJ_SKEW, "SelTrans");
                break;
            }
            case HANDLE_ROTATE:
            {
                auto tip = Glib::ustring::compose(_("<b>Rotate</b> selection; with <b>%1</b> to snap angle; with <b>%2</b> to rotate around the opposite corner"), confine_mod, center_mod);
                // PROOF: an invisible zone just outside the corner, with a rotate cursor.
                knots[i] = new SPKnot(_desktop, tip.c_str(), CANVAS_ITEM_CTRL_TYPE_INVISIPOINT, "SelTrans");
                break;
            }
            case HANDLE_CENTER:
            {
                auto tip = Glib::ustring::compose(_("<b>Center</b> of transformation: drag to reposition; scaling, rotation and skew with %1 also uses this center"), center_mod);
                knots[i] = new SPKnot(_desktop, tip.c_str(), CANVAS_ITEM_CTRL_TYPE_ADJ_CENTER, "SelTrans");
                break;
            }
            case HANDLE_SIDE_ALIGN:
                knots[i] = new SPKnot(_desktop, 
                    _("<b>Align</b> objects to the side clicked; <b>Shift</b> click to invert side; <b>Ctrl</b> to group whole selection."),
                    CANVAS_ITEM_CTRL_TYPE_ADJ_SALIGN, "SelTrans");
                break;
            case HANDLE_CORNER_ALIGN:
                knots[i] = new SPKnot(_desktop,
                    _("<b>Align</b> objects to the corner clicked; <b>Shift</b> click to invert side; <b>Ctrl</b> to group whole selection."),
                    CANVAS_ITEM_CTRL_TYPE_ADJ_CALIGN, "SelTrans");
                break;
            case HANDLE_CENTER_ALIGN:
                knots[i] = new SPKnot(_desktop,
                    _("<b>Align</b> objects to center; <b>Shift</b> click to center vertically instead of horizontally."),
                    CANVAS_ITEM_CTRL_TYPE_ADJ_MALIGN, "SelTrans");
                break;
            default:
                knots[i] = new SPKnot(_desktop, "", CANVAS_ITEM_CTRL_TYPE_ADJ_HANDLE, "SelTrans");
        }

        // PROOF: box handles sit centred on their point; their zones are set in _showHandles.
        bool const box_handle = hands[i].type == HANDLE_SCALE || hands[i].type == HANDLE_STRETCH || hands[i].type == HANDLE_ROTATE;
        knots[i]->setAnchor(box_handle ? SP_ANCHOR_CENTER : hands[i].anchor);
        knots[i]->updateCtrl();

        knots[i]->request_signal.connect(sigc::bind(sigc::ptr_fun(sp_sel_trans_handle_request), &hands[i]));
        knots[i]->moved_signal.connect(sigc::bind(sigc::ptr_fun(sp_sel_trans_handle_new_event), &hands[i]));
        knots[i]->grabbed_signal.connect(sigc::bind(sigc::ptr_fun(sp_sel_trans_handle_grab), &hands[i]));
        knots[i]->ungrabbed_signal.connect(sigc::bind(sigc::ptr_fun(sp_sel_trans_handle_ungrab), &hands[i]));
        knots[i]->click_signal.connect(sigc::bind(sigc::ptr_fun(sp_sel_trans_handle_click), &hands[i]));
        knots[i]->event_signal.connect(sigc::bind(sigc::ptr_fun(sp_sel_trans_handle_event), &hands[i]));
    }

    // Corners win over edges, and edges over the rotation zones around them.
    for (auto type : {HANDLE_ROTATE, HANDLE_STRETCH, HANDLE_SCALE}) {
        for (int i = 0; i < NUMHANDS; i++) {
            if (hands[i].type == type) {
                knots[i]->ctrl->raise_to_top();
            }
        }
    }
}

static void sp_sel_trans_handle_grab(SPKnot *knot, guint state, SPSelTransHandle const* data)
{
    SP_SELECT_CONTEXT(knot->desktop->getTool())->_seltrans->handleGrab(
        knot, state, *(SPSelTransHandle const *) data
        );
}

static void sp_sel_trans_handle_ungrab(SPKnot *knot, guint /*state*/, SPSelTransHandle const* /*data*/)
{
    SP_SELECT_CONTEXT(knot->desktop->getTool())->_seltrans->ungrab();
}

static void sp_sel_trans_handle_new_event(SPKnot *knot, Geom::Point const& position, guint state, SPSelTransHandle const *data)
{
    Geom::Point pos = position;

    if (!knot->is_grabbed()) {
        return;
    }

    SP_SELECT_CONTEXT(knot->desktop->getTool())->_seltrans->handleNewEvent(
        &pos, state, *(SPSelTransHandle const *) data
    );
}

static gboolean sp_sel_trans_handle_request(SPKnot *knot, Geom::Point *position, guint state, SPSelTransHandle const *data)
{
    return SP_SELECT_CONTEXT(knot->desktop->getTool())->_seltrans->handleRequest(
        knot, position, state, *(SPSelTransHandle const *) data
        );
}

static void sp_sel_trans_handle_click(SPKnot *knot, guint state, SPSelTransHandle const* data)
{
    SP_SELECT_CONTEXT(knot->desktop->getTool())->_seltrans->handleClick(
        knot, state, *(SPSelTransHandle const *) data
        );
}

void Inkscape::SelTrans::handleClick(SPKnot *knot, guint state, SPSelTransHandle const &handle)
{
    switch (handle.type) {
        case HANDLE_CENTER:
            if (state & GDK_SHIFT_MASK) {
                // Unset the  center position for all selected items
            	auto items = _desktop->getSelection()->items();
                for (auto it : items) {
                    it->unsetCenter();
                    it->updateRepr();
                    _center_is_set = false;  // center has changed
                    _updateHandles();
                }
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Reset center"), INKSCAPE_ICON("tool-pointer"));
            }
            // no break, continue.
        case HANDLE_SCALE:
            {
                bool was_selected = knot->is_selected();
                for (auto & child_knot : knots) {
                    child_knot->selectKnot(false);
                }
                if (!was_selected) {
                    knot->selectKnot(true);
                }
                _updateHandles();
            }
            break;
        case HANDLE_STRETCH:
        case HANDLE_ROTATE:
            _clickThrough(state);
            break;
        case HANDLE_SIDE_ALIGN:
        case HANDLE_CORNER_ALIGN:
        case HANDLE_CENTER_ALIGN:
            align(state, handle);
        default:
            break;
    }
}

void Inkscape::SelTrans::handleGrab(SPKnot *knot, guint /*state*/, SPSelTransHandle const &handle)
{
    grab(knot->position(), handle.x, handle.y, FALSE, FALSE);

    // Forcing handles visibility must be done after grab() to be effective
    switch (handle.type) {
        case HANDLE_CENTER:
            _grip->set_type(Inkscape::CANVAS_ITEM_CTRL_TYPE_CENTER);
            _norm->set_visible(false);
            _grip->set_visible(true);
            break;
        default:
            _grip->set_type(Inkscape::CANVAS_ITEM_CTRL_TYPE_MARKER);
            _norm->set_visible(true);
            _grip->set_visible(true);
            break;
    }
}


void Inkscape::SelTrans::handleNewEvent(Geom::Point *position, guint state, SPSelTransHandle const &handle)
{
    // in case items have been unhooked from the document, don't
    // try to continue processing events for them.
    for (auto & _item : _items) {
        if ( !_item->document ) {
            return;
        }
    }
    switch (handle.type) {
        case HANDLE_SCALE:
        case HANDLE_STRETCH:
            commitAbsoluteAffine();
            break;
        case HANDLE_SKEW:
        case HANDLE_ROTATE:
            commitRelativeAffine();
            break;
        case HANDLE_CENTER:
            setCenter(*position);
            break;
        case HANDLE_SIDE_ALIGN:
        case HANDLE_CORNER_ALIGN:
        case HANDLE_CENTER_ALIGN:
            break;
    }
}

int Inkscape::SelTrans::originRequest(Geom::Point &/*pt*/, unsigned int state, bool around_center)
{
    // Scaling is relative to the point opposite the handle, rotating and skewing to the center
    // set for the selection; the off-center modifier swaps the two. PROOF: this follows the
    // handle rather than the handle mode, since rotation is available while scaling.
    auto off_center = Modifiers::Modifier::get(Modifiers::Type::TRANS_OFF_CENTER)->active(state);
    if (!is_stkey() && off_center == around_center) {
        _origin = _opposite;
        _origin_for_bboxpoints = _opposite_for_bboxpoints;
        _origin_for_specpoints = _opposite_for_specpoints;
    } else if (_center) {
        _origin = *_center;
        _origin_for_bboxpoints = *_center;
        _origin_for_specpoints = *_center;
    }
    return true;
}

gboolean Inkscape::SelTrans::handleRequest(SPKnot *knot, Geom::Point *position, guint state, SPSelTransHandle const &handle)
{
    if (!knot->is_grabbed()) {
        return TRUE;
    }
    if (request(handle, *position, state)) {
        knot->setPosition(*position, state);
        _grip->set_position(*position);
        if (handle.type == HANDLE_CENTER) {
            _norm->set_position(*position);
        } else {
            _norm->set_position(_origin);
        }
    }

    return TRUE;
}


void Inkscape::SelTrans::_selChanged(Inkscape::Selection *selection)
{
    if (is_stkey()) {
        ungrab_stkey();
    }
    if (!_grabbed) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        // reread in case it changed on the fly:
        int prefs_bbox = prefs->getBool("/tools/bounding_box");
         _snap_bbox_type = !prefs_bbox ?
            SPItem::VISUAL_BBOX : SPItem::GEOMETRIC_BBOX;

        _updateVolatileState();
        _current_relative_affine.setIdentity();
        _center_is_set = false; // center(s) may have changed
        _updateHandles();
    }
}

void Inkscape::SelTrans::_selModified(Inkscape::Selection */*selection*/, guint /*flags*/)
{
    if (!_grabbed) {
        _updateVolatileState();
        _current_relative_affine.setIdentity();

        // reset internal flag
        _changed = false;

        _center_is_set = false;  // center(s) may have changed

        _updateHandles();
    }
}

void Inkscape::SelTrans::_boundingBoxPrefsChanged(int prefs_bbox)
{
    _snap_bbox_type = !prefs_bbox ?
        SPItem::VISUAL_BBOX : SPItem::GEOMETRIC_BBOX;

    _updateVolatileState();
    _updateHandles();
}

/*
 * handlers for handle move-request
 */

/** Returns -1 or 1 according to the sign of x.  Returns 1 for 0 and NaN. */
static double sign(double const x)
{
    return ( x < 0
             ? -1
             : 1 );
}

gboolean Inkscape::SelTrans::scaleRequest(Geom::Point &pt, guint state)
{
    originRequest(pt, state);

    // Calculate the scale factors, which can be either visual or geometric
    // depending on which type of bbox is currently being used (see preferences -> selector tool)
    Geom::Scale default_scale = calcScaleFactors(_point, pt, _origin);

    // Find the scale factors for the geometric bbox
    Geom::Point pt_geom = _getGeomHandlePos(pt);
    Geom::Scale geom_scale = calcScaleFactors(_point_geom, pt_geom, _origin_for_specpoints);

    _absolute_affine = Geom::identity(); //Initialize the scaler

    auto increments = Modifiers::Modifier::get(Modifiers::Type::TRANS_INCREMENT)->active(state);
    if (increments) { // scale by an integer multiplier/divider
        // We're scaling either the visual or the geometric bbox here (see the comment above)
        for ( unsigned int i = 0 ; i < 2 ; i++ ) {
            if (fabs(default_scale[i]) > 1) {
                default_scale[i] = round(default_scale[i]);
            } else if (default_scale[i] != 0) {
                default_scale[i] = 1/round(1/(MIN(default_scale[i], 10)));
            }
        }
        // Update the knot position
        pt = _calcAbsAffineDefault(default_scale);
        // When scaling by an integer, snapping is not needed
    } else {
        // In all other cases we should try to snap now
        Inkscape::PureScale  *bb, *sn;

        auto confine = Modifiers::Modifier::get(Modifiers::Type::TRANS_CONFINE)->active(state);
        if (confine || Inkscape::Preferences::get()->getBool("/tools/select/lock_aspect_ratio", false)) {
            // Scale is locked to a 1:1 aspect ratio, so that s[X] must be made to equal s[Y].
            //
            // The aspect-ratio must be locked before snapping
            if (fabs(default_scale[Geom::X]) > fabs(default_scale[Geom::Y])) {
                default_scale[Geom::X] = fabs(default_scale[Geom::Y]) * sign(default_scale[Geom::X]);
                geom_scale[Geom::X] = fabs(geom_scale[Geom::Y]) * sign(geom_scale[Geom::X]);
            } else {
                default_scale[Geom::Y] = fabs(default_scale[Geom::X]) * sign(default_scale[Geom::Y]);
                geom_scale[Geom::Y] = fabs(geom_scale[Geom::X]) * sign(geom_scale[Geom::Y]);
            }

            // Snap along a suitable constraint vector from the origin.

            bb = new Inkscape::PureScaleConstrained(default_scale, _origin_for_bboxpoints);
            sn = new Inkscape::PureScaleConstrained(geom_scale, _origin_for_specpoints);
        } else {
            /* Scale aspect ratio is unlocked */
            bb = new Inkscape::PureScale(default_scale, _origin_for_bboxpoints, false);
            sn = new Inkscape::PureScale(geom_scale, _origin_for_specpoints, false);
        }
        SnapManager &m = _desktop->getNamedView()->snap_manager;
        m.setup(_desktop, false, _objects_const);
        m.snapTransformed(_bbox_points, _point, (*bb));
        m.snapTransformed(_snap_points, _point, (*sn));
        m.unSetup();

        // These lines below are duplicated in stretchRequest
        //TODO: Eliminate this code duplication
        if (bb->best_snapped_point.getSnapped() || sn->best_snapped_point.getSnapped()) {
            if (bb->best_snapped_point.getSnapped() && !bb->best_snapped_point.isOtherSnapBetter(sn->best_snapped_point, false)) {
                // We snapped the bbox (which is either visual or geometric)
                _desktop->getSnapIndicator()->set_new_snaptarget(bb->best_snapped_point);
                default_scale = bb->getScaleSnapped();
                // Calculate the new transformation and update the handle position
                pt = _calcAbsAffineDefault(default_scale);
            } else if (sn->best_snapped_point.getSnapped()) {
                _desktop->getSnapIndicator()->set_new_snaptarget(sn->best_snapped_point);
                // We snapped the special points (e.g. nodes), which are not at the visual bbox
                // The handle location however (pt) might however be at the visual bbox, so we
                // will have to calculate pt taking the stroke width into account
                geom_scale = sn->getScaleSnapped();
                pt = _calcAbsAffineGeom(geom_scale);
            }
        } else {
            // We didn't snap at all! Don't update the handle position, just calculate the new transformation
            _calcAbsAffineDefault(default_scale);
            _desktop->getSnapIndicator()->remove_snaptarget();
        }

        delete bb;
        delete sn;
    }

    /* Status text */
    auto confine_mod = Modifiers::Modifier::get(Modifiers::Type::TRANS_CONFINE)->get_label();
    _message_context.setF(Inkscape::IMMEDIATE_MESSAGE,
                          _("<b>Scale</b>: %0.2f%% x %0.2f%%; with <b>%s</b> to lock ratio"),
                          100 * _absolute_affine[0], 100 * _absolute_affine[3], confine_mod.c_str());

    return TRUE;
}

gboolean Inkscape::SelTrans::stretchRequest(Geom::Point &pt, guint state, bool is_horz)
{
    Geom::Dim2 axis = is_horz ? Geom::X : Geom::Y;
    Geom::Dim2 perp = is_horz ? Geom::Y : Geom::X;

    originRequest(pt, state);

    // Calculate the scale factors, which can be either visual or geometric
    // depending on which type of bbox is currently being used (see preferences -> selector tool)
    Geom::Scale default_scale = calcScaleFactors(_point, pt, _origin);
    default_scale[perp] = 1;

    // Find the scale factors for the geometric bbox
    Geom::Point pt_geom = _getGeomHandlePos(pt);
    Geom::Scale geom_scale = calcScaleFactors(_point_geom, pt_geom, _origin_for_specpoints);
    geom_scale[perp] = 1;

    _absolute_affine = Geom::identity(); //Initialize the scaler

    auto increments = Modifiers::Modifier::get(Modifiers::Type::TRANS_INCREMENT)->active(state);
    if (increments) { // stretch by an integer multiplier/divider
        if (fabs(default_scale[axis]) > 1) {
            default_scale[axis] = round(default_scale[axis]);
        } else if (default_scale[axis] != 0) {
            default_scale[axis] = 1/round(1/(MIN(default_scale[axis], 10)));
        }
        // Calculate the new transformation and update the handle position
        pt = _calcAbsAffineDefault(default_scale);
        // When stretching by an integer, snapping is not needed
    } else {
        // In all other cases we should try to snap now

        SnapManager &m = _desktop->getNamedView()->snap_manager;
        m.setup(_desktop, false, _objects_const);

        auto confine = Modifiers::Modifier::get(Modifiers::Type::TRANS_CONFINE)->active(state);
        Inkscape::PureStretchConstrained bb = Inkscape::PureStretchConstrained(Geom::Coord(default_scale[axis]), _origin_for_bboxpoints, Geom::Dim2(axis), confine);
        Inkscape::PureStretchConstrained sn = Inkscape::PureStretchConstrained(Geom::Coord(geom_scale[axis]), _origin_for_specpoints, Geom::Dim2(axis), confine);

        m.snapTransformed(_bbox_points, _point, bb);
        m.snapTransformed(_snap_points, _point, sn);
        m.unSetup();

        if (bb.best_snapped_point.getSnapped()) {
            // We snapped the bbox (which is either visual or geometric)
            default_scale[axis] = bb.getMagnitude();
        }

        if (sn.best_snapped_point.getSnapped()) {
            geom_scale[axis] = sn.getMagnitude();
        }

        if (confine) {
            // on scale_confine, apply symmetrical scaling instead of stretching
            // Preserve aspect ratio, but never flip in the dimension not being edited (by using fabs())
            default_scale[perp] = fabs(default_scale[axis]);
            geom_scale[perp] = fabs(geom_scale[axis]);
        }

        // These lines below are duplicated in scaleRequest
        if (bb.best_snapped_point.getSnapped() || sn.best_snapped_point.getSnapped()) {
            if (bb.best_snapped_point.getSnapped() && !bb.best_snapped_point.isOtherSnapBetter(sn.best_snapped_point, false)) {
                // We snapped the bbox (which is either visual or geometric)
                _desktop->getSnapIndicator()->set_new_snaptarget(bb.best_snapped_point);
                default_scale = bb.getStretchSnapped();
                // Calculate the new transformation and update the handle position
                pt = _calcAbsAffineDefault(default_scale);
            } else if (sn.best_snapped_point.getSnapped()) {
                _desktop->getSnapIndicator()->set_new_snaptarget(sn.best_snapped_point);
                // We snapped the special points (e.g. nodes), which are not at the visual bbox
                // The handle location however (pt) might however be at the visual bbox, so we
                // will have to calculate pt taking the stroke width into account
                geom_scale = sn.getStretchSnapped();
                pt = _calcAbsAffineGeom(geom_scale);
            }
        } else {
            // We didn't snap at all! Don't update the handle position, just calculate the new transformation
            _calcAbsAffineDefault(default_scale);
            _desktop->getSnapIndicator()->remove_snaptarget();
        }
    }

    // status text
    auto confine_mod = Modifiers::Modifier::get(Modifiers::Type::TRANS_CONFINE)->get_label();
    _message_context.setF(Inkscape::IMMEDIATE_MESSAGE,
                          _("<b>Scale</b>: %0.2f%% x %0.2f%%; with <b>%s</b> to lock ratio"),
                          100 * _absolute_affine[0], 100 * _absolute_affine[3], confine_mod.c_str());

    return TRUE;
}

gboolean Inkscape::SelTrans::request(SPSelTransHandle const &handle, Geom::Point &pt, guint state)
{
    // These _should_ be in the handstype somewhere instead
    switch (handle.type) {
        case HANDLE_SCALE:
            return scaleRequest(pt, state);
        case HANDLE_STRETCH:
            return stretchRequest(pt, state, handle.anchor == SP_ANCHOR_W || handle.anchor == SP_ANCHOR_E);
        case HANDLE_SKEW:
            return skewRequest(pt, state, handle.anchor == SP_ANCHOR_W || handle.anchor == SP_ANCHOR_E);
        case HANDLE_ROTATE:
            return rotateRequest(pt, state);
        case HANDLE_CENTER:
            return centerRequest(pt, state);
        case HANDLE_SIDE_ALIGN:
        case HANDLE_CORNER_ALIGN:
        case HANDLE_CENTER_ALIGN:
            break; // Do nothing, no dragging
    }
    return FALSE;
}

gboolean Inkscape::SelTrans::skewRequest(Geom::Point &pt, guint state, bool is_horz)
{
    /* When skewing (or rotating):
     * 1) the stroke width will not change. This makes life much easier because we don't have to
     *    account for that (like for scaling or stretching). As a consequence, all points will
     *    have the same origin for the transformation and for the snapping.
     * 2) When holding shift, the transformation will be relative to the point opposite of
     *    the handle; otherwise it will be relative to the center as set for the selection
     */

    Geom::Dim2 dim_a = is_horz ? Geom::X : Geom::Y;
    Geom::Dim2 dim_b = is_horz ? Geom::Y : Geom::X;

    originRequest(pt, state, true);

    // _point and _origin are noisy, ranging from 1 to 1e-9 or even smaller; this is due to the
    // limited SVG output precision, which can be arbitrarily set in the preferences
    Geom::Point const initial_delta = _point - _origin;

    // The handle and the origin shouldn't be too close to each other; let's check for that!
    // Due to the limited resolution though (see above), we'd better use a relative error here
    if (_bbox) {
        Geom::Coord d = (*_bbox).dimensions()[dim_a];
        if (fabs(initial_delta[dim_a]/d) < 1e-4) {
            return false;
        }
    }

    // Calculate the scale factors, which can be either visual or geometric
    // depending on which type of bbox is currently being used (see preferences -> selector tool)
    Geom::Scale scale = calcScaleFactors(_point, pt, _origin, false);
    Geom::Scale skew = calcScaleFactors(_point, pt, _origin, true);
    scale[dim_b] = 1;
    skew[dim_b] = 1;

    if (fabs(scale[dim_a]) < 1) {
        // Prevent shrinking of the selected object, while allowing mirroring
        scale[dim_a] = sign(scale[dim_a]);
    } else {
        // Allow expanding of the selected object by integer multiples
        scale[dim_a] = floor(scale[dim_a] + 0.5);
    }

    double radians = atan(skew[dim_a] / scale[dim_a]);

    auto increments = Modifiers::Modifier::get(Modifiers::Type::TRANS_INCREMENT)->active(state);
    if (increments) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        // Snap to defined angle increments
        double snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);
        double sections = floor(radians * snaps / M_PI + .5);
        if (fabs(sections) >= snaps / 2) {
            sections = sign(sections) * (snaps / 2 - 1);
        }
        radians = (M_PI / snaps) * sections;
        skew[dim_a] = tan(radians) * scale[dim_a];
    } else {
        // Snap to objects, grids, guides

        SnapManager &m = _desktop->getNamedView()->snap_manager;
        m.setup(_desktop, false, _objects_const);

        // When skewing, we cannot snap the corners of the bounding box, see the comment in PureSkewConstrained for details
        Inkscape::PureSkewConstrained sn = Inkscape::PureSkewConstrained(skew[dim_a], scale[dim_a], _origin, Geom::Dim2(dim_b));
        m.snapTransformed(_snap_points, _point, sn);

        if (sn.best_snapped_point.getSnapped()) {
            // We snapped something, so change the skew to reflect it
            skew[dim_a] = sn.getSkewSnapped();
             _desktop->getSnapIndicator()->set_new_snaptarget(sn.best_snapped_point);
        } else {
            _desktop->getSnapIndicator()->remove_snaptarget();
        }

        m.unSetup();
    }

    // Update the handle position
    pt[dim_b] = initial_delta[dim_a] * skew[dim_a] + _point[dim_b];
    pt[dim_a] = initial_delta[dim_a] * scale[dim_a] + _origin[dim_a];

    // Calculate the relative affine
    _relative_affine = Geom::identity();
    _relative_affine[2*dim_a + dim_a] = (pt[dim_a] - _origin[dim_a]) / initial_delta[dim_a];
    _relative_affine[2*dim_a + (dim_b)] = (pt[dim_b] - _point[dim_b]) / initial_delta[dim_a];
    _relative_affine[2*(dim_b) + (dim_a)] = 0;
    _relative_affine[2*(dim_b) + (dim_b)] = 1;

    for (int i = 0; i < 2; i++) {
        if (fabs(_relative_affine[3*i]) < 1e-15) {
            _relative_affine[3*i] = 1e-15;
        }
    }

    // Update the status text
    auto increment_mod = Modifiers::Modifier::get(Modifiers::Type::TRANS_INCREMENT)->get_label();
    double degrees = mod360symm(Geom::deg_from_rad(radians));
    _message_context.setF(Inkscape::IMMEDIATE_MESSAGE,
                          // TRANSLATORS: don't modify the first ";"
                          // (it will NOT be displayed as ";" - only the second one will be)
                          _("<b>Skew</b>: %0.2f&#176;; with <b>%s</b> to snap angle"),
                          degrees, increment_mod.c_str());

    return TRUE;
}

gboolean Inkscape::SelTrans::rotateRequest(Geom::Point &pt, guint state)
{
    /* When rotating (or skewing):
     * 1) the stroke width will not change. This makes life much easier because we don't have to
     *    account for that (like for scaling or stretching). As a consequence, all points will
     *    have the same origin for the transformation and for the snapping.
     * 2) When holding shift, the transformation will be relative to the point opposite of
     *    the handle; otherwise it will be relative to the center as set for the selection
     */

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);

    originRequest(pt, state, true);

    // rotate affine in rotate
    Geom::Point const d1 = _point - _origin;
    Geom::Point const d2 = pt     - _origin;

    Geom::Coord const h1 = Geom::L2(d1); // initial radius
    if (h1 < 1e-15) return FALSE;
    Geom::Point q1 = d1 / h1; // normalized initial vector to handle
    Geom::Coord const h2 = Geom::L2(d2); // new radius
    if (fabs(h2) < 1e-15) return FALSE;
    Geom::Point q2 = d2 / h2; // normalized new vector to handle

    Geom::Rotate r1(q1);
    Geom::Rotate r2(q2);

    double radians = atan2(Geom::dot(Geom::rot90(d1), d2), Geom::dot(d1, d2));;
    auto increments = Modifiers::Modifier::get(Modifiers::Type::TRANS_INCREMENT)->active(state);
    auto confine = Modifiers::Modifier::get(Modifiers::Type::TRANS_CONFINE)->active(state);
    // Either key will now snap the rotation to specific points
    if (increments || confine) {
        // Snap to defined angle increments
        double cos_t = Geom::dot(q1, q2);
        double sin_t = Geom::dot(Geom::rot90(q1), q2);
        radians = atan2(sin_t, cos_t);
        radians = ( M_PI / snaps ) * floor( radians * snaps / M_PI + .5 );
        r1 = Geom::Rotate(0); //q1 = Geom::Point(1, 0);
        r2 = Geom::Rotate(radians); //q2 = Geom::Point(cos(radians), sin(radians));
    } else {
        SnapManager &m = _desktop->getNamedView()->snap_manager;
        m.setup(_desktop, false, _objects_const);
        // When rotating, we cannot snap the corners of the bounding box, see the comment in "constrainedSnapRotate" for details
        Inkscape::PureRotateConstrained sn = Inkscape::PureRotateConstrained(radians, _origin);
        m.snapTransformed(_snap_points, _point, sn);
        m.unSetup();

        if (sn.best_snapped_point.getSnapped()) {
            _desktop->getSnapIndicator()->set_new_snaptarget(sn.best_snapped_point);
            // We snapped something, so change the rotation to reflect it
            radians = sn.getAngleSnapped();
            r1 = Geom::Rotate(0);
            r2 = Geom::Rotate(radians);
        } else {
            _desktop->getSnapIndicator()->remove_snaptarget();
        }

    }


    // Calculate the relative affine
    _relative_affine = r2 * r1.inverse();

    // Update the handle position
    pt = _point * Geom::Translate(-_origin) * _relative_affine * Geom::Translate(_origin);

    // Update the status text
    auto increment_mod = Modifiers::Modifier::get(Modifiers::Type::TRANS_INCREMENT)->get_label();
    double degrees = mod360symm(Geom::deg_from_rad(radians));
    _message_context.setF(Inkscape::IMMEDIATE_MESSAGE,
                          // TRANSLATORS: don't modify the first ";"
                          // (it will NOT be displayed as ";" - only the second one will be)
                          _("<b>Rotate</b>: %0.2f&#176;; with <b>%s</b> to snap angle"), degrees, increment_mod.c_str());

    return TRUE;
}

// Move the item's transformation center
gboolean Inkscape::SelTrans::centerRequest(Geom::Point &pt, guint state)
{
    // When dragging the transformation center while multiple items have been selected, then those
    // items will share a single center. While dragging that single center, it should never snap to the
    // centers of any of the selected objects. Therefore we will have to pass the list of selected items
    // to the snapper, to avoid self-snapping of the rotation center
    auto items = _selection->items_vector();
    SnapManager &m = _desktop->getNamedView()->snap_manager;
    m.setup(_desktop);
    m.setRotationCenterSource(items);

    auto no_snap = Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING)->active(state);
    auto confine = Modifiers::Modifier::get(Modifiers::Type::MOVE_CONFINE)->active(state);
    if (confine) {
        std::vector<Inkscape::Snapper::SnapConstraint> constraints;
        constraints.emplace_back(_point, Geom::Point(1, 0));
        constraints.emplace_back(_point, Geom::Point(0, 1));
        Inkscape::SnappedPoint sp = m.multipleConstrainedSnaps(Inkscape::SnapCandidatePoint(pt, Inkscape::SNAPSOURCE_ROTATION_CENTER), constraints, no_snap);
        pt = sp.getPoint();
    }
    else if (!no_snap) {
        m.freeSnapReturnByRef(pt, Inkscape::SNAPSOURCE_ROTATION_CENTER);
    }

    m.unSetup();

    // status text
    Inkscape::Util::Quantity x_q = Inkscape::Util::Quantity(pt[Geom::X], "px");
    Inkscape::Util::Quantity y_q = Inkscape::Util::Quantity(pt[Geom::Y], "px");
    Glib::ustring xs(x_q.string(_desktop->getNamedView()->display_units));
    Glib::ustring ys(y_q.string(_desktop->getNamedView()->display_units));
    _message_context.setF(Inkscape::NORMAL_MESSAGE, _("Move <b>center</b> to %s, %s"),
            xs.c_str(), ys.c_str());
    return TRUE;
}

void Inkscape::SelTrans::align(guint state, SPSelTransHandle const &handle)
{
    Glib::ustring argument;
    int index = handle.control + ALIGN_OFFSET + ((state & GDK_SHIFT_MASK) ? ALIGN_SHIFT_OFFSET : 0);
    if (index < 0 || index >= AlignArguments.size()) {
        std::cerr << "Inkscape::Seltrans::align: index out of bounds! " << index << std::endl;
        index = 0;
    }

    auto variant = Glib::Variant<Glib::ustring>::create(AlignArguments[index]);
    auto app = Gio::Application::get_default();
    app->activate_action("object-align", variant);
}

void Inkscape::SelTrans::commitAbsoluteAffine()
{
    transform(_absolute_affine, Geom::Point(0, 0));
}

void Inkscape::SelTrans::commitRelativeAffine()
{
    transform(_relative_affine, _origin);
}

bool Inkscape::SelTrans::moveTo(Geom::Point const &xy, guint state)
{
    SnapManager &m = _desktop->getNamedView()->snap_manager;

    /* The amount that we've moved by during this drag */
    Geom::Point dxy = xy - _point;

    auto increments = Modifiers::Modifier::get(Modifiers::Type::MOVE_INCREMENT)->active(state);
    auto no_snap = Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING)->active(state);
    auto confine = Modifiers::Modifier::get(Modifiers::Type::MOVE_CONFINE)->active(state);
    increments = increments;

    if (confine) {
        if (fabs(dxy[Geom::X]) > fabs(dxy[Geom::Y])) {
            dxy[Geom::Y] = 0;
        } else {
            dxy[Geom::X] = 0;
        }
    }

    if (increments) {// Alt pressed means: move only by integer multiples of the grid spacing
        m.setup(_desktop, true, _objects_const);
        dxy = m.multipleOfGridPitch(dxy, _point);
        m.unSetup();
    } else if (!no_snap) {
        /* We're snapping to things, possibly with a constraint to horizontal or
        ** vertical movement.  Obtain a list of possible translations and then
        ** pick the smallest.
        */

        m.setup(_desktop, false, _objects_const);

        /* This will be our list of possible translations */
        std::list<Inkscape::SnappedPoint> s;

        Inkscape::PureTranslate *bb, *sn;

        if (confine) { // constrained movement with snapping

            /* Snap to things, and also constrain to horizontal or vertical movement */

            Geom::Dim2 dim = fabs(dxy[Geom::X]) > fabs(dxy[Geom::Y]) ? Geom::X : Geom::Y;
            // When doing a constrained translation, all points will move in the same direction, i.e.
            // either horizontally or vertically. Therefore we only have to specify the direction of
            // the constraint-line once. The constraint lines are parallel, but might not be colinear.
            // Therefore we will have to set the point through which the constraint-line runs
            // individually for each point to be snapped; this will be handled however by snapTransformed()
            bb = new Inkscape::PureTranslateConstrained(dxy[dim], dim);
            sn = new Inkscape::PureTranslateConstrained(dxy[dim], dim);
        } else {
            /* Snap to things with no constraint */
            bb = new Inkscape::PureTranslate(dxy);
            sn = new Inkscape::PureTranslate(dxy);
        }
        // Let's leave this timer code here for a while. I'll probably need it in the near future (Diederik van Lierop)
        /* GTimeVal starttime;
        GTimeVal endtime;
        g_get_current_time(&starttime); */

        m.snapTransformed(_bbox_points, _point, (*bb));
        m.snapTransformed(_snap_points, _point, (*sn));
        m.unSetup();

        /*g_get_current_time(&endtime);
        double elapsed = ((((double)endtime.tv_sec - starttime.tv_sec) * G_USEC_PER_SEC + (endtime.tv_usec - starttime.tv_usec))) / 1000.0;
        std::cout << "Time spent snapping: " << elapsed << std::endl; */

        /* Pick one */
        Inkscape::SnappedPoint best_snapped_point;

        bool sn_is_best = sn->best_snapped_point.getSnapped();
        bool bb_is_best = bb->best_snapped_point.getSnapped();

        if (bb_is_best && sn_is_best) {
            sn_is_best = bb->best_snapped_point.isOtherSnapBetter(sn->best_snapped_point, true);
            bb_is_best = !sn_is_best;
        }

        if (sn_is_best) {
            best_snapped_point = sn->best_snapped_point;
            dxy = sn->getTranslationSnapped();
        } else if (bb_is_best) {
            best_snapped_point = bb->best_snapped_point;
            dxy = bb->getTranslationSnapped();
        }

        if (best_snapped_point.getSnapped()) {
            _desktop->getSnapIndicator()->set_new_snaptarget(best_snapped_point);
        } else {
            // We didn't snap, so remove any previous snap indicator
            _desktop->getSnapIndicator()->remove_snaptarget();
            if (confine) {
                // If we didn't snap, then we should still constrain horizontally or vertically
                // (When we did snap, then this constraint has already been enforced by
                // calling constrainedSnapTranslate() above)
                if (fabs(dxy[Geom::X]) > fabs(dxy[Geom::Y])) {
                    dxy[Geom::Y] = 0;
                } else {
                    dxy[Geom::X] = 0;
                }
            }
        }
        delete bb;
        delete sn;
    }

    Geom::Affine const move((Geom::Translate(dxy)));
    Geom::Point const norm(0, 0);
    transform(move, norm);

    // status text
    auto confine_mod = Modifiers::Modifier::get(Modifiers::Type::MOVE_CONFINE)->get_label();
    auto no_snap_mod = Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING)->get_label();
    Inkscape::Util::Quantity x_q = Inkscape::Util::Quantity(dxy[Geom::X], "px");
    Inkscape::Util::Quantity y_q = Inkscape::Util::Quantity(dxy[Geom::Y], "px");
    Glib::ustring xs(x_q.string(_desktop->getNamedView()->display_units));
    Glib::ustring ys(y_q.string(_desktop->getNamedView()->display_units));
    _message_context.setF(Inkscape::NORMAL_MESSAGE,
            _("<b>Move</b> by %s, %s; with <b>%s</b> to restrict to horizontal/vertical; with <b>%s</b> to disable snapping"),
            xs.c_str(), ys.c_str(), confine_mod.c_str(), no_snap_mod.c_str());

    return move != Geom::identity();
}

// Given a location of a handle at the visual bounding box, find the corresponding location at the
// geometrical bounding box
Geom::Point Inkscape::SelTrans::_getGeomHandlePos(Geom::Point const &visual_handle_pos)
{
    if ( _snap_bbox_type == SPItem::GEOMETRIC_BBOX) {
        // When the selector tool is using geometric bboxes, then the handle is already
        // located at one of the geometric bbox corners
        return visual_handle_pos;
    }

    if (!_geometric_bbox) {
        //_getGeomHandlePos() can only be used after _geometric_bbox has been defined!
        return visual_handle_pos;
    }

    // Using the Geom::Rect constructor below ensures that "min() < max()", which is important
    // because this will also hold for _bbox, and which is required for get_scale_transform_for_stroke()
    Geom::Rect new_bbox = Geom::Rect(_origin_for_bboxpoints, visual_handle_pos); // new visual bounding box
    // Please note that the new_bbox might in fact be just a single line, for example when stretching (in
    // which case the handle and origin will be aligned vertically or horizontally)
    Geom::Point normalized_handle_pos = (visual_handle_pos - new_bbox.min()) * Geom::Scale(new_bbox.dimensions()).inverse();

    // Calculate the absolute affine while taking into account the scaling of the stroke width
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool transform_stroke = prefs->getBool("/options/transform/stroke", true);
    bool preserve = prefs->getBool("/options/preservetransform/value", false);
    Geom::Affine abs_affine = get_scale_transform_for_uniform_stroke (*_bbox, _strokewidth, _strokewidth, transform_stroke, preserve,
                    new_bbox.min()[Geom::X], new_bbox.min()[Geom::Y], new_bbox.max()[Geom::X], new_bbox.max()[Geom::Y]);

    // Calculate the scaled geometrical bbox
    Geom::Rect new_geom_bbox = Geom::Rect(_geometric_bbox->min() * abs_affine, _geometric_bbox->max() * abs_affine);
    // Find the location of the handle on this new geometrical bbox
    return normalized_handle_pos * Geom::Scale(new_geom_bbox.dimensions()) + new_geom_bbox.min(); //new position of the geometric handle
}

Geom::Scale Inkscape::calcScaleFactors(Geom::Point const &initial_point, Geom::Point const &new_point, Geom::Point const &origin, bool const skew)
{
    // Work out the new scale factors for the bbox

    Geom::Point const initial_delta = initial_point - origin;
    Geom::Point const new_delta = new_point - origin;
    Geom::Point const offset = new_point - initial_point;
    Geom::Scale scale(1, 1);

    for ( unsigned int i = 0 ; i < 2 ; i++ ) {
        if ( fabs(initial_delta[i]) > 1e-6 ) {
            if (skew) {
                scale[i] = offset[1-i] / initial_delta[i];
            } else {
                scale[i] = new_delta[i] / initial_delta[i];
            }
        }
    }

    return scale;
}

// Only for scaling/stretching
Geom::Point Inkscape::SelTrans::_calcAbsAffineDefault(Geom::Scale const default_scale)
{
    Geom::Affine abs_affine = Geom::Translate(-_origin) * Geom::Affine(default_scale) * Geom::Translate(_origin);
    Geom::Point new_bbox_min = _stroked_bbox->min() * abs_affine;
    Geom::Point new_bbox_max = _stroked_bbox->max() * abs_affine;

    bool transform_stroke = false;
    bool preserve = false;
    gdouble stroke_x = 0;
    gdouble stroke_y = 0;

    if ( _snap_bbox_type != SPItem::GEOMETRIC_BBOX) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        transform_stroke = prefs->getBool("/options/transform/stroke", true);
        preserve = prefs->getBool("/options/preservetransform/value", false);
        stroke_x = _stroked_bbox->width() - _geometric_bbox->width();
        stroke_y = _stroked_bbox->height() - _geometric_bbox->height();
    }

    _absolute_affine = get_scale_transform_for_uniform_stroke (*_stroked_bbox, stroke_x, stroke_y, transform_stroke, preserve,
                    new_bbox_min[Geom::X], new_bbox_min[Geom::Y], new_bbox_max[Geom::X], new_bbox_max[Geom::Y]);

    // return the new handle position
    return ( _point - _origin ) * default_scale + _origin;
}

// Only for scaling/stretching
Geom::Point Inkscape::SelTrans::_calcAbsAffineGeom(Geom::Scale const geom_scale)
{
    _relative_affine = Geom::Affine(geom_scale);
    _absolute_affine = Geom::Translate(-_origin_for_specpoints) * _relative_affine * Geom::Translate(_origin_for_specpoints);

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool const transform_stroke = prefs->getBool("/options/transform/stroke", true);
    if (_geometric_bbox) {
        Geom::Rect visual_bbox = get_visual_bbox(_geometric_bbox, _absolute_affine, _strokewidth, transform_stroke);
        // return the new handle position
        return visual_bbox.min() + visual_bbox.dimensions() * Geom::Scale(_handle_x, _handle_y);
    }

    // Fall back scenario, in case we don't have a geometric bounding box at hand;
    // (Due to some bugs related to bounding boxes having at least one zero dimension; For more details
    // see https://bugs.launchpad.net/inkscape/+bug/318726)
    g_warning("No geometric bounding box has been calculated; this is a bug that needs fixing!");
    return _calcAbsAffineDefault(geom_scale); // this is bogus, but we must return _something_
}

void Inkscape::SelTrans::_keepClosestPointOnly(Geom::Point const &p)
{
    SnapManager const &m = _desktop->getNamedView()->snap_manager;

    // If we're not going to snap nodes, then we might just as well get rid of their snappoints right away
    if (!(m.snapprefs.isTargetSnappable(SNAPTARGET_NODE_CATEGORY, SNAPTARGET_OTHERS_CATEGORY) || m.snapprefs.isAnyDatumSnappable())) {
        _snap_points.clear();
    }

    // If we're not going to snap bounding boxes, then we might just as well get rid of their snappoints right away
    if (!m.snapprefs.isTargetSnappable(SNAPTARGET_BBOX_CATEGORY) && !m.snapprefs.isTargetSnappable(SNAPTARGET_ALIGNMENT_CATEGORY)) {
        _bbox_points.clear();
    }

    _all_snap_sources_sorted = _snap_points;
    _all_snap_sources_sorted.insert(_all_snap_sources_sorted.end(), _bbox_points.begin(), _bbox_points.end());

    // Calculate and store the distance to the reference point for each snap candidate point
    for(auto & i : _all_snap_sources_sorted) {
        i.setDistance(Geom::L2(i.getPoint() - p));
    }

    // Sort them ascending, using the distance calculated above as the single criteria
    std::sort(_all_snap_sources_sorted.begin(), _all_snap_sources_sorted.end());

    // Now get the closest snap source
    _snap_points.clear();
    _bbox_points.clear();
    if (!_all_snap_sources_sorted.empty()) {
        _all_snap_sources_iter = _all_snap_sources_sorted.begin();
        if (_all_snap_sources_sorted.front().getSourceType() & SNAPSOURCE_BBOX_CATEGORY) {
            _bbox_points.push_back(_all_snap_sources_sorted.front());
        } else {
            _snap_points.push_back(_all_snap_sources_sorted.front());
        }
    }

}
// TODO: This code is duplicated in transform-handle-set.cpp; fix this!
void Inkscape::SelTrans::getNextClosestPoint(bool reverse)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/options/snapclosestonly/value", false)) {
        if (!_all_snap_sources_sorted.empty()) {
            if (reverse) { // Shift-tab will find a closer point
                if (_all_snap_sources_iter == _all_snap_sources_sorted.begin()) {
                    _all_snap_sources_iter = _all_snap_sources_sorted.end();
                }
                --_all_snap_sources_iter;
            } else { // Tab will find a point further away
                ++_all_snap_sources_iter;
                if (_all_snap_sources_iter == _all_snap_sources_sorted.end()) {
                    _all_snap_sources_iter = _all_snap_sources_sorted.begin();
                }
            }

            _snap_points.clear();
            _bbox_points.clear();

            if ((*_all_snap_sources_iter).getSourceType() & SNAPSOURCE_BBOX_CATEGORY) {
                _bbox_points.push_back(*_all_snap_sources_iter);
            } else {
                _snap_points.push_back(*_all_snap_sources_iter);
            }

            // Show the updated snap source now; otherwise it won't be shown until the selection is being moved again
            SnapManager &m = _desktop->getNamedView()->snap_manager;
            m.setup(_desktop);
            m.displaySnapsource(*_all_snap_sources_iter);
            m.unSetup();
        }
    }
}

void Inkscape::SelTrans::grab_stkey(Geom::Point const &p, StickyTransform type)
{
    _stkey = type;
    _stkey_transformed = false;
    _stkey_paused = false;
    grab(p, 0, 0, false, false);
    resetState(_stkey == StickyTransform::Rotate ? STATE_ROTATE : STATE_SCALE);
}

bool Inkscape::SelTrans::ungrab_stkey(bool cancel)
{
    ungrab();
    _stkey = StickyTransform::None;
    if (cancel && _stkey_transformed) {
        DocumentUndo::undo(_desktop->getDocument());
    }
    return _stkey_transformed;
}

bool Inkscape::SelTrans::request_stkey(Geom::Point const &p, int state)
{
    if (_stkey_paused) {
        // These happen between mouse press and mouse release, improve accuracy
        // by pausing these evets for this short window.
        return false;
    }
    Geom::Point mutable_p = p;
    switch (_stkey) {
        case StickyTransform::Grab:
            if (moveTo(p, state)) {
                _stkey_transformed = true;
            }
            break;
        case StickyTransform::Scale:
            if (scaleRequest(mutable_p, state)) {
                commitAbsoluteAffine();
                _stkey_transformed = true;
            }
            break;
        case StickyTransform::Rotate:
            if (rotateRequest(mutable_p, state)) {
                commitRelativeAffine();
                _stkey_transformed = true;
            }
            break;
        default:
            break;
    }
    return true;
}

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
