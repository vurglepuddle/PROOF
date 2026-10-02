// SPDX-License-Identifier: GPL-2.0-or-later
//
// Created by Michael Kowalski on 5/27/24.
//
#include "color-picker-panel.h"

#include <glib/gi18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/aspectframe.h>
#include <gtkmm/box.h>
#include <gtkmm/stack.h>
#include <string>
#include <utility>

#include "color-entry.h"
#include "color-plate.h"
#include "color-preview.h"
#include "color-page.h"
#include "color-wheel.h"
#include "desktop.h"
#include "generic/icon-combobox.h"
#include "generic/spin-button.h"
#include "inkscape.h"
#include "colors/color.h"
#include "colors/manager.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "ui/tools/dropper-tool.h"
#include "ui/util.h"

const std::string spinner_pattern = "999.9%";
constexpr int ROW_PLATE = 0; // color plate, if any
constexpr int ROW_EDIT = 1;  // dropper, rgb edit box, color type selector
constexpr int ROW_PAGE = 3;  // color page with sliders

namespace Inkscape::UI::Widget {

using namespace Colors;

class ColorPickerPanelImpl : public ColorPickerPanel {
public:
    ColorPickerPanelImpl(Space::Type space, PlateType type, std::shared_ptr<ColorSet> color, bool with_expander);

    void update_color() {
        if (_color_set && !_color_set->isEmpty()) {
            auto color = _color_set->getAverage();
            _preview.setRgba32(color.toRGBA());
            if (_plate) { _plate->set_color(color); }
        }
    }

    void remove_widgets() {
        if (_page) {
            _page->detach_page(_first_column, _last_column);
            // detach existing page
            remove(*_page);
            _page = nullptr;
        }
        if (_plate) {
            // removed managed wheel
            remove(_plate->get_widget());
            _plate = nullptr;
        }
    }

    void create_color_page(Space::Type type, PlateType plate_type) {
        auto space = _profile_space && _profile_space->getComponentType() == type
            ? _profile_space : Manager::get().find(type);
        _page = std::make_unique<ColorPage>(space, _color_set);
        _page->show_expander(false);
        _page->set_spinner_size_pattern(spinner_pattern);
        _page->attach_page(_first_column, _last_column);
        attach(*_page, 0, ROW_PAGE, 3);
        if (plate_type == Circle) {
            _plate = _page->create_color_wheel(type, true);
            if (_plate) {
                _plate->get_widget().set_margin_top(4);
                _plate->get_widget().set_margin_bottom(4);
            }
        }
        else if (plate_type == Rect) {
            _plate = _page->create_color_wheel(type, false);
            if (_plate) {
                _plate->get_widget().set_margin_bottom(0);
            }
        }
        if (_plate) {
            _plate->get_widget().set_expand();
            // counter internal padding reserved to show the current color indicator; align plate with the below widgets
            _plate->get_widget().set_margin_start(-4);
            _plate->get_widget().set_margin_end(-4);
            attach(_plate->get_widget(), 0, ROW_PLATE, 3);
        }
        update_color();
    }

    Widget* add_gap(int size, int row) {
        auto gap = Gtk::make_managed<Gtk::Box>();
        gap->set_size_request(1, size);
        attach(*gap, 0, row);
        return gap;
    }

    void set_desktop(SPDesktop* dekstop) override;
    void set_color(const Color& color) override;
    std::shared_ptr<Space::AnySpace> _profile_space;
    void set_picker_type(Space::Type type) override;
    void set_plate_type(PlateType plate) override;
    PlateType get_plate_type() const override;
    void switch_page(Space::Type space, PlateType plate_type);
    void pick_color();

    Glib::RefPtr<Gtk::SizeGroup> get_first_column_size() override {
        return _first_column;
    }

    Glib::RefPtr<Gtk::SizeGroup> get_last_column_size() override {
        return _last_column;
    }

    sigc::signal<void (Space::Type)> get_color_space_changed() override {
        return _color_space_changed;
    }

    Glib::RefPtr<Gtk::SizeGroup> _first_column = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL);
    Glib::RefPtr<Gtk::SizeGroup> _last_column = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL);
    // eye dropper - color picker
    Gtk::Button _dropper;
    // frame for RGB edit box
    Gtk::Box _frame;
    ColorPreview _preview;
    ColorEntry _rgb_edit;
    Gtk::Image _warning;
    //todo: let user choose display/input format of ColorEntry
    // Gtk::MenuButton _display_choices;
    // color type space selector
    IconComboBox _spaces = IconComboBox(true, IconComboBox::LabelOnly);
    bool _with_expander = true;
    // color type this picker is working in
    Space::Type _space_type = Space::Type::NONE;
    std::shared_ptr<ColorSet> _color_set;
    PlateType _plate_type;
    std::unique_ptr<ColorPage> _page;
    ColorWheel* _plate = nullptr;
    sigc::scoped_connection _color_picking;
    SPDesktop* _desktop = nullptr;
    sigc::signal<void (Space::Type)> _color_space_changed;
};

std::unique_ptr<ColorPickerPanel> ColorPickerPanel::create(Space::Type space, PlateType type, std::shared_ptr<ColorSet> color) {
    return std::make_unique<ColorPickerPanelImpl>(space, type, color, false);
}

