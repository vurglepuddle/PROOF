// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Frank Felfe <innerspace@iname.com>
 *
 * Copyright (C) 1999-2002 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLS_TOOL_BASE_H
#define INKSCAPE_UI_TOOLS_TOOL_BASE_H

#include <boost/noncopyable.hpp>
#include <gdkmm/cursor.h>

#include "desktop.h"
#include "object/weakptr.h"
#include "preferences.h"
#include "ui/modifiers.h"
#include "ui/widget/events/enums.h"
#include "util/action-accel.h"

namespace Gtk {
class EventControllerKey;
class Widget;
} // namespace Gtk

class GrDrag;
class SPDesktop;
class SPObject;
class SPItem;
class SPGroup;
class KnotHolder;

namespace Inkscape {
class MessageContext;
class SelCue;
struct CanvasEvent;
struct MotionEvent;
struct KeyEvent;
struct ButtonPressEvent;

namespace UI {
class ShapeEditor;

namespace Tools {
class ToolBase;

class DelayedSnapEvent
{
public:
    enum Origin
    {
        UNDEFINED_HANDLER,
        EVENTCONTEXT_ROOT_HANDLER,
        EVENTCONTEXT_ITEM_HANDLER,
        KNOT_HANDLER,
        CONTROL_POINT_HANDLER,
        GUIDE_HANDLER,
        GUIDE_HRULER,
        GUIDE_VRULER
    };

    DelayedSnapEvent(ToolBase *tool, gpointer item, gpointer item2, MotionEvent const &event, DelayedSnapEvent::Origin origin);

