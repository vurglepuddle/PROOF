// SPDX-License-Identifier: GPL-2.0-or-later
//
// Authors: Tavmjong Bah
// Mike Kowalski
//

#include "spin-button.h"

#include <array>
#include <cassert>
#include <iomanip>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <gtkmm/accelerator.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/root.h>
#include <gtkmm/snapshot.h>

#include "ui/containerize.h"
#include "ui/controller.h"
#include "ui/defocus-target.h"
#include "util/expression-evaluator.h"

namespace Inkscape::UI::Widget {

// CSS styles for InkSpinButton
// language=CSS
auto ink_spinbutton_css = R"=====(
@define-color border-color @borders;
@define-color bgnd-color alpha(@theme_base_color, 1.0);
@define-color focus-color alpha(@theme_selected_bg_color, 0.5);
/* :root { --border-color: lightgray; } - this is not working yet, so using nonstandard @define-color */
ink-spinbutton { border: 0 solid @border-color; border-radius: 2px; background-color: @bgnd-color; }
ink-spinbutton.frame { border: 1px solid @border-color; }
ink-spinbutton:hover button { opacity: 1; }
ink-spinbutton:focus-within { outline: 2px solid @focus-color; outline-offset: -2px; }
ink-spinbutton label#InkSpinButton-Label { opacity: 0.5; margin-left: 3px; margin-right: 3px; }
ink-spinbutton image#InkSpinButton-Icon { opacity: 0.5; }
ink-spinbutton button { border: 0 solid alpha(@border-color, 0.30); border-radius: 2px; padding: 1px; min-width: 6px; min-height: 8px; -gtk-icon-size: 10px; background-image: none; }
ink-spinbutton button.left  { border-top-right-radius: 0; border-bottom-right-radius: 0; border-right-width: 1px; }
ink-spinbutton button.right { border-top-left-radius: 0; border-bottom-left-radius: 0; border-left-width: 1px; }
ink-spinbutton entry#InkSpinButton-Entry { border: none; border-radius: 3px; padding: 0; min-height: 13px; background-color: @bgnd-color; outline-width: 0; }
.linked:not(.vertical) > ink-spinbutton:dir(ltr):not(:first-child) { border-top-left-radius: 0; border-bottom-left-radius: 0; }
.linked:not(.vertical) > ink-spinbutton:dir(ltr):not(:last-child)  { border-right-style: none; border-top-right-radius: 0; border-bottom-right-radius: 0; }
.linked:not(.vertical) > ink-spinbutton:dir(rtl):not(:first-child) { border-right-style: none; border-top-right-radius: 0; border-bottom-right-radius: 0; }
.linked:not(.vertical) > ink-spinbutton:dir(rtl):not(:last-child)  { border-top-left-radius: 0; border-bottom-left-radius: 0; }
)=====";

constexpr int timeout_click = 500;
constexpr int timeout_repeat = 50;
constexpr int icon_margin = 2;

