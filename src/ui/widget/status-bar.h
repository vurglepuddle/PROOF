// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Author:
 *   Tavmjong Bah
 *   Others
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_WIDGET_STATUSBAR_H
#define INKSCAPE_UI_WIDGET_STATUSBAR_H

#include "config.h"

#include <gtkmm/box.h>

#include "message.h"
#include "preferences.h" // observer
#include "ui/defocus-target.h"
#include "ui/operation-blocker.h"
#include "ui/popup-menu.h"

namespace Gtk {
class Grid;
class Label;
class Popover;
} // namespace Gtk

namespace Geom {
class Point;
} // namespace Geom

class SPDesktop;
class SPDesktopWidget;

namespace Inkscape::UI::Widget {

class InkSpinButton;
class SelectedStyle;
class LayerSelector;
class PageSelector;

class StatusBar
    : public Gtk::Box
    , public Inkscape::UI::DefocusTarget
{
public:
    StatusBar();
    ~StatusBar() override;

    void set_desktop(SPDesktop* desktop);
    void set_message(const Inkscape::MessageType type, const char* message);
    void set_coordinate(const Geom::Point& p);
    void update_visibility();

    void update_zoom();
    void update_rotate();

    void rotate_grab_focus();
    void zoom_grab_focus();

    void onDefocus() override;

private:
    void zoom_value_changed(double value);
    void zoom_menu_handler();
    bool zoom_popup(PopupMenuOptionalClick);

    void rotate_value_changed(double value);
    void rotate_menu_handler();
    bool rotate_popup(PopupMenuOptionalClick);

    // From left to right
    SelectedStyle* selected_style = nullptr;
    LayerSelector* layer_selector = nullptr;
    PageSelector *_page_selector = nullptr;
    Gtk::Label*    selection = nullptr;
    Gtk::Label*     coordinates = nullptr;
    UI::Widget::InkSpinButton *_zoom = nullptr;
    UI::Widget::InkSpinButton *_rotate = nullptr;
    Gtk::Box*      snapshot = nullptr;

    SPDesktopWidget* desktop_widget = nullptr;
    std::unique_ptr<Gtk::Popover> zoom_popover;
    std::unique_ptr<Gtk::Popover> rotate_popover;

    SPDesktop* desktop = nullptr;

    OperationBlocker _blocker;

    Inkscape::PrefObserver preference_observer;
#ifdef WITH_MCP_BRIDGE
    sigc::scoped_connection _mcp_state;
#endif
};

} // namespace Inkscape::UI::Widget

#endif // INKSCAPE_UI_WIDGET_STATUSBAR_H

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
