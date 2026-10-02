// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Build a set of color page for a given color space
 *//*
 * Copyright (C) 2024 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/widget/color-page.h"

#include <glibmm/i18n.h>
#include <gtkmm/expander.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>

#include "color-wheel-factory.h"
#include "colors/color-set.h"
#include "ink-color-wheel.h"
#include "generic/spin-button.h"
#include "ui/builder-utils.h"
#include "ui/util.h"
#include "util/signal-blocker.h"

namespace Inkscape::UI::Widget {

ColorPage::ColorPage(std::shared_ptr<Space::AnySpace> space, std::shared_ptr<ColorSet> colors)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 4)
    , _space(std::move(space))
    , _selected_colors(colors)
    , _specific_colors(std::make_shared<Colors::ColorSet>(_space, colors->getAlphaConstraint().value_or(true)))
{
    set_name("ColorPage");
    append(_expander);
    _expander.set_label(_("Color wheel"));
    append(_grid);
    _grid.set_column_spacing(2);
    _grid.set_row_spacing(4);

    // Keep the selected colorset in-sync with the space specific colorset.
    _specific_changed_connection = _specific_colors->signal_changed.connect([this]() {
        auto scoped = SignalBlocker{_selected_changed_connection};
        for (auto &[id, color] : *_specific_colors) {
            _selected_colors->set(id, color);
        }
    });

    // Keep the child in-sync with the selected colorset.
    _selected_changed_connection = _selected_colors->signal_changed.connect([this]() {
        auto scoped = SignalBlocker{_specific_changed_connection};
        for (auto &[id, color] : *_selected_colors) {
            _specific_colors->set(id, color);
        }
        if (_color_wheel && _color_wheel->get_widget().is_drawable()) {
            _color_wheel->set_color(_specific_colors->getAverage());
        }
    });

    _selected_colors->signal_cleared.connect([this]() {
        auto scoped = SignalBlocker(_specific_changed_connection);
        _specific_colors->clear();
    });

    // Control signals when widget isn't mapped (not visible to the user)
    signal_map().connect([this]() {
        // Showing a page only refreshes its controls. In particular, the first
        // map must not write a converted RGB/HSL preview over CMYK ink values.
        _specific_changed_connection.block();
        _specific_colors->setAll(*_selected_colors);
        _specific_changed_connection.unblock();
        _selected_changed_connection.unblock();
    });

    signal_unmap().connect([this]() {
        _specific_colors->clear();
        _specific_changed_connection.block();
        _selected_changed_connection.block();
    });

    int row = 0;
    for (auto &component : _specific_colors->getComponents()) {
        std::string index = std::to_string(component.index + 1);
        auto label = Gtk::make_managed<Gtk::Label>();
        auto slider = Gtk::make_managed<ColorSlider>(_specific_colors, component);
        auto spin = Gtk::make_managed<InkSpinButton>();
        spin->set_digits(component.id == "alpha" ? 0 : 1);
        if (component.scale < 100) {
            // for small values increase precision
            spin->set_digits(2);
            spin->get_adjustment()->set_step_increment(0.1);
        }
        _grid.attach(*label, 0, row);
        _grid.attach(*slider, 1, row);
        _grid.attach(*spin, 2, row++);
        _channels.emplace_back(std::make_unique<ColorPageChannel>(_specific_colors, *label, *slider, *spin));
    }

    // Color wheel
    auto wheel_type = _specific_colors->getComponents().get_wheel_type();
    // there are only a few types of color wheel supported:
    if (can_create_color_wheel(wheel_type)) {
        _expander.property_expanded().signal_changed().connect([this, wheel_type]() {
            auto on = _expander.property_expanded().get_value();
            if (on && !_color_wheel) {
                // create color wheel now
                create_color_wheel(wheel_type, true);
                _expander.set_child(_color_wheel->get_widget());
            }
            if (_color_wheel) {
                if (on) {
                    // update, wheel may be stale if it was hidden
                    _color_wheel->set_color(_specific_colors->getAverage());
                }
            }
        });
    }
    else {
        _expander.set_visible(false);
    }
}

