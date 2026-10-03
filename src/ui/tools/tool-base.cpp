// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Main event handling, and related helper functions.
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Frank Felfe <innerspace@iname.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *
 * Copyright (C) 1999-2012 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/tools/tool-base.h"

#include <gdkmm/seat.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/version.h>

#include "actions/actions-tools.h"
#include "desktop-events.h"
#include "desktop-style.h"
#include "display/control/canvas-item-catchall.h" // Grab/Ungrab
#include "display/control/snap-indicator.h"
#include "gradient-drag.h"
#include "inkscape-application.h"
#include "layer-manager.h"
#include "message-context.h"
#include "object/sp-namedview.h"
#include "selcue.h"
#include "selection-chemistry.h"
#include "selection.h"
#include "ui/contextmenu.h"
#include "ui/cursor-utils.h"
#include "ui/knot/knot-holder.h"
#include "ui/knot/knot-ptr.h"
#include "ui/knot/knot.h"
#include "ui/popup-menu.h"
#include "ui/toolbar/toolbars.h"
#include "ui/toolbar/toolbar.h"
#include "ui/shape-editor.h"
#include "ui/shortcuts.h"
#include "ui/tool/control-point.h"
#include "ui/tools/calligraphic-tool.h"
#include "ui/tools/dropper-tool.h"
#include "ui/tools/node-tool.h"
#include "ui/tools/select-tool.h"
#include "ui/util.h"
#include "ui/widget/canvas-grid.h"
#include "ui/widget/canvas.h"
#include "ui/widget/desktop-widget.h"
#include "ui/widget/combo-box-entry-tool-item.h"
#include <gtkmm/entry.h>
#include <gtkmm/spinbutton.h>
#include "ui/widget/events/canvas-event.h"
#include "ui/widget/events/debug.h"
#include "ui/widget/generic/spin-button.h"

// globals for temporary switching to selector by space
static bool selector_toggled = false;
static Glib::ustring switch_selector_to;

// globals for temporary switching to dropper by 'D'
static bool dropper_toggled = false;
static Glib::ustring switch_dropper_to;

// globals for keeping track of keyboard scroll events in order to accelerate
static guint32 scroll_event_time = 0;
static double scroll_multiply = 1;
static unsigned scroll_keyval = 0;

// globals for key processing
static bool latin_keys_group_valid = false;
static int latin_keys_group;
static std::set<int> latin_keys_groups;

namespace Inkscape {
namespace UI {
namespace Tools {

static void set_event_location(SPDesktop *desktop, CanvasEvent const &event);

DelayedSnapEvent::DelayedSnapEvent(ToolBase *tool, gpointer item, gpointer item2, MotionEvent const &event, Origin origin)
    : _tool(tool)
    , _item(item)
    , _item2(item2)
    , _origin(origin)
{
    static_assert(std::is_final_v<MotionEvent>); // Or the next line will slice!
    _event = std::make_unique<MotionEvent>(event);
    _event->time = GDK_CURRENT_TIME;
}

ToolBase::ToolBase(SPDesktop *desktop, std::string &&prefs_path, std::string &&cursor_filename, bool uses_snap)
    : _prefs_path(std::move(prefs_path))
    , _cursor_filename("none")
    , _cursor_default(std::move(cursor_filename))
    , _uses_snap(uses_snap)
    , _desktop(desktop)
    , _acc_undo{"doc.undo"}
    , _acc_redo{"doc.redo"}
    , _acc_quick_preview{"tool.all.quick-preview"}
    , _acc_quick_zoom{"tool.all.quick-zoom"}
    , _acc_quick_pan{"tool.all.quick-pan"}
    , _acc_focus_first_widget{"tool.all.focus-first-widget"}
    , mod_canvas_pan_drag(Modifiers::Modifier::get(Modifiers::Type::CANVAS_PAN_DRAG))
    , mod_canvas_rotate_drag(Modifiers::Modifier::get(Modifiers::Type::CANVAS_ROTATE_DRAG))
    , mod_canvas_rotate_reset(Modifiers::Modifier::get(Modifiers::Type::CANVAS_ROTATE_RESET))
    , mod_canvas_rotate_snapping(Modifiers::Modifier::get(Modifiers::Type::CANVAS_ROTATE_SNAPPING))
    , mod_canvas_zoom_invert(Modifiers::Modifier::get(Modifiers::Type::CANVAS_ZOOM_INVERT))
    , mod_canvas_zoom_rubberband(Modifiers::Modifier::get(Modifiers::Type::CANVAS_ZOOM_RUBBERBAND))
    , mod_select_force_drag(Modifiers::Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
{
    pref_observer = Inkscape::Preferences::PreferencesObserver::create(_prefs_path, [this] (auto &val) { set(val); });
    set_cursor(_cursor_default);
    _desktop->getCanvas()->grab_focus();

    message_context = std::make_unique<Inkscape::MessageContext>(*desktop->messageStack());

    // Make sure no delayed snapping events are carried over after switching tools
    // (this is only an additional safety measure against sloppy coding, because each
    // tool should take care of this by itself)
    discard_delayed_snap_event();

    sp_event_context_read(this, "changelayer");
    sp_event_context_read(this, "changepage");

    _hide_selection_connection = desktop->connectHideSelectionBoxes(sigc::mem_fun(*this, &ToolBase::onHideSelectionChanged));
}

ToolBase::~ToolBase()
{
    enableSelectionCue(false);
}

/**
 * @brief Helper function to recursively search a Gtk::Widget container for the first focusable and sensitive widget.
 * @param container The Gtk::Widget (which might be a container) to search within.
 * @return A pointer to the first focusable widget found, or nullptr if none is found.
 */
static Gtk::Widget* find_first_focusable_input(Gtk::Widget* container) {
    if (!container || !container->get_sensitive() || !container->get_visible()) {
        return nullptr;
    }

    if (auto* combo = dynamic_cast<UI::Widget::ComboBoxEntryToolItem*>(container)) {
        Gtk::Entry* entry = combo->get_entry();
        return (entry && entry->get_sensitive()) ? entry : nullptr;
    }

    if (dynamic_cast<Widget::InkSpinButton*>(container) || dynamic_cast<Gtk::Entry*>(container)) {
        return (container->get_focusable() && container->get_sensitive()) ? container : nullptr;
    }


    // If the widget is not an input type itself, it might be a generic container. Search its children.
    for (auto child = container->get_first_child(); child != nullptr; child = child->get_next_sibling()) {
        if (auto* found_widget = find_first_focusable_input(child)) {
            return found_widget;
        }
    }

    return nullptr;
}

/**
 * @brief Find the first focusable widget on the tool's toolbar and grab focus.
 * This is the generic implementation that will be used by all tools unless they override it.
 */
void ToolBase::focus_first_widget()
{
    if (!_desktop) return;

    auto* dt_widget = _desktop->getDesktopWidget();
    if (!dt_widget) return;

    auto* curr_toolbar = dt_widget->get_current_toolbar();
    if (!curr_toolbar) return;

    if (auto* first_focusable = find_first_focusable_input(curr_toolbar)) {
        first_focusable->grab_focus();
    }
}

/**
 * Called by our pref_observer if a preference has been changed.
 */
void ToolBase::set(Inkscape::Preferences::Entry const &value)
{
    // whether the layer and page should be changed with the selection
    // defaults to true
    Glib::ustring entry_name = value.getEntryName();
    if (entry_name == "changelayer") {
        _desktop->getSelection()->setChangeLayer(value.getBool(true));
    } else if (entry_name == "changepage") {
        _desktop->getSelection()->setChangePage(value.getBool(true));
    }
}

SPGroup *ToolBase::currentLayer() const
{
    return _desktop->layerManager().currentLayer();
}

/**
 * Sets the current cursor to the given filename. Does not readload if not changed.
 */
void ToolBase::set_cursor(std::string filename)
{
    if (filename != _cursor_filename) {
        _cursor_filename = std::move(filename);
        use_tool_cursor();
    }
}

/**
 * Returns the Gdk Cursor for the given filename
 *
 * WARNING: currently this changes the window cursor, see load_svg_cursor
 * TODO: GTK4: Is the above warning still applicable?
 */
Glib::RefPtr<Gdk::Cursor> ToolBase::get_cursor(Gtk::Widget &widget, std::string const &filename) const
{
    auto fillColor = sp_desktop_get_color_tool(_desktop, getPrefsPath(), true);
    if (fillColor) {
        fillColor->addOpacity(sp_desktop_get_opacity_tool(_desktop, getPrefsPath(), true));
    }

    auto strokeColor = sp_desktop_get_color_tool(_desktop, getPrefsPath(), false);
    if (strokeColor) {
        strokeColor->addOpacity(sp_desktop_get_opacity_tool(_desktop, getPrefsPath(), false));
    }
    return load_svg_cursor(widget, filename, fillColor, strokeColor);
}

/**
 * Uses the saved cursor, based on the saved filename.
 */
void ToolBase::use_tool_cursor()
{
    auto &widget = dynamic_cast<Gtk::Widget &>(*_desktop->getCanvas());
    widget.set_cursor(get_cursor(widget, _cursor_filename));

    _desktop->waiting_cursor = false;
}

/**
 * Set the cursor to this specific one, don't remember it.
 *
 * If RefPtr is empty, sets the remembered cursor (reverting it)
 */
void ToolBase::use_cursor(Glib::RefPtr<Gdk::Cursor> cursor)
{
    // PROOF: GTK 4 shows the cursor of the widget under the pointer, and the canvas always has
    // the tool cursor, so a cursor set on the window (as before) never appeared over a knot.
    if (cursor) {
        _desktop->getCanvas()->set_cursor(cursor);
    } else {
        use_tool_cursor();
    }
}

/**
 * Toggles current tool between active tool and selector tool.
 * Subroutine of sp_event_context_private_root_handler().
 */
static void sp_toggle_selector(SPDesktop *dt) {

    if (!dt->getTool()) {
        return;
    }

    if (dynamic_cast<Inkscape::UI::Tools::SelectTool *>(dt->getTool())) {
        if (selector_toggled) {
            set_active_tool(dt, switch_selector_to);
            selector_toggled = false;
        }
    } else {
        selector_toggled = TRUE;
        switch_selector_to = get_active_tool(dt);
        set_active_tool(dt, "Select");
    }
}

/**
 * Toggles current tool between active tool and dropper tool.
 * Subroutine of sp_event_context_private_root_handler().
 */
void sp_toggle_dropper(SPDesktop *dt)
{
    if (!dt->getTool()) {
        return;
    }

    if (dynamic_cast<Inkscape::UI::Tools::DropperTool *>(dt->getTool())) {
        if (dropper_toggled) {
            set_active_tool(dt, switch_dropper_to);
            dropper_toggled = FALSE;
        }
    } else {
        dropper_toggled = TRUE;
        switch_dropper_to = get_active_tool(dt);
        set_active_tool(dt, "Dropper");
    }
}

/**
 * Calculates and keeps track of scroll acceleration.
 * Subroutine of sp_event_context_private_root_handler().
 */
static double accelerate_scroll(KeyEvent const &event, double acceleration)
{
    auto time_diff = event.time - scroll_event_time;

    /* key pressed within 500ms ? (1/2 second) */
    if (time_diff > 500 || event.keyval != scroll_keyval) {
        scroll_multiply = 1; // abort acceleration
    } else {
        scroll_multiply += acceleration; // continue acceleration
    }

    scroll_event_time = event.time;
    scroll_keyval = event.keyval;

    return scroll_multiply;
}

/** Moves the selected points along the supplied unit vector according to
 * the modifier state of the supplied event. */
bool ToolBase::_keyboardMove(KeyEvent const &event, Geom::Point const &dir)
{
    if (mod_ctrl(event)) return false;
    unsigned num = 1 + gobble_key_events(event.keyval, 0);

    auto prefs = Preferences::get();

    Geom::Point delta = dir * num;

    if (mod_shift(event)) {
        delta *= 10;
    }

    if (mod_alt(event)) {
        delta /= _desktop->current_zoom();
    } else {
        double nudge = prefs->getDoubleLimited("/options/nudgedistance/value", 2, 0, 1000, "px");
        delta *= nudge;
    }

    bool const rotated = prefs->getBool("/options/moverotated/value", true);
    if (rotated) {
        delta *= _desktop->current_rotation().inverse();
    }

    bool moved = false;
    if (shape_editor && shape_editor->has_knotholder()) {
        auto &knotholder = shape_editor->knotholder;
        if (knotholder && knotholder->knot_selected()) {
            knotholder->transform_selected(Geom::Translate(delta));
            moved = true;
        }
    } else {
        // TODO: eliminate this dynamic cast by using inheritance
        auto nt = dynamic_cast<Inkscape::UI::Tools::NodeTool *>(_desktop->getTool());
        if (nt) {
            for (auto &_shape_editor : nt->_shape_editors) {
                ShapeEditor *shape_editor = _shape_editor.second.get();
                if (shape_editor && shape_editor->has_knotholder()) {
                    auto &knotholder = shape_editor->knotholder;
                    if (knotholder && knotholder->knot_selected()) {
                        knotholder->transform_selected(Geom::Translate(delta));
                        moved = true;
                    }
                }
            }
        }
    }

    return moved;
}

bool ToolBase::root_handler(CanvasEvent const &event)
{
    if constexpr (DEBUG_EVENTS) {
        dump_event(event, "ToolBase::root_handler");
    }

    using Modifiers::Type;
    using Modifiers::Triggers;

    static Geom::Point button_w;
    static unsigned int panning_cursor = 0;
    static unsigned int zoom_rb = 0;

    auto prefs = Inkscape::Preferences::get();

    /// @todo Remove redundant /value in preference keys
    // Todo: Make these into preference watchers, rather than fetching on every event.
    tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);
    bool allow_panning = prefs->getBool("/options/spacebarpans/value");
    bool ret = false;

    auto compute_angle = [&] (Geom::Point const &pt) {
        // Hack: Undo coordinate transformation applied by canvas to get events back to window coordinates.
        // Real solution: Move all this functionality out of this file to somewhere higher up in the chain.
        auto cursor = pt * _desktop->getCanvas()->get_geom_affine().inverse() * _desktop->getCanvas()->get_affine() - _desktop->getCanvas()->get_pos();
        return Geom::deg_from_rad(Geom::atan2(cursor - Geom::Point(_desktop->getCanvas()->get_dimensions()) / 2.0));
    };

    static Geom::Point panZoomPoint = Geom::Point(); //Initial cursor point used for zooming when using a graphics tablet
    static Geom::Point lastZoomPoint = Geom::Point(); //Used to determine the zoom direction and amount when using a graphics tablet

    inspect_event(event,
    [&] (ButtonPressEvent const &event) {
        if (event.num_press == 2) {
            if (panning) {
                panning = PANNING_NONE;
                ungrabCanvasEvents();
                ret = true;
            }
        } else if (event.num_press == 1) {
            //Resetting the points used by zooming with Ctrl + MMB each time zooming starts
            panZoomPoint = _desktop->point();
            lastZoomPoint = _desktop->point();

            // save drag origin
            xyp = event.pos.floor();
            within_tolerance = true;

            button_w = event.pos;

            check_lost_button_up = !(event.device && event.device->get_source() != Gdk::InputSource::TABLET_PAD);

            switch (event.button) {
            case 1:
                // TODO Does this make sense? Panning starts on passive mouse motion while space
                // bar is pressed, it's not necessary to press the mouse button.
                if (is_space_panning()) {
                    // When starting panning, make sure there are no snap events pending because these might disable the panning again
                    if (_uses_snap) {
                        discard_delayed_snap_event();
                    }
                    panning = PANNING_SPACE_BUTTON1;

                    grabCanvasEvents(EventType::KEY_RELEASE    |
                                     EventType::BUTTON_RELEASE |
                                     EventType::MOTION);

                    ret = true;
                }
                break;

            case 2:
                if (mod_canvas_rotate_drag->active(event.modifiers) && !_desktop->get_rotation_lock()) {
                    // Canvas ctrl + middle-click to rotate
                    rotating = true;

                    start_angle = current_angle = compute_angle(event.pos);

                    grabCanvasEvents(EventType::KEY_PRESS      |
                                     EventType::KEY_RELEASE    |
                                     EventType::BUTTON_RELEASE |
                                     EventType::MOTION);

                } else if (mod_canvas_zoom_rubberband->active(event.modifiers)) {
                    zoom_rb = 2;
                } else {
                    // When starting panning, make sure there are no snap events pending because these might disable the panning again
                    if (_uses_snap) {
                        discard_delayed_snap_event();
                    }
                    panning = PANNING_BUTTON2;

                    grabCanvasEvents(EventType::BUTTON_RELEASE | EventType::MOTION);
                }

                ret = true;
                break;

            case 3:
                if (mod_canvas_pan_drag->active(event.modifiers)) {
                    // When starting panning, make sure there are no snap events pending because these might disable the panning again
                    if (_uses_snap) {
                        discard_delayed_snap_event();
                    }
                    panning = PANNING_BUTTON3;

                    grabCanvasEvents(EventType::BUTTON_RELEASE | EventType::MOTION);
                    ret = true;
                } else if (!are_buttons_1_and_3_on(event)) {
                    menu_popup(event);
                    ret = true;
                }
                break;

            default:
                break;
            }
        }
    },

    [&] (MotionEvent const &event) {
        //Zoom while holding Ctrl + Middle mouse and moving the cursor
        if (Modifiers::Modifier::which( Triggers::CANVAS | Triggers::DRAG_MIDDLE_BUTTON, event.modifiers) == Type::CANVAS_DRAG_ZOOM && _button2on ) {
            if( lastZoomPoint.y() == 0 ) //First time zooming starts
                lastZoomPoint = panZoomPoint;

            double zoomSpeed = 8; //Should be user configurable
            double maxZoom = zoomSpeed / 10;

            //Keeping canvas area relative to the amount it was zoomed in/out
            auto worldHeight = _desktop->getCanvas()->get_area_world().height();
            auto currentZoom = _desktop->current_zoom();

            if( currentZoom && worldHeight )  {
                double canvasHeight = worldHeight / currentZoom / zoomSpeed;
                double positionDelta = _desktop->point().y() - lastZoomPoint.y();

                if( positionDelta != 0 )  {
                    double discreteZoomFactor = std::clamp( std::abs( positionDelta / canvasHeight ), .01, maxZoom );
                    //Prevents zooming in too much when lastPosition doesn't get updated ( prb a missed button event )
                    if( std::abs( positionDelta / canvasHeight ) <= maxZoom )  {
                        double zoom = positionDelta > 0 ? 1 + discreteZoomFactor : 1 - discreteZoomFactor;
                        _desktop->zoom_relative( panZoomPoint, zoom );
                     }
                 }
             }

            lastZoomPoint = _desktop->point();

            //Updating these two so we won't also get a pan to where the mouse button gets released
            xyp = event.pos.floor();
            button_w = event.pos;
            panning = PANNING_NONE;
            ungrabCanvasEvents();
            return;
         }

        if (panning) {
            if (panning == 4 && !xyp.x() && !xyp.y()) {
                // <Space> + mouse panning started, save location and grab canvas
                xyp = event.pos.floor();
                button_w = event.pos;

                grabCanvasEvents(EventType::KEY_RELEASE    |
                                 EventType::BUTTON_RELEASE |
                                 EventType::MOTION);
            }

            if (check_lost_button_up && ((panning == 2 && !(event.modifiers & GDK_BUTTON2_MASK)) ||
                                         (panning == 1 && !(event.modifiers & GDK_BUTTON1_MASK)) ||
                                         (panning == 3 && !(event.modifiers & GDK_BUTTON3_MASK)))) {
                // Gdk seems to lose button release for us sometimes :-(
                panning = PANNING_NONE;
                ungrabCanvasEvents();
                ret = true;
            } else {
                // To fix https://bugs.launchpad.net/inkscape/+bug/1458200
                // we increase the tolerance because no sensible data for panning
                if (within_tolerance && Geom::LInfty(event.pos.floor() - xyp) < tolerance * 3) {
                    // do not drag if we're within tolerance from origin
                    return;
                }

                // Once the user has moved farther than tolerance from
                // the original location (indicating they intend to move
                // the object, not click), then always process the motion
                // notify coordinates as given (no snapping back to origin)
                within_tolerance = false;

                // gobble subsequent motion events to prevent "sticking"
                // when scrolling is slow
                gobble_motion_events(  panning == 2
                                     ? GDK_BUTTON2_MASK
                                     : panning == 1
                                     ? GDK_BUTTON1_MASK
                                     : GDK_BUTTON3_MASK);

                if (panning_cursor == 0) {
                    panning_cursor = 1;
                    auto window = dynamic_cast<Gtk::Window *>(_desktop->getCanvas()->get_root());
                    auto cursor = Gdk::Cursor::create(Glib::ustring("move"));
                    window->set_cursor(cursor);
                }

                auto const motion_w = event.pos;
                auto const moved_w = motion_w - button_w;
                _desktop->scroll_relative(moved_w);
                ret = true;
            }
        } else if (zoom_rb) {
            if (!checkDragMoved(event.pos)) {
                return;
            }

            if (auto rubberband = Inkscape::Rubberband::get(_desktop); rubberband->isStarted()) {
                auto const motion_w = event.pos;
                auto const motion_dt = _desktop->w2d(motion_w);

                rubberband->move(motion_dt);
            } else {
                // Start the box where the mouse was clicked, not where it is now
                // because otherwise our box would be offset by the amount of tolerance.
                auto const motion_w = xyp;
                auto const motion_dt = _desktop->w2d(motion_w);

                rubberband->start(_desktop, motion_dt);
            }

            if (zoom_rb == 2) {
                gobble_motion_events(GDK_BUTTON2_MASK);
            }
        } else if (rotating) {
            auto angle = compute_angle(event.pos);

            double constexpr rotation_snap = 15.0;
            double delta_angle = angle - start_angle;
            if (mod_canvas_rotate_reset->active(event.modifiers)) {
                delta_angle = 0.0;
            } else if (mod_canvas_rotate_snapping->active(event.modifiers)) {
                delta_angle = std::round(delta_angle / rotation_snap) * rotation_snap;
            } else {
                delta_angle = std::floor(delta_angle);
            }
            angle = start_angle + delta_angle;

            _desktop->rotate_relative_keep_point(_desktop->w2d(Geom::Rect(_desktop->getCanvas()->get_area_world()).midpoint()),
                                                 Geom::rad_from_deg(angle - current_angle));
            current_angle = angle;
            ret = true;
        }
    },

    [&] (ButtonReleaseEvent const &event) {
        bool middle_mouse_zoom = prefs->getBool("/options/middlemousezoom/value");

        xyp = {};

        if (panning_cursor == 1) {
            panning_cursor = 0;
            dynamic_cast<Gtk::Window &>(*_desktop->getCanvas()->get_root()).set_cursor(_cursor);
        }

        if (event.button == 2 && rotating) {
            rotating = false;
            ungrabCanvasEvents();
        }

        if (middle_mouse_zoom && within_tolerance && (panning || zoom_rb) &&
            Modifiers::Modifier::which(Triggers::CANVAS | Triggers::DRAG_MIDDLE_BUTTON, event.modifiers) !=

            Modifiers::Type::CANVAS_DRAG_ZOOM) {
            zoom_rb = 0;

            if (panning) {
                panning = PANNING_NONE;
                ungrabCanvasEvents();
            }

            auto const event_w = event.pos;
            auto const event_dt = _desktop->w2d(event_w);

            double const zoom_inc = prefs->getDoubleLimited("/options/zoomincrement/value", M_SQRT2, 1.01, 10);
            auto const zoom_invert = mod_canvas_zoom_invert->active(event.modifiers);

            _desktop->zoom_relative(event_dt, zoom_invert ? 1 / zoom_inc : zoom_inc);
            ret = true;
        } else if (panning == event.button &&
                   Modifiers::Modifier::which(Triggers::CANVAS | Triggers::DRAG_MIDDLE_BUTTON, event.modifiers) !=
                   Modifiers::Type::CANVAS_DRAG_ZOOM) {
            panning = PANNING_NONE;
            ungrabCanvasEvents();

            // in slow complex drawings, some of the motion events are lost;
            // to make up for this, we scroll it once again to the button-up event coordinates
            // (i.e. canvas will always get scrolled all the way to the mouse release point,
            // even if few intermediate steps were visible)
            auto const motion_w = event.pos;
            auto const moved_w = motion_w - button_w;

            _desktop->scroll_relative(moved_w);
            ret = true;
        } else if (zoom_rb == event.button) {
            zoom_rb = 0;

            Geom::OptRect const b = Inkscape::Rubberband::get(_desktop)->getRectangle();
            Inkscape::Rubberband::get(_desktop)->stop();

            if (b && !within_tolerance) {
                _desktop->set_display_area(*b, 10);
            }

            ret = true;
        }
    },

    [&] (KeyPressEvent const &event) {
        double const acceleration = prefs->getDoubleLimited("/options/scrollingacceleration/value", 0, 0, 6);
        int const key_scroll = prefs->getIntLimited("/options/keyscroll/value", 10, 0, 1000);

        if (_acc_quick_preview.isTriggeredBy(event)) {
            _desktop->quick_preview(true);
            ret = true;
        }
        if (_acc_quick_zoom.isTriggeredBy(event)) {
            _desktop->zoom_quick(true);
            ret = true;
        }
        if (_acc_quick_pan.isTriggeredBy(event) && allow_panning) {
            xyp = {};
            within_tolerance = true;
            panning = PANNING_SPACE;
            message_context->set(Inkscape::INFORMATION_MESSAGE, _("<b>Space+mouse move</b> to pan canvas"));
            ret = true;
        }
        if (_acc_focus_first_widget.isTriggeredBy(event)) {
            focus_first_widget();
            ret = true;
            return;
        }

        switch (get_latin_keyval(event)) {
        // GDK insists on stealing the tab keys for cycling widgets in the
        // editing window. So we resteal them back and run our regular shortcut
        // invoker on them. Tab is hardcoded. When actions are triggered by tab,
        // we end up stealing events from GTK widgets.
        case GDK_KEY_Tab:
            if (mod_ctrl(event)) {
                _desktop->getDesktopWidget()->advanceTab(1);
            } else {
                sp_selection_item_next(_desktop);
            }
            ret = true;
            break;
        case GDK_KEY_ISO_Left_Tab:
            if (mod_ctrl(event)) {
                _desktop->getDesktopWidget()->advanceTab(-1);
            } else {
                sp_selection_item_prev(_desktop);
            }
            ret = true;
            break;

        case GDK_KEY_W:
        case GDK_KEY_w:
#ifndef __APPLE__
        case GDK_KEY_F4:
#endif
            // Close tab
            if (mod_ctrl_only(event)) {
                auto app = InkscapeApplication::instance();
                app->destroyDesktop(_desktop, true); // Keep inkscape alive!
                ret = true;
            }
            break;

        case GDK_KEY_Left: // Ctrl Left
        case GDK_KEY_KP_Left:
        case GDK_KEY_KP_4:
            if (mod_ctrl_only(event)) {
                int i = std::floor(key_scroll * accelerate_scroll(event, acceleration));

                gobble_key_events(get_latin_keyval(event), GDK_CONTROL_MASK);
                _desktop->scroll_relative(Geom::Point(i, 0));
            } else if (!_keyboardMove(event, Geom::Point(-1, 0))) {
                Inkscape::Shortcuts::getInstance().invoke_action(event);
            }
            ret = true;
            break;

        case GDK_KEY_Up: // Ctrl Up
        case GDK_KEY_KP_Up:
        case GDK_KEY_KP_8:
            if (mod_ctrl_only(event)) {
                int i = std::floor(key_scroll * accelerate_scroll(event, acceleration));

                gobble_key_events(get_latin_keyval(event), GDK_CONTROL_MASK);
                _desktop->scroll_relative(Geom::Point(0, i));
            } else if (!_keyboardMove(event, Geom::Point(0, -_desktop->yaxisdir()))) {
                Inkscape::Shortcuts::getInstance().invoke_action(event);
            }
            ret = true;
            break;

        case GDK_KEY_Right: // Ctrl Right
        case GDK_KEY_KP_Right:
        case GDK_KEY_KP_6:
            if (mod_ctrl_only(event)) {
                int i = std::floor(key_scroll * accelerate_scroll(event, acceleration));

                gobble_key_events(get_latin_keyval(event), GDK_CONTROL_MASK);
                _desktop->scroll_relative(Geom::Point(-i, 0));
            } else if (!_keyboardMove(event, Geom::Point(1, 0))) {
                Inkscape::Shortcuts::getInstance().invoke_action(event);
            }
            ret = true;
            break;

        case GDK_KEY_Down: // Ctrl Down
        case GDK_KEY_KP_Down:
        case GDK_KEY_KP_2:
            if (mod_ctrl_only(event)) {
                int i = std::floor(key_scroll * accelerate_scroll(event, acceleration));

                gobble_key_events(get_latin_keyval(event), GDK_CONTROL_MASK);
                _desktop->scroll_relative(Geom::Point(0, -i));
            } else if (!_keyboardMove(event, Geom::Point(0, _desktop->yaxisdir()))) {
                Inkscape::Shortcuts::getInstance().invoke_action(event);
            }
            ret = true;
            break;

        case GDK_KEY_Menu:
            menu_popup(event);
            ret = true;
            break;

        case GDK_KEY_F10:
            if (mod_shift_only(event)) {
                menu_popup(event);
                ret = true;
            }
            break;

        case GDK_KEY_r:
        case GDK_KEY_R:
            if (mod_alt_only(event)) {
                _desktop->rotate_grab_focus();
                ret = false; // don't steal key events, so keyboard shortcut works, if defined
            }
            break;

        case GDK_KEY_z:
        case GDK_KEY_Z:
            if (mod_alt_only(event)) {
                _desktop->zoom_grab_focus();
                ret = false; // don't steal key events, so keyboard shortcut works, if defined
            }
            break;

        default:
            break;
        }
    },

    [&] (KeyReleaseEvent const &event) {
        // Stop panning on any key release
        if (is_space_panning()) {
            message_context->clear();
        }

        if (panning) {
            panning = PANNING_NONE;
            xyp = {};

            ungrabCanvasEvents();
        }

        if (panning_cursor == 1) {
            panning_cursor = 0;
            dynamic_cast<Gtk::Window &>(*_desktop->getCanvas()->get_root()).set_cursor(_cursor);
        }

        if (_acc_quick_preview.isTriggeredBy(event)) {
            _desktop->quick_preview(false);
            ret = true;
        }
        if (_acc_quick_zoom.isTriggeredBy(event) && _desktop->quick_zoomed()) {
            _desktop->zoom_quick(false);
            ret = true;
        }

        switch (get_latin_keyval(event)) {
        case GDK_KEY_space:
            if (within_tolerance) {
                // Space was pressed, but not panned
                sp_toggle_selector(_desktop);

                // Be careful, sp_toggle_selector will delete ourselves.
                // Thus, make sure we return immediately.
                ret = true;
                return;
            }
            break;

        default:
            break;
        }
    },

    [&] (ScrollEvent const &event) {
        // Factor of 2 for legacy reasons: previously we did two wheel_scrolls for each mouse scroll.
        auto get_scroll_inc = [&] { return prefs->getIntLimited("/options/wheelscroll/value", 40, 0, 1000) * 2; };

        auto const action = Modifiers::Modifier::which(Triggers::CANVAS | Triggers::SCROLL, event.modifiers);

        if (action == Type::CANVAS_ROTATE) {
            // Rotate by the amount vertically scrolled.

            if (_desktop->get_rotation_lock()) {
                return;
            }

            double const delta_y = event.delta.y();
            if (delta_y == 0) {
                return;
            }

            double angle;
            if (event.unit == Gdk::ScrollUnit::WHEEL) {
                double rotate_inc = prefs->getDoubleLimited("/options/rotateincrement/value", 15, 1, 90, "°");
                rotate_inc = Geom::rad_from_deg(rotate_inc);
                angle = delta_y * rotate_inc;
            } else {
                angle = delta_y * (Geom::rad_from_deg(15) / 10.0); // logical pixels to radians, arbitrary
                angle = std::clamp(angle, -1.0, 1.0); // values > 1 result in excessive rotating
            }

            _desktop->rotate_relative_keep_point(_desktop->point(), -angle);
            ret = true;

        } else if (action == Type::CANVAS_PAN_X) {
            // Scroll horizontally by the amount vertically scrolled.

            double delta_y = event.delta.y();
            if (delta_y == 0) {
                return;
            }

            if (event.unit == Gdk::ScrollUnit::WHEEL) {
                delta_y *= get_scroll_inc();
            } else {
                delta_y *= 8; // subjective factor
            }

            _desktop->scroll_relative({-delta_y, 0});
            ret = true;

        } else if (action == Type::CANVAS_ZOOM && ( action != Type::CANVAS_DRAG_ZOOM ) ) {
            // Zoom by the amount vertically scrolled.

            double const delta_y = event.delta.y();
            if (delta_y == 0) {
                return;
            }

            double scale;
            if (event.unit == Gdk::ScrollUnit::WHEEL) {
                double const zoom_inc = prefs->getDoubleLimited("/options/zoomincrement/value", M_SQRT2, 1.01, 10);
                scale = std::pow(zoom_inc, delta_y);
            } else {
                scale = delta_y / 10; // logical pixels to scale, arbitrary
                scale = std::clamp(scale, -1.0, 1.0); // values > 1 result in excessive zooming
                scale = std::pow(M_SQRT2, scale);
            }

            _desktop->zoom_relative(_desktop->point(), 1.0 / scale);
            ret = true;

        } else if (action == Type::CANVAS_PAN_Y) {
            // Scroll both horizontally and vertically.

            auto delta = event.delta;
            if (delta == Geom::Point(0, 0)) {
                return;
            }

            if (event.unit == Gdk::ScrollUnit::WHEEL) {
                delta *= get_scroll_inc();
            } else {
                delta *= 8; // subjective factor
            }

            _desktop->scroll_relative(-delta);
            ret = true;

        } else {
            g_warning("unhandled scroll event with scroll.state=0x%x", event.modifiers);
        }
    },

    [&] (CanvasEvent const &event) {}
    );

    return ret;
}

/**
 * This function allows to handle global tool events if _pre function is not fully overridden.
 */
void ToolBase::set_on_buttons(CanvasEvent const &event)
{
    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (event.num_press != 1) {
                return;
            }
            switch (event.button) {
                case 1:
                    _button1on = true;
                    break;
                case 2:
                    _button2on = true;
                    break;
                case 3:
                    _button3on = true;
                    break;
                default:
                    break;
            }
        },
        [&] (ButtonReleaseEvent const &event) {
            switch (event.button) {
                case 1:
                    _button1on = false;
                    break;
                case 2:
                    _button2on = false;
                    break;
                case 3:
                    _button3on = false;
                    break;
                default:
                    break;
            }
        },
        [&] (MotionEvent const &event) {
            _button1on = event.modifiers & (unsigned)Gdk::ModifierType::BUTTON1_MASK;
            _button2on = event.modifiers & (unsigned)Gdk::ModifierType::BUTTON2_MASK;
            _button3on = event.modifiers & (unsigned)Gdk::ModifierType::BUTTON3_MASK;
        },
        [&] (CanvasEvent const &event) {}
    );
}

bool ToolBase::are_buttons_1_and_3_on() const
{
    return _button1on && _button3on;
}

bool ToolBase::are_buttons_1_and_3_on(CanvasEvent const &event)
{
    set_on_buttons(event);
    return are_buttons_1_and_3_on();
}

/**
 * Handles item specific events. Gets called from Gdk.
 *
 * Only reacts to right mouse button at the moment.
 * \todo Fixme: do context sensitive popup menu on items.
 */
bool ToolBase::item_handler(SPItem *item, CanvasEvent const &event)
{
    bool ret = false;

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (!are_buttons_1_and_3_on(event) && event.button == 3 &&
                !mod_canvas_pan_drag->active(event.modifiers)) {
                menu_popup(event);
                ret = true;
            } else if (event.button == 1 && shape_editor && shape_editor->has_knotholder()) {
                // This allows users to select an arbitary position in a pattern to edit on canvas.
                auto &knotholder = shape_editor->knotholder;
                auto point = event.pos;
                if (_desktop->getItemAtPoint(point, true) == knotholder->getItem()) {
                    ret = knotholder->set_item_clickpos(_desktop->w2d(point) * _desktop->dt2doc());
                }
            }
        },
        [&] (CanvasEvent const &event) {}
    );
    return ret;
}

