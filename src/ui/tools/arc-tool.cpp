// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Ellipse drawing context.
 */
/* Authors:
 *   Mitsuru Oka
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <johan@shouraizou.nl>
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2000-2006 Authors
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "arc-tool.h"

#include <glibmm/i18n.h>

#include "context-fns.h"
#include "desktop-style.h"
#include "document-undo.h"
#include "document.h"
#include "message-context.h"
#include "object/sp-ellipse.h"
#include "object/sp-namedview.h"
#include "selection.h"
#include "ui/icon-names.h"
#include "ui/modifiers.h"
#include "ui/shape-editor.h"
#include "ui/widget/events/canvas-event.h"
#include "util/units.h"

using Inkscape::DocumentUndo;

namespace Inkscape::UI::Tools {

ArcTool::ArcTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/shapes/arc", "arc.svg")
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
    , mod_trans_confine(Modifiers::Modifier::get(Modifiers::Type::TRANS_CONFINE))
    , mod_trans_increment(Modifiers::Modifier::get(Modifiers::Type::TRANS_INCREMENT))
    , mod_trans_off_center(Modifiers::Modifier::get(Modifiers::Type::TRANS_OFF_CENTER))
{
    Inkscape::Selection *selection = desktop->getSelection();

    this->shape_editor = new ShapeEditor(desktop);
    this->shape_editor->useTransformBox();

    SPItem *item = desktop->getSelection()->singleItem();
    if (item) {
        this->shape_editor->set_item(item);
    }

    this->sel_changed_connection.disconnect();
    this->sel_changed_connection = selection->connectChanged(
        sigc::mem_fun(*this, &ArcTool::selection_changed)
    );

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    if (prefs->getBool("/tools/shapes/gradientdrag")) {
        this->enableGrDrag();
    }
}

ArcTool::~ArcTool()
{
    ungrabCanvasEvents();
    this->finishItem();
    this->sel_changed_connection.disconnect();

    this->enableGrDrag(false);

    this->sel_changed_connection.disconnect();

    delete this->shape_editor;
    this->shape_editor = nullptr;

    /* fixme: This is necessary because we do not grab */
    if (this->arc) {
        this->finishItem();
    }
}

/**
 * Callback that processes the "changed" signal on the selection;
 * destroys old and creates new knotholder.
 */
void ArcTool::selection_changed(Inkscape::Selection* selection) {
    this->shape_editor->unset_item();
    this->shape_editor->set_item(selection->singleItem());
}

bool ArcTool::item_handler(SPItem *item, CanvasEvent const &event)
{
    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (event.num_press == 1 && event.button == 1) {
                setup_for_drag_start(event);
            }
            // motion and release are always on root (why?)
        },
        [&] (CanvasEvent const &event) {}
    );

    return ToolBase::item_handler(item, event);
}

