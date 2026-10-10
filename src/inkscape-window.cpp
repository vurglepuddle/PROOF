// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Inkscape - An SVG editor.
 */
/*
 * Authors:
 *   Tavmjong Bah
 *
 * Copyright (C) 2018 Authors
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 * Read the file 'COPYING' for more information.
 */

#include "inkscape-window.h"

#include <iostream>
#include <gdkmm/surface.h>
#include <gtkmm/box.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/shortcutcontroller.h>
#include <sigc++/functors/mem_fun.h>

#include "desktop.h"
#include "desktop-events.h" // Handle key events
#include "document.h"
#include "enums.h"      // PREFS_WINDOW_GEOMETRY_NONE
#include "inkscape-application.h"

#include "actions/actions-canvas-mode.h"
#include "actions/actions-canvas-snapping.h"
#include "actions/actions-canvas-transform.h"
#include "actions/actions-dialogs.h"
#include "actions/actions-edit-window.h"
#include "actions/actions-file-window.h"
#include "actions/actions-help-url.h"
#include "actions/actions-layer.h"
#include "actions/actions-node-align.h" // Node alignment.
#include "actions/actions-node-tool.h"
#include "actions/actions-pages.h"
#include "actions/actions-paths.h"  // TEMP
#include "actions/actions-selection-window.h"
#include "actions/actions-tools.h"
#include "actions/actions-transform.h"
#include "actions/actions-view-mode.h"
#include "actions/actions-view-window.h"
#include "inkscape.h"
#include "object/sp-namedview.h"
#include "ui/desktop/menubar.h"
#include "ui/desktop/menu-set-tooltips-shift-icons.h"
#include "ui/dialog/dialog-manager.h"
#include "ui/dialog/dialog-window.h"
#include "ui/shortcuts.h"
#include "ui/util.h"
#include "ui/widget/desktop-widget.h"
#include "ui/widget/canvas/framecheck.h"
#include "util/enums.h"

using Inkscape::UI::Dialog::DialogManager;
using Inkscape::UI::Dialog::DialogContainer;
using Inkscape::UI::Dialog::DialogWindow;