/**
 * Returns true if we're hovering above a knot (needed because we don't want to pre-snap in that case).
 */
bool ToolBase::sp_event_context_knot_mouseover() const
{
    if (shape_editor) {
        return shape_editor->knot_mouseover();
    }

    return false;
}

/**
 * Enables/disables the ToolBase's SelCue.
 */
void ToolBase::enableSelectionCue(bool enable)
{
    if (enable) {
        if (!_selcue) {
            _selcue = new Inkscape::SelCue(_desktop);
        }
    } else {
        delete _selcue;
        _selcue = nullptr;
    }
}

/*
 * Enables/disables the ToolBase's GrDrag.
 */
void ToolBase::enableGrDrag(bool enable)
{
    if (enable) {
        if (!_grdrag) {
            _grdrag = new GrDrag(_desktop);
        }
    } else {
        if (_grdrag) {
            delete _grdrag;
            _grdrag = nullptr;
        }
    }
}

/**
 * Delete a selected GrDrag point
 */
bool ToolBase::deleteSelectedDrag(bool just_one)
{
    if (_grdrag && !_grdrag->selected.empty()) {
        _grdrag->deleteSelected(just_one);
        return true;
    }
    return false;
}

/**
 * Return true if there is a gradient drag.
 */
bool ToolBase::hasGradientDrag() const
{
    return _grdrag && _grdrag->isNonEmpty();
}

