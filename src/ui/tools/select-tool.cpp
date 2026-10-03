// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Selection and transformation context
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2010      authors
 * Copyright (C) 2006      Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 1999-2005 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include "document-undo.h"
#include "document.h"
#include "layer-manager.h"
#include "selection-chemistry.h"
#include "selection-describer.h"
#include "selection.h"
#include "seltrans.h"

#include "actions/actions-tools.h" // set_active_tool()

#include "display/drawing-item.h"
#include "display/control/canvas-item-catchall.h"
#include "display/control/canvas-item-drawing.h"
#include "display/control/snap-indicator.h"

#include "object/box3d.h"
#include "style.h"

#include "ui/tools/select-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::DocumentUndo;
using Inkscape::Modifiers::Modifier;

namespace Inkscape::UI::Tools {

static gint rb_escaped = 0; // if non-zero, rubberband was canceled by esc, so the next button release should not deselect
static gint drag_escaped = 0; // if non-zero, drag was canceled by esc
static bool is_cycling = false;

SelectTool::SelectTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/select", "select.svg")
    , _acc_st_grab{"tool.sel.stkey-grab"}
    , _acc_st_scale{"tool.sel.stkey-scale"}
    , _acc_st_rotate{"tool.sel.stkey-rotate"}
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
    , mod_select_always_box(Modifier::get(Modifiers::Type::SELECT_ALWAYS_BOX))
    , mod_select_cycle(Modifiers::Modifier::get(Modifiers::Type::SELECT_CYCLE))
    , mod_select_duplicate(Modifiers::Modifier::get(Modifiers::Type::SELECT_DUPLICATE))
    , mod_select_force_drag(Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
    , mod_select_in_groups(Modifier::get(Modifiers::Type::SELECT_IN_GROUPS))
    , mod_select_remove_from(Modifier::get(Modifiers::Type::SELECT_REMOVE_FROM))
    , mod_select_remove_snap(Modifier::get(Modifiers::Type::SELECT_REMOVE_SNAP))
    , mod_select_touch_path(Modifier::get(Modifiers::Type::SELECT_TOUCH_PATH))
{
    // cursors in select context
    _default_cursor = "select.svg";

    no_selection_msg = g_strdup_printf(
        _("No objects selected. Click, %s+Click, %s+Scroll mouse on top of objects, or drag around objects to select."),
        mod_select_add_to->get_label().c_str(), mod_select_cycle->get_label().c_str());

    _describer = new Inkscape::SelectionDescriber(
                desktop->getSelection(),
                *desktop->messageStack(),
                _("Drag a corner or edge to scale, or just outside a corner to rotate"),
                no_selection_msg);

    _seltrans = new Inkscape::SelTrans(desktop);

    sp_event_context_read(this, "show");
    sp_event_context_read(this, "transform");

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    if (prefs->getBool("/tools/select/gradientdrag")) {
        enableGrDrag();
    }
}

SelectTool::~SelectTool()
{
    enableGrDrag(false);

    if (grabbed) {
        grabbed->ungrab();
        grabbed = nullptr;
    }

    delete _seltrans;
    _seltrans = nullptr;

    delete _describer;
    _describer = nullptr;
    g_free(no_selection_msg);

    if (item) {
        sp_object_unref(item);
        item = nullptr;
    }
}

void SelectTool::set(const Inkscape::Preferences::Entry& val) {
    Glib::ustring path = val.getEntryName();

    if (path == "show") {
        if (val.getString() == "outline") {
            _seltrans->setShow(Inkscape::SelTrans::SHOW_OUTLINE);
        } else {
            _seltrans->setShow(Inkscape::SelTrans::SHOW_CONTENT);
        }
    }
}

bool SelectTool::sp_select_context_abort() {

    if (dragging) {
        if (moved) { // cancel dragging an object
            _seltrans->ungrab();
            moved = false;
            dragging = false;
            discard_delayed_snap_event();
            drag_escaped = 1;

            if (item) {
                // only undo if the item is still valid
                if (item->document) {
                    DocumentUndo::undo(_desktop->getDocument());
                }

                sp_object_unref( item, nullptr);
            }
            item = nullptr;

            _desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Move canceled."));
            return true;
        }
    } else {
        if (Inkscape::Rubberband::get(_desktop)->isStarted()) {
            Inkscape::Rubberband::get(_desktop)->stop();
            rb_escaped = 1;
            defaultMessageContext()->clear();
            _desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Selection canceled."));
            return true;
        }
    }
    _duplicate_drag_reset();
    return false;
}

static void
sp_select_context_up_one_layer(SPDesktop *desktop)
{
    /* Click in empty place, go up one level -- but don't leave a layer to root.
     *
     * (Rationale: we don't usually allow users to go to the root, since that
     * detracts from the layer metaphor: objects at the root level can in front
     * of or behind layers.  Whereas it's fine to go to the root if editing
     * a document that has no layers (e.g. a non-Inkscape document).)
     *
     * Once we support editing SVG "islands" (e.g. <svg> embedded in an xhtml
     * document), we might consider further restricting the below to disallow
     * leaving a layer to go to a non-layer.
     */
    if (SPObject *const current_layer = desktop->layerManager().currentLayer()) {
        SPObject *const parent = current_layer->parent;
        auto current_group = cast<SPGroup>(current_layer);
        if ( parent
             && ( parent->parent
                  || !( current_group
                        && ( SPGroup::LAYER == current_group->layerMode() ) ) ) )
        {
            desktop->layerManager().setCurrentLayer(parent);
            if (current_group && (SPGroup::LAYER != current_group->layerMode())) {
                desktop->getSelection()->set(current_layer);
            }
        }
    }
}

bool SelectTool::item_handler(SPItem *local_item, CanvasEvent const &event)
{
    // Make sure we still have valid objects to move around.
    if (item && item->document == nullptr) {
        sp_select_context_abort();
    }

    auto *prefs = Inkscape::Preferences::get();
    tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

    bool ret = false;

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (_seltrans->is_stkey()) {
                ret = true;
            } else if (event.num_press == 1 && event.button == 1) {
                /* Left mousebutton */

                saveDragOrigin(event.pos);

                // remember what modifiers were on before button press
                button_press_state = event.modifiers;
                bool always_box = mod_select_always_box->active(button_press_state);
                bool touch_path = mod_select_touch_path->active(button_press_state);
                bool duplicate_drag = mod_select_duplicate->active(button_press_state);

                bool is_modified_click = always_box || touch_path || duplicate_drag;

                // If a modifier is pressed to perform an action other than
                // dragging, pass the event to the root handler to perform
                // rubberband and other actions.
                if (!is_modified_click) {
                    dragging = true;
                    moved = false;

                    set_cursor("select-dragging.svg");

                    // Remember the clicked item in item:
                    if (item) {
                        sp_object_unref(item, nullptr);
                        item = nullptr;
                    }

                    bool force_drag = mod_select_force_drag->active(button_press_state);
                    item = sp_event_context_find_item (_desktop, event.pos, force_drag, false);
                    sp_object_ref(item, nullptr);

                    rb_escaped = drag_escaped = 0;

                    if (grabbed) {
                        grabbed->ungrab();
                        grabbed = nullptr;
                    }

                    grabbed = _desktop->getCanvasDrawing();
                    grabbed->grab(EventType::KEY_PRESS      |
                                  EventType::KEY_RELEASE    |
                                  EventType::BUTTON_PRESS   |
                                  EventType::BUTTON_RELEASE |
                                  EventType::MOTION);

                    ret = true;
                }
            } else if (event.button == 3 && !dragging) {
                // right click; do not eat it so that right-click menu can appear, but cancel dragging & rubberband
                sp_select_context_abort();
            }
        },
        [&] (EnterEvent const &event) {
            if (!dragging && !_alt_on && !_desktop->isWaitingCursor()) {
                set_cursor("select-mouseover.svg");
            }
        },
        [&] (LeaveEvent const &event) {
            if (!dragging && !_force_dragging && !_desktop->isWaitingCursor()) {
                set_cursor("select.svg");
            }
        },
        [&] (KeyPressEvent const &event) {
            switch (get_latin_keyval (event)) {
                case GDK_KEY_space:
                    if (dragging && grabbed) {
                        /* stamping mode: show content mode moving */
                        _seltrans->stamp();
                        ret = true;
                    }
                    break;
                case GDK_KEY_Tab:
                    if (!mod_ctrl(event)) {
                        if (dragging && grabbed) {
                            _seltrans->getNextClosestPoint(false);
                        } else {
                            sp_selection_item_next(_desktop);
                        }
                        ret = true;
                    }
                    break;
                case GDK_KEY_ISO_Left_Tab:
                    if (!mod_ctrl(event)) {
                        if (dragging && grabbed) {
                            _seltrans->getNextClosestPoint(true);
                        } else {
                            sp_selection_item_prev(_desktop);
                        }
                        ret = true;
                    }
                    break;
            }
        },
        [&] (ButtonReleaseEvent const &event) {
            if (_alt_on) {
                _default_cursor = "select-mouseover.svg";
            }
        },
        [&] (KeyReleaseEvent const &event) {
            if (_alt_on) {
                _default_cursor = "select-mouseover.svg";
            }
        },
        [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::item_handler(item, event);
}

void SelectTool::sp_select_context_cycle_through_items(Selection *selection, ScrollEvent const &scroll_event)
{
    if ( cycling_items.empty() )
        return;

    // Find next item and activate it

    std::vector<SPItem *>::iterator next = cycling_items.end();

    if (scroll_event.delta.y() < 0) {
        if (! cycling_cur_item) {
            next = cycling_items.begin();
        } else {
            next = std::find( cycling_items.begin(), cycling_items.end(), cycling_cur_item );
            g_assert (next != cycling_items.end());
            ++next;
            if (next == cycling_items.end()) {
                if ( cycling_wrap ) {
                    next = cycling_items.begin();
                } else {
                    --next;
                }
            }
        }
    } else if (scroll_event.delta.y() > 0) {
        if (! cycling_cur_item) {
            next = cycling_items.end();
            --next;
        } else {
            next = std::find( cycling_items.begin(), cycling_items.end(), cycling_cur_item );
            g_assert (next != cycling_items.end());
            if (next == cycling_items.begin()){
                if ( cycling_wrap ) {
                    next = cycling_items.end();
                    --next;
                }
            } else {
                --next;
            }
        }
    }
    else {
        // ignore no-scroll that mouse driver can deliver
        return;
    }

    Inkscape::DrawingItem *arenaitem;

    if (cycling_cur_item) {
        arenaitem = cycling_cur_item->get_arenaitem(_desktop->dkey);
        arenaitem->setOpacityOverride(0.3);
    }

    cycling_cur_item = *next;
    g_assert(next != cycling_items.end());
    g_assert(cycling_cur_item != nullptr);

    arenaitem = cycling_cur_item->get_arenaitem(_desktop->dkey);
    arenaitem->setOpacityOverride(1.0);

    if (mod_select_add_to->active(scroll_event.modifiers)) {
        selection->add(cycling_cur_item);
    } else {
        selection->set(cycling_cur_item);
    }
}

void SelectTool::sp_select_context_reset_opacities() {
    for (auto item : cycling_items_cmp) {
        if (item) {
            Inkscape::DrawingItem *arenaitem = item->get_arenaitem(_desktop->dkey);
            arenaitem->setOpacityOverride({});
        } else {
            g_assert_not_reached();
        }
    }

    cycling_items_cmp.clear();
    cycling_cur_item = nullptr;
}

bool SelectTool::root_handler(CanvasEvent const &event)
{
    SPItem *item_at_point = nullptr, *group_at_point = nullptr, *item_in_group = nullptr;

    auto selection = _desktop->getSelection();
    auto prefs = Preferences::get();

    // make sure we still have valid objects to move around
    if (item && item->document == nullptr) {
        sp_select_context_abort();
    }

    bool ret = false;

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (_seltrans->is_stkey()) {
                _seltrans->pause_stkey();
                ret = true;
            } else if (event.num_press == 2 && event.button == 1) {
                if (!selection->isEmpty()) {
                    SPItem *clicked_item = selection->items().front();

                    if (is<SPGroup>(clicked_item) && !is<SPBox3D>(clicked_item)) { // enter group if it's not a 3D box
                        _desktop->layerManager().setCurrentLayer(clicked_item);
                        _desktop->getSelection()->clear();
                        dragging = false;
                        discard_delayed_snap_event();

                    } else { // switch tool
                        Geom::Point const p(_desktop->w2d(event.pos));
                        set_active_tool(_desktop, clicked_item, p);
                    }
                } else {
                    sp_select_context_up_one_layer(_desktop);
                }

                ret = true;
            }
            if (event.num_press == 1 && event.button == 1) {

                saveDragOrigin(event.pos);

                bool has_selection = !selection->isEmpty();
                _duplicate_drag_on_press = has_selection && _duplicate_drag_state(event.modifiers);
                auto item_down = has_selection ? _desktop->getItemAtPoint(event.pos, false) : nullptr;
                _duplicate_down_on_selected = item_down && selection->includes(item_down, true);
                bool suppress_touch_path = _duplicate_drag_on_press && _duplicate_down_on_selected;
                auto rubberband = Inkscape::Rubberband::get(_desktop);
                if (!suppress_touch_path && mod_select_touch_path->active(event.modifiers)) {
                    rubberband->setMode(Rubberband::Mode::TOUCHPATH);
                    rubberband->setHandle(CanvasItemCtrlType::RUBBERBAND_TOUCHPATH_SELECT);
                } else {
                    auto const [mode, handle] = get_default_rubberband_state();
                    rubberband->setMode(mode);
                    rubberband->setHandle(handle);
                }

                Geom::Point const p(_desktop->w2d(event.pos));
                rubberband->start(_desktop, p);

                if (grabbed) {
                    grabbed->ungrab();
                    grabbed = nullptr;
                }

                grabbed = _desktop->getCanvasCatchall();
                grabbed->grab(EventType::KEY_PRESS      |
                              EventType::KEY_RELEASE    |
                              EventType::BUTTON_PRESS   |
                              EventType::BUTTON_RELEASE |
                              EventType::MOTION);

                // remember what modifiers were on before button press
                button_press_state = event.modifiers;

                moved = false;

                rb_escaped = drag_escaped = 0;

                ret = true;
            } else if (event.button == 3) {
                // right click; do not eat it so that right-click menu can appear, but cancel dragging & rubberband
                sp_select_context_abort();
            }
        },
        [&] (MotionEvent const &event) {
            _live_point = event.pos;

            if (!(event.modifiers & GDK_BUTTON1_MASK) && !_seltrans->isGrabbed()) {
                _seltrans->setPointer(_desktop->w2d(event.pos));
            }

            if (grabbed && mod_select_remove_snap->active(event.modifiers)) {
                _desktop->getSnapIndicator()->remove_snaptarget();
            }

            if (_seltrans->is_stkey() && !_seltrans->isEmpty()) {
                ret = _seltrans->request_stkey(_desktop->w2d(event.pos), event.modifiers);
                gobble_motion_events(GDK_BUTTON1_MASK);
                return;
            }

            tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

            bool duplicate_drag = _duplicate_drag_on_press;
            bool force_drag = mod_select_force_drag->active(button_press_state);
            bool always_box = mod_select_always_box->active(button_press_state);

            if (event.modifiers & GDK_BUTTON1_MASK) {
                if (!checkDragMoved(event.pos)) {
                    return;
                }

                Geom::Point const p(_desktop->w2d(event.pos));

                if (force_drag && !always_box && !selection->isEmpty()) {
                    // if it's not click and alt was pressed (with some selection
                    // but not with shift) we want to drag rather than rubberband
                    dragging = true;
                    set_cursor("select-dragging.svg");
                }

                if (!dragging && duplicate_drag && (force_drag || _duplicate_down_on_selected) && !selection->isEmpty()) {
                    // allow duplicate-drag to initiate a drag even when force-drag is off
                    dragging = true;
                }

                if (dragging) {
                    /* User has dragged fast, so we get events on root (lauris)*/
                    // not only that; we will end up here when ctrl-dragging as well
                    // and also when we started within tolerance, but trespassed tolerance outside of item
                    if (Inkscape::Rubberband::get(_desktop)->isStarted()) {
                        Inkscape::Rubberband::get(_desktop)->stop();
                    }
                    defaultMessageContext()->clear();

                    // Look for an item where the mouse was reported to be by mouse press (not mouse move).
                    item_at_point = _desktop->getItemAtPoint(xyp, false);

                    if (item_at_point || moved || force_drag) {
                        // drag only if starting from an item, or if something is already grabbed, or if alt-dragging
                        if (!moved) {
                            item_in_group = _desktop->getItemAtPoint(event.pos, true);
                            group_at_point = _desktop->getGroupAtPoint(event.pos);

                            {
                                auto selGroup = cast<SPGroup>(selection->single());
                                if (selGroup && (selGroup->layerMode() == SPGroup::LAYER)) {
                                    group_at_point = selGroup;
                                }
                            }

                            // group-at-point is meant to be topmost item if it's a group,
                            // not topmost group of all items at point
                            if (group_at_point != item_in_group &&
                                !(group_at_point && item_at_point &&
                                  group_at_point->isAncestorOf(item_at_point))) {
                                group_at_point = nullptr;
                            }

                            // if neither a group nor an item (possibly in a group) at point are selected, set selection to the item at point
                            if ((!item_in_group || !selection->includes(item_in_group)) &&
                                (!group_at_point || !selection->includes(group_at_point)) && !force_drag) {
                                // select what is under cursor
                                if (!_seltrans->isEmpty()) {
                                    _seltrans->resetState();
                                }

                                // when simply ctrl-dragging, we don't want to go into groups
                                if (item_at_point && !selection->includes(item_at_point)) {
                                    selection->set(item_at_point);
                                }
                            } // otherwise, do not change selection so that dragging selected-within-group items, as well as alt-dragging, is possible

                            bool down_on_selected = item_at_point && selection->includes(item_at_point, true);
                            bool allow_duplicate = duplicate_drag && (_duplicate_down_on_selected || down_on_selected);

                            // PROOF: measure the move from where the button went down, so the
                            // selection keeps up with the pointer. Grabbing here, once past the
                            // drag tolerance, left it that many pixels behind for the whole drag.
                            auto const origin = _desktop->w2d(Geom::Point(xyp));
                            if (allow_duplicate) {
                                _duplicate_drag(origin);
                            } else {
                                _seltrans->grab(origin, -1, -1, false, true);
                            }
                            moved = true;
                        }

                        if (!_seltrans->isEmpty()) {
                            // discard_delayed_snap_event();
                            _seltrans->moveTo(p, event.modifiers);
                        }

                        _desktop->getCanvas()->enable_autoscroll();
                        gobble_motion_events(GDK_BUTTON1_MASK);
                        ret = true;
                    } else {
                        dragging = false;
                        discard_delayed_snap_event();
                    }

                } else {
                    if (Inkscape::Rubberband::get(_desktop)->isStarted()) {
                        auto rubberband = Inkscape::Rubberband::get(_desktop);
                        rubberband->move(p);

                        // set selection color
                        if (mod_select_remove_from->active(event.modifiers)) {
                            rubberband->setOperation(Rubberband::Operation::REMOVE);
                        } else {
                            rubberband->setOperation(Rubberband::Operation::ADD);
                        }

                        auto touch_path = mod_select_touch_path->get_label();
                        auto remove_from = mod_select_remove_from->get_label();
                        auto mode = Inkscape::Rubberband::get(_desktop)->getMode();
                        if (mode == Rubberband::Mode::TOUCHPATH) {
                            defaultMessageContext()->setF(Inkscape::NORMAL_MESSAGE,
                                _("<b>Draw over</b> objects to select them; press <b>%s</b> to deselect them; release <b>%s</b> to switch to rubberband selection"), remove_from.c_str(), touch_path.c_str());
                        } else if (mode == Rubberband::Mode::TOUCHRECT) {
                            defaultMessageContext()->setF(Inkscape::NORMAL_MESSAGE,
                                _("<b>Drag near</b> objects to select them; press <b>%s</b> to deselect them; press <b>%s</b> to switch to touch selection"), remove_from.c_str(), touch_path.c_str());
                        } else {
                            defaultMessageContext()->setF(Inkscape::NORMAL_MESSAGE,
                                _("<b>Drag around</b> objects to select them; press <b>%s</b> to deselect them; press <b>%s</b> to switch to touch selection"), remove_from.c_str(), touch_path.c_str());
                        }

                        gobble_motion_events(GDK_BUTTON1_MASK);
                    }
                }
            }
        },
        [&] (ButtonReleaseEvent const &event) {
            xyp = {};

            if (_seltrans->is_stkey()) {
                _seltrans->ungrab_stkey(event.button != 1);
                ret = true;
            } else if ((event.button == 1) && (grabbed)) {
                if (dragging) {
                    if (moved) {
                        // item has been moved
                        _seltrans->ungrab();
                        moved = false;
                    } else if (item && !drag_escaped) {
                        // item has not been moved -> simply a click, do selecting
                        handleClick(event, selection);
                    }

                    dragging = false;

                    if (!_alt_on) {
                        if (_force_dragging) {
                            set_cursor(_default_cursor);
                            _force_dragging = false;
                        } else {
                            set_cursor("select-mouseover.svg");
                        }
                    }

                    discard_delayed_snap_event();

                    if (item) {
                        sp_object_unref( item, nullptr);
                    }

                    item = nullptr;
                    _duplicate_drag_reset();
                } else {
                    Inkscape::Rubberband *r = Inkscape::Rubberband::get(_desktop);

                    if (r->isStarted() && !within_tolerance) {
                        // this was a rubberband drag
                        set_cursor(_default_cursor);
                        std::vector<SPItem*> items;

                        if (r->getMode() == Rubberband::Mode::RECT) {
                            Geom::OptRect const b = r->getRectangle();
                            items = _desktop->getDocument()->getItemsInBox(_desktop->dkey, (*b) * _desktop->dt2doc());
                        } else if (r->getMode() == Rubberband::Mode::TOUCHRECT) {
                            Geom::OptRect const b = r->getRectangle();
                            items = _desktop->getDocument()->getItemsPartiallyInBox(_desktop->dkey, (*b) * _desktop->dt2doc());
                        } else if (r->getMode() == Rubberband::Mode::TOUCHPATH) {
                            bool topmost_items_only = prefs->getBool("/options/selection/touchsel_topmost_only");
                            items = _desktop->getItemsAtPoints(r->getPoints(), true, topmost_items_only);
                        }

                        _seltrans->resetState();
                        r->stop();
                        defaultMessageContext()->clear();

                        if (mod_select_remove_from->active(event.modifiers)) {
                            // with ctrl and shift, remove from selection
                            selection->removeList(items);
                        } else if (mod_select_add_to->active(event.modifiers)) {
                            // with shift, add to selection
                            selection->addList(items);
                        } else {
                            // without shift, simply select anew
                            selection->setList(items);
                        }

                    } else { // it was just a click, or a too small rubberband
                        r->stop();

                        handleClick(event, selection);
                    }

                    ret = true;
                }
                if (grabbed) {
                    grabbed->ungrab();
                    grabbed = nullptr;
                }
            }

            if (event.button == 1) {
                Inkscape::Rubberband::get(_desktop)->stop(); // might have been started in another tool!
            }

            button_press_state = 0;
        },
        [&] (ScrollEvent const &event) {
            // do nothing specific if alt was not pressed
            if (!mod_select_cycle->active(event.modifiers)) {
                return;
            }

            is_cycling = true;

            /* Rebuild list of items underneath the mouse pointer */
            Geom::Point p = _desktop->d2w(_desktop->point());
            SPItem *local_item = _desktop->getItemAtPoint(p, true, nullptr);
            cycling_items.clear();

            SPItem *tmp = nullptr;
            while(local_item != nullptr) {
                cycling_items.push_back(local_item);
                local_item = _desktop->getItemAtPoint(p, true, local_item);
                if (local_item && selection->includes(local_item)) tmp = local_item;
            }

            /* Compare current item list with item list during previous scroll ... */
            bool item_lists_differ = cycling_items != cycling_items_cmp;

            if(item_lists_differ) {
                sp_select_context_reset_opacities();
                for (auto l : cycling_items_cmp)
                    selection->remove(l); // deselects the previous content of the cycling loop
                cycling_items_cmp = (cycling_items);

                // set opacities in new stack
                for(auto cycling_item : cycling_items) {
                    if (cycling_item) {
                        Inkscape::DrawingItem *arenaitem = cycling_item->get_arenaitem(_desktop->dkey);
                        arenaitem->setOpacityOverride(0.3);
                    }
                }
            }
            if(!cycling_cur_item) cycling_cur_item = tmp;

            cycling_wrap = prefs->getBool("/options/selection/cycleWrap", true);

            // Cycle through the items underneath the mouse pointer, one-by-one
            sp_select_context_cycle_through_items(selection, event);

            ret = true;

            // TODO Simplify this (or remove it, if canvas exists, window must exist).
            GtkWindow *w = GTK_WINDOW(gtk_widget_get_root(_desktop->getCanvas()->Gtk::Widget::gobj()));
            if (w) {
                gtk_window_present(w);
                _desktop->getCanvas()->grab_focus();
            }
        },
        [&] (KeyPressEvent const &event) {
            auto keyval = get_latin_keyval (event);

            // Workaround for non-working modifiers code
            // TODO check what the Option key emits
            bool alt = keyval == GDK_KEY_Alt_L  ||
                       keyval == GDK_KEY_Alt_R  ||
                       keyval == GDK_KEY_Meta_L ||
                       keyval == GDK_KEY_Meta_R;
            if (alt) {
                _alt_on = true; // Turn off in KeyReleaseEvent
            }

            if (!selection->isEmpty() && !_seltrans->is_stkey()) {
                if (_acc_st_grab.isTriggeredBy(event)) {
                    _seltrans->grab_stkey(_desktop->w2d(_live_point), SelTrans::StickyTransform::Grab);
                    ret = true;
                } else if (_acc_st_scale.isTriggeredBy(event)) {
                    _seltrans->grab_stkey(_desktop->w2d(_live_point), SelTrans::StickyTransform::Scale);
                    ret = true;
                } else if (_acc_st_rotate.isTriggeredBy(event)) {
                    _seltrans->grab_stkey(_desktop->w2d(_live_point), SelTrans::StickyTransform::Rotate);
                    ret = true;
                }
            }
            if (_seltrans->is_stkey()) {
                if (keyval == GDK_KEY_Escape) {
                    // Canceled stKey transformation
                    _seltrans->ungrab_stkey(true);
                    ret = true;
                }
                return;
            }

            if (!Modifiers::keyval_is_a_modifier(keyval)) {
                defaultMessageContext()->clear();
            } else if (grabbed || _seltrans->isGrabbed()) {
                if (auto rubberband = Inkscape::Rubberband::get(_desktop); rubberband->isStarted()) {
                    // if Ctrl then change rubberband operation to remove (changes color)
                    if (mod_select_remove_from->active(event.modifiersAfter())) {
                        rubberband->setOperation(Rubberband::Operation::REMOVE);
                        // update the rubberband
                        rubberband->move(_desktop->point());
                    }
                    // if Alt then change mode to touch path mode
                    if (mod_select_touch_path->active(event.modifiersAfter())) {
                        rubberband->setMode(Rubberband::Mode::TOUCHPATH);
                        rubberband->setHandle(CanvasItemCtrlType::RUBBERBAND_TOUCHPATH_SELECT);
                        set_cursor("select-touch.svg");
                    }
                } else {
                    // do not change the statusbar text when mousekey is down to move or transform the object,
                    // because the statusbar text is already updated somewhere else.
                    return;
                }
            } else {
                Modifiers::responsive_tooltip(defaultMessageContext(), event, 6,
                                              Modifiers::Type::SELECT_IN_GROUPS, Modifiers::Type::MOVE_CONFINE,
                                              Modifiers::Type::SELECT_ADD_TO, Modifiers::Type::SELECT_TOUCH_PATH,
                                              Modifiers::Type::SELECT_CYCLE, Modifiers::Type::SELECT_FORCE_DRAG);

                // if Alt and nonempty selection, show moving cursor ("move selected"):
                if (alt && !_desktop->isWaitingCursor()) {
                        if (!selection->isEmpty()) {
                            set_cursor("select-dragging.svg");
                            _force_dragging = true;
                        } else {
                           set_cursor("select-touch.svg");
                        }
                    _default_cursor = "select.svg";
                }
                return;
            }

            gdouble const nudge = prefs->getDoubleLimited("/options/nudgedistance/value", 2, 0, 1000, "px"); // in px
            auto const y_dir = _desktop->yaxisdir();

            bool const rotated = prefs->getBool("/options/moverotated/value", true);

            double delta = 1.0;
            if (mod_shift(event)) { // shift
                delta = 10.0;
            }

            bool screen = true;
            if (!mod_alt(event)) { // no alt
                delta *= nudge;
                screen = false;
            }

            int const mul = 1 + gobble_key_events(keyval, 0);

            switch (keyval) {
                case GDK_KEY_Left: // move selection left
                case GDK_KEY_KP_Left:
                    if (!mod_ctrl(event)) { // not ctrl
                        _desktop->getSelection()->move(-delta * mul, 0, rotated, screen);
                        ret = true;
                    }
                    break;

                case GDK_KEY_Up: // move selection up
                case GDK_KEY_KP_Up:
                    if (!mod_ctrl(event)) { // not ctrl
                        _desktop->getSelection()->move(0, -delta * mul * y_dir, rotated, screen);
                        ret = true;
                    }
                    break;

                case GDK_KEY_Right: // move selection right
                case GDK_KEY_KP_Right:
                    if (!mod_ctrl(event)) { // not ctrl
                        _desktop->getSelection()->move(delta * mul, 0, rotated, screen);
                        ret = true;
                    }
                    break;

                case GDK_KEY_Down: // move selection down
                case GDK_KEY_KP_Down:
                    if (!mod_ctrl(event)) { // not ctrl
                        _desktop->getSelection()->move(0, delta * mul * y_dir, rotated, screen);
                        ret = true;
                    }
                    break;

                case GDK_KEY_Escape:
                    if (!sp_select_context_abort()) {
                        if (mod_ctrl_only(event) || selection->isEmpty()) {
                            // Escape any current non-layer group we entered (see Ctrl+Enter)
                            sp_select_context_up_one_layer(_desktop);
                        } else {
                            selection->clear();
                        }
                    }

                    ret = true;
                    break;

                case GDK_KEY_a:
                case GDK_KEY_A:
                    if (mod_ctrl_only(event)) {
                        sp_edit_select_all(_desktop);
                        ret = true;
                    }
                    break;

                case GDK_KEY_space:
                case GDK_KEY_c:
                case GDK_KEY_C:
                    /* stamping mode: show outline mode moving */
                    if (dragging && grabbed) {
                        _seltrans->stamp(keyval != GDK_KEY_space);
                        ret = true;
                    }
                    break;

                case GDK_KEY_Return:
                    if (mod_ctrl_only(event)) {
                        // Enter a group as a layer (get out with Ctrl+Enter or Ctrl+Backspace)
                        if (auto selected_object = selection->single()) {
                            auto group = cast<SPGroup>(selected_object);
                            if (group && group->layerMode() != SPGroup::LAYER) {
                                _desktop->layerManager().setCurrentLayer(selected_object);
                                _desktop->getSelection()->clear();
                            } else {
                                _desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Selected object is not a group. Cannot enter."));
                            }
                        }

                        ret = true;
                    }
                    break;

                case GDK_KEY_BackSpace:
                    if (mod_ctrl_only(event)) {
                        sp_select_context_up_one_layer(_desktop);
                        ret = true;
                    }
                    break;

                case GDK_KEY_s:
                case GDK_KEY_S:
                    if (mod_shift_only(event)) {
                        if (!selection->isEmpty()) {
                            _seltrans->increaseState();
                        }

                        ret = true;
                    }
                    break;

                case GDK_KEY_g:
                case GDK_KEY_G:
                    if (mod_shift_only(event)) {
                        _desktop->getSelection()->toGuides();
                        ret = true;
                    }
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

            // Workaround for non-working modifier detection
            bool alt = keyval == GDK_KEY_Alt_L  ||
                       keyval == GDK_KEY_Alt_R  ||
                       keyval == GDK_KEY_Meta_L ||
                       keyval == GDK_KEY_Meta_R;
            if (alt) {
                _alt_on = false; // Turned on in KeyPressEvent
            }

            if (auto rubberband = Inkscape::Rubberband::get(_desktop); rubberband->isStarted()) {
                // if Alt release then change mode back to default
                if (alt) {
                    auto const [mode, handle] = get_default_rubberband_state();
                    rubberband->setMode(mode);
                    rubberband->setHandle(handle);
                    set_cursor(_default_cursor);
                }
                // if Ctrl release then change rubberband operation to add
                if (!mod_select_remove_from->active(event.modifiersAfter())) {
                    rubberband->setOperation(Rubberband::Operation::ADD);
                    // update the rubberband
                    rubberband->move(_desktop->point());
                }
            } else {
                if (alt) {
                    // quit cycle-selection and reset opacities
                    if (is_cycling) {
                        sp_select_context_reset_opacities();
                        is_cycling = false;
                    }
                }
            }

            // set cursor to default.
            if (alt && !(grabbed || _seltrans->isGrabbed()) && !_desktop->isWaitingCursor()) {
                set_cursor(_default_cursor);
                _force_dragging = false;
            }
        },
        [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

void SelectTool::handleClick(ButtonReleaseEvent const &event, Selection *selection)
{
    bool force_drag = mod_select_force_drag->active(event.modifiers);
    if ((rb_escaped || drag_escaped) && !force_drag) {
        rb_escaped = 0;

        return;
    }

    bool in_groups = mod_select_in_groups->active(event.modifiers);

    SPItem *local_item = sp_event_context_find_item(_desktop, event.pos, force_drag, in_groups);

    if (!selection->isEmpty()) {
        if (local_item) {
            if (mod_select_add_to->active(event.modifiers)) {
                // with shift, toggle selection
                _seltrans->resetState();
                selection->toggle(local_item);
            } else {
                SPObject *single = selection->single();
                auto singleGroup = cast<SPGroup>(single);

                // PROOF: clicking the selection again keeps it as it is. Inkscape toggled to
                // rotate/skew handles here; the transform box rotates from outside its corners.
                bool const clicked_selection = selection->includes(local_item) ||
                    (singleGroup && singleGroup->layerMode() == SPGroup::LAYER && single->isAncestorOf(local_item));
                if (!clicked_selection) {
                    _seltrans->resetState();
                    selection->set(local_item);
                }
            }
        } else {
            selection->clear();
        }
    } else if (local_item) {
        // Simple or shift click, no previous selection
        _seltrans->resetState();
        selection->set(local_item);
    }
}

void SelectTool::_duplicate_drag(Geom::Point const &p)
{
    auto selection = _desktop->getSelection();
    if (selection->isEmpty()) {
        return;
    }

    selection->duplicate(true);

    // Text layout/bbox can lag behind immediately after duplication; update before starting drag.
    _desktop->getDocument()->ensureUpToDate();
    _seltrans->grab(p, -1, -1, false, true);
}

bool SelectTool::_duplicate_drag_state(unsigned int state) const
{
    // Ignore the modifier when it's effectively unset (no required keys).
    auto mask = mod_select_duplicate->get_and_mask();
    if (mask == Modifiers::ALWAYS || mask == Modifiers::NEVER) {
        return false;
    }

    return mod_select_duplicate->active(state);
}

void SelectTool::_duplicate_drag_reset()
{
    _duplicate_drag_on_press = false;
    _duplicate_down_on_selected = false;
}

/**
 * Update the toolbar description to this selection.
 */
void SelectTool::updateDescriber(Inkscape::Selection *selection)
{
    _describer->updateMessage(selection);
}

/**
 * Get the default rubberband state for select tool.
 */
std::pair<Rubberband::Mode, CanvasItemCtrlType> SelectTool::get_default_rubberband_state()
{
    auto mode = Rubberband::default_mode;
    auto handle = Rubberband::default_handle;
    if (Inkscape::Preferences::get()->getBool("/tools/select/touch_box", false)) {
        mode = Rubberband::Mode::TOUCHRECT;
        handle = CanvasItemCtrlType::RUBBERBAND_TOUCHRECT;
    }
    return {mode, handle};
}

void SelectTool::onHideSelectionChanged(bool hide)
{
    _seltrans->setBoxesHidden(hide);
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
