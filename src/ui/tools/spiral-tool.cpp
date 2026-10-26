// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Spiral drawing context
 *
 * Authors:
 *   Mitsuru Oka
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2001 Lauris Kaplinski
 * Copyright (C) 2001-2002 Mitsuru Oka
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/i18n.h>

#include "spiral-tool.h"

#include "context-fns.h"
#include "desktop-style.h"
#include "document-undo.h"
#include "document.h"
#include "message-context.h"
#include "selection.h"

#include "object/sp-namedview.h"
#include "object/sp-spiral.h"

#include "ui/icon-names.h"
#include "ui/shape-editor.h"
#include "ui/widget/events/canvas-event.h"

#include "util/units.h"

using Inkscape::DocumentUndo;

namespace Inkscape::UI::Tools {

SpiralTool::SpiralTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/shapes/spiral", "spiral.svg")
    , mod_freehand_angle_snapping(Modifiers::Modifier::get(Modifiers::Type::FREEHAND_ANGLE_SNAPPING))
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
{
    sp_event_context_read(this, "expansion");
    sp_event_context_read(this, "revolution");
    sp_event_context_read(this, "t0");

    this->shape_editor = new ShapeEditor(desktop);

    SPItem *item = desktop->getSelection()->singleItem();
    if (item) {
        this->shape_editor->set_item(item);
    }

    Inkscape::Selection *selection = desktop->getSelection();
    this->sel_changed_connection.disconnect();

    this->sel_changed_connection = selection->connectChanged(sigc::mem_fun(*this, &SpiralTool::selection_changed));

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    if (prefs->getBool("/tools/shapes/selcue")) {
        this->enableSelectionCue();
    }

    if (prefs->getBool("/tools/shapes/gradientdrag")) {
        this->enableGrDrag();
    }
}

SpiralTool::~SpiralTool() {
    ungrabCanvasEvents();

    this->finishItem();
    this->sel_changed_connection.disconnect();

    this->enableGrDrag(false);

    delete this->shape_editor;
    this->shape_editor = nullptr;

    /* fixme: This is necessary because we do not grab */
    if (this->spiral) {
    	this->finishItem();
    }
}

/**
 * Callback that processes the "changed" signal on the selection;
 * destroys old and creates new knotholder.
 */
void SpiralTool::selection_changed(Inkscape::Selection *selection) {
    this->shape_editor->unset_item();
    this->shape_editor->set_item(selection->singleItem());
}


void SpiralTool::set(const Inkscape::Preferences::Entry& val) {
    Glib::ustring name = val.getEntryName();

    if (name == "expansion") {
        this->exp = CLAMP(val.getDouble(), 0.0, 1000.0);
    } else if (name == "revolution") {
        this->revo = CLAMP(val.getDouble(3.0), 0.05, 40.0);
    } else if (name == "t0") {
        this->t0 = CLAMP(val.getDouble(), 0.0, 0.999);
    }
}

bool SpiralTool::root_handler(CanvasEvent const &event)
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

                // Snap center
                auto &m = _desktop->getNamedView()->snap_manager;
                m.setup(_desktop);
                m.freeSnapReturnByRef(center, SNAPSOURCE_NODE_HANDLE);
                m.unSetup();