/**
 * Grab events from the Canvas Catchall. (Common configuration.)
 */
void ToolBase::grabCanvasEvents(EventMask mask)
{
    _desktop->getCanvasCatchall()->grab(mask); // Cursor is null.
}

/**
 * Ungrab events from the Canvas Catchall. (Common configuration.)
 */
void ToolBase::ungrabCanvasEvents()
{
    _desktop->getSnapIndicator()->remove_snaptarget();
    _desktop->getCanvasCatchall()->ungrab();
}

/** Enable (or disable) high precision for motion events
  *
  * This is intended to be used by drawing tools, that need to process motion events with high accuracy
  * and high update rate (for example free hand tools)
  *
  * With standard accuracy some intermediate motion events might be discarded
  *
  * Call this function when an operation that requires high accuracy is started (e.g. mouse button is pressed
  * to draw a line). Make sure to call it again and restore standard precision afterwards. **/
void ToolBase::set_high_motion_precision(bool high_precision)
{
    // Todo: High-precision mode must now be implemented on a tool-by-tool basis.
    // This function stub allows us to see where this is required.
}

void ToolBase::setup_for_drag_start(ButtonPressEvent const &ev)
{
    saveDragOrigin(ev.pos);
    auto const force_drag = mod_select_force_drag->active(ev.modifiers);
    item_to_select = sp_event_context_find_item(_desktop, ev.pos, force_drag, true);
}