void InkSpinButton::construct() {
    set_name("InkSpinButton");

    set_overflow(Gtk::Overflow::HIDDEN);

    _minus.set_name("InkSpinButton-Minus");
    _minus.add_css_class("left");
    _value.set_name("InkSpinButton-Value");
    _plus.set_name("InkSpinButton-Plus");
    _plus.add_css_class("right");
    _entry.set_name("InkSpinButton-Entry");
    _entry.set_alignment(0.5f);
    _entry.set_max_width_chars(3); // let it shrink, we can always stretch it
    _label.set_name("InkSpinButton-Label");
    _icon.set_name("InkSpinButton-Icon");

    _value.set_expand();
    _entry.set_expand();

    _minus.set_margin(0);
    _minus.set_size_request(8, -1);
    _value.set_margin(0);
    _value.set_single_line_mode();
    _value.set_overflow(Gtk::Overflow::HIDDEN);
    _plus.set_margin(0);
    _plus.set_size_request(8, -1);
    _minus.set_can_focus(false);
    _plus.set_can_focus(false);
    _label.set_can_focus(false);
    _label.set_xalign(0.0f);
    _label.set_visible(false);
    _label.set_can_target(false);
    _icon.set_can_target(false);
    _icon.set_valign(Gtk::Align::CENTER);
    _icon.set_visible(false);
    // use symbolic icons as labels
    _icon.add_css_class("symbolic");

    _minus.set_icon_name("go-previous-symbolic");
    _plus.set_icon_name("go-next-symbolic");

    // a fade-out mask for overflowing numbers
    _mask.set_can_target(false);

    containerize(*this);
    _icon.insert_at_end(*this);
    _label.insert_at_end(*this);
    _minus.insert_at_end(*this);
    _value.insert_at_end(*this);
    _mask.insert_at_end(*this);
    _entry.insert_at_end(*this);
    _plus.insert_at_end(*this);

    set_focus_child(_entry);

    static Glib::RefPtr<Gtk::CssProvider> provider;
    if (!provider) {
        provider = Gtk::CssProvider::create();
        provider->load_from_data(ink_spinbutton_css);
        auto const display = Gdk::Display::get_default();
        Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    }

    // ------------- CONTROLLERS -------------

    // mouse clicks to open context menu
    _click_value = Gtk::GestureClick::create();
    _click_value->set_button(); // all buttons
    _click_value->set_propagation_phase(Gtk::PropagationPhase::CAPTURE); // before GTK's popup handler
    _click_value->signal_pressed().connect([this](auto n, auto x, auto y) { on_value_clicked(); });
    add_controller(_click_value);

    // This is a mouse movement. Shows/hides +/- buttons.
    // Shows/hides +/- buttons.
    _motion = Gtk::EventControllerMotion::create();
    _motion->signal_enter().connect(sigc::mem_fun(*this, &InkSpinButton::on_motion_enter));
    _motion->signal_leave().connect(sigc::mem_fun(*this, &InkSpinButton::on_motion_leave));
    add_controller(_motion);

    // This is mouse drag movement. Changes value.
    _drag_value = Gtk::GestureDrag::create();
    _drag_value->signal_begin().connect( Controller::use_state([this](auto&, auto&& ...args) { return on_drag_begin_value(args...); }, *_drag_value));
    _drag_value->signal_update().connect(Controller::use_state([this](auto&, auto&& ...args) { return on_drag_update_value(args...); }, *_drag_value));
    _drag_value->signal_end().connect(   Controller::use_state([this](auto&, auto&& ...args) { return on_drag_end_value(args...); }, *_drag_value));
    _drag_value->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    // limit drag messages to our button only, so they are not handled by anything else
    _drag_value->set_propagation_limit(Gtk::PropagationLimit::SAME_NATIVE);
    _value.add_controller(_drag_value);
    _value.set_cursor("text");

    // Changes value.
    _scroll = Gtk::EventControllerScroll::create();
    _scroll->signal_scroll_begin().connect(sigc::mem_fun(*this, &InkSpinButton::on_scroll_begin));
    _scroll->signal_scroll().connect(sigc::mem_fun(*this, &InkSpinButton::on_scroll), false);
    _scroll->signal_scroll_end().connect(sigc::mem_fun(*this, &InkSpinButton::on_scroll_end));
    _scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES); // Mouse wheel is on y.
    add_controller(_scroll);

    _click_minus = Gtk::GestureClick::create();
    _click_minus->signal_pressed().connect(sigc::mem_fun(*this, &InkSpinButton::on_pressed_minus));
    _click_minus->signal_released().connect([this](int, double, double){ stop_spinning(); });
    _click_minus->signal_unpaired_release().connect([this](auto, auto, auto, auto){ stop_spinning(); });
    _click_minus->set_propagation_phase(Gtk::PropagationPhase::CAPTURE); // Steal from the default handler.
    _minus.add_controller(_click_minus);

    _click_plus = Gtk::GestureClick::create();
    _click_plus->signal_pressed().connect(sigc::mem_fun(*this, &InkSpinButton::on_pressed_plus));
    _click_plus->signal_released().connect([this](int, double, double){ stop_spinning(); });
    _click_plus->signal_unpaired_release().connect([this](auto, auto, auto, auto){ stop_spinning(); });
    _click_plus->set_propagation_phase(Gtk::PropagationPhase::CAPTURE); // Steal from the default handler.
    _plus.add_controller(_click_plus);

    _focus = Gtk::EventControllerFocus::create();
    _focus->signal_enter().connect([this]{
        // show editable button if '*this' is focused, but not its entry
        if (_focus->is_focus()) {
            set_focusable(false);
            enter_edit();
        }
    });
    _focus->signal_leave().connect([this]{
        if (_entry.is_visible()) {
            commit_entry();
        }
        exit_edit();
        set_focusable(true);
    });
    add_controller(_focus);
    _entry.set_focus_on_click(false);
    _entry.set_focusable(false);
    _entry.set_can_focus();
    set_can_focus();
    set_focusable();
    set_focus_on_click();

    _key_entry = Gtk::EventControllerKey::create();
    _key_entry->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _key_entry->signal_key_pressed().connect([this](auto keyval, auto keycode, auto modifiers){ return on_key_pressed(keyval, modifiers); }, false); // Before default handler.
    _entry.add_controller(_key_entry);

    //                  GTK4
    // -------------   SIGNALS   -------------

    _entry.signal_activate().connect([this] { on_activate(); });

    _minus.set_visible();
    auto m = _minus.measure(Gtk::Orientation::HORIZONTAL);
    _button_width = m.sizes.natural;
    m = _entry.measure(Gtk::Orientation::VERTICAL);
    _entry_height = m.sizes.natural;
    _baseline = m.baselines.natural;
    {
        auto layout = create_pango_layout("9");
        int text_width = 0;
        int text_height = 0;
        layout->get_pixel_size(text_width, text_height);
        _text_width_min = text_width;
        layout = create_pango_layout("12345.678");
        layout->get_pixel_size(text_width, text_height);
        _text_width_wide = text_width;
        if (_text_width_wide <= _text_width_min) {
            _text_width_wide = _text_width_min + 1;
        }
    }

    set_value(_num_value);
    set_step(_step_value);
    set_has_frame(_has_frame);
    set_has_arrows(_show_arrows);
    set_scaling_factor(_scaling_factor);
    show_arrows(false);
    _entry.hide();
    set_range(_min_value.get_value(), _max_value.get_value());

    property_icon().signal_changed().connect([this] {
        set_icon(_icon_name.get_value());
    });
    set_icon(_icon_name.get_value());

    property_label().signal_changed().connect([this] {
        set_label(_label_text.get_value().raw());
    });
    set_label(_label_text.get_value());

    property_adjustment().signal_changed().connect([this] {
        set_adjustment(_adjust.get_value());
        _step_value.set_value(_adjustment->get_step_increment());
        _min_value.set_value(_adjustment->get_lower());
        _max_value.set_value(_adjustment->get_upper());
    });
    property_digits().signal_changed().connect([this] { queue_resize(); update(false); });
    property_has_frame().signal_changed().connect([this]{ set_has_frame(_has_frame); });
    property_show_arrows().signal_changed().connect([this]{ set_has_arrows(_show_arrows); });
    property_scaling_factor().signal_changed().connect([this]{ set_scaling_factor(_scaling_factor); });
    property_step_value().signal_changed().connect([this]{ set_step(_step_value); });
    property_min_value().signal_changed().connect([this]{ _adjustment->set_lower(_min_value); });
    property_max_value().signal_changed().connect([this]{ _adjustment->set_upper(_max_value); });
    property_value().signal_changed().connect([this]{ set_value(_num_value); });
    property_prefix().signal_changed().connect([this]{ update(false); });
    property_suffix().signal_changed().connect([this]{ update(false); });
    property_width_chars().signal_changed().connect([this] { set_width_chars(_width_chars.get_value()); });

    // if the adjustment property has been set, it takes precedence over min/max values and step
    if (auto adj = _adjust.get_value()) {
        _adjustment = adj;
    }
    _connection = _adjustment->signal_value_changed().connect([this]{ update(); });

    if (auto width = _width_chars.get_value()) {
        set_width_chars(width);
    }
    update();
}