InkscapeWindow::InkscapeWindow(SPDesktop *desktop)
    : _app{InkscapeApplication::instance()}
    , _document{desktop->getDocument()}
    , _desktop{desktop}
{
    assert(_document);

    set_name("InkscapeWindow");
    set_show_menubar(true);
    set_resizable(true);

    _app->gtk_app()->add_window(*this);

    // =================== Actions ===================

    // After canvas has been constructed.. move to canvas proper.
    add_actions_canvas_mode(this);          // Actions to change canvas display mode.
    add_actions_canvas_snapping(this);      // Actions to toggle on/off snapping modes.
    add_actions_canvas_transform(this);     // Actions to transform canvas view.
    add_actions_dialogs(this);              // Actions to open dialogs.
    add_actions_edit_window(this);          // Actions to edit.
    add_actions_file_window(this);          // Actions for file actions which are desktop dependent.
    add_actions_help_url(this);             // Actions to help url.
    add_actions_layer(this);                // Actions for layer.
    add_actions_node_align(this);           // Actions to align and distribute nodes (requiring Node tool).
    add_actions_node_tool(this);            // Actions to add, delete, etc. nodes (requiring Node tool).
    add_actions_page_tools(this);           // Actions specific to pages tool and toolbar
    add_actions_path(this);                 // Actions for paths. TEMP
    add_actions_select_window(this);        // Actions with desktop selection
    add_actions_tools(this);                // Actions to switch between tools.
    add_actions_transform(this);            // Actions for transforming against the screen zoom
    add_actions_view_mode(this);            // Actions to change how Inkscape canvas is displayed.
    add_actions_view_window(this);          // Actions to add/change window of Inkscape

    // Add document action group to window and export to DBus.
    add_document_actions();

    auto connection = _app->gio_app()->get_dbus_connection();
    if (connection) {
        std::string document_action_group_name = _app->gio_app()->get_dbus_object_path() + "/document/" + std::to_string(get_id());
        connection->export_action_group(document_action_group_name, _document->getActionGroup());
    }

    static bool first_window = true;
    if (first_window) {
        // This is called here (rather than in InkscapeApplication) solely to add win level action
        // tooltips to the menu label-to-tooltip map.
        build_menu();

       // On macOS, once a main window is opened, closing it should not quit the app.
#ifdef __APPLE__
        _app->gtk_app()->hold();
#endif

        first_window = false;
    }

    // =============== Build interface ===============

#ifdef _WIN32
    // One title/menu row on Windows. HeaderBar retains GTK's window controls,
    // drag region and double-click maximize behaviour; menus keep the same
    // action model and keyboard navigation as the application menubar.
    set_show_menubar(false);
    auto header = Gtk::make_managed<Gtk::HeaderBar>();
    header->add_css_class("workspace-header");
    header->set_decoration_layout(":minimize,maximize,close");
    auto menu = Gtk::make_managed<Gtk::PopoverMenuBar>(_app->gtk_app()->get_menubar());
    header->pack_start(*menu);
    // The document name takes the room the row has and gives way when there isn't enough;
    // the note after it (the color mode, then any view mode) always shows in full.
    auto title = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    title->add_css_class("workspace-window-title");
    auto name = Gtk::make_managed<Gtk::Label>();
    name->set_ellipsize(Pango::EllipsizeMode::END);
    auto note = Gtk::make_managed<Gtk::Label>();
    title->append(*name);
    title->append(*note);
    header->set_title_widget(*title);
    property_title().signal_changed().connect([this, title, name, note] {
        auto full = get_title();
        auto shown = full.substr(0, full.find(" - Inkscape"));
        // "name (CMYK)", "name (RGB, outline)": the note is the last bracket.
        auto at = shown.rfind(" (");
        name->set_text(at == Glib::ustring::npos ? shown : shown.substr(0, at));
        note->set_text(at == Glib::ustring::npos ? Glib::ustring() : shown.substr(at));
        title->set_tooltip_text(full);
    });
    set_titlebar(*header);
    show_icons_and_tooltips(*menu);
#endif

    // Desktop widget (=> MultiPaned) (After actions added as this initializes shortcuts via CommandDialog.)
    _desktop_widget = Gtk::make_managed<SPDesktopWidget>(this);
    set_child(*_desktop_widget);

    _desktop_widget->addDesktop(desktop);

    // Resize the window to match the document properties early, before we can possibly be shown.
    Widget::realize();
    sp_namedview_window_from_document(desktop);

    // ================== Callbacks ==================
    property_is_active().signal_changed().connect(sigc::mem_fun(*this, &InkscapeWindow::on_is_active_changed));
    signal_close_request().connect(sigc::mem_fun(*this, &InkscapeWindow::on_close_request), false); // before
    property_default_width ().signal_changed().connect(sigc::mem_fun(*this, &InkscapeWindow::on_size_changed));
    property_default_height().signal_changed().connect(sigc::mem_fun(*this, &InkscapeWindow::on_size_changed));
    property_maximized().signal_changed().connect(sigc::mem_fun(*this, &InkscapeWindow::on_size_changed));
    property_fullscreened().signal_changed().connect(sigc::mem_fun(*this, &InkscapeWindow::on_size_changed));

    // Show dialogs after the main window, otherwise dialogs may be associated as the main window of the program.
    // Restore short-lived floating dialogs state if this is the first window being opened
    bool include_short_lived = _app->get_number_of_windows() == 1;
    DialogManager::singleton().restore_dialogs_state(_desktop_widget->getDialogContainer(), include_short_lived);

    // ================= Menu icons/tooltips =================
    // Note: The menu is defined at the app level but showing icons/tooltips requires actual widgets and
    // must be done on the window level.
    for (auto &child : Inkscape::UI::children(*this)) {
        if (auto const menubar = dynamic_cast<Gtk::PopoverMenuBar *>(&child)) {
            show_icons_and_tooltips(*menubar);
        }
    }

    // ================== Shortcuts ==================
    auto& shortcuts_instance = Inkscape::Shortcuts::getInstance();
    _shortcut_controller = Gtk::ShortcutController::create(shortcuts_instance.get_liststore());
    _shortcut_controller->set_scope(Gtk::ShortcutScope::LOCAL);
    _shortcut_controller->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
    add_controller(_shortcut_controller);

    // Update shortcuts in menus (due to bug in Gtk4 where menus are not updated when liststore is changed).
    // However, this will not remove a shortcut label if there is no longer a shortcut for menu item.
    shortcuts_instance.connect_changed([this]() {
        remove_controller(_shortcut_controller);
        add_controller(_shortcut_controller);
        // Todo: Trigger update_gui_text_recursive here rather than in preferences dialog.
    });

    // Add shortcuts to tooltips, etc. (but not menus).
    shortcuts_instance.update_gui_text_recursive(this);
}

void InkscapeWindow::on_realize()
{
    Gtk::ApplicationWindow::on_realize();

    // Note: Toplevel only becomes non-null after realisation.
    _toplevel_state_connection = get_toplevel()->property_state().signal_changed().connect(
        sigc::mem_fun(*this, &InkscapeWindow::on_toplevel_state_changed)
    );

    // PROOF: with Framecheck on, log GTK's own frame phases for the whole window. These handlers
    // run after GTK's, so each phase is measured from the end of the previous one.
    if (Inkscape::FrameCheck::enabled()) {
        static gint64 last = 0;
        auto phase = +[] (GdkFrameClock *, gpointer name) {
            auto now = g_get_monotonic_time();
            if (name) {
                Inkscape::FrameCheck::Event event(static_cast<char const *>(name));
                event.start = last;
            }
            last = now;
        };
        auto clock = G_OBJECT(gtk_widget_get_frame_clock(GTK_WIDGET(gobj())));
        g_signal_connect(clock, "before-paint", G_CALLBACK(phase), nullptr);
        g_signal_connect(clock, "update", G_CALLBACK(phase), (gpointer) "gtk_update");
        g_signal_connect(clock, "layout", G_CALLBACK(phase), (gpointer) "gtk_layout");
        g_signal_connect(clock, "paint", G_CALLBACK(phase), (gpointer) "gtk_paint");
        g_signal_connect(clock, "after-paint", G_CALLBACK(phase), (gpointer) "gtk_after_paint");
    }
}

InkscapeWindow::~InkscapeWindow() = default;

// Change a document, leaving desktop/view the same. (Eventually move all code here.)
void InkscapeWindow::change_document(SPDocument *document)
{
    if (!_app) {
        std::cerr << "Inkscapewindow::change_document: app is nullptr!" << std::endl;
        return;
    }

    _document = document;
    _app->set_active_document(_document);
    add_document_actions();

    update_dialogs();
}

/**
 * If "dialogs on top" is activated in the preferences, set `parent` as the
 * new transient parent for all DialogWindow windows of the application.
 */
static void retransientize_dialogs(Gtk::Window &parent)
{
    assert(!dynamic_cast<DialogWindow *>(&parent));

    auto prefs = Inkscape::Preferences::get();
    bool window_above = prefs->getInt("/options/transientpolicy/value", PREFS_DIALOGS_WINDOWS_NORMAL) != PREFS_DIALOGS_WINDOWS_NONE;

    for (auto const &window : parent.get_application()->get_windows()) {
        if (auto dialog_window = dynamic_cast<DialogWindow *>(window)) {
            if (window_above) {
                dialog_window->set_transient_for(parent);
            } else {
                dialog_window->unset_transient_for();
            }
        }
    }
}

Glib::RefPtr<Gdk::Toplevel const>
InkscapeWindow::get_toplevel() const
{
    return std::dynamic_pointer_cast<Gdk::Toplevel const>(get_surface());
}

Gdk::Toplevel::State InkscapeWindow::get_toplevel_state() const
{
    if (auto const toplevel = get_toplevel()) {
        return toplevel->get_state();
    }
    return {};
}

bool InkscapeWindow::isFullscreen() const
{
    return Inkscape::Util::has_flag(get_toplevel_state(), Gdk::Toplevel::State::FULLSCREEN);
}

bool InkscapeWindow::isMaximised() const
{
    return Inkscape::Util::has_flag(get_toplevel_state(), Gdk::Toplevel::State::MAXIMIZED);
}