    ToolBase *getTool() const { return _tool; }
    gpointer getItem() const { return _item; }
    gpointer getItem2() const { return _item2; }
    MotionEvent const &getEvent() const { return *_event; }
    Origin getOrigin() const { return _origin; }

private:
    ToolBase *_tool;
    gpointer _item;
    gpointer _item2;
    std::unique_ptr<MotionEvent> _event;
    Origin _origin;
};

/**
 * Base class for Event processors.
 *
 * This is per desktop object, which (its derivatives) implements
 * different actions bound to mouse events.
 *
 * ToolBase is an abstract base class of all tools. As the name
 * indicates, event context implementations process UI events (mouse
 * movements and keypresses) and take actions (like creating or modifying
 * objects). There is one event context implementation for each tool,
 * plus few abstract base classes. Writing a new tool involves
 * subclassing ToolBase.
 */
class ToolBase
    : public sigc::trackable
    , boost::noncopyable
{
public:
    ToolBase(SPDesktop *desktop, std::string &&prefs_path, std::string &&cursor_filename, bool uses_snap = true);
    virtual ~ToolBase();

    virtual void set(Preferences::Entry const &val);
    virtual bool root_handler(CanvasEvent const &event);
    virtual bool item_handler(SPItem *item, CanvasEvent const &event);
    virtual void menu_popup(CanvasEvent const &event, SPObject *obj = nullptr);
    virtual bool can_undo(bool redo = false) { return false; }
    virtual void focus_first_widget();
    virtual bool is_ready() const { return true; }
    virtual void switching_away(std::string const &new_tool) {}

    std::string const &getPrefsPath() const { return _prefs_path; };
    void enableSelectionCue(bool enable = true);

    MessageContext *defaultMessageContext() const { return message_context.get(); }

    SPDesktop *getDesktop() const { return _desktop; }
    SPGroup *currentLayer() const;

    // name of the last active tool (if known) before *this* one, or empty string
    void set_last_active_tool(Glib::ustring last_tool);
    const Glib::ustring& get_last_active_tool() const;

private:
    std::unique_ptr<Preferences::PreferencesObserver> pref_observer;
    std::string _prefs_path;

    void set_on_buttons(CanvasEvent const &event);
    bool are_buttons_1_and_3_on() const;
    bool are_buttons_1_and_3_on(CanvasEvent const &event);

protected:
    Glib::RefPtr<Gdk::Cursor> _cursor;
    std::string _cursor_filename = "select.svg";
    std::string _cursor_default = "select.svg";

    Geom::IntPoint xyp;             ///< where drag started
    bool dragging = false;          ///< are we dragging?
    int tolerance = 0;
    bool within_tolerance = false;  ///< are we still within tolerance of origin
    bool _button1on = false;
    bool _button2on = false;
    bool _button3on = false;
    /// The item where mouse_press occurred, to be selected if this is a click not drag. Weak: a
    /// tool may replace it before the release (the Mesh tool turns a shape into a mesh object on
    /// a double-click's second press), and selecting it then crashed PROOF (owner, 2026-10-26).
    SPWeakPtr<SPItem> item_to_select;

    void setup_for_drag_start(ButtonPressEvent const &ev);
    void saveDragOrigin(Geom::Point const &pos);
    bool checkDragMoved(Geom::Point const &pos);

    // Commonly used CanvasItemCatchall grab/ungrab.
    void grabCanvasEvents(EventMask mask =
                          EventType::KEY_PRESS |
                          EventType::BUTTON_RELEASE |
                          EventType::MOTION |
                          EventType::BUTTON_PRESS);
    void ungrabCanvasEvents();

    sigc::scoped_connection _hide_selection_connection;
    virtual void onHideSelectionChanged(bool hide) {};

private:
    enum Panning
    {
        PANNING_NONE = 0,          //
        PANNING_SPACE_BUTTON1 = 1, // TODO is this mode relevant?
        PANNING_BUTTON2 = 2,       //
        PANNING_BUTTON3 = 3,       //
        PANNING_SPACE = 4
    };
    Panning panning = PANNING_NONE;

    bool rotating = false;
    double start_angle, current_angle;
    bool check_lost_button_up = false;

public:
    bool start_root_handler(CanvasEvent const &event);
    bool tool_root_handler(CanvasEvent const &event);
    bool start_item_handler(SPItem *item, CanvasEvent const &event);
    bool virtual_item_handler(SPItem *item, CanvasEvent const &event);

    /// True if we're panning with any method (space bar, middle-mouse, right-mouse+Ctrl)
    bool is_panning() const { return panning != 0; }

    /// True if we're panning with the space bar
    bool is_space_panning() const { return panning == PANNING_SPACE || panning == PANNING_SPACE_BUTTON1; }

    std::unique_ptr<MessageContext> message_context;
    SelCue *_selcue = nullptr;

    GrDrag *_grdrag = nullptr;

    ShapeEditor *shape_editor = nullptr;

    void snap_delay_handler(gpointer item, gpointer item2, MotionEvent const &event, DelayedSnapEvent::Origin origin);
    void process_delayed_snap_event();
    void discard_delayed_snap_event();
    bool _uses_snap = false;

    void set_cursor(std::string filename);
    void use_cursor(Glib::RefPtr<Gdk::Cursor> cursor);
    Glib::RefPtr<Gdk::Cursor> get_cursor(Gtk::Widget &widget,
                                         std::string const &filename) const;
    void use_tool_cursor();

    void enableGrDrag(bool enable = true);
    bool deleteSelectedDrag(bool just_one);
    bool hasGradientDrag() const;
    GrDrag *get_drag() { return _grdrag; }

    bool has_focus() const;

protected:
    bool sp_event_context_knot_mouseover() const;
    void set_high_motion_precision(bool high_precision = true);

    SPDesktop *_desktop = nullptr;

    // Tool based shortcuts
    Util::ActionAccel _acc_undo;
    Util::ActionAccel _acc_redo;
    Util::ActionAccel _acc_quick_preview;
    Util::ActionAccel _acc_quick_zoom;
    Util::ActionAccel _acc_quick_pan;
    Util::ActionAccel _acc_focus_first_widget;
private:
    void _filterEventForSnapping(SPItem *item, CanvasEvent const &event, DelayedSnapEvent::Origin origin);
    bool _keyboardMove(KeyEvent const &event, Geom::Point const &dir);

    std::optional<DelayedSnapEvent> _dse;
    void _schedule_delayed_snap_event();
    sigc::scoped_connection _dse_timeout_conn;
    bool _dse_callback_in_process = false;
    Glib::ustring _last_active_tool;

    Modifiers::Modifier *mod_canvas_pan_drag;
    Modifiers::Modifier *mod_canvas_rotate_drag;
    Modifiers::Modifier *mod_canvas_rotate_reset;
    Modifiers::Modifier *mod_canvas_rotate_snapping;
    Modifiers::Modifier *mod_canvas_zoom_invert;
    Modifiers::Modifier *mod_canvas_zoom_rubberband;
    Modifiers::Modifier *mod_select_force_drag;
};

void sp_event_context_read(ToolBase *tool, char const *key);

// Gobbling is no longer possible in GTK4. The following stubs
// are left in place in case an alternative solution is required.
inline int gobble_key_events(unsigned keyval, unsigned mask) { return 0; }
inline void gobble_motion_events(unsigned mask) {}

void sp_event_show_modifier_tip(MessageContext *message_context, KeyEvent const &event,
                                char const *ctrl_tip, char const *shift_tip, char const *alt_tip);

void init_latin_keys_group();
// Prefer one of the non-_impl versions below. This is public for shortcuts.cpp.
unsigned get_latin_keyval_impl(unsigned event_keyval, unsigned event_keycode,
                               GdkModifierType event_state, unsigned event_group,
                               unsigned *consumed_modifiers);
unsigned get_latin_keyval(GtkEventControllerKey const *controller,
                          unsigned keyval, unsigned keycode, GdkModifierType state,
                          unsigned *consumed_modifiers = nullptr);
unsigned get_latin_keyval(Gtk::EventControllerKey const &controller,
                          unsigned keyval, unsigned keycode, Gdk::ModifierType state,
                          unsigned *consumed_modifiers = nullptr);
unsigned get_latin_keyval(KeyEvent const &event, unsigned *consumed_modifiers = nullptr);

SPItem *sp_event_context_find_item(SPDesktop *desktop, Geom::Point const &p, bool select_under, bool into_groups);
SPItem *sp_event_context_over_item(SPDesktop *desktop, SPItem *item, Geom::Point const &p);

void sp_toggle_dropper(SPDesktop *dt);

} // namespace Tools
} // namespace UI
} // namespace Inkscape

#endif // INKSCAPE_UI_TOOLS_TOOL_BASE_H

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
