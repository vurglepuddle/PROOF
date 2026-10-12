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
 *
 */

#ifndef INKSCAPE_WINDOW_H
#define INKSCAPE_WINDOW_H

#include <glibmm/refptr.h>
#include <gdkmm/toplevel.h>
#include <gtkmm/applicationwindow.h>

#include <sigc++/scoped_connection.h>
#include <vector>

#include "preferences.h"

namespace Gtk { class Box; }

class InkscapeApplication;
class SPDocument;
class SPDesktop;
class SPDesktopWidget;

class InkscapeWindow : public Gtk::ApplicationWindow
{
public:
    InkscapeWindow(SPDesktop *desktop);
    ~InkscapeWindow() override;

    SPDocument*      get_document()       { return _document; }
    SPDesktop*       get_desktop()        { return _desktop; }
    SPDesktopWidget* get_desktop_widget() { return _desktop_widget; }
    void change_document(SPDocument* document);

    Gdk::Toplevel::State get_toplevel_state() const;

    bool isFullscreen() const;
    bool isMaximised() const;
    bool isMinimised() const;

    void toggleFullscreen();

    void setActiveTab(SPDesktop *desktop);

    // Release action preference callbacks before GtkWindow tears down its
    // action group, even if a menu still retains one of the actions.
    void addPreferenceObserver(Inkscape::PrefObserver observer) { _preference_observers.push_back(std::move(observer)); }

private:
    InkscapeApplication *_app = nullptr;
    SPDocument*          _document = nullptr;
    SPDesktop*           _desktop = nullptr;
    SPDesktopWidget*     _desktop_widget = nullptr;
    Glib::RefPtr<Gtk::ShortcutController> _shortcut_controller;

    void add_document_actions();

    sigc::scoped_connection _toplevel_state_connection;
    Gdk::Toplevel::State _old_toplevel_state{};
    std::vector<Inkscape::PrefObserver> _preference_observers;

    void on_realize() override;
    Glib::RefPtr<Gdk::Toplevel const> get_toplevel() const;
    void on_toplevel_state_changed();
    void on_is_active_changed();
    bool on_close_request() override;
    void on_size_changed();

    void update_dialogs();
};

#endif // INKSCAPE_WINDOW_H

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
