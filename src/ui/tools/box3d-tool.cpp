// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 3D box drawing context
 *
 * Author:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2007      Maximilian Albert <Anhalter42@gmx.de>
 * Copyright (C) 2006      Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 2000-2005 authors
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/i18n.h>

#include "context-fns.h"
#include "document-undo.h"
#include "message-context.h"
#include "perspective-line.h"

#include "object/box3d-side.h"
#include "object/box3d.h"
#include "object/sp-defs.h"
#include "object/sp-namedview.h"

#include "ui/icon-names.h"
#include "ui/shape-editor.h"
#include "ui/tools/box3d-tool.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::DocumentUndo;

namespace Inkscape::UI::Tools {

Box3dTool::Box3dTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/shapes/3dbox", "box.svg")
    , mod_box3d_extrude_one(Modifiers::Modifier::get(Modifiers::Type::BOX3D_EXTRUDE_ONE))
    , mod_box3d_extrude_two(Modifiers::Modifier::get(Modifiers::Type::BOX3D_EXTRUDE_TWO))
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
    , mod_select_force_drag(Modifiers::Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
    , mod_select_in_groups(Modifiers::Modifier::get(Modifiers::Type::SELECT_IN_GROUPS))
{
    shape_editor = new ShapeEditor(_desktop);

    if (auto item = desktop->getSelection()->singleItem()) {
        shape_editor->set_item(item);
    }

    sel_changed_connection = desktop->getSelection()->connectChanged(
        sigc::mem_fun(*this, &Box3dTool::selection_changed)
    );

    _vpdrag = std::make_unique<Box3D::VPDrag>(desktop->getDocument());

    auto prefs = Preferences::get();

    if (prefs->getBool("/tools/shapes/selcue")) {
        enableSelectionCue();
    }

    if (prefs->getBool("/tools/shapes/gradientdrag")) {
        enableGrDrag();
    }
}

Box3dTool::~Box3dTool()
{
    ungrabCanvasEvents();
    finishItem();

    enableGrDrag(false);

    delete shape_editor;
    shape_editor = nullptr;
}

/**
 * Callback that processes the "changed" signal on the selection;
 * destroys old and creates new knotholder.
 */
void Box3dTool::selection_changed(Selection *selection)
{
    shape_editor->unset_item();
    shape_editor->set_item(selection->singleItem());

    if (selection->perspList().size() == 1) {
        // selecting a single box changes the current perspective
        _desktop->doc()->setCurrentPersp3D(selection->perspList().front());
    }
}

/* Create a default perspective in document defs if none is present (which can happen, among other
 * circumstances, after 'vacuum defs' or when a pre-0.46 file is opened).
 */
static void ensure_persp_in_defs(SPDocument *document)
{
    auto defs = document->getDefs();

    for (auto const &child : defs->children) {
        if (is<Persp3D>(&child)) {
            return;
        }
    }

    document->setCurrentPersp3D(Persp3D::create_xml_element(document));
}

bool Box3dTool::item_handler(SPItem *item, CanvasEvent const &event)
{
    if (event.type() == EventType::BUTTON_PRESS) {
        auto &button_event = static_cast<ButtonPressEvent const &>(event);
        if (button_event.num_press == 1 && button_event.button == 1) {
            setup_for_drag_start(button_event);
        }
    }

    return ToolBase::item_handler(item, event);
}

bool Box3dTool::root_handler(CanvasEvent const &event)
{
    auto document = _desktop->getDocument();
    auto const y_dir = _desktop->yaxisdir();
    auto selection = _desktop->getSelection();

    auto prefs = Preferences::get();
    double const snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);
    tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

    auto cur_persp = document->getCurrentPersp3D();

    bool ret = false;

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (event.num_press != 1 || event.button != 1) {
                return;
            }

            auto const button_w = event.pos;
            auto button_dt = _desktop->w2d(button_w);

            saveDragOrigin(button_w);

            // remember clicked box3d, *not* disregarding groups (since a 3D box is a group), honoring Alt
            item_to_select = sp_event_context_find_item(_desktop, button_w,
                                                        mod_select_force_drag->active(event.modifiers),
                                                        mod_select_in_groups->active(event.modifiers));

            dragging = true;

            auto &m = _desktop->getNamedView()->snap_manager;
            m.setup(_desktop, true, box3d.get());
            m.freeSnapReturnByRef(button_dt, Inkscape::SNAPSOURCE_NODE_HANDLE);
            m.unSetup();
            center = button_dt;

            drag_origin = button_dt;
            drag_ptB = button_dt;
            drag_ptC = button_dt;

            // This can happen after saving when the last remaining perspective was purged and must be recreated.
            if (!cur_persp) {
                ensure_persp_in_defs(document);
                cur_persp = document->getCurrentPersp3D();
            }

            // Projective preimages of clicked point under current perspective.
            drag_origin_proj = cur_persp->perspective_impl->tmat.preimage(button_dt, 0, Proj::Z);
            drag_ptB_proj = drag_origin_proj;
            drag_ptC_proj = drag_origin_proj;
            drag_ptC_proj.normalize();
            drag_ptC_proj[Proj::Z] = 0.25;

            grabCanvasEvents();
            ret = true;
        },