ColorPage::~ColorPage() = default;

void ColorPage::show_expander(bool show) {
     _expander.set_visible(show);
}

ColorWheel* ColorPage::create_color_wheel(Space::Type type, bool disc) {
    if (_color_wheel) {
        g_message("Color wheel has already been created.");
        return _color_wheel;
    }

    _color_wheel = create_managed_color_wheel(type, disc);
    if (!_color_wheel) return nullptr;

    if (!_specific_colors->isEmpty()) {
        _color_wheel->set_color(_specific_colors->getAverage());
    }
    _color_wheel_changed = _color_wheel->connect_color_changed([this](const Color& color) {
        auto scoped = SignalBlocker{_color_wheel_changed};
        // add alpha; color wheel doesn't use it, but current color does
        auto opacity = _specific_colors->getAverage().getOpacity();
        auto c = color;
        c.setOpacity(opacity);
        _specific_colors->setAll(c);
    });
    return _color_wheel;
}

void ColorPage::set_spinner_size_pattern(const std::string& pattern) {
    for (auto& c : _channels) {
        c->get_spin().set_min_size(pattern);
    }
}

void ColorPage::attach_page(Glib::RefPtr<Gtk::SizeGroup> first_column, Glib::RefPtr<Gtk::SizeGroup> last_column) {
    if (_channels.empty()) {
        g_warning("No channels in color page");
        return;
    }
    auto& c = *_channels.front();
    first_column->add_widget(c.get_label());
    last_column->add_widget(c.get_spin());
}

void ColorPage::detach_page(Glib::RefPtr<Gtk::SizeGroup> first_column, Glib::RefPtr<Gtk::SizeGroup> last_column) {
    if (_channels.empty()) {
        g_warning("No channels in color page");
        return;
    }
    auto& c = *_channels.front();
    first_column->remove_widget(c.get_label());
    last_column->remove_widget(c.get_spin());
}

ColorPageChannel::ColorPageChannel(
    std::shared_ptr<Colors::ColorSet> color,
    Gtk::Label &label,
    ColorSlider &slider,
    InkSpinButton &spin)
    : _label(label)
    , _slider(slider)
    , _spin(spin)
    , _adj(spin.get_adjustment()), _color(std::move(color))
{
    auto &component = _slider._component;
    _label.set_markup_with_mnemonic(component.name);
    _label.set_tooltip_text(component.tip);
    _label.set_halign(Gtk::Align::CENTER);
    _label.set_xalign(0.5);

    _slider.set_hexpand(true);

    _adj->set_lower(0.0);
    _adj->set_upper(component.scale);
    _adj->set_page_increment(0.0);
    _adj->set_page_size(0.0);

    _spin.set_has_frame(true);
    _spin.set_digits(0);
    _spin.set_adjustment(_adj);

    if (component.unit == Space::Unit::Degree) {
        set_degree_suffix(_spin);
    }
    else if (component.unit == Space::Unit::Percent) {
        set_percent_suffix(_spin);
    }
    else if (component.unit == Space::Unit::Chroma40) {
        // (very) limited chroma range; increase precision
        _spin.set_digits(2);
    }

    _color_changed = _color->signal_changed.connect([this]() {
        if (_color->isValid(_slider._component)) {
            _adj->set_value(_slider.getScaled());
        }
    });
    _adj_changed = _adj->signal_value_changed().connect([this]() {
        auto scoped = SignalBlocker{_slider_changed};
        _slider.setScaled(_adj->get_value());
    });
    _slider_changed = _slider.signal_value_changed.connect([this]() {
        auto scoped = SignalBlocker{_adj_changed};
        _adj->set_value(_slider.getScaled());
    });
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
// vim:filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8: textwidth=99:
