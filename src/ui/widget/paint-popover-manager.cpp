// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Manager for shared paint popovers (Fill/Stroke).
 *//*
 * Authors:
 *   Ayan Das <ayandazzz@outlook.com>
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "paint-popover-manager.h"

#include <glibmm/main.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>

#include "ui/widget/paint-switch.h"
#include "ui/widget/popover-utils.h"

namespace Inkscape::UI::Widget {

PaintPopoverManager::Registration::Registration(PaintPopoverManager* mgr, Gtk::MenuButton* btn, bool fill)
    : _mgr(mgr), _btn(btn), _fill(fill) {}

PaintPopoverManager::Registration::Registration(Registration&& other) noexcept {
    *this = std::move(other);
}

PaintPopoverManager::Registration& PaintPopoverManager::Registration::operator=(Registration&& other) noexcept {
    if (this != &other) {
        if (_mgr && _btn) _mgr->unregister_button(*_btn, _fill);
        
        _mgr = other._mgr;
        _btn = other._btn;
        _fill = other._fill;
        
        other._mgr = nullptr;
        other._btn = nullptr;
    }
    return *this;
}

PaintPopoverManager::Registration::~Registration() {
    if (_mgr && _btn) {
        _mgr->unregister_button(*_btn, _fill);
    }
}

PaintPopoverManager& PaintPopoverManager::get() {
    static PaintPopoverManager instance;
    return instance;
}

PaintPopoverManager::Registration PaintPopoverManager::register_button(Gtk::MenuButton& btn, bool is_fill, SetupCallback setup, ConnectCallback connect) {
    auto& data = is_fill ? _fill_data : _stroke_data;
    Gtk::MenuButton* btn_ptr = &btn;

    btn.set_create_popup_func([this, btn_ptr, &data, is_fill, setup = std::move(setup), connect = std::move(connect)]() {
        if (!data.paint_switch) {
            data.create_resources(is_fill);
        }

        if (data.popover->get_parent() != btn_ptr) {
            if (auto old = dynamic_cast<Gtk::MenuButton*>(data.popover->get_parent())) {
                old->unset_popover();
            }
            btn_ptr->set_popover(*data.popover);     
        }
        data.clear_connections();

        if(setup) setup();
        if(connect) data.connections = connect();
        auto pop = data.popover.get();
        data.connections.push_back(
            pop->signal_map().connect([pop, btn_ptr]() {
                Inkscape::UI::Widget::Utils::smart_position(*pop, *btn_ptr);
            })
        );
        // PROOF: the paint switch is shared and stays alive after the popover closes, so keep
        // it connected to the object only while the popover is open. Otherwise anything the
        // switch emitted later, e.g. on window activation, changed the last object edited.
        // Disconnect on idle, so an entry that commits as focus leaves still applies.
        data.connections.push_back(
            pop->signal_closed().connect([&data, pop]() {
                Glib::signal_idle().connect_once([&data, pop]() {
                    if (!pop->is_visible()) data.clear_connections();
                });
            })
        );
    });

    return Registration(this, &btn, is_fill);
}

void PaintPopoverManager::unregister_button(Gtk::MenuButton& btn, bool is_fill) {
    auto& data = is_fill ? _fill_data : _stroke_data;
    if (data.popover && data.popover->get_parent() == &btn) {
        btn.unset_popover();
        data.clear_connections();
    }
}

PaintSwitch* PaintPopoverManager::get_switch(bool is_fill) {
    auto& data = is_fill ? _fill_data : _stroke_data;
    if (!data.paint_switch) {
        data.create_resources(is_fill);
    }
    return data.paint_switch.get();
}

Gtk::Popover* PaintPopoverManager::get_popover(bool is_fill) {
  auto& data = is_fill ? _fill_data : _stroke_data;
    if (!data.paint_switch) {
        data.create_resources(is_fill);
    }
    return data.popover.get();
}

void PaintPopoverManager::SharedData::create_resources(bool is_fill) {
    paint_switch = PaintSwitch::create(false, is_fill);
    popover = std::make_unique<Gtk::Popover>();
    popover->set_child(*paint_switch);
    Inkscape::UI::Widget::Utils::wrap_in_scrolled_window(*popover, 250);
}

void PaintPopoverManager::SharedData::clear_connections() {
    for(auto& c : connections) c.disconnect();
    connections.clear();
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
// vim:filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99:
