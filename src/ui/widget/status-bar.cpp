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

#include "status-bar.h"
#ifdef WITH_MCP_BRIDGE
#include <gtkmm/button.h>
#include "inkscape-application.h"
#include "mcp-bridge/bridge.h"
#endif

#include <gtkmm/applicationwindow.h>
#include <gtkmm/popovermenu.h>

#include "desktop.h"
#include "selected-style.h"
#include "ui/builder-utils.h"
#include "ui/widget/canvas.h"
#include "ui/widget/desktop-widget.h"
#include "ui/widget/generic/spin-button.h"
#include "ui/widget/layer-selector.h"
#include "ui/widget/page-selector.h"

namespace Inkscape::UI::Widget {

StatusBar::StatusBar()
    : Gtk::Box(Gtk::Orientation::HORIZONTAL)
{
    auto builder = Inkscape::UI::create_builder("statusbar.ui");

    auto &statusbar = UI::get_widget<Gtk::Box>(builder, "statusbar");

    // selected_style = &UI::get_widget_derived<UI::Widget::SpinButton>(builder, "statusbar-selected-style");
    // layer_selector = &UI::get_widget_derived<UI::Widget::SpinButton>(builder, "statusbar-layer-selector");
    selection = &UI::get_widget<Gtk::Label>(builder, "statusbar-selection");

    // **** Coordinates  ****

    coordinates  = &UI::get_widget<Gtk::Label> (builder, "statusbar-coordinates");
#ifdef WITH_MCP_BRIDGE
    if (auto app = InkscapeApplication::instance(); app && app->mcp_bridge()) {
        auto bridge = app->mcp_bridge();
        auto indicator = Gtk::make_managed<Gtk::Button>();
        indicator->signal_clicked().connect([app] {
            if (auto bridge = app->mcp_bridge()) bridge->disconnect();
        });
        statusbar.insert_child_after(*indicator, *coordinates->get_prev_sibling());
        auto update_indicator = [indicator, bridge] {
            indicator->set_label("AI: " + bridge->permission_label());
            indicator->set_visible(bridge->connected());
            indicator->set_tooltip_text(bridge->client_name() + " is connected (" + bridge->permission_label() + "). Click to disconnect.");
        };
        update_indicator();
        _mcp_state = bridge->signal_changed().connect(update_indicator);
    }
#endif

    // **** Snapshot  ****

    snapshot  = &UI::get_widget<Gtk::Box> (builder, "statusbar-snapshot");
   
    // ******** Zoom ********

    _zoom = &UI::get_derived_widget<UI::Widget::InkSpinButton>(builder, "statusbar-zoom");

    // Can't seem to add actions with double parameters to .ui file, add here.
    const std::vector<std::pair<std::string, std::string>> zoom_entries =
    {
        {  "10%", "win.canvas-zoom-absolute(0.1)" },
        {  "20%", "win.canvas-zoom-absolute(0.2)" },
        {  "50%", "win.canvas-zoom-absolute(0.5)" },
        { "100%", "win.canvas-zoom-absolute(1.0)" }, // Must include decimal point!
        {  "200%", "win.canvas-zoom-absolute(2.0)" },
        {  "500%", "win.canvas-zoom-absolute(5.0)" },
        { "1000%", "win.canvas-zoom-absolute(10.0)"},
    };

    auto zoom_menu = UI::get_object<Gio::Menu>(builder, "statusbar-zoom-menu");
    for (auto entry : zoom_entries) {
        auto menu_item = Gio::MenuItem::create(entry.first, entry.second);
        zoom_menu->prepend_item(menu_item); // In reverse order.
    }

    zoom_popover = std::make_unique<Gtk::PopoverMenu>(zoom_menu, Gtk::PopoverMenu::Flags::NESTED);
    zoom_popover->set_parent(*_zoom);

    _zoom->set_trim_zeros(false);
    _zoom->set_min_size("12345%");
    // Show zoom as a percentage, where 1:1 is 100%.
    // Internally use log2(zoom_factor) values, so step up/down operates in log space.
    _zoom->set_transformers(
        [] (double value) { return std::log(value / 100.0) / std::log(2); },
        [this] (double value) {
            auto zoom = std::pow(2, value) * 100.0;
            if (zoom >= 10 - 0.05) {
                _zoom->set_digits(0);
            }
            else if (zoom >= 2 - 0.005) {
                _zoom->set_digits(1);
            }
            else {
                _zoom->set_digits(2);
            }
            return zoom;
        }
    );
    _zoom->signal_value_changed().connect(sigc::mem_fun(*this, &StatusBar::zoom_value_changed));
    on_popup_menu(*_zoom, sigc::mem_fun(*this, &StatusBar::zoom_popup));
    _zoom->setDefocusTarget(this);
    _zoom->set_range(log(SP_DESKTOP_ZOOM_MIN)/log(2), log(SP_DESKTOP_ZOOM_MAX)/log(2));

    // ******* Rotate *******

    _rotate = &UI::get_derived_widget<UI::Widget::InkSpinButton>(builder, "statusbar-rotate");

    // Can't seem to add actions with double parameters to .ui file, add here.
    const std::vector<std::pair<std::string, std::string>> rotate_entries =
    {
        {  "180°", "win.canvas-rotate-absolute-degrees( 180.0)" }, // Must include decimal point!
        {  "135°", "win.canvas-rotate-absolute-degrees( 135.0)" },
        {   "90°", "win.canvas-rotate-absolute-degrees(  90.0)" },
        {   "45°", "win.canvas-rotate-absolute-degrees(  45.0)" },
        {    "0°", "win.canvas-rotate-absolute-degrees(   0.0)" },
        {  "-45°", "win.canvas-rotate-absolute-degrees( -45.0)" },
        {  "-90°", "win.canvas-rotate-absolute-degrees( -90.0)" },
        { "-135°", "win.canvas-rotate-absolute-degrees(-135.0)" },
    };

    auto rotate_menu = UI::get_object<Gio::Menu>(builder, "statusbar-rotate-menu");
    for (auto entry : rotate_entries) {
        auto menu_item = Gio::MenuItem::create(entry.first, entry.second);
        rotate_menu->prepend_item(menu_item); // In reverse order.
    }

    rotate_popover = std::make_unique<Gtk::PopoverMenu>(rotate_menu, Gtk::PopoverMenu::Flags::NESTED);
    rotate_popover->set_parent(*_rotate);

    _rotate->signal_value_changed().connect(sigc::mem_fun(*this, &StatusBar::rotate_value_changed));
    on_popup_menu(*_rotate, sigc::mem_fun(*this, &StatusBar::rotate_popup));
    _rotate->setDefocusTarget(this);

    // Add rest by hand for now.

    // Selected Style
    selected_style = Gtk::make_managed<Inkscape::UI::Widget::SelectedStyle>();
    statusbar.prepend(*selected_style);

    // Layer Selector
    layer_selector = Gtk::make_managed<Inkscape::UI::Widget::LayerSelector>();
    layer_selector->set_hexpand(false);
    statusbar.insert_child_after(*layer_selector, *selected_style);

    // Page selector
    _page_selector = Gtk::make_managed<PageSelector>();
    _page_selector->set_hexpand(false);
    statusbar.insert_child_after(*_page_selector, *layer_selector);

    // Selector status
    append(statusbar);

    preference_observer = Preferences::get()->createObserver("/statusbar/visibility", [this] {
        update_visibility();
    });
    update_visibility();
}

StatusBar::~StatusBar() = default;

void StatusBar::set_desktop(SPDesktop *desktop_in)
{
    desktop = desktop_in;

    selected_style->setDesktop(desktop);
    layer_selector->setDesktop(desktop);
    _page_selector->setDesktop(desktop);

    // A desktop is always "owned" by a desktop widget.
    desktop_widget = desktop ? desktop->getDesktopWidget() : nullptr;

    if (desktop) {
        update_zoom();
        update_rotate();
    }
}

void
StatusBar::set_message(const Inkscape::MessageType type, const char* message)
{
    Glib::ustring pattern = "%1";
#ifndef _WIN32
#if PANGO_VERSION_CHECK(1,50,0)
    // line height give delays on windows so better unset, also is not necesary label is well placed without
    pattern = "<span line_height='0.8'>%1</span>";
#endif
#endif

    auto const msg = Glib::ustring::compose(pattern, message ? message : "");
    selection->set_markup(msg);
    // we don't use Inkscape::MessageType because previous queue_draw is not needed, is called on allocation and is overridden by next message sent anyway
    // Allow user to view the entire message even if it doesn't fit into label (after removing markup).
    selection->set_tooltip_text(selection->get_text());
}

void StatusBar::set_coordinate(Geom::Point const &p)
{
    char * str_total = g_strdup_printf("(%7.2f, %7.2f)", p.x(), p.y());
    coordinates->set_markup(str_total);
    g_free(str_total);
}

void StatusBar::rotate_grab_focus()
{
    _rotate->grab_focus();
}

void StatusBar::zoom_grab_focus()
{
    _zoom->grab_focus();
}

void StatusBar::onDefocus()
{
    desktop_widget->get_canvas()->grab_focus();
}

// ******** Zoom ********

void StatusBar::zoom_value_changed(double value)
{
    if (_blocker.pending()) {
        return;
    }
    auto guard = _blocker.block();

    double const zoom_factor = std::pow(2, value);
    if (auto const window = dynamic_cast<Gtk::ApplicationWindow *>(get_root())) {
        auto variant = Glib::Variant<double>::create(zoom_factor);
        window->activate_action("win.canvas-zoom-absolute", variant);
    } else {
        std::cerr << "StatusBar::zoom_value_changed(): failed to get window!" << std::endl;
    }
}

bool StatusBar::zoom_popup(PopupMenuOptionalClick)
{
    popup_at_center(*zoom_popover, *_zoom);
    return true;
}

void StatusBar::update_zoom()
{
    if (_blocker.pending()) {
        return;
    }
    auto guard = _blocker.block();

    auto prefs = Preferences::get();

    double correction = 1.0;
    if (prefs->getDouble("/options/zoomcorrection/shown", true)) {
        correction = prefs->getDouble("/options/zoomcorrection/value", 1.0);
    }

    _zoom->set_value(log(desktop->current_zoom() / correction) / log(2));
}

// ******* Rotate *******

void StatusBar::rotate_value_changed(double value)
{
    if (_blocker.pending()) {
        return;
    }
    auto guard = _blocker.block();

    if (auto const window = dynamic_cast<Gtk::ApplicationWindow *>(get_root())) {
        auto variant = Glib::Variant<double>::create(value);
        window->activate_action("win.canvas-rotate-absolute-degrees", variant);
    } else {
        std::cerr << "StatusBar::rotate_value_changed(): failed to get window!" << std::endl;
    }
}

bool StatusBar::rotate_popup(PopupMenuOptionalClick)
{
    popup_at_center(*rotate_popover, *_rotate);
    return true;
}

void StatusBar::update_rotate()
{
    if (_blocker.pending()) {
        return;
    }
    auto guard = _blocker.block();

    _rotate->set_value(Geom::deg_from_rad(desktop->current_rotation().angle()));
}

void StatusBar::update_visibility()
{
    auto prefs = Inkscape::Preferences::get();
    Glib::ustring path("/statusbar/visibility/");

    layer_selector->set_visible(prefs->getBool(path + "layer",       true));
    selected_style->set_visible(prefs->getBool(path + "style",       true));
    coordinates->set_visible(   prefs->getBool(path + "coordinates", true));
    _rotate->set_visible(       prefs->getBool(path + "rotation",    true));
    snapshot->set_visible(      prefs->getBool(path + "snapshot",    true));
}

} // namespace Inkscape::UI::Widget

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