ColorPickerPanelImpl::ColorPickerPanelImpl(Space::Type space, PlateType type, std::shared_ptr<ColorSet> color, bool with_expander):
    _rgb_edit(color),
    _with_expander(with_expander),
    _space_type(space),
    _color_set(color),
    _plate_type(type)
{
    _color_set->signal_changed.connect([this]() { update_color(); });

    set_row_spacing(0);
    set_column_spacing(0);

    // list available color space types
    for (auto&& meta : Manager::get().spaces(Space::Traits::Picker)) {
        _spaces.add_row(meta->getIcon(), _(meta->getShortName().c_str()), int(meta->getType()));
    }
    _spaces.refilter();
    _spaces.set_tooltip_text(_("Select color picker type"));
    // Important: add "regular" class to render non-symbolic color icons;
    // otherwise they will be rendered black&white
    _spaces.add_css_class("regular");
    _spaces.set_active_by_id(int(space));
    _spaces.signal_changed().connect([this](int id) {
        auto type = static_cast<Space::Type>(id);
        if (type != Space::Type::NONE) {
            set_picker_type(type);
            _color_space_changed.emit(type);
        }
    });

    // color picker button
    _dropper.set_icon_name("color-picker");
    _first_column->add_widget(_dropper);
    _dropper.signal_clicked().connect([this]() { pick_color(); });
    // RGB edit box
    _frame.set_hexpand();
    _frame.set_spacing(4);
    _frame.add_css_class("border-box");
    _frame.add_css_class("entry-box");
    // match frame size visually with color sliders width
    _frame.set_margin_start(8);
    _frame.set_margin_end(8);
    _preview.setStyle(ColorPreview::Simple);
    _preview.set_frame(true);
    _preview.set_border_radius(0);
    _preview.set_size_request(16, 16);
    _preview.set_checkerboard_tile_size(4);
    _preview.set_margin_start(4);
    _preview.set_halign(Gtk::Align::START);
    _preview.set_valign(Gtk::Align::CENTER);
    _frame.append(_preview);
    _rgb_edit.set_hexpand();
    _rgb_edit.set_has_frame(false);
    _rgb_edit.set_alignment(Gtk::Align::CENTER);
    _rgb_edit.add_css_class("small-entry");
    _rgb_edit.get_out_of_gamut_signal().connect([this](auto& msg) {
        _warning.set_opacity(msg.empty() ? 0 : 1);
        _warning.set_tooltip_text(msg);
    });
    _warning.set_from_icon_name("warning");
    _warning.set_margin_end(3);
    _warning.set_opacity(0);
    _frame.append(_rgb_edit);
    _frame.append(_warning);

    // color space type selector
    _spaces.set_halign(Gtk::Align::END);
    _last_column->add_widget(_spaces);

    Widget* one_row[3] ={&_dropper, &_frame, &_spaces};
    for (auto widget : one_row) {
        widget->set_margin_top(4);
        widget->set_margin_bottom(4);
    }
    attach(_dropper, 0, ROW_EDIT);
    attach(_frame, 1, ROW_EDIT);
    attach(_spaces, 2, ROW_EDIT);

    create_color_page(space, type);
}

void ColorPickerPanelImpl::set_desktop(SPDesktop* dekstop) {
    _desktop = dekstop;
}

void ColorPickerPanelImpl::set_color(const Color& color) {
    auto incoming = color.getSpace();
    auto profile = incoming->getType() == Space::Type::CMS && incoming->hasValidCmsProfile() &&
                   (incoming->getComponentType() == Space::Type::CMYK || incoming->getComponentType() == Space::Type::RGB)
        ? incoming : nullptr;
    if (profile != _profile_space) {
        _profile_space = profile;
        auto type = profile ? profile->getComponentType() : _space_type;
        switch_page(type, _plate_type);
        _spaces.set_active_by_id(int(type));
        _spaces.set_tooltip_text(profile ? profile->getProfile()->getName() : _("Select color picker type"));
    }
    _color_set->set(color);
}

void ColorPickerPanelImpl::set_picker_type(Space::Type type) {
    switch_page(type, _plate_type);
}

void ColorPickerPanelImpl::set_plate_type(PlateType plate) {
    if (plate == _plate_type) return;

    switch_page(_space_type, plate);
}

void ColorPickerPanelImpl::switch_page(Space::Type space, PlateType plate_type) {
    remove_widgets();
    create_color_page(space, plate_type);
    _space_type = space;
    _plate_type = plate_type;
}

ColorPickerPanel::PlateType ColorPickerPanelImpl::get_plate_type() const {
    return _plate_type;
}

ColorPickerPanel::PlateType get_plate_type_preference(const char* pref_path_base, ColorPickerPanel::PlateType def_type) {
    Glib::ustring path(pref_path_base);
    return static_cast<ColorPickerPanel::PlateType>(Preferences::get()->getIntLimited(path + "/color-plate", def_type, 0, 2));
}

void set_plate_type_preference(const char* pref_path_base, ColorPickerPanel::PlateType type) {
    Glib::ustring path(pref_path_base);
    Preferences::get()->setInt(path + "/color-plate", type);
}

void ColorPickerPanelImpl::pick_color() {
    // Set the dropper into a "one-click" mode, so it reverts to the previous tool after a click
    if (_color_picking.connected()) {
        _color_picking.disconnect();
        return;
    }

    // TODO: pass make clients call set_desktop()
    auto desktop = _desktop ? _desktop : SP_ACTIVE_DESKTOP;
    if (!desktop) return;

    Tools::sp_toggle_dropper(desktop);
    if (auto tool = dynamic_cast<Tools::DropperTool*>(desktop->getTool())) {
        _color_picking = tool->onetimepick_signal.connect([this](auto& color) {
            _color_set->setAll(color);
        });
        // Close parent popover to release input grab
        UI::close_parent_popover(*this);
    }
}

} // namespace
