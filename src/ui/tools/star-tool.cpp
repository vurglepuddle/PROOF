// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Star drawing context
 *
 * Authors:
 *   Mitsuru Oka <oka326@parkcity.ne.jp>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2001-2002 Mitsuru Oka
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/i18n.h>

#include "star-tool.h"

#include "context-fns.h"
#include "desktop-style.h"
#include "document-undo.h"
#include "document.h"
#include "message-context.h"
#include "selection.h"

#include "object/sp-namedview.h"
#include "object/sp-star.h"

#include "ui/icon-names.h"
#include "ui/shape-editor.h"
#include "ui/widget/events/canvas-event.h"

#include "util/units.h"

using Inkscape::DocumentUndo;

namespace Inkscape {
namespace UI {
namespace Tools {

StarTool::StarTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/shapes/star", "star.svg")
    , mod_freehand_angle_snapping(Modifiers::Modifier::get(Modifiers::Type::FREEHAND_ANGLE_SNAPPING))
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
{
    sp_event_context_read(this, "isflatsided");
    sp_event_context_read(this, "magnitude");
    sp_event_context_read(this, "proportion");
    sp_event_context_read(this, "rounded");
    sp_event_context_read(this, "randomized");

    this->shape_editor = new ShapeEditor(desktop);
    this->shape_editor->useTransformBox();

    SPItem *item = desktop->getSelection()->singleItem();
    if (item) {
        this->shape_editor->set_item(item);
    }

    Inkscape::Selection *selection = desktop->getSelection();

    this->sel_changed_connection.disconnect();

    this->sel_changed_connection = selection->connectChanged(sigc::mem_fun(*this, &StarTool::selection_changed));

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    if (prefs->getBool("/tools/shapes/gradientdrag")) {
        this->enableGrDrag();
    }
}

StarTool::~StarTool() {
    ungrabCanvasEvents();

    this->finishItem();
    this->sel_changed_connection.disconnect();

    this->enableGrDrag(false);

    delete this->shape_editor;
    this->shape_editor = nullptr;

    /* fixme: This is necessary because we do not grab */
    if (this->star) {
    	this->finishItem();
    }
}

/**
 * Callback that processes the "changed" signal on the selection;
 * destroys old and creates new knotholder.
 *
 * @param  selection Should not be NULL.
 */
void StarTool::selection_changed(Inkscape::Selection* selection) {
    g_assert (selection != nullptr);

    this->shape_editor->unset_item();
    this->shape_editor->set_item(selection->singleItem());
}


void StarTool::set(const Inkscape::Preferences::Entry& val) {
    Glib::ustring path = val.getEntryName();

    if (path == "magnitude") {
        this->magnitude = CLAMP(val.getInt(5), this->isflatsided ? 3 : 2, 1024);
    } else if (path == "proportion") {
        this->proportion = CLAMP(val.getDouble(0.5), 0.01, 2.0);
    } else if (path == "isflatsided") {
        this->isflatsided = val.getBool();
    } else if (path == "rounded") {
        this->rounded = val.getDouble();
    } else if (path == "randomized") {
        this->randomized = val.getDouble();
    }
}

bool StarTool::root_handler(CanvasEvent const &event)
{
    auto selection = _desktop->getSelection();
    auto prefs = Inkscape::Preferences::get();

    tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

    bool ret = false;

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (event.num_press == 1 && event.button == 1) {
                dragging = true;

                setup_for_drag_start(event);
                center = _desktop->w2d(event.pos);

                // Snap center.
                auto &m = _desktop->getNamedView()->snap_manager;
                m.setup(_desktop);
                m.freeSnapReturnByRef(center, SNAPSOURCE_NODE_HANDLE);

                grabCanvasEvents();

                ret = true;
                m.unSetup();
            }
        },
        [&] (MotionEvent const &event) {
            if (!(event.modifiers & GDK_BUTTON1_MASK)) shape_editor->setPointer(_desktop->w2d(event.pos));
            if (dragging && (event.modifiers & GDK_BUTTON1_MASK)) {
                if (!checkDragMoved(event.pos)) {
                    return;
                }

                auto const motion_dt = _desktop->w2d(event.pos);
                drag(motion_dt, event.modifiers);

                gobble_motion_events(GDK_BUTTON1_MASK);

                ret = true;
            } else if (!sp_event_context_knot_mouseover()) {
                auto &m = _desktop->getNamedView()->snap_manager;
                m.setup(_desktop);

                auto const motion_dt = _desktop->w2d(event.pos);
                m.preSnap(SnapCandidatePoint(motion_dt, SNAPSOURCE_NODE_HANDLE));
                m.unSetup();
            }
        },
        [&] (ButtonReleaseEvent const &event) {
            xyp = {};
            if (dragging && event.button == 1) {
                dragging = false;
                discard_delayed_snap_event();

                if (star) {
                    // We've been dragging, finish the star.
                    finishItem();
                } else if (item_to_select) {
                    // No dragging, select clicked item if any.
                    if (mod_select_add_to->active(event.modifiers)) {
                        selection->toggle(item_to_select);
                    } else if (!selection->includes(item_to_select)) {
                        selection->set(item_to_select);
                    }
                } else {
                    // Click in an empty space.
                    selection->clear();
                }

                item_to_select = nullptr;
                ret = true;
            }
            ungrabCanvasEvents();
        },
        [&] (KeyPressEvent const &event) {
            auto const keyval = get_latin_keyval(event);

            if (Modifiers::keyval_is_a_modifier(keyval)) {
                Modifiers::responsive_tooltip(
                    defaultMessageContext(), event, 1, Modifiers::Type::FREEHAND_ANGLE_SNAPPING
                );
            }

            switch (keyval) {
                case GDK_KEY_Escape:
                    if (dragging) {
                        dragging = false;
                        discard_delayed_snap_event();
                        // If drawing, cancel, otherwise pass it up for deselecting.
                        cancel();
                        ret = true;
                    }
                    break;

                case GDK_KEY_space:
                    if (dragging) {
                        ungrabCanvasEvents();

                        dragging = false;

                        discard_delayed_snap_event();

                        if (!within_tolerance) {
                            // We've been dragging, finish the star.
                            finishItem();
                        }
                        // Do not return true, so that space would work switching to selector.
                    }
                    break;

                case GDK_KEY_Delete:
                case GDK_KEY_KP_Delete:
                case GDK_KEY_BackSpace:
                    ret = deleteSelectedDrag(mod_ctrl_only(event));
                    break;

                default:
                    break;
            }
        },
        [&] (KeyReleaseEvent const &event) {
            auto const keyval = get_latin_keyval(event);

            if (Modifiers::keyval_is_a_modifier(keyval)) {
                defaultMessageContext()->clear();
            }
        },
        [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

void StarTool::drag(Geom::Point p, unsigned state)
{
    shape_editor->setDrawing(true);
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double const snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);

    if (!this->star) {
        if (Inkscape::have_viable_layer(_desktop, defaultMessageContext()) == false) {
            return;
        }

        _tr = currentLayer()->i2doc_affine().inverse();

        // Create object
        Inkscape::XML::Document *xml_doc = _desktop->doc()->getReprDoc();
        Inkscape::XML::Node *repr = xml_doc->createElement("svg:path");
        repr->setAttribute("sodipodi:type", "star");

        // Set style
        _desktop->applyCurrentOrToolStyle(repr, "/tools/shapes/star", false);

        this->star = cast<SPStar>(currentLayer()->appendChildRepr(repr));
        Inkscape::GC::release(repr);
        this->star->updateRepr();

        // Adjust stroke width to cope with parent's transform
        this->star->adjust_stroke_width_recursive(_tr.descrim());
    }

    /* Snap corner point with no constraints */
    SnapManager &m = _desktop->getNamedView()->snap_manager;

    m.setup(_desktop, true, star.get());
    Geom::Point pt2g = p;
    m.freeSnapReturnByRef(pt2g, Inkscape::SNAPSOURCE_NODE_HANDLE);
    m.unSetup();

    Geom::Point const p0 = _desktop->dt2doc(this->center) * _tr;
    Geom::Point const p1 = _desktop->dt2doc(pt2g) * _tr;

    double const sides = (gdouble) this->magnitude;
    Geom::Point const d = p1 - p0;
    Geom::Coord const r1 = Geom::L2(d);
    double arg1 = atan2(d);

    if (mod_freehand_angle_snapping->active(state)) {
        /* Snap angle */
        double snaps_radian = M_PI / snaps;
        arg1 = std::round(arg1 / snaps_radian) * snaps_radian;
    }

    sp_star_position_set(star.get(), this->magnitude, p0, r1, r1 * this->proportion,
                         arg1, arg1 + M_PI / sides, this->isflatsided, this->rounded, this->randomized);

    /* status text */
    Inkscape::Util::Quantity q = Inkscape::Util::Quantity(r1, "px");
    Glib::ustring rads = q.string(_desktop->getNamedView()->display_units);
    this->message_context->setF(Inkscape::IMMEDIATE_MESSAGE,
                               ( this->isflatsided?
                                 _("<b>Polygon</b>: radius %s, angle %.2f&#176;; with <b>%s</b> to snap angle") :
                                 _("<b>Star</b>: radius %s, angle %.2f&#176;; with <b>%s</b> to snap angle") ),
                               rads.c_str(), arg1 * 180 / M_PI, mod_freehand_angle_snapping->get_label().c_str());
}

void StarTool::finishItem() {
    this->message_context->clear();

    if (star) {
        if (this->star->r[1] == 0) {
        	// Don't allow the creating of zero sized arc, for example
        	// when the start and and point snap to the snap grid point
            this->cancel();
            return;
        }

        // Set the *transform* center, so select tool rotations work from the star's
        // center instead of the bbox center for odd spoked stars.
        this->star->setCenter(this->center);
        this->star->set_shape();
        this->star->updateRepr(SP_OBJECT_WRITE_EXT);

        // update while creating inside a LPE group
        sp_lpe_item_update_patheffect(this->star.get(), true, true);
        _desktop->getSelection()->set(star.get());
        shape_editor->setDrawing(false);
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Create star"), INKSCAPE_ICON("draw-polygon-star"));

        this->star = nullptr;
    }
}

void StarTool::cancel() {
    shape_editor->setDrawing(false);
    _desktop->getSelection()->clear();
    ungrabCanvasEvents();

    if (star) {
        star->deleteObject();
    }

    this->within_tolerance = false;
    this->xyp = {};
    this->item_to_select = nullptr;

    DocumentUndo::cancel(_desktop->getDocument());
}

}
}
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