void ToolBase::saveDragOrigin(Geom::Point const &pos)
{
    xyp = pos.floor();
    within_tolerance = true;
}

/**
 * Analyse the current position and return true once it has moved farther than tolerance
 * from the drag origin (indicating they intend to move the object, not click).
 */
bool ToolBase::checkDragMoved(Geom::Point const &pos)
{
    if (within_tolerance) {
        if (Geom::LInfty(pos.floor() - xyp) < tolerance) {
            // Do not drag if within tolerance from origin.
            return false;
        }
        // Mark drag as started.
        within_tolerance = false;
    }
    // Always return true once the drag has started.
    return true;
}

/**
 * Calls virtual set() function of ToolBase.
 */
void sp_event_context_read(ToolBase *tool, char const *key)
{
    if (!tool || !key) return;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Inkscape::Preferences::Entry val = prefs->getEntry(tool->getPrefsPath() + '/' + key);
    tool->set(val);
}

/**
 * Common code between root and item handlers related to delayed snap events.
 */
void ToolBase::_filterEventForSnapping(SPItem *item, CanvasEvent const &event, DelayedSnapEvent::Origin origin)
{
    inspect_event(event,
        [&] (MotionEvent const &event) {
            snap_delay_handler(item, nullptr, event, origin);
        },
        [&] (ButtonReleaseEvent const &event) {
            // If we have any pending snapping action, then invoke it now
            process_delayed_snap_event();
        },
        [&] (ButtonPressEvent const &event) {
            // Snapping will be on hold if we're moving the mouse at high speeds. When starting
            // drawing a new shape we really should snap though.
            _desktop->getNamedView()->snap_manager.snapprefs.setSnapPostponedGlobally(false);
        },
        [&] (CanvasEvent const &event) {}
    );
}