#define INIT_PROPERTIES \
    _adjust(*this, "adjustment", Glib::RefPtr<Gtk::Adjustment>{}), \
    _digits(*this, "digits", 3), \
    _num_value(*this, "value", 0.0), \
    _min_value(*this, "min-value", 0.0), \
    _max_value(*this, "max-value", 100.0), \
    _step_value(*this, "step-value", 1.0), \
    _scaling_factor(*this, "scaling-factor", 1.0), \
    _has_frame(*this, "has-frame", true), \
    _show_arrows(*this, "show-arrows", true), \
    _enter_exit(*this, "enter-exit-editing", false), \
    _wrap_around(*this, "wrap-around", false), \
    _icon_name(*this, "icon", {}), \
    _label_text(*this, "label", {}), \
    _prefix(*this, "prefix", {}), \
    _suffix(*this, "suffix", {}), \
    _width_chars(*this, "width-chars", 0), \
    _climb_rate(*this, "climb-rate", 0.0)

#define CALL_CONSTRUCTORS \
    Glib::ObjectBase("InkSpinButton"), \
    CssNameClassInit{"ink-spinbutton"}

InkSpinButton::InkSpinButton():
    CALL_CONSTRUCTORS,
    INIT_PROPERTIES {

    construct();
}

InkSpinButton::InkSpinButton(BaseObjectType* cobject, const Glib::RefPtr<Gtk::Builder>& builder):
    CALL_CONSTRUCTORS,
    BuildableWidget(cobject, builder),
    INIT_PROPERTIES {

    construct();
}

#undef INIT_PROPERTIES
#undef CALL_CONSTRUCTORS

InkSpinButton::~InkSpinButton() = default;

Gtk::SizeRequestMode InkSpinButton::get_request_mode_vfunc() const {
    return Gtk::Widget::get_request_mode_vfunc();
}

