// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Erasor aux toolbar
 */
/* Authors:
 *   MenTaLguY <mental@rydia.net>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Frank Felfe <innerspace@iname.com>
 *   John Cliff <simarilius@yahoo.com>
 *   David Turner <novalis@gnu.org>
 *   Josh Andler <scislac@scislac.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Maximilian Albert <maximilian.albert@gmail.com>
 *   Tavmjong Bah <tavmjong@free.fr>
 *   Abhishek Sharma
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *   Vaibhav Malik <vaibhavmalik2018@gmail.com>
 *
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2003 MenTaLguY
 * Copyright (C) 1999-2011 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "eraser-toolbar.h"

#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>

#include "desktop.h"
#include "document-undo.h"
#include "ui/builder-utils.h"
#include "ui/simple-pref-pusher.h"
#include "ui/tools/eraser-tool.h"
#include "ui/util.h"
#include "ui/widget/spinbutton.h"

using Inkscape::DocumentUndo;

namespace Inkscape::UI::Toolbar {

EraserToolbar::EraserToolbar()
    : EraserToolbar{create_builder("toolbar-eraser.ui")}
{}

EraserToolbar::EraserToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "eraser-toolbar")}
    , _params_sep(get_widget<Gtk::Separator>(builder, "_params_sep"))
    , _width_box(get_widget<Gtk::Box>(builder, "_width_box"))
    , _width_item(get_derived_widget<UI::Widget::SpinButton>(builder, "_width_item"))
    , _thinning_box(get_widget<Gtk::Box>(builder, "_thinning_box"))
    , _thinning_item(get_derived_widget<UI::Widget::SpinButton>(builder, "_thinning_item"))
    , _cap_rounding_box(get_widget<Gtk::Box>(builder, "_cap_rounding_box"))
    , _cap_rounding_item(get_derived_widget<UI::Widget::SpinButton>(builder, "_cap_rounding_item"))
    , _tremor_box(get_widget<Gtk::Box>(builder, "_tremor_box"))
    , _tremor_item(get_derived_widget<UI::Widget::SpinButton>(builder, "_tremor_item"))
    , _mass_box(get_widget<Gtk::Box>(builder, "_mass_box"))
    , _mass_item(get_derived_widget<UI::Widget::SpinButton>(builder, "_mass_item"))
    , _usepressure_btn(&get_widget<Gtk::ToggleButton>(builder, "_usepressure_btn"))
    , _split_sep(get_widget<Gtk::Separator>(builder, "_split_sep"))
    , _split_btn(get_widget<Gtk::ToggleButton>(builder, "_split_btn"))
{
    auto prefs = Preferences::get();
    int const eraser_mode = prefs->getInt("/tools/eraser/mode", _modeAsInt(Tools::DEFAULT_ERASER_MODE));

    // Setup the spin buttons.
    setup_derived_spin_button(_width_item, "width", 15, &EraserToolbar::width_value_changed);
    setup_derived_spin_button(_thinning_item, "thinning", 10, &EraserToolbar::velthin_value_changed);
    setup_derived_spin_button(_cap_rounding_item, "cap_rounding", 0.0, &EraserToolbar::cap_rounding_value_changed);
    setup_derived_spin_button(_tremor_item, "tremor", 0.0, &EraserToolbar::tremor_value_changed);
    setup_derived_spin_button(_mass_item, "mass", 10, &EraserToolbar::mass_value_changed);

     _width_item.set_custom_numeric_menu_data({
        {  0, _("(no width)")},
        {  1, _("(hairline)")},
        {  3, ""},
        {  5, ""},
        { 10, ""},
        { 15, _("(default)")},
        { 20, ""},
        { 30, ""},
        { 50, ""},
        { 75, ""},
        {100, _("(broad stroke)")}
    });

     _thinning_item.set_custom_numeric_menu_data({
        {-100, _("(speed blows up stroke)")},
        { -40, ""},
        { -20, ""},
        { -10, _("(slight widening)")},
        {   0, _("(constant width)")},
        {  10, _("(slight thinning, default)")},
        {  20, ""},
        {  40, ""},
        { 100, _("(speed deflates stroke)")}
    });

    _cap_rounding_item.set_custom_numeric_menu_data({
        {  0, _("(blunt caps, default)")},
        {0.3, _("(slightly bulging)")},
        {0.5, ""},
        {1.0, ""},
        {1.4, _("(approximately round)")},
        {5.0, _("(long protruding caps)")}
    });

     _tremor_item.set_custom_numeric_menu_data({
        {  0, _("(smooth line)")},
        { 10, _("(slight tremor)")},
        { 20, _("(noticeable tremor)")},
        { 40, ""},
        { 60, ""},
        {100, _("(maximum tremor)")}
    });

     _mass_item.set_custom_numeric_menu_data({
        {  0, _("(no inertia)")},
        {  2, _("(slight smoothing, default)")},
        { 10, _("(noticeable lagging)")},
        { 20, ""},
        { 50, ""},
        {100, _("(maximum inertia)")}
    });

    // Configure mode buttons
    int btn_index = 0;
    for (auto &item : children(get_widget<Gtk::Box>(builder, "mode_buttons_box"))) {
        auto &btn = dynamic_cast<Gtk::ToggleButton &>(item);
        btn.set_active(btn_index == eraser_mode);
        btn.signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &EraserToolbar::mode_changed), btn_index++));
    }

    // Pressure button
    _pressure_pusher = std::make_unique<UI::SimplePrefPusher>(_usepressure_btn, "/tools/eraser/usepressure");

    // Split button
    _split_btn.set_active(prefs->getBool("/tools/eraser/break_apart", false));

    _initMenuBtns();

    // Signals.
    _usepressure_btn->signal_toggled().connect(sigc::mem_fun(*this, &EraserToolbar::usepressure_toggled));
    _split_btn.signal_toggled().connect(sigc::mem_fun(*this, &EraserToolbar::toggle_break_apart));

    set_eraser_mode_visibility(eraser_mode);
}

EraserToolbar::~EraserToolbar() = default;

void EraserToolbar::setup_derived_spin_button(UI::Widget::SpinButton &btn, Glib::ustring const &name,
                                              double default_value, ValueChangedMemFun const value_changed_mem_fun)
{
    auto const path = "/tools/eraser/" + name;
    auto const val = Preferences::get()->getDouble(path, default_value);

    auto adj = btn.get_adjustment();
    adj->set_value(val);
    adj->signal_value_changed().connect(sigc::mem_fun(*this, value_changed_mem_fun));

    btn.setDefocusTarget(this);
}

/**
 * @brief Computes the integer value representing eraser mode
 * @param mode A mode of the eraser tool, from the enum EraserToolMode
 * @return the integer to be stored in the prefs as the selected mode
 */
