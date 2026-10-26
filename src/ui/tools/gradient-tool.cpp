// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gradient drawing and editing tool
 *
 * Authors:
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Abhishek Sharma
 *
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2005 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <unordered_set>
#include <glibmm/i18n.h>

#include "document-undo.h"
#include "gradient-chemistry.h"
#include "gradient-drag.h"
#include "message-context.h"
#include "rubberband.h"
#include "selection-chemistry.h"
#include "selection.h"

#include "object/sp-namedview.h"
#include "object/sp-stop.h"

#include "display/control/canvas-item-curve.h"

#include "ui/icon-names.h"
#include "ui/tools/gradient-tool.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::DocumentUndo;

namespace Inkscape::UI::Tools {

GradientTool::GradientTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/gradient", "gradient.svg")
    , mod_freehand_angle_snapping(Modifiers::Modifier::get(Modifiers::Type::FREEHAND_ANGLE_SNAPPING))
    , mod_gradient_create(Modifiers::Modifier::get(Modifiers::Type::GRADIENT_CREATE))
    , mod_node_insert(Modifiers::Modifier::get(Modifiers::Type::NODE_INSERT))
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
    , mod_select_force_drag(Modifiers::Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
{
    // TODO: This value is overwritten in the root handler
    tolerance = 6;

    auto prefs = Preferences::get();

    if (prefs->getBool("/tools/gradient/selcue", true)) {
        enableSelectionCue();
    }

    enableGrDrag();

    auto selection = desktop->getSelection();
    selcon = selection->connectChanged([this] (auto) { selection_changed(); });

    subselcon = desktop->connect_gradient_stop_selected([this] (SPStop *stop) {
        selection_changed();
        if (stop) {
            // Sync stop selection.
            _grdrag->selectByStop(stop, false, true);
        }
    });

    selection_changed();
}

GradientTool::~GradientTool()
{
    enableGrDrag(false);
}

// This must match GrPointType enum sp-gradient.h
// We should move this to a shared header (can't simply move to gradient.h since that would require
// including <glibmm/i18n.h> which messes up "N_" in extensions... argh!).
static char const *gr_handle_descr[] = {
    N_("Linear gradient <b>start</b>"), // POINT_LG_BEGIN
    N_("Linear gradient <b>end</b>"),
    N_("Linear gradient <b>mid stop</b>"),
    N_("Radial gradient <b>center</b>"),
    N_("Radial gradient <b>radius</b>"),
    N_("Radial gradient <b>radius</b>"),
    N_("Radial gradient <b>focus</b>"), // POINT_RG_FOCUS
    N_("Radial gradient <b>mid stop</b>"),
    N_("Radial gradient <b>mid stop</b>"),
    N_("Mesh gradient <b>corner</b>"),
    N_("Mesh gradient <b>handle</b>"),
    N_("Mesh gradient <b>tensor</b>")
};

void GradientTool::selection_changed()
{
    auto const selection = _desktop->getSelection();
    if (!selection) {
        return;
    }
    unsigned const n_obj = std::ranges::distance(selection->items());

    if (!_grdrag->isNonEmpty() || selection->isEmpty()) {
        return;
    }
    unsigned const n_tot = _grdrag->numDraggers();
    unsigned const n_sel = _grdrag->numSelected();

    // Note: The use of ngettext in the following code is intentional even if the English singular form would never be used.
    if (n_sel == 1) {
        if (_grdrag->singleSelectedDraggerNumDraggables() == 1) {
            auto const message = Glib::ustring::format(
                // TRANSLATORS: %s will be substituted with the point name (see previous messages); This is part of a compound message
                _("%s selected"),
                // TRANSLATORS: Mind the space in front. This is part of a compound message
                ngettext(" out of %d gradient handle"," out of %d gradient handles", n_tot),
                ngettext(" on %d selected object"," on %d selected objects", n_obj));
            message_context->setF(NORMAL_MESSAGE,
                                  message.c_str(), _(gr_handle_descr[_grdrag->singleSelectedDraggerSingleDraggableType()]), n_tot, n_obj);
        } else {
            auto const message = Glib::ustring::format(
                // TRANSLATORS: This is a part of a compound message (out of two more indicating: grandint handle count & object count)
                ngettext("One handle merging %d stop (drag with <b>Shift</b> to separate) selected",
                         "One handle merging %d stops (drag with <b>Shift</b> to separate) selected", _grdrag->singleSelectedDraggerNumDraggables()),
                ngettext(" out of %d gradient handle"," out of %d gradient handles", n_tot),
                ngettext(" on %d selected object"," on %d selected objects", n_obj));
            message_context->setF(NORMAL_MESSAGE, message.c_str(), _grdrag->singleSelectedDraggerNumDraggables(), n_tot, n_obj);
        }
    } else if (n_sel > 1) {
        // TRANSLATORS: The plural refers to number of selected gradient handles. This is part of a compound message (part two indicates selected object count)
        auto const message = Glib::ustring::format(
            ngettext("<b>%d</b> gradient handle selected out of %d","<b>%d</b> gradient handles selected out of %d",n_sel),
            // TRANSLATORS: Mind the space in front. (Refers to gradient handles selected). This is part of a compound message
            ngettext(" on %d selected object"," on %d selected objects", n_obj));
        message_context->setF(NORMAL_MESSAGE, message.c_str(), n_sel, n_tot, n_obj);
    } else if (n_sel == 0) {
        message_context->setF(NORMAL_MESSAGE,
                              // TRANSLATORS: The plural refers to number of selected objects
                              ngettext("<b>No</b> gradient handles selected out of %d on %d selected object",
                                       "<b>No</b> gradient handles selected out of %d on %d selected objects", n_obj), n_tot, n_obj);
    }
}

void GradientTool::select_next()
{
    g_assert(_grdrag);
    auto d = _grdrag->select_next();
    _desktop->scroll_to_point(d->point);
}

void GradientTool::select_prev()
{
    g_assert(_grdrag);
    auto d = _grdrag->select_prev();
    _desktop->scroll_to_point(d->point);
}

SPItem *GradientTool::is_over_curve(Geom::Point const &event_p)
{
    // Translate mouse point into proper coord system: needed later.
    mousepoint_doc = _desktop->w2d(event_p);

    for (auto &it : _grdrag->item_curves) {
        if (it.curve->contains(event_p, tolerance)) {
            return it.item;
        }
    }

    return nullptr;
}

struct StopIntervals
{
    std::vector<Geom::Point> coords;
    std::vector<SPStop*> these_stops;
    std::vector<SPStop*> next_stops;
};

static auto get_stop_intervals(GrDrag *drag)
{
    StopIntervals result;

    // for all selected draggers
    for (auto const dragger : drag->selected) {
        // remember the coord of the dragger to reselect it later
        result.coords.emplace_back(dragger->point);
        // for all draggables of dragger
        for (auto const d : dragger->draggables) {
            // find the gradient
            auto const gradient = getGradient(d->item, d->fill_or_stroke);
            auto const vector = sp_gradient_get_forked_vector_if_necessary(gradient, false);

            // these draggable types cannot have a next draggable to insert a stop between them
            if (d->point_type == POINT_LG_END ||
                d->point_type == POINT_RG_FOCUS ||
                d->point_type == POINT_RG_R1 ||
                d->point_type == POINT_RG_R2)
            {
                continue;
            }

            // from draggables to stops
            auto const this_stop = sp_get_stop_i(vector, d->point_i);
            auto const next_stop = this_stop->getNextStop();
            auto const last_stop = sp_last_stop(vector);

            auto const fs = d->fill_or_stroke;
            auto const item = d->item;
            auto const type = d->point_type;
            auto const p_i = d->point_i;

            // if there's a next stop,
            if (next_stop) {
                GrDragger *dnext = nullptr;
                // find its dragger
                // (complex because it may have different types, and because in radial,
                // more than one dragger may correspond to a stop, so we must distinguish)
                if (type == POINT_LG_BEGIN || type == POINT_LG_MID) {
                    if (next_stop == last_stop) {
                        dnext = drag->getDraggerFor(item, POINT_LG_END, p_i+1, fs);
                    } else {
                        dnext = drag->getDraggerFor(item, POINT_LG_MID, p_i+1, fs);
                    }
                } else { // radial
                    if (type == POINT_RG_CENTER || type == POINT_RG_MID1) {
                        if (next_stop == last_stop) {
                            dnext = drag->getDraggerFor(item, POINT_RG_R1, p_i+1, fs);
                        } else {
                            dnext = drag->getDraggerFor(item, POINT_RG_MID1, p_i+1, fs);
                        }
                    }
                    if ((type == POINT_RG_MID2) ||
                        (type == POINT_RG_CENTER && dnext && !dnext->isSelected()))
                    {
                        if (next_stop == last_stop) {
                            dnext = drag->getDraggerFor(item, POINT_RG_R2, p_i+1, fs);
                        } else {
                            dnext = drag->getDraggerFor(item, POINT_RG_MID2, p_i+1, fs);
                        }
                    }
                }

                // if both adjacent draggers selected,
                if ((std::find(result.these_stops.begin(), result.these_stops.end(), this_stop) == result.these_stops.end()) && dnext && dnext->isSelected()) {
                    // remember the coords of the future dragger to select it
                    result.coords.emplace_back((dragger->point + dnext->point) / 2);

                    // do not insert a stop now, it will confuse the loop;
                    // just remember the stops
                    result.these_stops.emplace_back(this_stop);
                    result.next_stops.emplace_back(next_stop);
                }
            }
        }
    }

    return result;
}

void GradientTool::add_stops_between_selected_stops()
{
    if (_grdrag->hasSelection()) {
        auto dragger = *_grdrag->selected.begin();
        auto draggable = dragger->draggables[0];
        auto gradient = getGradient(draggable->item, draggable->fill_or_stroke);
        auto vector = sp_gradient_get_forked_vector_if_necessary(gradient, false);

        // Treat single stop gradients separately.
        if (vector->getStopCount() == 1) {
            auto newstop = sp_gradient_add_stop(vector, vector->getFirstStop());
            gradient->ensureVector();
            _grdrag->updateDraggers();
            _grdrag->local_change = true;
            _grdrag->selectByStop(newstop);
            DocumentUndo::done(gradient->document, RC_("Undo", "Add gradient stop"), INKSCAPE_ICON("color-gradient"));
            return;
        }
    }

    auto ret = get_stop_intervals(_grdrag);

    if (ret.these_stops.empty() && _grdrag->numSelected() == 1) {
        // if a single stop is selected, add between that stop and the next one
        auto dragger = *_grdrag->selected.begin();
        for (auto d : dragger->draggables) {
            if (d->point_type == POINT_RG_FOCUS) {
                // There are 2 draggables at the center (start) of a radial gradient
                // To avoid creating 2 separate stops, ignore this draggable point type
                continue;
            }

            // Mark the second-to-last stop as the current stop if the last gradient point is
            // selected to insert a stop.
            auto const point_i =
                d->point_type == POINT_LG_END || d->point_type == POINT_RG_R1 || d->point_type == POINT_RG_R2
                    ? d->point_i - 1
                    : d->point_i;
            auto gradient = getGradient(d->item, d->fill_or_stroke);
            auto vector = sp_gradient_get_forked_vector_if_necessary(gradient, false);
            if (auto this_stop = sp_get_stop_i(vector, point_i)) {
                if (auto next_stop = this_stop->getNextStop()) {
                    ret.these_stops.emplace_back(this_stop);
                    ret.next_stops.emplace_back(next_stop);
                }
            }
        }
    }

    // now actually create the new stops
    auto i = ret.these_stops.rbegin();
    auto j = ret.next_stops.rbegin();
    std::vector<SPStop *> new_stops;
    SPDocument *doc = nullptr;

    for (; i != ret.these_stops.rend() && j != ret.next_stops.rend(); ++i, ++j) {
        SPStop *this_stop = *i;
        SPStop *next_stop = *j;
        float offset = (this_stop->offset + next_stop->offset) / 2;
        if (auto grad = cast<SPGradient>(this_stop->parent)) {
            doc = grad->document;
            auto new_stop = sp_vector_add_stop(grad, this_stop, next_stop, offset);
            new_stops.emplace_back(new_stop);
            grad->ensureVector();
        }
    }

    if (!ret.these_stops.empty() && doc) {
        DocumentUndo::done(doc, RC_("Undo", "Add gradient stop"), INKSCAPE_ICON("color-gradient"));
        _grdrag->updateDraggers();
        // so that it does not automatically update draggers in idle loop, as this would deselect
        _grdrag->local_change = true;

        // select the newly created stops
        for (auto s : new_stops) {
            _grdrag->selectByStop(s);
        }
    }
}

/**
 * Remove unnecessary stops in the adjacent currently selected stops
 *
 * For selected stops that are adjacent to each other, remove
 * stops that don't change the gradient visually, within a range of tolerance.
 *
 * @param tolerance maximum difference between stop and expected color at that position
 */
void GradientTool::simplify(double tolerance)
{
    SPDocument *doc = nullptr;
    GrDrag *drag = _grdrag;

    auto const ret = get_stop_intervals(drag);

    std::unordered_set<SPStop *> todel;

    auto i = ret.these_stops.begin();
    auto j = ret.next_stops.begin();
    for (; i != ret.these_stops.end() && j != ret.next_stops.end(); ++i, ++j) {
        SPStop *stop0 = *i;
        SPStop *stop1 = *j;

        // find the next adjacent stop if it exists and is in selection
        auto i1 = std::find(ret.these_stops.begin(), ret.these_stops.end(), stop1);
        if (i1 != ret.these_stops.end()) {
            if (ret.next_stops.size() > i1 - ret.these_stops.begin()) {
                SPStop *stop2 = *(ret.next_stops.begin() + (i1 - ret.these_stops.begin()));

                if (todel.find(stop0) != todel.end() || todel.find(stop2) != todel.end()) {
                    continue;
                }

                // compare color of stop1 to the average color of stop0 and stop2
                auto coord = (stop1->offset - stop0->offset) / (stop2->offset - stop0->offset);
                auto avg = stop0->getColor().averaged(stop2->getColor(), coord);
                if (avg.difference(stop1->getColor()) < tolerance) {
                    todel.emplace(stop1);
                }
            }
        }
    }

    for (auto stop : todel) {
        doc = stop->document;
        auto parent = stop->getRepr()->parent();
        parent->removeChild(stop->getRepr());
    }

    if (!todel.empty()) {
        DocumentUndo::done(doc, RC_("Undo", "Simplify gradient"), INKSCAPE_ICON("color-gradient"));
        drag->local_change = true;
        drag->updateDraggers();
        drag->selectByCoords(ret.coords);
    }
}

void GradientTool::add_stop_near_point(SPItem *item, Geom::Point const &mouse_p)
{
    // item is the selected item. mouse_p the location in doc coordinates of where to add the stop
    auto newstop = get_drag()->addStopNearPoint(item, mouse_p, tolerance / _desktop->current_zoom());

    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Add gradient stop"), INKSCAPE_ICON("color-gradient"));

    get_drag()->updateDraggers();
    get_drag()->local_change = true;
    get_drag()->selectByStop(newstop);
}

bool GradientTool::root_handler(CanvasEvent const &event)
{
    auto selection = _desktop->getSelection();

    auto prefs = Preferences::get();
    tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

    bool ret = false;

    inspect_event(event,
    [&] (ButtonPressEvent const &event) {
        if (event.button != 1) {
            return;
        }

        if (event.num_press == 2) {

            if (is_over_curve(event.pos)) {
                // we take the first item in selection, because with doubleclick, the first click
                // always resets selection to the single object under cursor
                add_stop_near_point(selection->items().front(), mousepoint_doc);
            } else {
                for (auto item : selection->items()) {
                    auto const new_type = static_cast<SPGradientType>(prefs->getInt("/tools/gradient/newgradient", SP_GRADIENT_TYPE_LINEAR));
                    auto const fsmode = prefs->getInt("/tools/gradient/newfillorstroke", 1) != 0 ? FOR_FILL : FOR_STROKE;

                    SPGradient *vector = sp_gradient_vector_for_object(_desktop->getDocument(), _desktop, item, fsmode);

                    SPGradient *priv = sp_item_set_gradient(item, vector, new_type, fsmode);
                    sp_gradient_reset_to_userspace(priv, item);
                }
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Create default gradient"), INKSCAPE_ICON("color-gradient"));
            }
            ret = true;

        } else if (event.num_press == 1) {

            saveDragOrigin(event.pos);
            dragging = true;

            auto button_dt = _desktop->w2d(event.pos);
            if (mod_select_add_to->active(event.modifiers) && !mod_gradient_create->active(event.modifiers)) {
                auto rubberband = Rubberband::get(_desktop);
                rubberband->start(_desktop, button_dt);
            } else {
                // remember clicked item, disregarding groups, honoring Alt; do nothing with Crtl to
                // enable Ctrl+doubleclick of exactly the selected item(s)
                if (!mod_gradient_create->active(event.modifiers)) {
                    auto force_drag = mod_select_force_drag->active(event.modifiers);
                    item_to_select = sp_event_context_find_item(_desktop, event.pos, force_drag, true);
                }

                if (!selection->isEmpty()) {
                    auto &m = _desktop->getNamedView()->snap_manager;
                    m.setup(_desktop);
                    m.freeSnapReturnByRef(button_dt, SNAPSOURCE_NODE_HANDLE);
                    m.unSetup();
                }

                origin = button_dt;
            }
            ret = true;
        }
    },

    [&] (MotionEvent const &event) {
        if (dragging && (event.modifiers & GDK_BUTTON1_MASK)) {
            if (!checkDragMoved(event.pos)) {
                return;
            }

            auto const motion_dt = _desktop->w2d(event.pos);

            if (Rubberband::get(_desktop)->isStarted()) {
                Rubberband::get(_desktop)->move(motion_dt);
                defaultMessageContext()->set(NORMAL_MESSAGE, _("<b>Draw around</b> handles to select them"));
            } else {
                drag(motion_dt, event.time);
            }

            gobble_motion_events(GDK_BUTTON1_MASK);

            ret = true;
        } else {
            if (!_grdrag->mouseOver() && !selection->isEmpty()) {
                auto &m = _desktop->getNamedView()->snap_manager;
                m.setup(_desktop);

                auto const motion_dt = _desktop->w2d(event.pos);

                m.preSnap(SnapCandidatePoint(motion_dt, SNAPSOURCE_OTHER_HANDLE));
                m.unSetup();
            }

            auto item = is_over_curve(event.pos);

            if (cursor_addnode && !item) {
                set_cursor("gradient.svg");
                cursor_addnode = false;
            } else if (!cursor_addnode && item) {
                set_cursor("gradient-add.svg");
                cursor_addnode = true;
            }
        }
    },

        [&] (ButtonReleaseEvent const &event) {
            if (event.button != 1) {
                return;
            }

            xyp = {};

            auto item = is_over_curve(event.pos);

            if (mod_node_insert->active(event.modifiers)) {
                if (item) {
                    add_stop_near_point(item, mousepoint_doc);
                    ret = true;
                }
            } else {
                dragging = false;

                // unless clicked with intent to create (to enable Ctrl+doubleclick).
                if (mod_gradient_create->active(event.modifiers) && !mod_select_add_to->active(event.modifiers)) {
                    ret = true;
                    Rubberband::get(_desktop)->stop();
                    return;
                }

                if (!within_tolerance) {
                    // we've been dragging, either do nothing (grdrag handles that),
                    // or rubberband-select if we have rubberband
                    auto r = Rubberband::get(_desktop);

                    if (r->isStarted() && !within_tolerance) {
                        // this was a rubberband drag
                        if (r->getMode() == Rubberband::Mode::RECT) {
                            _grdrag->selectRect(*r->getRectangle());
                        }
                    }
                } else if (item_to_select) {
                    if (item) {
                        // Clicked on an existing gradient line, don't change selection. This stops
                        // possible change in selection during a double click with overlapping objects
                    } else {
                        // no dragging, select clicked item if any
                        if (mod_select_add_to->active(event.modifiers)) {
                            selection->toggle(item_to_select.get());
                        } else {
                            _grdrag->deselectAll();
                            selection->set(item_to_select.get());
                        }
                    }
                } else {
                    // click in an empty space; do the same as Esc
                    if (!_grdrag->selected.empty()) {
                        _grdrag->deselectAll();
                    } else {
                        selection->clear();
                    }
                }

                item_to_select = nullptr;
                ret = true;
            }

            Rubberband::get(_desktop)->stop();
        },

    [&] (KeyPressEvent const &event) {
        auto keyval = get_latin_keyval(event);

        if (Modifiers::keyval_is_a_modifier(keyval)) {
            Modifiers::responsive_tooltip_with_labels(
                defaultMessageContext(), event, 2,
                Modifiers::Type::FREEHAND_ANGLE_SNAPPING, _("Snap gradient angle"),
                Modifiers::Type::TRANS_OFF_CENTER, _("Draw gradient around the starting point")
            );
        }

        switch (keyval) {
        case GDK_KEY_A:
        case GDK_KEY_a:
            if (mod_ctrl_only(event) && _grdrag->isNonEmpty()) {
                _grdrag->selectAll();
                ret = true;
            }
            break;

        case GDK_KEY_L:
        case GDK_KEY_l:
            if (mod_ctrl_only(event) && _grdrag->isNonEmpty() && _grdrag->hasSelection()) {
                simplify(1e-4);
                ret = true;
            }
            break;

        case GDK_KEY_Escape:
            if (!_grdrag->selected.empty()) {
                _grdrag->deselectAll();
            } else {
                SelectionHelper::selectNone(_desktop);
            }
            ret = true;
            //TODO: make dragging escapable by Esc
            break;

        case GDK_KEY_r:
        case GDK_KEY_R:
            if (mod_shift_only(event)) {
                sp_gradient_reverse_selected_gradients(_desktop);
                ret = true;
            }
            break;

        case GDK_KEY_Insert:
        case GDK_KEY_KP_Insert:
            // with any modifiers:
            add_stops_between_selected_stops();
            ret = true;
            break;

        case GDK_KEY_i:
        case GDK_KEY_I:
            if (mod_shift_only(event)) {
                // Shift+I - insert stops (alternate keybinding for keyboards
                //           that don't have the Insert key)
                add_stops_between_selected_stops();
                ret = true;
            }
            break;

        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:
        case GDK_KEY_BackSpace:
            ret = deleteSelectedDrag(mod_ctrl_only(event));
            break;

        case GDK_KEY_Tab:
            if (hasGradientDrag()) {
                select_next();
                ret = true;
            }
            break;

        case GDK_KEY_ISO_Left_Tab:
            if (hasGradientDrag()) {
                select_prev();
                ret = true;
            }
            break;

        default:
            ret = _grdrag->key_press_handler(event);
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

// Creates a new linear or radial gradient.
void GradientTool::drag(Geom::Point const &pt, uint32_t etime)
{
    auto selection = _desktop->getSelection();
    auto document = _desktop->getDocument();

    if (!selection->isEmpty()) {
        auto prefs = Preferences::get();
        int type = prefs->getInt("/tools/gradient/newgradient", 1);
        auto fill_or_stroke = prefs->getInt("/tools/gradient/newfillorstroke", 1) != 0 ? FOR_FILL : FOR_STROKE;

        SPGradient *vector;
        if (item_to_select) {
            // pick color from the object where drag started
            vector = sp_gradient_vector_for_object(document, _desktop, item_to_select.get(), fill_or_stroke);
        } else {
            // Starting from empty space:
            // Sort items so that the topmost comes last
            auto items = selection->items_vector();
            std::sort(items.begin(), items.end(), sp_item_repr_compare_position_bool);
            // take topmost
            vector = sp_gradient_vector_for_object(document, _desktop, items.back(), fill_or_stroke);
        }

        // HACK: reset fill-opacity - that 0.75 is annoying; BUT remove this when we have an opacity slider for all tabs
        auto css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "fill-opacity", "1.0");

        for (auto item : selection->items()) {

            //FIXME: see above
            sp_repr_css_change_recursive(item->getRepr(), css, "style");

            sp_item_set_gradient(item, vector, static_cast<SPGradientType>(type), fill_or_stroke);

            if (type == SP_GRADIENT_TYPE_LINEAR) {
                sp_item_gradient_set_coords(item, POINT_LG_BEGIN, 0, origin, fill_or_stroke, true, false);
                sp_item_gradient_set_coords (item, POINT_LG_END, 0, pt, fill_or_stroke, true, false);
            } else if (type == SP_GRADIENT_TYPE_RADIAL) {
                sp_item_gradient_set_coords(item, POINT_RG_CENTER, 0, origin, fill_or_stroke, true, false);
                sp_item_gradient_set_coords (item, POINT_RG_R1, 0, pt, fill_or_stroke, true, false);
            }
            item->requestModified(SP_OBJECT_MODIFIED_FLAG);
        }

        sp_repr_css_attr_unref(css);

        if (_grdrag) {
            _grdrag->updateDraggers();
            // prevent regenerating draggers by selection modified signal, which sometimes
            // comes too late and thus destroys the knot which we will now grab:
            _grdrag->local_change = true;
            // give the grab out-of-bounds values of xp/yp because we're already dragging
            // and therefore are already out of tolerance
            _grdrag->grabKnot (selection->items().front(),
                                   type == SP_GRADIENT_TYPE_LINEAR? POINT_LG_END : POINT_RG_R1,
                                   -1, // ignore number (though it is always 1)
                                   fill_or_stroke, 99999, 99999, etime);
        }
        // We did an undoable action, but SPDocumentUndo::done will be called by the knot when released

        // status text; we do not track coords because this branch is run once, not all the time
        // during drag
        int const n_objects = std::ranges::distance(selection->items());
        message_context->setF(NORMAL_MESSAGE,
                              ngettext("<b>Gradient</b> for %d object; with <b>%s</b> to snap angle",
                                       "<b>Gradient</b> for %d objects; with <b>%s</b> to snap angle", n_objects),
                              n_objects, mod_freehand_angle_snapping->get_label().c_str());
    } else {
        _desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>objects</b> on which to create gradient."));
    }
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