                grabCanvasEvents();
                ret = true;
            }
        },
        [&] (MotionEvent const &event) {
            if (dragging && (event.modifiers & GDK_BUTTON1_MASK)) {
                if (!checkDragMoved(event.pos)) {
                    return;
                }

                auto motion_dt = _desktop->w2d(event.pos);

                auto &m = _desktop->getNamedView()->snap_manager;
                m.setup(_desktop, true, spiral.get());
                m.freeSnapReturnByRef(motion_dt, Inkscape::SNAPSOURCE_NODE_HANDLE);
                m.unSetup();

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

                if (spiral) {
                    // We've been dragging, finish the spiral.
                    finishItem();
                } else if (item_to_select) {
                    // No dragging, select clicked item if any.
                    if (mod_select_add_to->active(event.modifiers)) {
                        selection->toggle(item_to_select.get());
                    } else if (!selection->includes(item_to_select.get())) {
                        selection->set(item_to_select.get());
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
            auto keyval = get_latin_keyval(event);

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
                            // We've been dragging, finish the spiral.
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
            auto keyval = get_latin_keyval(event);

            if (Modifiers::keyval_is_a_modifier(keyval)) {
                defaultMessageContext()->clear();
            }
        },
        [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

void SpiralTool::drag(Geom::Point const &p, guint state) {

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double const snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);

    if (!this->spiral) {
        if (Inkscape::have_viable_layer(_desktop, defaultMessageContext()) == false) {
            return;
        }

        // Create object
        Inkscape::XML::Document *xml_doc = _desktop->doc()->getReprDoc();
        Inkscape::XML::Node *repr = xml_doc->createElement("svg:path");
        repr->setAttribute("sodipodi:type", "spiral");

        // Set style
        _desktop->applyCurrentOrToolStyle(repr, "/tools/shapes/spiral", false);

        this->spiral = cast<SPSpiral>(currentLayer()->appendChildRepr(repr));
        Inkscape::GC::release(repr);
        this->spiral->transform = currentLayer()->i2doc_affine().inverse();
        this->spiral->updateRepr();
    }

    SnapManager &m = _desktop->getNamedView()->snap_manager;
    m.setup(_desktop, true, spiral.get());
    Geom::Point pt2g = p;
    m.freeSnapReturnByRef(pt2g, Inkscape::SNAPSOURCE_NODE_HANDLE);
    m.unSetup();
    Geom::Point const p0 = _desktop->dt2doc(this->center);
    Geom::Point const p1 = _desktop->dt2doc(pt2g);

    Geom::Point const delta = p1 - p0;
    gdouble const rad = Geom::L2(delta);

    // Start angle calculated from end angle and number of revolutions.
    gdouble arg = Geom::atan2(delta) - 2.0*M_PI * spiral->revo;

    if (mod_freehand_angle_snapping->active(state)) {
        /* Snap start angle */
        double snaps_radian = M_PI / snaps;
        arg = std::round(arg / snaps_radian) * snaps_radian;
    }

    /* Fixme: these parameters should be got from dialog box */
    this->spiral->setPosition(p0[Geom::X], p0[Geom::Y],
                           /*expansion*/ this->exp,
                           /*revolution*/ this->revo,
                           rad, arg,
                           /*t0*/ this->t0);

    /* status text */
    Inkscape::Util::Quantity q = Inkscape::Util::Quantity(rad, "px");
    Glib::ustring rads = q.string(_desktop->getNamedView()->display_units);
    this->message_context->setF(Inkscape::IMMEDIATE_MESSAGE,
                                _("<b>Spiral</b>: radius %s, angle %.2f&#176;; with <b>%s</b> to snap angle"),
                                rads.c_str(), arg * 180/M_PI + 360*spiral->revo,
                                mod_freehand_angle_snapping->get_label().c_str());
}

void SpiralTool::finishItem() {
    this->message_context->clear();

    if (spiral) {
    	if (this->spiral->rad == 0) {
    		this->cancel(); // Don't allow the creating of zero sized spiral, for example when the start and and point snap to the snap grid point
    		return;
    	}

        // Set the *transform* center, so select tool rotations work from the spiral's
        // center instead of the bbox center.
        spiral->setCenter(center);
        spiral->set_shape();
        spiral->updateRepr(SP_OBJECT_WRITE_EXT);
        // compensate stroke scaling couldn't be done in doWriteTransform
        double const expansion = spiral->transform.descrim();
        spiral->doWriteTransform(spiral->transform, nullptr, true);
        spiral->adjust_stroke_width_recursive(expansion);
        // update while creating inside a LPE group
        sp_lpe_item_update_patheffect(this->spiral.get(), true, true);
        _desktop->getSelection()->set(spiral.get());
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Create spiral"), INKSCAPE_ICON("draw-spiral"));

        this->spiral = nullptr;
    }
}

void SpiralTool::cancel() {
    _desktop->getSelection()->clear();
    ungrabCanvasEvents();

    if (spiral) {
        spiral->deleteObject();
    }

    within_tolerance = false;
    xyp = {};
    item_to_select = nullptr;

    DocumentUndo::cancel(_desktop->getDocument());
}

} // namespace Inkscape::UI::Tools

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