unsigned EraserToolbar::_modeAsInt(Tools::EraserToolMode const mode)
{
    using namespace Inkscape::UI::Tools;

    if (mode == EraserToolMode::DELETE) {
        return 0;
    } else if (mode == EraserToolMode::CUT) {
        return 1;
    } else if (mode == EraserToolMode::CLIP) {
        return 2;
    } else {
        return _modeAsInt(DEFAULT_ERASER_MODE);
    }
}

void EraserToolbar::mode_changed(int mode)
{
    if (DocumentUndo::getUndoSensitive(_desktop->getDocument())) {
        Preferences::get()->setInt("/tools/eraser/mode", mode);
    }

    set_eraser_mode_visibility(mode);

    // only take action if run by the attr_changed listener
    /*if (!_blocker.pending()) {
        // in turn, prevent listener from responding
        auto guard = _blocker.block();

        if ( eraser_mode != ERASER_MODE_DELETE ) {
        } else {
        }
        // TODO finish implementation
    }*/
}

void EraserToolbar::set_eraser_mode_visibility(unsigned const eraser_mode)
{
    using namespace Inkscape::UI::Tools;

    // Most items are only visible in CLIP and CUT modes (invisible in DELETE mode)
    bool const params_visibility = eraser_mode != _modeAsInt(EraserToolMode::DELETE);
    _params_sep.set_visible(params_visibility);
    _width_box.set_visible(params_visibility);
    _thinning_box.set_visible(params_visibility);
    _cap_rounding_box.set_visible(params_visibility);
    _tremor_box.set_visible(params_visibility);
    _mass_box.set_visible(params_visibility);
    _usepressure_btn->set_visible(params_visibility);

    // Split is only visible in CUT mode
    bool const split_visibility = eraser_mode == _modeAsInt(EraserToolMode::CUT);
    _split_sep.set_visible(split_visibility);
    _split_btn.set_visible(split_visibility);
}

void EraserToolbar::width_value_changed()
{
    Preferences::get()->setDouble("/tools/eraser/width", _width_item.get_adjustment()->get_value());
}

void EraserToolbar::mass_value_changed()
{
    Preferences::get()->setDouble("/tools/eraser/mass", _mass_item.get_adjustment()->get_value());
}

void EraserToolbar::velthin_value_changed()
{
    Preferences::get()->setDouble("/tools/eraser/thinning", _thinning_item.get_adjustment()->get_value());
}

void EraserToolbar::cap_rounding_value_changed()
{
    Preferences::get()->setDouble("/tools/eraser/cap_rounding", _cap_rounding_item.get_adjustment()->get_value());
}

void EraserToolbar::tremor_value_changed()
{
    Preferences::get()->setDouble("/tools/eraser/tremor", _tremor_item.get_adjustment()->get_value());
}

void EraserToolbar::toggle_break_apart()
{
    Preferences::get()->setBool("/tools/eraser/break_apart", _split_btn.get_active());
}

void EraserToolbar::usepressure_toggled()
{
    Preferences::get()->setBool("/tools/eraser/usepressure", _usepressure_btn->get_active());
}

} // namespace Inkscape::UI::Toolbar

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
