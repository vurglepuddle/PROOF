// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Advanced Tab for F&S
 */
/* Authors:
 *   Ayan Das <ayandazzz@outlook.com>
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "advanced-tab.h"

#include <glibmm/i18n.h>

#include "colors/document-cms.h"
#include "colors/manager.h"
#include "colors/spaces/cms.h"
#include "desktop-style.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "recolor-art-manager.h"
#include "style.h"
#include "ui/builder-utils.h"

namespace Inkscape::UI::Widget {

AdvancedTab::AdvancedTab()
    : Gtk::Box(Gtk::Orientation::VERTICAL)
    , _builder(create_builder("advanced-tab.ui"))
    , _cmb_format(&get_widget<Gtk::DropDown>(_builder, "cmb_format"))
    , _cmb_interp(&get_widget<Gtk::DropDown>(_builder, "cmb_interp"))
    , _expander(&get_widget<Gtk::Expander>(_builder, "recolor_expander"))
{
    append(get_widget<Gtk::Box>(_builder, "advanced-tab-root"));

    _expander->property_expanded().signal_changed().connect([this]() {
        bool is_open = _expander->get_expanded();
        _expander->set_vexpand(is_open);
        _expander->set_valign(is_open ? Gtk::Align::FILL : Gtk::Align::START);
        this->set_vexpand(is_open);

        // Remap the widget when opened. Auto-disconnects when closed.
        if (is_open && _desktop && _recolor_widget) {
            _recolor_widget->showForSelection(_desktop);
        }
    });

    _recolor_widget = Gtk::make_managed<RecolorArt>();
    _recolor_widget->set_margin_top(8);
    _expander->set_child(*_recolor_widget);

    // Remap the widget if the user switches back to this tab and the expander was left open.
    signal_map().connect([this]() {
        if (_desktop && _recolor_widget && _expander && _expander->get_expanded()) {
            _recolor_widget->showForSelection(_desktop);
        }
    });

    _interp_conn = _cmb_interp->property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &AdvancedTab::on_interp_changed)
    );

    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const &list_item) {
        auto label = Gtk::make_managed<Gtk::Label>();
        label->set_halign(Gtk::Align::START);
        list_item->set_child(*label);
    });

    factory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const &list_item) {
        auto string_obj = std::dynamic_pointer_cast<Gtk::StringObject>(list_item->get_item());
        auto label = dynamic_cast<Gtk::Label *>(list_item->get_child());
        if (string_obj && label) {
            label->set_text(string_obj->get_string());
        }
    });

    _cmb_format->set_list_factory(factory);
    _cmb_interp->set_list_factory(factory);
}

void AdvancedTab::setDesktop(SPDesktop *desktop)
{
    _desktop = desktop;
    _cms_conn.disconnect();

    if (_desktop && _desktop->getDocument()) {
        on_cms_changed();
        _cms_conn = _desktop->getDocument()->getDocumentCMS().connectChanged(
            sigc::mem_fun(*this, &AdvancedTab::on_cms_changed)
        );
    }

    // Only load the recolor widget if it is actually visible. PROOF: also on screen, not just
    // expanded in a dialog that is hidden; the map handler loads it when it shows.
    if (_desktop && _recolor_widget && _expander && _expander->get_expanded() && get_mapped()) {
        _recolor_widget->showForSelection(_desktop);
    }
}

void AdvancedTab::updateFromSelection(Selection *selection)
{
    bool is_valid = selection && RecolorArtManager::checkSelection(selection);
    if (_expander) {
        if (is_valid) {
            _expander->set_sensitive(true);
        } else {
            _expander->set_sensitive(false);
            _expander->set_expanded(false);
        }
    }

    if (selection && !selection->isEmpty()) {
        auto item = *selection->items().begin();
        if (item && item->style) {
            auto &interp = item->style->color_interpolation;
            auto space = interp.getInterpolationSpace();

            Glib::ustring target_name = "sRGB"; /* Default */
            if (space) {
                target_name = space->getSvgName();
            }
            rebuild_interp_model(!interp.set || interp.inherit, target_name);
        }
    }
}

