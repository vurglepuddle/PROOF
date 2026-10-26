// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_MCP_PREFERENCES_H
#define INKSCAPE_MCP_PREFERENCES_H

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/switch.h>
#include <sigc++/scoped_connection.h>
#include "../preferences.h"
#include "ui/widget/preferences-widget.h"

namespace Inkscape::MCP {
class PreferencesPanel final : public Gtk::Box
{
public:
    PreferencesPanel();
private:
    Gtk::Switch _enabled;
    UI::Widget::PrefCheckButton _look, _edit, _files;
    Gtk::Entry _command;
    Gtk::Label _status, _description;
    PrefObserver _observer;
    sigc::scoped_connection _state;
};
} // namespace Inkscape::MCP
#endif