    [&] (MotionEvent const &event) {
        if (dragging && event.modifiers & GDK_BUTTON1_MASK) {
            if (!cur_persp) {
                // Can happen if perspective is deleted while dragging, e.g. on document closure.
                ret = true;
                return;
            }

            if (!checkDragMoved(event.pos)) {
                return;
            }

            auto const motion_w = event.pos;
            auto motion_dt = _desktop->w2d(motion_w);

            auto &m = _desktop->getNamedView()->snap_manager;
            m.setup(_desktop, true, box3d.get());
            m.freeSnapReturnByRef(motion_dt, Inkscape::SNAPSOURCE_NODE_HANDLE);

            two_dimension_dragged = box3d && mod_box3d_extrude_two->active(event.modifiers);
            extruded = box3d && (mod_box3d_extrude_one->active(event.modifiers) ||
                                 mod_box3d_extrude_two->active(event.modifiers));

            if (!extruded) {
                drag_ptB = motion_dt;
                drag_ptC = motion_dt;

                drag_ptB_proj = cur_persp->perspective_impl->tmat.preimage(motion_dt, 0, Proj::Z);
                drag_ptC_proj = drag_ptB_proj;
                drag_ptC_proj.normalize();
                drag_ptC_proj[Proj::Z] = 0.25;
            } else {
                // Without Ctrl, motion of the extruded corner is constrained to the
                // perspective line from drag_ptB to vanishing point Y.
                if (!two_dimension_dragged) {
                    // snapping
                    auto pline = Box3D::PerspectiveLine(drag_ptB, Proj::Z, document->getCurrentPersp3D());
                    drag_ptC = pline.closest_to(motion_dt);

                    drag_ptB_proj.normalize();
                    drag_ptC_proj = cur_persp->perspective_impl->tmat.preimage(drag_ptC, drag_ptB_proj[Proj::X], Proj::X);
                } else {
                    drag_ptC = motion_dt;

                    drag_ptB_proj.normalize();
                    drag_ptC_proj = cur_persp->perspective_impl->tmat.preimage(motion_dt, drag_ptB_proj[Proj::X], Proj::X);
                }

                m.freeSnapReturnByRef(drag_ptC, Inkscape::SNAPSOURCE_NODE_HANDLE);
            }

            m.unSetup();

            drag();

            ret = true;
        } else if (!sp_event_context_knot_mouseover()) {
            auto &m = _desktop->getNamedView()->snap_manager;
            m.setup(_desktop);

            auto const motion_w = event.pos;
            auto motion_dt = _desktop->w2d(motion_w);
            m.preSnap(Inkscape::SnapCandidatePoint(motion_dt, Inkscape::SNAPSOURCE_NODE_HANDLE));
            m.unSetup();
        }
    },