void AdvancedTab::on_interp_changed()
{
    if (!_desktop || !_desktop->getSelection() || _desktop->getSelection()->isEmpty()) {
        return;
    }

    Glib::ustring selected_str = "";
    if (auto model = std::dynamic_pointer_cast<Gtk::StringList>(_cmb_interp->get_model())) {
        guint idx = _cmb_interp->get_selected();
        if (idx != GTK_INVALID_LIST_POSITION) {
            selected_str = model->get_string(idx);
        }
    }

    if (selected_str.empty()) {
        return;
    }

    // Remove " (inherited)" suffix, if the selected has it.
    // So, the it becomes a valid color-space
    Glib::ustring suffix = " (inherited)";
    if (selected_str.length() >= suffix.length() &&
        selected_str.substr(selected_str.length() - suffix.length()) == suffix) {
        selected_str = selected_str.substr(0, selected_str.length() - suffix.length());
    }

    std::shared_ptr<Colors::Space::AnySpace> new_space;
    if (auto doc = _desktop->getDocument()) {
        new_space = doc->getDocumentCMS().findSvgColorSpace(selected_str.raw());
    }
    if (!new_space) {
        new_space = Colors::Manager::get().findSvgColorSpace(selected_str.raw());
    }
    if (!new_space) {
        return;
    }

    SPCSSAttr *css = sp_repr_css_attr_new();
    Glib::ustring space_name = new_space->getSvgName();
    // Required for profile name containing a period (e.g., "CASIO-COMPUTER-CO.-LTD")
    css_quote(space_name);
    sp_repr_css_set_property(css, "color-interpolation", space_name.c_str());
    for (auto item : _desktop->getSelection()->items()) {
        sp_desktop_apply_css_recursive(item, css, true);
    }

    sp_repr_css_attr_unref(css);
    _desktop->getDocument()->ensureUpToDate();
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Change Color Interpolation"), "");
}

void AdvancedTab::on_cms_changed()
{
    rebuild_interp_model(false, "sRGB");
    rebuild_format_model(false, "RGB");
}

void AdvancedTab::rebuild_interp_model(bool is_inherited, Glib::ustring target_name)
{
    _interp_conn.block();
    auto interp_model = Gtk::StringList::create();
    auto add_item = [&](Glib::ustring const &name) {
        if (is_inherited && name == target_name) {
            interp_model->append(name + " (inherited)");
        } else {
            interp_model->append(name);
        }
    };

    /* standard spaces */
    add_item("sRGB");
    add_item("linearRGB");

    /* Document ICC profiles */
    if (_desktop && _desktop->getDocument()) {
        auto const &cms = _desktop->getDocument()->getDocumentCMS();
        for (auto const &doc_space : cms.getSpaces()) {
            add_item(doc_space->getSvgName());
        }
    }

    _cmb_interp->set_model(interp_model);

    Glib::ustring search_name = target_name + (is_inherited ? " (inherited)" : "");
    if (auto model = std::dynamic_pointer_cast<Gtk::StringList>(_cmb_interp->get_model())) {
        for (guint i = 0; i < model->get_n_items(); ++i) {
            if (model->get_string(i) == search_name) {
                _cmb_interp->set_selected(i);
                break;
            }
        }
    }
    _interp_conn.unblock();
}

void AdvancedTab::rebuild_format_model(bool is_inherited, Glib::ustring target_name)
{
    _format_conn.block();
    auto format_model = Gtk::StringList::create();
    auto add_item = [&](Glib::ustring const &name) {
        if (is_inherited && name == target_name) {
            format_model->append(name + " (inherited)");
        } else {
            format_model->append(name);
        }
    };

    /* Global Spaces */
    auto global_spaces = Colors::Manager::get().spaces(Colors::Space::Traits::Picker);
    for (auto const &space : global_spaces) {
        add_item(space->getName());
    }

    /* Document ICC profiles */
    if (_desktop && _desktop->getDocument()) {
        auto const &cms = _desktop->getDocument()->getDocumentCMS();
        for (auto const &doc_space : cms.getSpaces()) {
            add_item(doc_space->getName());
        }
    }

    _cmb_format->set_model(format_model);

    Glib::ustring search_name = target_name + (is_inherited ? " (inherited)" : "");
    if (auto model = std::dynamic_pointer_cast<Gtk::StringList>(_cmb_format->get_model())) {
        for (guint i = 0; i < model->get_n_items(); ++i) {
            if (model->get_string(i) == search_name) {
                _cmb_format->set_selected(i);
                break;
            }
        }
    }
    _format_conn.unblock();
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
