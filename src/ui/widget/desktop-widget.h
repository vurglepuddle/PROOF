// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * A class to hold:
 *   - Top toolbars
 *     - Command Toolbar (in horizontal mode)
 *     - Tool Toolbars (one at a time)
 *     - Snap Toolbar (in simple or advanced modes)
 *   - DesktopHBox
 *     - ToolboxCanvasPaned
 *       - Tool Toolbar (Tool selection)
 *       - Dialog Container
 *     - Snap Toolbar (in permanent mode)
 *     - Command Toolbar (in vertical mode)
 *   - Swatches
 *   - StatusBar.
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   MenTaLguY <mental@rydia.net>
 *   bulia byak <buliabyak@users.sf.net>
 *   Ralf Stephan <ralf@ark.in-berlin.de>
 *   John Bintz <jcoswell@coswellproductions.org>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2006 John Bintz
 * Copyright (C) 2004 MenTaLguY
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_SP_DESKTOP_WIDGET_H
#define SEEN_SP_DESKTOP_WIDGET_H

#include <gtkmm/box.h>

#include "message.h"
#include "preferences.h"

namespace Glib {
class ustring;
} // namespace Glib

namespace Gio {
class ActionMap;
} // namespace Gio

namespace Gtk {
class Grid;
class Paned;
class Toolbar;
class Widget;
} // namespace Gtk

class InkscapeWindow;
class SPDocument;
class SPDesktop;
class SPObject;

namespace Inkscape::UI {

namespace Dialog {
class DialogContainer;
class DialogMultipaned;
class SwatchesPanel;
} // namespace Dialog

namespace Toolbar {
class Toolbars;
class CommandToolbar;
class Toolbar;
class SnapToolbar;
class ToolToolbar;
} // namespace Toolbars

namespace Widget {
class Button;
class Canvas;
class CanvasGrid;
class SpinButton;
class StatusBar;
} // namespace Widget

} // namespace Inkscape::UI

/// A GtkBox on an SPDesktop.
class SPDesktopWidget : public Gtk::Box
{
    using parent_type = Gtk::Box;

public:
    SPDesktopWidget(InkscapeWindow *inkscape_window);
    ~SPDesktopWidget() override;

    Inkscape::UI::Toolbar::Toolbar* get_current_toolbar();
    Inkscape::UI::Widget::CanvasGrid *get_canvas_grid()  { return _canvas_grid; }  // Temp, I hope!
    Inkscape::UI::Widget::Canvas     *get_canvas()       { return _canvas; }
    std::vector<SPDesktop *> const   &get_desktops() const { return _desktops; }
    SPDesktop                        *get_desktop()      { return _desktop; }
    InkscapeWindow             const *get_window() const { return _window; }
    InkscapeWindow                   *get_window()       { return _window; }
    double                            get_dt2r()   const { return _dt2r; }

    Gio::ActionMap *get_action_map();

    void on_realize() override;
    void on_unrealize() override;

    void addDesktop(SPDesktop *desktop, int pos = -1);
    void removeDesktop(SPDesktop *desktop);
    void switchDesktop(SPDesktop *desktop);

    void advanceTab(int by);

private:
    sigc::scoped_connection modified_connection;
    sigc::scoped_connection _color_mode_connection;

    std::vector<SPDesktop *> _desktops;
    SPDesktop *_desktop = nullptr;

    InkscapeWindow *_window = nullptr;

    Gtk::Paned *_tbbox = nullptr;
    Gtk::Box *_hbox = nullptr;
    std::unique_ptr<Inkscape::UI::Dialog::DialogContainer> _container;
    Inkscape::UI::Dialog::DialogMultipaned *_columns = nullptr;
    Gtk::Grid* _top_toolbars = nullptr;

    Inkscape::UI::Widget::StatusBar *_statusbar = nullptr;
    Inkscape::UI::Dialog::SwatchesPanel *_panels;

    /** A grid to display the canvas, rulers, and scrollbars. */
    Inkscape::UI::Widget::CanvasGrid *_canvas_grid = nullptr;

    double _dt2r;
    Inkscape::UI::Widget::Canvas *_canvas = nullptr;

    sigc::scoped_connection _tool_changed_conn;

public:
    void setMessage(Inkscape::MessageType type, char const *message);
    void viewSetPosition (Geom::Point p);
    void letRotateGrabFocus();
    void letZoomGrabFocus();
    Geom::IntPoint getWindowSize() const;
    void setWindowSize(Geom::IntPoint const &size);
    void setWindowTransient(Gtk::Window &window, int transient_policy);
    void presentWindow();
    void showInfoDialog(Glib::ustring const &message);
    bool warnDialog (Glib::ustring const &text);
    Gtk::Widget *get_toolbar_by_name(const Glib::ustring &name);
    void setToolboxFocusTo(char const *);
    void setToolboxAdjustmentValue(char const *id, double value);
    bool isToolboxButtonActive(char const *id) const;
    void setCoordinateStatus(Geom::Point p);
    void onFocus(bool has_focus);
    Inkscape::UI::Dialog::DialogContainer *getDialogContainer();
    void showNotice(Glib::ustring const &msg, int timeout = 0);

    void desktopChangedDocument(SPDesktop *desktop);
    void desktopChangedTitle(SPDesktop *desktop);

    // Canvas Grid Widget
    void update_zoom();
    void update_rotation();
    void repack_snaptoolbar();

    void layoutWidgets();
    void toggle_scrollbars();
    void toggle_command_palette();
    void toggle_rulers();
    void sticky_zoom_toggled();
    void sticky_zoom_updated();

private:
    Inkscape::UI::Toolbar::ToolToolbar *tool_toolbox;
    std::unique_ptr<Inkscape::UI::Toolbar::Toolbars> tool_toolbars;
    std::unique_ptr<Inkscape::UI::Toolbar::CommandToolbar> command_toolbar;
    std::unique_ptr<Inkscape::UI::Toolbar::SnapToolbar> snap_toolbar;
    Inkscape::PrefObserver _tb_snap_pos;
    Inkscape::PrefObserver _tb_icon_sizes1;
    Inkscape::PrefObserver _tb_icon_sizes2;
    Inkscape::PrefObserver _tb_visible_buttons;
    Inkscape::PrefObserver _ds_sticky_zoom;
    Inkscape::PrefObserver _version_in_title_observer;

    void _updateUnit();
    void _updateNamedview();
    void _updateTitle();
    void apply_ctrlbar_settings();
    void remove_from_top_toolbar_or_hbox(Gtk::Widget &widget);
};

#endif /* !SEEN_SP_DESKTOP_WIDGET_H */

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
