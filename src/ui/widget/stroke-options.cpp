// SPDX-License-Identifier: GPL-2.0-or-later
//
// Created by Michael Kowalski on 9/3/24.

#include "stroke-options.h"

#include <glib/gi18n.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>

#include "property-utils.h"
#include "style.h"

namespace Inkscape::UI::Widget {

struct Props {
    const char* label;
    sigc::signal<void (const char*)>* signal;
    struct {
        Gtk::ToggleButton* button = nullptr;
        const char* icon;
        const char* style;
        const char* tooltip;
    } buttons[4];
};

StrokeOptions::StrokeOptions() {
    set_column_spacing(4);
    set_row_spacing(8);

    Props properties[] = {
        // TRANSLATORS: The line join style specifies the shape to be used at the
        //  corners of paths. It can be "miter", "round" or "bevel".
        {_("Join"), &_join_changed, {
            {&_join_bevel, "stroke-join-bevel", "bevel", _("Bevel join")},
            {&_join_round, "stroke-join-round", "round", _("Round join")},
            {&_join_miter, "stroke-join-miter", "miter", _("Miter join")},
            {} // <- exit sentry
        }},
        // TRANSLATORS: cap type specifies the shape for the ends of lines
        {_("Cap"), &_cap_changed, {
            {&_cap_butt,   "stroke-cap-butt",   "butt",   _("Butt cap")},
            {&_cap_round,  "stroke-cap-round",  "round",  _("Round cap")},
            {&_cap_square, "stroke-cap-square", "square", _("Square cap")},
            {}
        }},
    };

    Utils::SpinPropertyDef limit_prop = {&_miter_limit, { 0, 1e5, 0.1, 10, 3 }, nullptr, _("Maximum length of the miter (in units of stroke width)") };
    Utils::init_spin_button(limit_prop);

    int row = 0;
    for (auto& prop : properties) {
        auto& label = *Gtk::make_managed<Gtk::Label>(prop.label);
        label.set_xalign(0);
        attach(label, 0, row);
        auto box = Gtk::make_managed<Gtk::Box>();
        box->add_css_class("linked");
        box->add_css_class("large-icon");
        box->add_css_class("reduced-padding");
        box->set_spacing(0);
        attach(*box, 1, row, row == 0 ? 1 : 2);

        auto first = prop.buttons[0].button;
        for (auto btn : prop.buttons) {
            auto button = btn.button;
            if (!button) break;

            if (button != first) button->set_group(*first);
            button->set_icon_name(btn.icon);
            button->set_tooltip_text(btn.tooltip);
            button->signal_toggled().connect([this,btn,prop]() {
                if (_update.pending()) return;

                if (btn.button->get_active()) {
                    prop.signal->emit(btn.style);

                    if (prop.signal == &_join_changed) {
                        // enable/disable miter limit widget accordingly
                        _miter_limit.set_sensitive(strcmp(btn.style, "miter") == 0);
                    }
                }
            });
            box->append(*button);
        }

        if (first == &_join_bevel) {
            _miter_limit.set_valign(Gtk::Align::CENTER);
            attach(_miter_limit, 2, row);
        }

        ++row;
    }

    attach(_paint_order, 1, row, 2);
    auto vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_homogeneous();
    vbox->append(*Gtk::make_managed<Gtk::Label>(_("Order")));
    vbox->append(*Gtk::make_managed<Gtk::Box>());
    vbox->append(*Gtk::make_managed<Gtk::Box>());
    for (auto w : vbox->get_children()) {
        w->set_vexpand();
    }
    attach(*vbox, 0, row);

    _miter_limit.signal_value_changed().connect([this](double value) {
        if (_update.pending()) return;
        _miter_changed.emit(value);
    });

    _paint_order.signal_values_changed().connect([this] {
        if (_update.pending()) return;
        auto order = _paint_order.getValue().get_value();
        _order_changed.emit(order.c_str());
    });
}

void StrokeOptions::update_widgets(SPStyle& style) {
    auto scope(_update.block());

    auto limit = style.stroke_miterlimit.value;
    _miter_limit.set_value(limit);

    auto join = style.stroke_linejoin.value;
    if (join == SP_STROKE_LINEJOIN_BEVEL) {
        _join_bevel.set_active();
        _miter_limit.set_sensitive(false);
    }
    else if (join == SP_STROKE_LINEJOIN_ROUND) {
        _join_round.set_active();
        _miter_limit.set_sensitive(false);
    }
    else {
        _join_miter.set_active();
        _miter_limit.set_sensitive(!style.stroke_extensions.hairline);
    }

    auto cap = style.stroke_linecap.value;
    if (cap == SP_STROKE_LINECAP_SQUARE) {
        _cap_square.set_active();
    }
    else if (cap == SP_STROKE_LINECAP_ROUND) {
        _cap_round.set_active();
    }
    else {
        _cap_butt.set_active();
    }

    SPIPaintOrder order;
    order.read(style.paint_order.set ? style.paint_order.value : "normal");
    bool has_markers = true; // <- TODO
    _paint_order.setValue(order, has_markers);
}

} // namespace