void InkSpinButton::measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural, int& minimum_baseline, int& natural_baseline) const {

    std::string text;
    if (_min_size_pattern.empty()) {
        auto delta = _digits.get_value() > 0 ? pow(10.0, -_digits.get_value()) : 0;
        auto low = _adjustment->get_lower() + delta;
        auto high = _adjustment->get_upper() - delta;
        if (_output_transformer) {
            low = _output_transformer(low);
            high = _output_transformer(high);
        }
        auto low_str = format(low, true, false, true, true);
        auto high_str = format(high, true, false, true, true);
        text = low_str.size() > high_str.size() ? low_str : high_str;
    }
    else {
        text = _min_size_pattern;
    }

    // http://developer.gnome.org/pangomm/unstable/classPango_1_1Layout.html
    auto layout = const_cast<InkSpinButton*>(this)->create_pango_layout("\u2009" + text + "\u2009");

    int text_width = 0;
    int text_height = 0;
    // get the text dimensions
    layout->get_pixel_size(text_width, text_height);

    if (orientation == Gtk::Orientation::HORIZONTAL) {
        minimum_baseline = natural_baseline = -1;
        // always measure, so gtk doesn't complain
        auto _ = _minus.measure(orientation);
        _ = _plus.measure(orientation);
        _ = _entry.measure(orientation);
        _ = _value.measure(orientation);
        _ = _label.measure(orientation);
        _ = _mask.measure(orientation);
        _ = _icon.measure(orientation);

        auto btn = _enable_arrows ? _button_width : 0;
        // always reserve space for inc/dec buttons and label, whichever is greater
        natural = std::max(get_left_padding() + text_width, btn + text_width + btn);
        // allow spin button to shrink if pushed;
        // if it is wide, then we let it go down to 0.5 its normal size
        // if it is narrow, then we let it shrink less
        // if it is very narrow (one digit), we keep natural size
        auto shrink_factor = 1.0; // 100% size - no shrinking
        if (text_width > _text_width_min) {
            double range = _text_width_wide - _text_width_min;
            auto excess = text_width - _text_width_min;
            // calc shrink amount in proportion to the size of the button;
            // let it shrink, but to no less than to 50% of natural size
            shrink_factor = std::max(0.5, 1.0 - (excess / range) * 0.5);
        }
        minimum = static_cast<int>(std::ceil(natural * shrink_factor));
    }
    else {
        minimum_baseline = natural_baseline = _baseline;
        auto height = std::max(text_height, _entry_height);
        minimum = height;
        natural = std::max(static_cast<int>(1.5 * text_height), _entry_height);
    }
}

void InkSpinButton::size_allocate_vfunc(int width, int height, int baseline) {
    Gtk::Allocation allocation;
    allocation.set_height(height);
    allocation.set_width(_button_width);
    allocation.set_x(0);
    allocation.set_y(0);

    int left = 0;
    int right = width;

    // either label or buttons may be visible, but not both
    if (_label.get_visible()) {
        Gtk::Allocation alloc;
        alloc.set_height(height);
        alloc.set_width(_label_width);
        alloc.set_x(0);
        alloc.set_y(0);
        _label.size_allocate(alloc, baseline);
        left += _label_width;
        right -= _label_width;
    }
    if (_icon.get_visible()) {
        Gtk::Allocation alloc;
        alloc.set_height(height);
        alloc.set_width(_icon_width);
        alloc.set_x(icon_margin);
        alloc.set_y(0);
        _icon.size_allocate(alloc, baseline);
        auto w = 2 * icon_margin + _icon_width;
        left += w;
        right -= w;
    }
    if (_minus.get_visible()) {
        _minus.size_allocate(allocation, baseline);
        left += allocation.get_width();
    }
    if (_plus.get_visible()) {
        allocation.set_x(width - allocation.get_width());
        _plus.size_allocate(allocation, baseline);
        right -= allocation.get_width();
    }

    allocation.set_x(left);
    allocation.set_width(std::max(0, right - left));
    if (_value.get_visible()) {
        auto alloc = allocation;
        auto min = _value.measure(Gtk::Orientation::HORIZONTAL).sizes.minimum;
        auto delta = min - allocation.get_width();
        // does text fit in available space or overflows?
        auto overflow = delta > 0; //allocation.get_width() < min;
        // if text overflows start left-aligning rather than centering
        auto xalign = overflow ? 0.0f : 0.5f;
        if (_value.get_xalign() != xalign) {
            _value.set_xalign(xalign);
        }
        if (overflow && (_label.get_visible() || _icon.get_visible()) && get_left_padding() > 0) {
            // see if there's some space on the right to recover
            alloc.set_width(alloc.get_width() + std::min(delta, get_left_padding()));
        }
        _value.size_allocate(alloc, baseline);

        // value fade-out mask
        _mask.set_opacity(overflow ? 1.0 : 0.0);
        int mask_size = 20;
        alloc.set_x(width - mask_size);
        alloc.set_width(mask_size);
        _mask.size_allocate(alloc, baseline);

    }
    if (_entry.get_visible()) {
        _entry.size_allocate(allocation, baseline);
    }
}


Glib::RefPtr<Gtk::Adjustment>& InkSpinButton::get_adjustment() {
    return _adjustment;
}

void InkSpinButton::set_adjustment(const Glib::RefPtr<Gtk::Adjustment>& adjustment) {
    if (!adjustment) return;

    _connection.disconnect();
    _adjustment = adjustment;
    _connection = _adjustment->signal_value_changed().connect([this](){ update(); });
    update();
}

void InkSpinButton::set_digits(int digits) {
    if (_digits != digits) {
        _digits = digits;
        update(false);
    }
}