bool InkscapeWindow::isMinimised() const
{
    return Inkscape::Util::has_flag(get_toplevel_state(), Gdk::Toplevel::State::MINIMIZED);
}

void InkscapeWindow::toggleFullscreen()
{
    if (isFullscreen()) {
        unfullscreen();
    } else {
        fullscreen();
    }
}

void InkscapeWindow::on_toplevel_state_changed()
{
    // The initial old state is empty {}, as is the new state if we do not have a toplevel anymore.
    auto const new_toplevel_state = get_toplevel_state();
    auto const changed_mask = _old_toplevel_state ^ new_toplevel_state;
    _old_toplevel_state = new_toplevel_state;
    if (_desktop) {
        _desktop->onWindowStateChanged(changed_mask, new_toplevel_state);
    }
}

void InkscapeWindow::on_is_active_changed()
{
    _desktop_widget->onFocus(is_active());

    if (!is_active()) {
        return;
    }

    if (!_app) {
        std::cerr << "Inkscapewindow::on_focus_in_event: app is nullptr!" << std::endl;
        return;
    }

    _app->set_active_window(this);
    _app->set_active_document(_document);
    _app->set_active_desktop(_desktop);
    _app->set_active_selection(_desktop->getSelection());
    update_dialogs();
    retransientize_dialogs(*this);
}

void InkscapeWindow::setActiveTab(SPDesktop *desktop)
{
    _desktop = desktop;
    _document = _desktop ? _desktop->getDocument() : nullptr;
    _app->set_active_document(_document);
    _app->set_active_desktop(_desktop);
    _app->set_active_selection(_desktop ? _desktop->getSelection() : nullptr);
    if (_desktop) {
        update_dialogs();
        add_document_actions();
    }
}

// Called when a window is closed via the 'X' in the window bar.
bool InkscapeWindow::on_close_request()
{
    auto desktops = get_desktop_widget()->get_desktops();
    for (auto desktop : desktops) {
        if (!_app->destroyDesktop(desktop)) {
            return true; // abort closing
        }
    }

    // We are deleted by InkscapeApplication at this point, so return value doesn't matter.
    return false;
}

/**
 * Configure is called when the widget's size or stack changes.
 */
void InkscapeWindow::on_size_changed()
{
    // Store the desktop widget size on resize.
    if (!_desktop || !get_realized()) {
        return;
    }

    auto prefs = Inkscape::Preferences::get();
    bool maxed = isMaximised();
    bool full = isFullscreen();
    prefs->setBool("/desktop/geometry/fullscreen", full);
    prefs->setBool("/desktop/geometry/maximized", maxed);

    // Don't save geom for maximized, fullscreen or minimised windows.
    // It just tells you the current maximized size, which is not
    // as useful as whatever value it had previously.
    if (!_desktop->isMinimised() && !maxed && !full) {
        // Get size is more accurate than frame extends for window size.
        int w, h = 0;
        get_default_size(w, h);
        prefs->setInt("/desktop/geometry/width", w);
        prefs->setInt("/desktop/geometry/height", h);
    }
}

void InkscapeWindow::update_dialogs()
{
    for (auto const &window : _app->gtk_app()->get_windows()) {
        if (auto dialog_window = dynamic_cast<DialogWindow *>(window)) {
            // Update the floating dialogs, reset them to the new desktop.
            dialog_window->set_inkscape_window(this);
        }
    }

    // Update the docked dialogs in this InkscapeWindow
    _desktop->updateDialogs();
}

/**
 * Make document actions accessible from the window
 */
void InkscapeWindow::add_document_actions()
{
    auto doc_action_group = _document->getActionGroup();

    insert_action_group("doc", doc_action_group);

#ifdef __APPLE__
    // Workaround for https://gitlab.gnome.org/GNOME/gtk/-/issues/5667
    // Copy the document ("doc") actions to the window ("win") so that the
    // application menu on macOS can handle them. The menu only handles the
    // window actions (in gtk_application_impl_quartz_active_window_changed),
    // not the ones attached with "insert_action_group".
    for (auto const &action_name : doc_action_group->list_actions()) {
        add_action(doc_action_group->lookup_action(action_name));
    }
#endif
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
