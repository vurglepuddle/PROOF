// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp-bridge/preferences.h"

#include <gdkmm/clipboard.h>
#include <glibmm/i18n.h>
#include <gtkmm/button.h>
#include "bridge.h"
#include "inkscape-application.h"

namespace Inkscape::MCP {
PreferencesPanel::PreferencesPanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 6)
{
    auto prefs = Inkscape::Preferences::get();
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto label = Gtk::make_managed<Gtk::Label>(_("Let AI clients connect to PROOF"));
    label->set_hexpand(true);
    label->set_xalign(0);
    row->append(*label);
    row->append(_enabled);
    _enabled.set_valign(Gtk::Align::CENTER);
    _enabled.set_active(prefs->getBool("/options/mcp/enabled", false));
    _enabled.property_active().signal_changed().connect([this, prefs] { prefs->setBool("/options/mcp/enabled", _enabled.get_active()); });
    _observer = prefs->createObserver("/options/mcp/enabled", [this, prefs] {
        _enabled.set_active(prefs->getBool("/options/mcp/enabled", false));
    });
    append(*row);
    _look.init(_("Look: read the document and see previews"), "/options/mcp/look", true);
    _edit.init(_("Edit: change the open document (comes later)"), "/options/mcp/edit", false);
    _files.init(_("Files: open and export copies in one folder (comes later)"), "/options/mcp/files", false);
    _edit.set_sensitive(false);
    _files.set_sensitive(false);
    append(_look);
    append(_edit);
    append(_files);
    _status.set_xalign(0);
    _status.set_wrap(true);
    append(_status);
    if (auto app = InkscapeApplication::instance(); app && app->mcp_bridge()) {
        auto bridge = app->mcp_bridge();
        _status.set_text(bridge->status());
        _state = bridge->signal_changed().connect([this, bridge] { _status.set_text(bridge->status()); });
    } else _status.set_text(_("Off"));
    auto command = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    _command.set_text(Bridge::client_command());
    _command.set_editable(false);
    _command.set_hexpand(true);
    command->append(_command);
    auto copy = Gtk::make_managed<Gtk::Button>(_("Copy"));
    copy->signal_clicked().connect([this] { get_clipboard()->set_text(_command.get_text()); });
    command->append(*copy);
    append(*command);
    _description.set_text(_("Local only: PROOF asks you before an AI client connects, and nothing can run code or reach the network."));
    _description.set_xalign(0);
    _description.set_wrap(true);
    append(_description);
}
} // namespace Inkscape::MCP