/**
 * Handles snapping events for all tools and then passes to tool_root_handler.
 */
bool ToolBase::start_root_handler(CanvasEvent const &event)
{
    if constexpr (DEBUG_EVENTS) {
        dump_event(event, "ToolBase::start_root_handler");
    }

    if (!_uses_snap) {
        return tool_root_handler(event);
    }

    _filterEventForSnapping(nullptr, event, DelayedSnapEvent::EVENTCONTEXT_ROOT_HANDLER);

    return tool_root_handler(event);
}

/**
 * Calls the right tool's event handler, depending on the selected tool and state.
 */
bool ToolBase::tool_root_handler(CanvasEvent const &event)
{
    if constexpr (DEBUG_EVENTS) {
        dump_event(event, "ToolBase::tool_root_handler");
    }

    // Just set the on buttons for now. later, behave as intended.
    set_on_buttons(event);

    // refresh coordinates UI here while 'event' is still valid
    set_event_location(_desktop, event);

    // Panning has priority over tool-specific event handling
    if (is_panning()) {
        if (ToolBase::root_handler(event)) {
            return true;
        }
        if (event.type() != EventType::BUTTON_RELEASE) {
            return false;
        }
        // still try to deliver button release events not consumed by panning
    }
    return root_handler(event);
}

/**
 * Starts handling item snapping and pass to virtual_item_handler afterwards.
 */
bool ToolBase::start_item_handler(SPItem *item, CanvasEvent const &event)
{
    if (!_uses_snap) {
        return virtual_item_handler(item, event);
    }

    _filterEventForSnapping(item, event, DelayedSnapEvent::EVENTCONTEXT_ITEM_HANDLER);

    return virtual_item_handler(item, event);
}

bool ToolBase::virtual_item_handler(SPItem *item, CanvasEvent const &event)
{
    bool ret = false;

    // Just set the on buttons for now. later, behave as intended.
    set_on_buttons(event);

    // Panning has priority over tool-specific event handling
    if (is_panning()) {
        ret = ToolBase::item_handler(item, event);
    } else {
        ret = item_handler(item, event);
    }

    if (!ret) {
        ret = tool_root_handler(event);
    } else {
        set_event_location(_desktop, event);
    }

    return ret;
}

/**
 * Shows coordinates on status bar.
 */
static void set_event_location(SPDesktop *desktop, CanvasEvent const &event)
{
    if (event.type() != EventType::MOTION) {
        return;
    }

    auto const button_w = static_cast<MotionEvent const &>(event).pos;
    auto const button_dt = desktop->w2d(button_w);
    desktop->set_coordinate_status(button_dt);
}

//-------------------------------------------------------------------
/**
 * Create popup menu and tell Gtk to show it.
 */
void ToolBase::menu_popup(CanvasEvent const &event, SPObject *obj)
{
    if (!obj) {
        if (event.type() == EventType::KEY_PRESS && !_desktop->getSelection()->isEmpty()) {
            obj = _desktop->getSelection()->items().front();
        } else if (event.type() == EventType::BUTTON_PRESS) {
            // Using the same function call used on left click in sp_select_context_item_handler() to get top of z-order
            // fixme: sp_canvas_arena should set the top z-order object as arena->active
            auto p = static_cast<ButtonPressEvent const &>(event).pos;
            obj = sp_event_context_find_item(_desktop, p, false, false);
        }
    }

    auto const popup = [&] (std::optional<Geom::Point> const &pos) {
        // Get a list of items under the cursor, used for unhiding and unlocking.
        auto point_win = _desktop->point() * _desktop->d2w();
        auto items_under_cursor = _desktop->getItemsAtPoints({point_win}, true, false, 0, false);
        auto menu = Gtk::make_managed<ContextMenu>(_desktop, obj, items_under_cursor);
        _desktop->getDesktopWidget()->get_canvas_grid()->setPopover(menu);
        UI::popup_at(*menu, *_desktop->getCanvas(), pos);
    };

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            popup(event.orig_pos);
        },
        [&] (KeyPressEvent const &event) {
            popup(event.orig_pos);
        },
        [&] (CanvasEvent const &event) {}
    );
}

/**
 * Show tool context specific modifier tip.
 */
void sp_event_show_modifier_tip(MessageContext *message_context,
                                KeyEvent const &event, char const *ctrl_tip, char const *shift_tip,
                                char const *alt_tip)
{
    auto const keyval = get_latin_keyval(event);

    bool ctrl =  ctrl_tip  && (mod_ctrl(event)  || keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R);
    bool shift = shift_tip && (mod_shift(event) || keyval == GDK_KEY_Shift_L   || keyval == GDK_KEY_Shift_R);
    bool alt =   alt_tip   && (mod_alt(event)   || keyval == GDK_KEY_Alt_L     || keyval == GDK_KEY_Alt_R
                                                || keyval == GDK_KEY_Meta_L    || keyval == GDK_KEY_Meta_R);

    char *tip = g_strdup_printf("%s%s%s%s%s", ctrl ? ctrl_tip : "",
                                              ctrl && (shift || alt) ? "; " : "",
                                              shift ? shift_tip : "",
                                              (ctrl || shift) && alt ? "; " : "",
                                              alt ? alt_tip : "");

    if (std::strlen(tip) > 0) {
        message_context->flash(INFORMATION_MESSAGE, tip);
    }

    g_free(tip);
}