int InkSpinButton::get_digits() const {
    return _digits.get_value();
}

void InkSpinButton::set_range(double min, double max) {
    _adjustment->set_lower(min);
    _adjustment->set_upper(max);
    // enable/disable plus/minus buttons
    update(false);
}

void InkSpinButton::set_step(double step_increment) {
    _adjustment->set_step_increment(step_increment);
}

void InkSpinButton::set_page_step(double page_increment) {
    _adjustment->set_page_increment(page_increment);
}

void InkSpinButton::set_prefix(const std::string& prefix, bool add_space) {
    if (add_space && !prefix.empty()) {
        _prefix.set_value(prefix + " ");
    }
    else {
        _prefix.set_value(prefix);
    }
    update(false);
}

void InkSpinButton::set_suffix(const std::string& suffix, bool add_half_space) {
    if (add_half_space && !suffix.empty()) {
        // thin space
        _suffix.set_value("\u2009" + suffix);
    }
    else {
        _suffix.set_value(suffix);
    }
    update(false);
}

void InkSpinButton::set_has_frame(bool frame) {
    if (frame) {
        add_css_class("frame");
    }
    else {
        remove_css_class("frame");
    }
}

void InkSpinButton::set_trim_zeros(bool trim) {
    if (_trim_zeros != trim) {
        _trim_zeros = trim;
        update(false);
    }
}

void InkSpinButton::set_scaling_factor(double factor) {
    assert(factor > 0 && factor < 1e9);
    _fmt_scaling_factor = factor;
    queue_resize();
    update();
}

static void trim_zeros(std::string& ret) {
    while (ret.find('.') != std::string::npos &&
        (ret.substr(ret.length() - 1, 1) == "0" || ret.substr(ret.length() - 1, 1) == ".")) {
        ret.pop_back();
    }
}

std::string InkSpinButton::format_number(double value, int precision, bool trim_zeros, bool limit_size) {
    std::stringstream ss;
    ss.imbue(std::locale("C"));
    std::string number;
    if (value > 1e12 || value < -1e12) {
        // use scientific notation to limit the size of the output number
        ss << std::scientific << std::setprecision(std::numeric_limits<double>::digits10) << value;
        number = ss.str();
    }
    else {
        ss << std::fixed << std::setprecision(precision) << value;
        number = ss.str();
        if (trim_zeros) {
            UI::Widget::trim_zeros(number);
        }
        if (limit_size) {
            auto limit = std::numeric_limits<double>::digits10;
            if (value < 0) limit += 1;

            if (number.size() > limit) {
                number = number.substr(0, limit);
            }
        }
    }
    return number;
}

std::string InkSpinButton::format(double value, bool with_prefix_suffix, bool with_markup, bool trim_zeros, bool limit_size) const {
    auto number = format_number(value, _digits.get_value(), trim_zeros, limit_size);
    auto suffix = _suffix.get_value();
    auto prefix = _prefix.get_value();
    if (with_prefix_suffix && (!suffix.empty() || !prefix.empty())) {
        if (with_markup) {
            std::string markup;
            if (!prefix.empty()) {
                markup += "<span alpha='50%'>" + Glib::Markup::escape_text(prefix) + "</span>";
            }
            markup += "<span>" + number + "</span>";
            if (!suffix.empty()) {
                markup += "<span alpha='50%'>" + Glib::Markup::escape_text(suffix) + "</span>";
            }
            return markup;
        }
        else {
            return prefix + number + suffix;
        }
    }

    return number;
}

void InkSpinButton::update(bool fire_change_notification) {
    if (!_adjustment) return;

    auto original_value = _adjustment->get_value();
    auto value = original_value;
    if (_output_transformer) {
        value = _output_transformer(value);
    }
    auto text = format(value, false, false, _trim_zeros, false);
    _entry.set_text(text);
    if (_suffix.get_value().empty() && _prefix.get_value().empty()) {
        _value.set_text(text);
    }
    else {
        _value.set_markup(format(value, true, true, _trim_zeros, false));
    }

    bool wrap = _wrap_around.get_value();
    _minus.set_sensitive(wrap || _adjustment->get_value() > _adjustment->get_lower());
    _plus .set_sensitive(wrap || _adjustment->get_value() < _adjustment->get_upper());

    if (fire_change_notification) {
        _signal_value_changed.emit(original_value / _fmt_scaling_factor);
    }
}

void InkSpinButton::set_new_value(double value) {
    if (_wrap_around.get_value()) {
        value = wrap_around(value);
    }
    _adjustment->set_value(value);
    //TODO: reflect new value in _num_value property while avoiding cycle updates
}

// ---------------- CONTROLLERS -----------------

// ------------------  MOTION  ------------------

void InkSpinButton::on_motion_enter(double x, double y) {
    _mouse_entered = true;
    if (_focus->contains_focus()) return;

    show_label_icon(false);
    if (_mouse_entered) show_arrows();
}