        [&] (ButtonReleaseEvent const &event) {
            xyp = {};

            if (event.button != 1) {
                return;
            }

            dragging = false;
            discard_delayed_snap_event();

            if (!within_tolerance) {
                // we've been dragging (or switched tools if !box3d), finish the box
                if (box3d) {
                    // update while creating inside a LPE group
                    sp_lpe_item_update_patheffect(this->box3d.get(), true, true);
                    _desktop->getSelection()->set(box3d.get()); // Updating the selection will send signals to the box3d-toolbar ...
                }
                finishItem(); // .. but finishItem() will be called from the destructor too and shall NOT fire such signals!
            } else if (item_to_select) {
                // no dragging, select clicked box3d if any
                if (mod_select_add_to->active(event.modifiers)) {
                    selection->toggle(item_to_select.get());
                } else {
                    selection->set(item_to_select.get());
                }
            } else {
                // click in an empty space
                selection->clear();
            }

            item_to_select = nullptr;
            ret = true;
            ungrabCanvasEvents();
        },

    [&] (KeyPressEvent const &event) {
        switch (get_latin_keyval(event)) {
        case GDK_KEY_Up:
        case GDK_KEY_Down:
        case GDK_KEY_KP_Up:
        case GDK_KEY_KP_Down:
            // prevent the zoom field from activation
            if (!mod_ctrl_only(event)) {
                ret = true;
            }
            break;

        case GDK_KEY_bracketright:
            document->getCurrentPersp3D()->rotate_VP (Proj::X, 180 / snaps * y_dir, mod_alt(event));
            DocumentUndo::done(document, RC_("Undo", "Change perspective (angle of PLs)"), INKSCAPE_ICON("draw-cuboid"));
            ret = true;
            break;

        case GDK_KEY_bracketleft:
            document->getCurrentPersp3D()->rotate_VP (Proj::X, -180 / snaps * y_dir, mod_alt(event));
            DocumentUndo::done(document, RC_("Undo", "Change perspective (angle of PLs)"), INKSCAPE_ICON("draw-cuboid"));
            ret = true;
            break;

        case GDK_KEY_parenright:
            document->getCurrentPersp3D()->rotate_VP (Proj::Y, 180 / snaps * y_dir, mod_alt(event));
            DocumentUndo::done(document, RC_("Undo", "Change perspective (angle of PLs)"), INKSCAPE_ICON("draw-cuboid"));
            ret = true;
            break;

        case GDK_KEY_parenleft:
            document->getCurrentPersp3D()->rotate_VP (Proj::Y, -180 / snaps * y_dir, mod_alt(event));
            DocumentUndo::done(document, RC_("Undo", "Change perspective (angle of PLs)"), INKSCAPE_ICON("draw-cuboid"));
            ret = true;
            break;

        case GDK_KEY_braceright:
            document->getCurrentPersp3D()->rotate_VP (Proj::Z, 180 / snaps * y_dir, mod_alt(event));
            DocumentUndo::done(document, RC_("Undo", "Change perspective (angle of PLs)"), INKSCAPE_ICON("draw-cuboid"));
            ret = true;
            break;

        case GDK_KEY_braceleft:
            document->getCurrentPersp3D()->rotate_VP (Proj::Z, -180 / snaps * y_dir, mod_alt(event));
            DocumentUndo::done(document, RC_("Undo", "Change perspective (angle of PLs)"), INKSCAPE_ICON("draw-cuboid"));
            ret = true;
            break;

        case GDK_KEY_g:
        case GDK_KEY_G:
            if (mod_shift_only(event)) {
                _desktop->getSelection()->toGuides();
                ret = true;
            }
            break;

        case GDK_KEY_p:
        case GDK_KEY_P:
            if (mod_shift_only(event)) {
                if (document->getCurrentPersp3D()) {
                    document->getCurrentPersp3D()->print_debugging_info();
                }
                ret = true;
            }
            break;

        case GDK_KEY_x:
        case GDK_KEY_X:
            if (mod_shift_only(event)) {
                Persp3D::toggle_VPs(selection->perspList(), Proj::X);
                _vpdrag->updateLines(); // FIXME: Shouldn't this be done automatically?
                ret = true;
            }
            break;

        case GDK_KEY_y:
        case GDK_KEY_Y:
            if (mod_shift_only(event)) {
                Persp3D::toggle_VPs(selection->perspList(), Proj::Y);
                _vpdrag->updateLines(); // FIXME: Shouldn't this be done automatically?
                ret = true;
            }
            break;

        case GDK_KEY_z:
        case GDK_KEY_Z:
            if (mod_shift_only(event)) {
                Persp3D::toggle_VPs(selection->perspList(), Proj::Z);
                _vpdrag->updateLines(); // FIXME: Shouldn't this be done automatically?
                ret = true;
            }
            break;

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
                    // we've been dragging (or switched tools if !box3d), finish the box
                    if (box3d) {
                        _desktop->getSelection()->set(box3d.get()); // Updating the selection will send signals to the box3d-toolbar ...
                    }
                    finishItem(); // .. but finishItem() will be called from the destructor too and shall NOT fire such signals!
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

    [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

void Box3dTool::drag()
{
    if (!box3d) {
        if (!have_viable_layer(_desktop, defaultMessageContext())) {
            return;
        }

        // Create object
        auto newbox3d = SPBox3D::createBox3D(currentLayer());

        // Set group style. This style isn't visible, since only the faces are visible.
        _desktop->applyCurrentOrToolStyle(newbox3d, "/tools/shapes/3dbox", false);

        box3d = newbox3d;
        auto use_current = Preferences::get()->getString("/tools/shapes/3dbox/usecurrent", "0"); // "0", "1", or "3dbox"

        // TODO: Incorporate this in box3d-side.cpp!
        for (int i = 0; i < 6; ++i) {
            auto side = Box3DSide::createBox3DSide(newbox3d);

            auto [plane, front_or_rear] = Box3D::int_to_face(i);

            plane = Box3D::is_plane(plane) ? plane : Box3D::orth_plane_or_axis(plane);
            side->dir1 = Box3D::extract_first_axis_direction(plane);
            side->dir2 = Box3D::extract_second_axis_direction(plane);
            side->front_or_rear = front_or_rear;

            // Set style
            _desktop->applyCurrentOrToolStyle(side,
                Glib::ustring("/tools/shapes/3dbox/") + side->axes_string(),
                true,
                (use_current.size() == 1) ? use_current : side->axes_string()
            );
            side->updateRepr(); // calls Box3DSide::write() and updates, e.g., the axes string description
        }

        box3d->transform = currentLayer()->i2doc_affine().inverse();
        box3d->set_z_orders();
        box3d->updateRepr();

        // TODO: It would be nice to show the VPs during dragging, but since there is no selection
        //       at this point (only after finishing the box), we must do this "manually"
        // _vpdrag->updateDraggers();
    }

    box3d->orig_corner0 = drag_origin_proj;
    box3d->orig_corner7 = drag_ptC_proj;

    box3d->check_for_swapped_coords();

    // we need to call this from here (instead of from SPBox3D::position_set(), for example)
    // because z-order setting must not interfere with display updates during undo/redo.
    box3d->set_z_orders();

    box3d->position_set();

    // status text
    message_context->setF(Inkscape::NORMAL_MESSAGE,
                          _("<b>3D Box</b>; with <b>%s</b> to extrude along the Z axis"),
                          mod_box3d_extrude_one->get_label().c_str());
}

void Box3dTool::finishItem()
{
    message_context->clear();
    two_dimension_dragged = false;
    extruded = false;

    if (box3d) {
        if ((box3d->orig_corner0[Proj::X] == box3d->orig_corner7[Proj::X]) +
                (box3d->orig_corner0[Proj::Y] == box3d->orig_corner7[Proj::Y]) +
                (box3d->orig_corner0[Proj::Z] == box3d->orig_corner7[Proj::Z]) >=
            2) {
            this->cancel(); // Don't allow the creation of zero sized 3d boxes
            return;
        }
        auto doc = _desktop->getDocument();

        if (!doc || !doc->getCurrentPersp3D()) {
            return;
        }

        box3d->orig_corner0 = drag_origin_proj;
        box3d->orig_corner7 = drag_ptC_proj;

        box3d->updateRepr();
        box3d->doWriteTransform(box3d->transform, nullptr, true);

        box3d->relabel_corners();

        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Create 3D box"), INKSCAPE_ICON("draw-cuboid"));

        box3d = nullptr;
    }
}

void Box3dTool::cancel()
{
    _desktop->getSelection()->clear();
    ungrabCanvasEvents();

    if (box3d) {
        box3d->deleteObject();
    }

    this->within_tolerance = false;
    xyp = {};
    this->item_to_select = nullptr;

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