bool ArcTool::root_handler(CanvasEvent const &event)
{
    auto selection = _desktop->getSelection();
    auto prefs = Preferences::get();

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

                if (arc) {
                    // we've been dragging, finish the arc
                    finishItem();
                } else if (item_to_select) {
                    // no dragging, select clicked item if any
                    if (mod_select_add_to->active(event.modifiers)) {
                        selection->toggle(item_to_select);
                    } else if (!selection->includes(item_to_select)) {
                        selection->set(item_to_select);
                    }
                } else {
                    // click in an empty space
                    selection->clear();
                }

                xyp = {};
                item_to_select = nullptr;
                ret = true;
            }
            ungrabCanvasEvents();
        },
        [&] (KeyPressEvent const &event) {
            auto keyval = get_latin_keyval(event);

            if (Modifiers::keyval_is_a_modifier(keyval)) {
                if (!dragging) {
                    Modifiers::responsive_tooltip_with_labels(
                        defaultMessageContext(), event, 3,
                        Modifiers::Type::TRANS_CONFINE,
                        _("Make circle or integer-ratio ellipse, snap arc/segment angle"),
                        Modifiers::Type::TRANS_INCREMENT, _("Snap ellipse to mouse pointer"),
                        Modifiers::Type::TRANS_OFF_CENTER, _("Draw around the starting point")
                    );
                }
            }

            switch (keyval) {
                case GDK_KEY_Escape:
                    if (dragging) {
                        dragging = false;
                        discard_delayed_snap_event();
                        // if drawing, cancel, otherwise pass it up for deselecting
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
                            // we've been dragging, finish the arc
                            finishItem();
                        }
                        // do not return true, so that space would work switching to selector
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
            auto keyval = get_latin_keyval (event);

            if (Modifiers::keyval_is_a_modifier(keyval)) {
                defaultMessageContext()->clear();
            }
        },
        [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

void ArcTool::drag(Geom::Point const &pt, unsigned state)
{
    shape_editor->setDrawing(true);
    if (!this->arc) {
        if (Inkscape::have_viable_layer(_desktop, defaultMessageContext()) == false) {
            return;
        }

        // Create object
        Inkscape::XML::Document *xml_doc = _desktop->doc()->getReprDoc();
        Inkscape::XML::Node *repr = xml_doc->createElement("svg:path");
        repr->setAttribute("sodipodi:type", "arc");

        // Set style
        _desktop->applyCurrentOrToolStyle(repr, "/tools/shapes/arc", false);

        auto layer = currentLayer();
        this->arc = cast<SPGenericEllipse>(layer->appendChildRepr(repr));
        Inkscape::GC::release(repr);
        this->arc->transform = layer->i2doc_affine().inverse();
        this->arc->updateRepr();
    }

    auto confine = mod_trans_confine->active(state);
    // Third is weirdly wrong, surely incrememnts should do something else.
    auto circle_edge = mod_trans_increment->active(state);
    auto off_center = mod_trans_off_center->active(state);

    Geom::Rect r = Inkscape::snap_rectangular_box(_desktop, arc.get(), pt, this->center, state);

    Geom::Point dir = r.dimensions() / 2;


    if (circle_edge) {
        /* With Alt let the ellipse pass through the mouse pointer */
        Geom::Point c = r.midpoint();

        if (!confine) {
            if (fabs(dir[Geom::X]) > 1E-6 && fabs(dir[Geom::Y]) > 1E-6) {
                Geom::Affine const i2d ( (this->arc)->i2dt_affine() );
                Geom::Point new_dir = pt * i2d - c;
                new_dir[Geom::X] *= dir[Geom::Y] / dir[Geom::X];
                double lambda = new_dir.length() / dir[Geom::Y];
                r = Geom::Rect (c - lambda*dir, c + lambda*dir);
            }
        } else {
            /* with Alt+Ctrl we generate a perfect circle, shift decides if initial click is centre or end of diameter */
            if (off_center) {
                r = Geom::Rect((2 * center) - pt, pt);
            } else {
                r = Geom::Rect(center, pt);
            }
            dir = r.dimensions()/2;
            c = r.midpoint();
            double l = dir.length();
            Geom::Point d (l, l);
            r = Geom::Rect (c - d, c + d);
        }
    }

    this->arc->position_set(
        r.midpoint()[Geom::X], r.midpoint()[Geom::Y],
        r.dimensions()[Geom::X] / 2, r.dimensions()[Geom::Y] / 2);

    double rdimx = r.dimensions()[Geom::X];
    double rdimy = r.dimensions()[Geom::Y];

    Inkscape::Util::Quantity rdimx_q = Inkscape::Util::Quantity(rdimx, "px");
    Inkscape::Util::Quantity rdimy_q = Inkscape::Util::Quantity(rdimy, "px");
    Glib::ustring xs = rdimx_q.string(_desktop->getNamedView()->display_units);
    Glib::ustring ys = rdimy_q.string(_desktop->getNamedView()->display_units);

    if (confine) {
        int ratio_x, ratio_y;
        bool is_golden_ratio = false;

        if (fabs (rdimx) > fabs (rdimy)) {
            if (fabs(rdimx / rdimy - goldenratio) < 1e-6) {
                is_golden_ratio = true;
            }

            ratio_x = (int) rint (rdimx / rdimy);
            ratio_y = 1;
        } else {
            if (fabs(rdimy / rdimx - goldenratio) < 1e-6) {
                is_golden_ratio = true;
            }

            ratio_x = 1;
            ratio_y = (int) rint (rdimy / rdimx);
        }

        if (!is_golden_ratio) {
            this->message_context->setF(Inkscape::IMMEDIATE_MESSAGE,
                    _("<b>Ellipse</b>: %s &#215; %s (constrained to ratio %d:%d); with <b>%s</b> to draw around the starting point"),
                    xs.c_str(), ys.c_str(), ratio_x, ratio_y,
                    mod_trans_off_center->get_label().c_str());
        } else {
            if (ratio_y == 1) {
                this->message_context->setF(Inkscape::IMMEDIATE_MESSAGE,
                        _("<b>Ellipse</b>: %s &#215; %s (constrained to golden ratio 1.618 : 1); with <b>%s</b> to draw around the starting point"),
                        xs.c_str(), ys.c_str(), mod_trans_off_center->get_label().c_str());
            } else {
                this->message_context->setF(Inkscape::IMMEDIATE_MESSAGE,
                        _("<b>Ellipse</b>: %s &#215; %s (constrained to golden ratio 1 : 1.618); with <b>%s</b> to draw around the starting point"),
                        xs.c_str(), ys.c_str(), mod_trans_off_center->get_label().c_str());
            }
        }
    } else {
        this->message_context->setF(Inkscape::IMMEDIATE_MESSAGE,
                _("<b>Ellipse</b>: %s &#215; %s; with <b>%s</b> to make circle, integer-ratio, or golden-ratio ellipse; with <b>%s</b> to draw around the starting point"),
                xs.c_str(), ys.c_str(), mod_trans_confine->get_label().c_str(),
                mod_trans_off_center->get_label().c_str());
    }
}

void ArcTool::finishItem()
{
    message_context->clear();

    if (arc) {
        if (this->arc->rx.computed == 0 || this->arc->ry.computed == 0) {
            this->cancel(); // Don't allow the creating of zero sized arc, for example when the start and and point snap to the snap grid point
            return;
        }

        this->arc->updateRepr();
        this->arc->doWriteTransform(this->arc->transform, nullptr, true);
        // update while creating inside a LPE group
        sp_lpe_item_update_patheffect(this->arc.get(), true, true);
        _desktop->getSelection()->set(arc.get());
        shape_editor->setDrawing(false);

        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Create ellipse"), INKSCAPE_ICON("draw-ellipse"));

        this->arc = nullptr;
    }
}

void ArcTool::cancel()
{
    shape_editor->setDrawing(false);
    _desktop->getSelection()->clear();
    ungrabCanvasEvents();

    if (arc) {
        arc->deleteObject();
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
