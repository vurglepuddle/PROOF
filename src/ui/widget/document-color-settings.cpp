// SPDX-License-Identifier: GPL-2.0-or-later
#include "document-color-settings.h"
#include <glibmm/i18n.h>
#include "colors/cms/profile.h"
#include "colors/document-cms.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "document-undo.h"
#include "object/sp-root.h"
#include "preferences.h"

namespace Inkscape::UI::Widget {
using Colors::Space::Type;
namespace DC = Colors::DocumentColors;
namespace {
void fill(Gtk::ComboBoxText &combo, std::vector<std::shared_ptr<Colors::CMS::Profile>> const &profiles,
          std::shared_ptr<Colors::CMS::Profile> const &selected = {}) {
    combo.remove_all();
    int active = -1;
    for (unsigned i = 0; i < profiles.size(); ++i) {
        combo.append(std::to_string(i), profiles[i]->getName());
        if (selected && *profiles[i] == *selected) active = i;
    }
    if (active >= 0) combo.set_active(active);
}
std::shared_ptr<Colors::CMS::Profile> chosen(Gtk::ComboBoxText const &combo,
                     std::vector<std::shared_ptr<Colors::CMS::Profile>> const &profiles) {
    auto i = combo.get_active_row_number();
    return i >= 0 && static_cast<unsigned>(i) < profiles.size() ? profiles[i] : nullptr;
}
}

DocumentColorSettings::DocumentColorSettings() : Gtk::Box(Gtk::Orientation::VERTICAL, 10) {
    set_name("DocumentColorSettings");
    _mode.set_name("document-color-mode");
    _assignment.set_name("document-color-assignment");
    _profile.set_name("document-color-profile");
    _apply.set_name("document-color-apply");
    _grid.set_column_spacing(12);
    _grid.set_row_spacing(8);
    append(_grid);
    int row = 0;
    auto heading = [&](char const *text) {
        auto label = Gtk::make_managed<Gtk::Label>();
        label->set_markup(std::string("<b>") + text + "</b>");
        label->set_halign(Gtk::Align::START);
        label->set_margin_top(row ? 12 : 0);
        _grid.attach(*label, 0, row++, 2, 1);
    };
    auto line = [&](char const *text, Gtk::Widget &widget) {
        auto label = Gtk::make_managed<Gtk::Label>(text);
        label->set_halign(Gtk::Align::START);
        _grid.attach(*label, 0, row);
        widget.set_hexpand();
        _grid.attach(widget, 1, row++);
    };
    heading(_("Document Color"));
    _mode.append("rgb", "RGB");
    _mode.append("cmyk", "CMYK");
    line(_("Color mode"), _mode);
    _assignment.append("none", _("Don't color manage this document"));
    _assignment.append("working", _("Use working space"));
    _assignment.append("profile", _("Assign a profile"));
    line(_("Assignment"), _assignment);
    line(_("Profile"), _profile);
    _apply.set_halign(Gtk::Align::END);
    _grid.attach(_apply, 1, row++);
    _status.set_wrap();
    _status.set_max_width_chars(48);
    _status.set_halign(Gtk::Align::START);
    _status.set_xalign(0);
    _grid.attach(_status, 0, row++, 2, 1);
    _help.set_text(_("Assigning a profile keeps channel values. Changing RGB/CMYK mode converts process colors. Spot ink definitions and placed images keep their own colors."));
    _help.set_wrap();
    _help.set_max_width_chars(48);
    _help.set_xalign(0);
    _help.add_css_class("dim-label");
    _grid.attach(_help, 0, row++, 2, 1);

    heading(_("Working Spaces"));
    line(_("RGB"), _rgb);
    line(_("CMYK"), _cmyk);
    auto defaults = Gtk::make_managed<Gtk::Label>(_("Defaults for profile assignment. Existing documents keep their embedded profiles."));
    defaults->set_wrap();
    defaults->set_max_width_chars(48);
    defaults->set_xalign(0);
    defaults->add_css_class("dim-label");
    _grid.attach(*defaults, 0, row++, 2, 1);
    heading(_("Conversion Options"));
    auto engine = Gtk::make_managed<Gtk::Label>("LittleCMS 2");
    engine->set_halign(Gtk::Align::START);
    line(_("Engine"), *engine);
    _intent.append("2", _("Perceptual"));
    _intent.append("3", _("Relative Colorimetric"));
    _intent.append("4", _("Saturation"));
    _intent.append("5", _("Absolute Colorimetric"));
    line(_("Intent"), _intent);
    _bpc.set_label(_("Black point compensation"));
    _grid.attach(_bpc, 1, row++);
    _bpc.set_tooltip_text(_("Applies to Relative Colorimetric conversion."));

    _mode.signal_changed().connect([this] { if (!_updating) update_profiles(); });
    _assignment.signal_changed().connect([this] {
        _profile.set_sensitive(_assignment.get_active_id() == "profile");
    });
    _apply.signal_clicked().connect(sigc::mem_fun(*this, &DocumentColorSettings::apply));
    _rgb.signal_changed().connect([this] { if (!_updating) DC::setWorkingProfile(Type::RGB, chosen(_rgb, _rgb_profiles)); });
    _cmyk.signal_changed().connect([this] { if (!_updating) DC::setWorkingProfile(Type::CMYK, chosen(_cmyk, _cmyk_profiles)); });
    _intent.signal_changed().connect([this] {
        _bpc.set_sensitive(_intent.get_active_id() == "3");
        if (!_updating && !_intent.get_active_id().empty())
            Preferences::get()->setInt("/options/workingcolors/intent", std::stoi(_intent.get_active_id()));
    });
    _bpc.signal_toggled().connect([this] { if (!_updating) Preferences::get()->setBool("/options/workingcolors/bpc", _bpc.get_active()); });
    _observer.signal_changed().connect([this](auto change, char const *name) {
        if (!_updating && change == XML::SignalObserver::Attribute && name &&
            (std::string_view(name) == "proof:color-mode" || std::string_view(name) == "proof:color-profile")) update();
    });
    signal_map().connect([this] { populate(); update(); });
}

void DocumentColorSettings::populate() {
    if (_populated) return;
    _updating = true;
    _rgb_profiles = DC::profiles(Type::RGB);
    _cmyk_profiles = DC::profiles(Type::CMYK);
    fill(_rgb, _rgb_profiles, DC::workingProfile(Type::RGB));
    fill(_cmyk, _cmyk_profiles, DC::workingProfile(Type::CMYK));
    auto prefs = Preferences::get();
    _intent.set_active_id(std::to_string(prefs->getIntLimited("/options/workingcolors/intent", 3, 2, 5)));
    _bpc.set_active(prefs->getBool("/options/workingcolors/bpc", true));
    _populated = true;
    _updating = false;
}

void DocumentColorSettings::set_document(SPDocument *document) {
    if (_document == document) return;
    _observer.set(document ? document->getRoot() : nullptr);
    _document = document;
    if (get_mapped()) { populate(); update(); }
}

Type DocumentColorSettings::selected_mode() const { return _mode.get_active_id() == "cmyk" ? Type::CMYK : Type::RGB; }

void DocumentColorSettings::update_profiles() {
    auto type = selected_mode();
    _document_profiles = type == Type::CMYK ? _cmyk_profiles : _rgb_profiles;
    auto assigned = DC::assignedSpace(_document);
    auto current = assigned && assigned->getComponentType() == type ? assigned->getProfile() : DC::workingProfile(type);
    if (current && std::none_of(_document_profiles.begin(), _document_profiles.end(), [&](auto const &p) { return *p == *current; }))
        _document_profiles.push_back(current); // embedded profile may not be installed here
    fill(_profile, _document_profiles, current);
    _assignment.set_active_id("working");
    _apply.set_label(DC::mode(_document) == type ? _("Assign Profile") : _("Convert to This Mode"));
}

void DocumentColorSettings::update() {
    if (!_populated) return;
    _updating = true;
    set_sensitive(_document != nullptr);
    auto type = DC::mode(_document);
    _mode.set_active_id(type == Type::CMYK ? "cmyk" : "rgb");
    update_profiles();
    auto space = DC::assignedSpace(_document);
    _assignment.set_active_id(space ? "profile" : "none");
    _status.set_text(space ? Glib::ustring::compose(_("Current: %1 · %2"), type == Type::CMYK ? "CMYK" : "RGB", space->getProfile()->getName())
                          : _("Current: unmanaged. Linked profiles alone do not change the preview."));
    _updating = false;
}

void DocumentColorSettings::apply() {
    if (!_document) return;
    auto type = selected_mode();
    auto choice = _assignment.get_active_id();
    auto profile = choice == "working" ? DC::workingProfile(type) : choice == "profile" ? chosen(_profile, _document_profiles) : nullptr;
    if (choice != "none" && !profile) {
        _status.set_text(_("Choose an installed working profile or select a profile from the list."));
        return;
    }
    bool converting = DC::mode(_document) != type;
    _updating = true;
    auto error = DC::assign(_document, type, profile, DC::workingIntent());
    if (error.empty()) {
        if (converting) DocumentUndo::done(_document, RC_("Undo", "Change document color mode"), "document-properties");
        else DocumentUndo::done(_document, RC_("Undo", "Assign document color profile"), "document-properties");
    }
    _updating = false;
    update();
    if (!error.empty()) _status.set_text(error);
}
}