void InkSpinButton::on_motion_leave() {
    _mouse_entered = false;
    if (_focus->contains_focus()) return;

    show_arrows(false);
    if (!_mouse_entered) show_label_icon();

    if (_entry.is_visible()) {
        // We left the spinbutton, save value and update.
        commit_entry();
        exit_edit();
    }
}

// ---------------   DRAG VALUE  ----------------

static double get_accel_factor(Gdk::ModifierType state) {
    double scale = 1.0;
    // Ctrl modifier slows down, Shift speeds up
    if ((state & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK) {
        scale = 0.1;
    } else if ((state & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK) {
        scale = 10.0;
    }
    return scale;
}

Gtk::EventSequenceState InkSpinButton::on_drag_begin_value(Gdk::EventSequence* sequence) {
    _value.set_cursor("col-resize");
    _initial_value = _adjustment->get_value();
    _drag_value->get_point(sequence, _drag_start.x,_drag_start.y);
    return Gtk::EventSequenceState::CLAIMED;
}

Gtk::EventSequenceState InkSpinButton::on_drag_update_value(Gdk::EventSequence* sequence) {
    if (_drag_full_travel <= 0) return Gtk::EventSequenceState::NONE;

    double dx = 0.0;
    double dy = 0.0;
    _drag_value->get_offset(dx, dy);

    // If we don't move, then it probably was a button click.
    auto delta = 3.0; // tweak this value to reject real clicks, or else we'll change the value inadvertently
    if (!_drag.started && (std::fabs(dx) > delta || std::fabs(dy) > delta)) {
        _drag.started = true;
        // remember where we crossed the move threshold; this is our new zero point
        _drag.x = std::clamp(dx, -delta, delta);
        _drag.y = std::clamp(dy, -delta, delta);
        auto angle = std::fabs(std::atan2(dx, dy));
        // lock into horizontal or vertical adjustment based on where the mouse travelled
        _drag.horizontal = angle >= M_PI_4 && angle <= M_PI+M_PI_4;
    }
    if (_drag.started) {
        auto state = _drag_value->get_current_event_state();
        auto distance = _drag.horizontal ? dx - _drag.x : dy - _drag.y;
        auto value = _initial_value + get_accel_factor(state) * distance * _adjustment->get_step_increment();
        set_new_value(value);
    }
    return Gtk::EventSequenceState::CLAIMED;
}

Gtk::EventSequenceState InkSpinButton::on_drag_end_value(Gdk::EventSequence* sequence) {
    double dx = 0.0;
    double dy = 0.0;
    _drag_value->get_offset(dx, dy);

    if (dx == 0 && !_drag.started) {
        // Must have been a click!
        enter_edit();
    }
    _drag.started = false;
    _value.set_cursor("text");
    return Gtk::EventSequenceState::CLAIMED;
}

void InkSpinButton::show_arrows(bool on) {
    _minus.set_visible(on && _enable_arrows);
    _plus.set_visible(on && _enable_arrows);
}

void InkSpinButton::show_label_icon(bool on) {
    _icon.set_visible(on && _icon_width > 0);
    _label.set_visible(on && _label_width > 0 && _icon_width == 0);
}

static char const *get_text(Gtk::Editable const &editable) {
    return gtk_editable_get_text(const_cast<GtkEditable *>(editable.gobj())); // C API is const-incorrect
}

bool InkSpinButton::commit_entry() {
    try {
        double value = 0.0;
        auto text = get_text(_entry);
        if (_dont_evaluate) {
            value = std::stod(text);
        }
        else if (_evaluator) {
            value = _evaluator(text);
        }
        else {
            value = Util::ExpressionEvaluator{text}.evaluate().value;
        }
        // apply input transformer
        if (_input_transformer) {
            value = _input_transformer(value);
        }
        set_new_value(value);
        return true;
    }
    catch (const std::exception& e) {
        g_message("Expression error: %s", e.what());
    }
    return false;
}

void InkSpinButton::exit_edit() {
    show_arrows(false);
    _entry.hide();
    show_label_icon();
    _value.show();
    _mask.show();
}

bool InkSpinButton::edit_pending() const {
    return _entry.get_visible();
}

void InkSpinButton::cancel_editing() {
    update(false); // take the current recorder value and update text/display
    exit_edit();
}

inline void InkSpinButton::enter_edit() {
    show_arrows(false);
    show_label_icon(false);
    stop_spinning();
    _value.hide();
    _mask.hide();
    _entry.select_region(0, _entry.get_text_length());
    _entry.show();
    // postpone it, it won't work immediately:
    Glib::signal_idle().connect_once([this](){_entry.grab_focus();}, Glib::PRIORITY_HIGH_IDLE);
}

bool InkSpinButton::defocus() {
    if (_focus->contains_focus()) {
        // move focus away
        if (_defocus_target) {
            _defocus_target->onDefocus();
            return true;
        }
        if (_entry.child_focus(Gtk::DirectionType::TAB_FORWARD)) {
            return true;
        }
        if (auto root = get_root()) {
            root->unset_focus();
            return true;
        }
    }
    return false;
}

// ------------------  SCROLL  ------------------

void InkSpinButton::on_scroll_begin() {
    if (_drag_full_travel <= 0) return;

    _scroll_counter = 0;
}

bool InkSpinButton::on_scroll(double dx, double dy) {
    if (_drag_full_travel <= 0) return false;

    // Positive scroll delta represents movement down or to the left (what gtk+
    // docs call the "south" direction). Modify the spin value based on which is
    // larger.
    auto delta = std::abs(dx) > std::abs(dy) ? -dx : -dy;
    _scroll_counter += delta;

    // this is a threshold to control the rate at which scrolling increments/decrements current value;
    // the larger the threshold, the slower the rate; it may need to be tweaked on different platforms
#ifdef _WIN32
    // default for mouse wheel on windows
    constexpr double threshold = 1.0;
#elif defined __APPLE__
    // scrolling is very sensitive on macOS
    constexpr double threshold = 5.0;
#else
    //todo: default for Linux
    constexpr double threshold = 1.0;
#endif
    if (std::abs(_scroll_counter) >= threshold) {
        auto inc = std::round(_scroll_counter / threshold);
        _scroll_counter = 0;
        auto state = _scroll->get_current_event_state();
        change_value(inc, state);
    }
    return true;
}

void InkSpinButton::on_scroll_end() {
    if (_drag_full_travel <= 0) return;

    _scroll_counter = 0;
}

void InkSpinButton::set_value(double new_value) {
    set_new_value(new_value * _fmt_scaling_factor);
}

double InkSpinButton::get_value() const {
    return _adjustment->get_value() / _fmt_scaling_factor;
}

void InkSpinButton::change_value(double inc, Gdk::ModifierType state, bool page) {
    double scale = get_accel_factor(state);
    double step = page ? _adjustment->get_page_increment() : _adjustment->get_step_increment();
    auto value = _adjustment->get_value() + step * scale * inc;
    set_new_value(value);
}

double InkSpinButton::wrap_around(double value) {
    auto min = _adjustment->get_lower();
    auto max = _adjustment->get_upper();
    auto range = max - min;

    if (range > 0 && (value <= min || value > max)) {
        auto safemod = [](double a, double b) { return a - std::floor(a / b) * b; };
        value = max - safemod(max - value, range);
    }

    return value;
}

// ------------------   KEY    ------------------

bool InkSpinButton::on_key_pressed(guint keyval, Gdk::ModifierType state) {
    state &= Gtk::Accelerator::get_default_mod_mask();

    // Note:
    // event->triggers_context_menu() doesn't work with key messages

    switch (keyval) {
    case GDK_KEY_Escape: // Cancel
        // Esc pressed - cancel editing
        if (edit_pending() && state == Gdk::ModifierType::NO_MODIFIER_MASK) {
            cancel_editing();
            defocus();
            return true;
        }
        // allow Esc to be handled by dialog too
        break;

    // signal "activate" uses this key, so we may not see it
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
#ifdef __APPLE__
        // ctrl+return is a macOS context menu shortcut
        if (Controller::has_flag(state, Gdk::ModifierType::CONTROL_MASK)) {
            return _context_menu_call ? _context_menu_call() : false;
        }
#endif
        if (edit_pending() && state == Gdk::ModifierType::NO_MODIFIER_MASK) {
            // The capture controller consumes Enter before Gtk::Entry can activate. Emit its
            // activation signal after a valid commit so dialogs such as Transform apply the value.
            if (commit_entry()) {
                defocus();
                _signal_activate.emit();
            }
            return true;
        }
        break;

    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
        change_value(1, state);
        return true;

    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
        change_value(-1, state);
        return true;

    case GDK_KEY_Page_Up:
        change_value(1, state, true);
        return true;

    case GDK_KEY_Page_Down:
        change_value(-1, state, true);
        return true;

#ifndef __APPLE__
    case GDK_KEY_F10:
        if (Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK)) {
            return _context_menu_call ? _context_menu_call() : false;
        }
        break;
#endif

    case GDK_KEY_Menu:
        return _context_menu_call ? _context_menu_call() : false;

    default:
        break;
    }

    return false;
}

// ------------------  CLICK   ------------------

void InkSpinButton::on_pressed_plus(int n_press, double x, double y) {
    auto state = _click_plus->get_current_event_state();
    double inc = (state & Gdk::ModifierType::BUTTON3_MASK) == Gdk::ModifierType::BUTTON3_MASK ? 5 : 1;
    change_value(inc, state);
    start_spinning(inc, state, _click_plus);
}

void InkSpinButton::on_pressed_minus(int n_press, double x, double y) {
    auto state = _click_minus->get_current_event_state();
    double inc = (state & Gdk::ModifierType::BUTTON3_MASK) == Gdk::ModifierType::BUTTON3_MASK ? 5 : 1;
    change_value(-inc, state);
    start_spinning(-inc, state, _click_minus);
}

void InkSpinButton::on_value_clicked() {
    if (!_context_menu_call) return;

    auto event = _click_value->get_current_event();
    if (event->triggers_context_menu()) {
        if (_context_menu_call()) {
            _click_value->set_state(Gtk::EventSequenceState::CLAIMED);
        }
    }
}

void InkSpinButton::on_activate() {
    bool ok = commit_entry();
    if (ok && _enter_exit_edit) {
        set_focusable(true);
        defocus();
        exit_edit();
        _signal_activate.emit();
    }
}

void InkSpinButton::on_changed() {
    // NOT USED
}

void InkSpinButton::on_editing_done() {
    // NOT USED
}

void InkSpinButton::start_spinning(double steps, Gdk::ModifierType state, Glib::RefPtr<Gtk::GestureClick>& gesture) {
    _spinning = Glib::signal_timeout().connect([=,this]() {
        change_value(steps, state);
        // speed up
        _spinning = Glib::signal_timeout().connect([=,this]() {
            change_value(steps, state);
            //TODO: find a way to read mouse button state
            auto active = gesture->is_active();
            auto btn = gesture->get_current_button();
            if (!active || !btn) return false;
            return true;
        }, timeout_repeat);
        return false;
    }, timeout_click);
}

void InkSpinButton::stop_spinning() {
    if (_spinning) _spinning.disconnect();
}

int InkSpinButton::get_left_padding() const {
    // icon takes precedence if visible
    return _icon_width > 0 ? 2 * icon_margin + _icon_width : _label_width;
}

void InkSpinButton::set_width_chars(int width) {
    std::string pattern(std::clamp(width, 0, 50), '9');
    set_min_size(pattern);
}

void InkSpinButton::set_drag_sensitivity(double distance) {
    _drag_full_travel = distance;
}

void InkSpinButton::set_label(const std::string& label) {
    _label.set_text(label);
    // show label if given (and if there's no icon)
    if (label.empty() || _icon_width > 0) {
        _label.set_visible(false);
        _label_width = 0;
    }
    else {
        _label.set_visible(true);
        _label_width = _label.measure(Gtk::Orientation::HORIZONTAL).sizes.minimum;
    }
}

sigc::signal<void(double)>& InkSpinButton::signal_value_changed() {
    return _signal_value_changed;
}

void InkSpinButton::set_min_size(const std::string& pattern) {
    _min_size_pattern = pattern;
    queue_resize();
}

void InkSpinButton::set_evaluator_function(std::function<double(const Glib::ustring&)> cb) {
    _evaluator = cb;
}

void InkSpinButton::set_has_arrows(bool enable) {
    if (_enable_arrows == enable) return;

    _enable_arrows = enable;
    queue_resize();
    show_arrows(enable);
}

void InkSpinButton::set_enter_exit_edit(bool enable) {
    _enter_exit_edit = enable;
}

void InkSpinButton::set_icon(const Glib::ustring& icon_name) {
    _icon.set_from_icon_name(icon_name);
    if (icon_name.empty()) {
        _icon.set_visible(false);
        _icon_width = 0;
        // restore the label if it was defined
        _label.set_visible(_label_width > 0);
    }
    else {
        // hide the label if we are showing icon
        _label.set_visible(false);
        _icon.set_visible(true);
        _icon_width = _icon.measure(Gtk::Orientation::HORIZONTAL).sizes.minimum;
    }
}

void InkSpinButton::set_wrap_around(bool wrap) {
    _wrap_around.set_value(wrap);
}

void InkSpinButton::set_transformers(std::function<double(double)> input, std::function<double(double)> output) {
    _input_transformer = std::move(input);
    _output_transformer = std::move(output);
    update(false); // apply transformer
}

sigc::signal<void()> & InkSpinButton::signal_activate() {
    return _signal_activate;
}

void InkSpinButton::set_activates_default(bool setting) {
    _entry.set_activates_default(setting);
}

// a fade-out mask for overflowing numbers
void InkSpinButton::FadeOut::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
    auto rect = Gdk::Graphene::Rect(0, 0, get_width(), get_height());
    auto start = rect.get_top_left();
    auto end = rect.get_top_right();
    auto style = get_style_context();
    Gdk::RGBA bg(1,1,1);
    // look up our background color
    style->lookup_color("theme_base_color", bg);
    Gdk::RGBA transparent(bg);
    transparent.set_alpha(0.0f);

    std::array<GskColorStop, 2> colors = {
        GskColorStop{0.0f, *transparent.gobj()},
        {1.0f, *bg.gobj()}
    };
    gtk_snapshot_append_linear_gradient(
        snapshot->gobj(),
        rect.gobj(),
        start.gobj(),
        end.gobj(),
        colors.data(),
        colors.size()
    );
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