/**
 * Try to determine the keys group of Latin layout.
 * Check available keymap entries for Latin 'a' key and find the minimal integer value.
 */
static void update_latin_keys_group()
{
    GdkKeymapKey* keys;
    gint n_keys;

    latin_keys_group_valid = FALSE;
    latin_keys_groups.clear();

    if (gdk_display_map_keyval(gdk_display_get_default(), GDK_KEY_a, &keys, &n_keys)) {
        for (int i = 0; i < n_keys; i++) {
            latin_keys_groups.insert(keys[i].group);

            if (!latin_keys_group_valid || keys[i].group < latin_keys_group) {
                latin_keys_group = keys[i].group;
                latin_keys_group_valid = true;
            }
        }
        g_free(keys);
    }
}

/**
 * Initialize Latin keys group handling.
 */
void init_latin_keys_group()
{
    auto const keyboard = Gdk::Display::get_default()->get_default_seat()->get_keyboard();
    g_assert(keyboard);
    keyboard->signal_changed().connect(&update_latin_keys_group);
#if GTKMM_CHECK_VERSION(4, 18, 0)
    keyboard->property_layout_names().signal_changed().connect(&update_latin_keys_group);
#endif
    update_latin_keys_group();
}

unsigned get_latin_keyval_impl(unsigned const event_keyval, unsigned const event_keycode,
                               GdkModifierType const event_state, unsigned const event_group,
                               unsigned *consumed_modifiers)
{
    auto keyval = 0u;
    GdkModifierType modifiers;

    auto group = latin_keys_group_valid ? latin_keys_group : event_group;
    if (latin_keys_groups.count(event_group)) {
        // Keyboard group is a latin layout, so just use it.
        group = event_group;
    }
    gdk_display_translate_key(gdk_display_get_default(),
                              event_keycode,
                              event_state,
                              group,
                              &keyval,
                              nullptr,
                              nullptr,
                              &modifiers);
    if (consumed_modifiers) {
        *consumed_modifiers = modifiers;
    }
    // gdk_display_translate_key does not have information on whether numlock was active during the press.
    // So if numlock is on, we'll get the wrong keyval as a result (which is not a latin layout difference, it's just
    // how numlock & numpads work). So just skip translating numpad keys.
    if (keyval >= GDK_KEY_KP_Space && keyval <= GDK_KEY_KP_9) {
        keyval = event_keyval;
    }
#ifndef __APPLE__
    // on macOS <option> key inserts special characters and below condition fires all the time
    if (keyval != event_keyval) {
        std::cerr << "get_latin_keyval: OH OH OH keyval did change! "
                  << "  keyval: " << keyval << " (" << (char)keyval << ")"
                  << "  event_keyval: " << event_keyval << "(" << (char)event_keyval << ")" << std::endl;
    }
#endif

    return keyval;
}

/**
 * Return the keyval corresponding to the event controller key in Latin group.
 *
 * Use this instead of simply a signal's keyval, so that your keyboard shortcuts
 * work regardless of layouts (e.g., in Cyrillic).
 */
unsigned get_latin_keyval(GtkEventControllerKey const * const controller,
                          unsigned const keyval, unsigned const keycode, GdkModifierType const state,
                          unsigned *consumed_modifiers /*= nullptr*/)
{
    auto const group = gtk_event_controller_key_get_group(const_cast<GtkEventControllerKey *>(controller));
    return get_latin_keyval_impl(keyval, keycode, state, group, consumed_modifiers);
}

unsigned get_latin_keyval(Gtk::EventControllerKey const &controller,
                          unsigned keyval, unsigned keycode, Gdk::ModifierType state,
                          unsigned *consumed_modifiers /*= nullptr*/)
{
    auto const group = controller.get_group();
    return get_latin_keyval_impl(keyval, keycode, static_cast<GdkModifierType>(state), group, consumed_modifiers);
}

unsigned get_latin_keyval(KeyEvent const &event, unsigned *consumed_modifiers)
{
    return get_latin_keyval_impl(event.keyval, event.keycode,
                                 static_cast<GdkModifierType>(event.modifiers),
                                 event.group, consumed_modifiers);
}

/**
 * Returns item at point p in desktop.
 *
 * If state includes alt key mask, cyclically selects under; honors
 * into_groups.
 */
SPItem *sp_event_context_find_item(SPDesktop *desktop, Geom::Point const &p,
                                   bool select_under, bool into_groups)
{
    SPItem *item = nullptr;

    if (select_under) {
        auto tmp = desktop->getSelection()->items();
        std::vector<SPItem *> vec(tmp.begin(), tmp.end());
        SPItem *selected_at_point = desktop->getItemFromListAtPointBottom(vec, p);
        item = desktop->getItemAtPoint(p, into_groups, selected_at_point);
        if (!item) { // we may have reached bottom, flip over to the top
            item = desktop->getItemAtPoint(p, into_groups, nullptr);
        }
    } else {
        item = desktop->getItemAtPoint(p, into_groups, nullptr);
    }

    return item;
}

/**
 * Returns item if it is under point p in desktop, at any depth; otherwise returns NULL.
 *
 * Honors into_groups.
 */
SPItem *sp_event_context_over_item(SPDesktop *desktop, SPItem *item, Geom::Point const &p)
{
    std::vector<SPItem*> temp;
    temp.push_back(item);
    SPItem *item_at_point = desktop->getItemFromListAtPointBottom(temp, p);
    return item_at_point;
}

ShapeEditor *sp_event_context_get_shape_editor(ToolBase *tool)
{
    return tool->shape_editor;
}

/**
 * Analyses the current event, calculates the mouse speed, turns snapping off (temporarily) if the
 * mouse speed is above a threshold, and stores the current event such that it can be re-triggered when needed
 * (re-triggering is controlled by a timeout).
 *
 * @param item Pointer that store a reference to a canvas or to an item.
 * @param item2 Another pointer, storing a reference to a knot or controlpoint.
 * @param event Pointer to the motion event.
 * @param origin Identifier (enum) specifying where the delay (and the call to this method) were initiated.
 */
void ToolBase::snap_delay_handler(gpointer item, gpointer item2, MotionEvent const &event, DelayedSnapEvent::Origin origin)
{
    static uint32_t prev_time;
    static std::optional<Geom::Point> prev_pos;

    if (!_uses_snap || _dse_callback_in_process) {
        return;
    }

    // No point duplicating events if no delay.
    auto const delay = Preferences::get()->getDoubleLimited("/options/snapdelay/value", 0, 0, 1000);
    if (delay == 0) {
        return;
    }

    // Snapping occurs when dragging with the left mouse button down, or when hovering e.g. in the pen tool with left mouse button up
    bool const c1 = event.modifiers & GDK_BUTTON2_MASK; // We shouldn't hold back any events when other mouse buttons have been
    bool const c2 = event.modifiers & GDK_BUTTON3_MASK; // pressed, e.g. when scrolling with the middle mouse button; if we do then
    // Inkscape will get stuck in an unresponsive state
    bool const c3 = dynamic_cast<CalligraphicTool*>(this);
    // The snap delay will repeat the last motion event, which will lead to
    // erroneous points in the calligraphy context. And because we don't snap
    // in this context, we might just as well disable the snap delay all together
    bool const c4 = is_panning(); // Don't snap while panning

    if (c1 || c2 || c3 || c4) {
        // Make sure that we don't send any pending snap events to a context if we know in advance
        // that we're not going to snap any way (e.g. while scrolling with middle mouse button)
        // Any motion event might affect the state of the context, leading to unexpected behavior
        discard_delayed_snap_event();
    } else if (getDesktop() && getDesktop()->getNamedView()->snap_manager.snapprefs.getSnapEnabledGlobally()) {
        // Snap when speed drops below e.g. 0.02 px/msec, or when no motion events have occurred for some period.
        // i.e. snap when we're at stand still. A speed threshold enforces snapping for tablets, which might never
        // be fully at stand still and might keep spitting out motion events.
        getDesktop()->getNamedView()->snap_manager.snapprefs.setSnapPostponedGlobally(true); // put snapping on hold

        auto event_pos = event.pos;
        uint32_t event_t = event.time;

        if (prev_pos) {
            auto dist = Geom::L2(event_pos - *prev_pos);
            uint32_t delta_t = event_t - prev_time;
            double speed = delta_t > 0 ? dist / delta_t : 1000;
            //std::cout << "Mouse speed = " << speed << " px/msec " << std::endl;
            if (speed > 0.02) { // Jitter threshold, might be needed for tablets
                // We're moving fast, so postpone any snapping until the next GDK_MOTION_NOTIFY event. We
                // will keep on postponing the snapping as long as the speed is high.
                // We must snap at some point in time though, so set a watchdog timer at some time from
                // now, just in case there's no future motion event that drops under the speed limit (when
                // stopping abruptly)
                _dse.emplace(this, item, item2, event, origin);
                _schedule_delayed_snap_event(); // watchdog is reset, i.e. pushed forward in time
                // If the watchdog expires before a new motion event is received, we will snap (as explained
                // above). This means however that when the timer is too short, we will always snap and that the
                // speed threshold is ineffective. In the extreme case the delay is set to zero, and snapping will
                // be immediate, as it used to be in the old days ;-).
            } else { // Speed is very low, so we're virtually at stand still
                // But if we're really standing still, then we should snap now. We could use some low-pass filtering,
                // otherwise snapping occurs for each jitter movement. For this filtering we'll leave the watchdog to expire,
                // snap, and set a new watchdog again.
                if (!_dse) { // no watchdog has been set
                    // it might have already expired, so we'll set a new one; the snapping frequency will be limited this way
                    _dse.emplace(this, item, item2, event, origin);
                    _schedule_delayed_snap_event();
                } // else: watchdog has been set before and we'll wait for it to expire
            }
        } else {
            // This is the first GDK_MOTION_NOTIFY event, so postpone snapping and set the watchdog
            g_assert(!_dse);
            _dse.emplace(this, item, item2, event, origin);
            _schedule_delayed_snap_event();
        }

        prev_pos = event_pos;
        prev_time = event_t;
    }
}

/**
 * When the delayed snap event timer expires, this method will be called and will re-inject the last motion
 * event in an appropriate place, with snapping being turned on again.
 */
void ToolBase::process_delayed_snap_event()
{
    // Snap NOW! For this the "postponed" flag will be reset and the last motion event will be repeated

    _dse_timeout_conn.disconnect();

    if (!_dse) {
        // This might occur when this method is called directly, i.e. not through the timer
        // E.g. on GDK_BUTTON_RELEASE in start_root_handler()
        return;
    }

    auto dt = getDesktop();
    if (!dt) {
        _dse.reset();
        return;
    }

    _dse_callback_in_process = true;
    dt->getNamedView()->snap_manager.snapprefs.setSnapPostponedGlobally(false);

    // Depending on where the delayed snap event originated from, we will inject it back at its origin.
    // The switch below takes care of that and prepares the relevant parameters.
    switch (_dse->getOrigin()) {
    case DelayedSnapEvent::EVENTCONTEXT_ROOT_HANDLER:
        tool_root_handler(_dse->getEvent());
        break;
    case DelayedSnapEvent::EVENTCONTEXT_ITEM_HANDLER: {
        auto item = reinterpret_cast<SPItem*>(_dse->getItem());
        if (item) {
            virtual_item_handler(item, _dse->getEvent());
        }
        break;
    }
    case DelayedSnapEvent::KNOT_HANDLER: {
        auto knot = reinterpret_cast<SPKnot*>(_dse->getItem2());
        check_if_knot_deleted(knot);
        if (knot) {
            bool was_grabbed = knot->is_grabbed();
            knot->setFlag(SP_KNOT_GRABBED, true); // Must be grabbed for Inkscape::SelTrans::handleRequest() to pass
            knot->handler_request_position(_dse->getEvent());
            knot->setFlag(SP_KNOT_GRABBED, was_grabbed);
        }
        break;
    }
    case DelayedSnapEvent::CONTROL_POINT_HANDLER: {
        using Inkscape::UI::ControlPoint;
        auto point = reinterpret_cast<ControlPoint*>(_dse->getItem2());
        if (point) {
            if (point->position().isFinite() && dt == point->_desktop) {
                point->_eventHandler(this, _dse->getEvent());
            } else {
                //workaround:
                //[Bug 781893] Crash after moving a Bezier node after Knot path effect?
                // --> at some time, some point with X = 0 and Y = nan (not a number) is created ...
                //     even so, the desktop pointer is invalid and equal to 0xff
                g_warning("encountered non-finite point when evaluating snapping callback");
            }
        }
        break;
    }
    case DelayedSnapEvent::GUIDE_HANDLER: {
        auto guideline = reinterpret_cast<CanvasItemGuideLine*>(_dse->getItem());
        auto guide     = reinterpret_cast<SPGuide*>            (_dse->getItem2());
        if (guideline && guide) {
            sp_dt_guide_event(_dse->getEvent(), guideline, guide);
        }
        break;
    }
    case DelayedSnapEvent::GUIDE_HRULER:
    case DelayedSnapEvent::GUIDE_VRULER: {
        auto canvas_grid = reinterpret_cast<Widget::CanvasGrid*>(_dse->getItem());
        bool horiz = _dse->getOrigin() == DelayedSnapEvent::GUIDE_HRULER;
        canvas_grid->rulerMotion(_dse->getEvent(), horiz);
        break;
    }
    default:
        g_warning("Origin of snap-delay event has not been defined!");
        break;
    }

    _dse_callback_in_process = false;
    _dse.reset();
}

/**
 * If a delayed snap event has been scheduled, this function will cancel it.
 */
void ToolBase::discard_delayed_snap_event()
{
    _desktop->getNamedView()->snap_manager.snapprefs.setSnapPostponedGlobally(false);
    _dse.reset();
}

/**
 * Internal function used to set process_delayed_snap_event() to occur a given delay in the future
 * from now. Subsequent calls will reset the timer. Calling process_delayed_snap_event() manually
 * will cancel the timer.
 */
void ToolBase::_schedule_delayed_snap_event()
{
    // Get timeout value in seconds.
    double value = Preferences::get()->getDoubleLimited("/options/snapdelay/value", 0, 0, 1000);

    // If the timeout value is too large, we assume it comes from an old preferences file
    // where it used to be measured in milliseconds, and convert it appropriately.
    if (value > 1.0) {
        value /= 1000.0; // convert milliseconds to seconds
    }

    _dse_timeout_conn = Glib::signal_timeout().connect([this] {
        process_delayed_snap_event();
        return false; // one-shot
    }, value * 1000.0);
}

void ToolBase::set_last_active_tool(Glib::ustring last_tool) {
    _last_active_tool = std::move(last_tool);
}

const Glib::ustring& ToolBase::get_last_active_tool() const {
    return _last_active_tool;
}

bool ToolBase::has_focus() const {
    return UI::contains_focus(*_desktop->getCanvas());
}

} // namespace Tools
} // namespace UI
} // namespace Inkscape

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
