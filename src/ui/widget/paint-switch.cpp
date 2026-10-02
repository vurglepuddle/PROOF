// SPDX-License-Identifier: GPL-2.0-or-later

#include "paint-switch.h"
#include <glib/gi18n.h>
#include <glibmm/markup.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>
#include <gtkmm/stack.h>
#include <gtkmm/togglebutton.h>
#include <memory>

#include "color-picker-panel.h"
#include "colors/color-set.h"
#include "colors/manager.h"
#include "colors/spaces/base.h"
#include "style-internal.h"
#include "ui/builder-utils.h"
#include "ui/operation-blocker.h"
#include "ui/widget/gradient-editor.h"
#include "ui/widget/gradient-selector.h"
#include "ui/widget/pattern-editor.h"
#include "ui/widget/swatch-editor.h"
#include "object/sp-hatch.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-pattern.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-stop.h"
#include "desktop.h"
#include "document.h"
#include "mesh-editor.h"
#include "paint-inherited.h"
#include "pattern-manager.h"
#include "widget-group.h"
#include "actions/actions-tools.h"

namespace Inkscape::UI::Widget {

PaintMode get_mode_from_paint(const SPIPaint& paint) {
    if (!paint.set) {
        return PaintMode::Derived;
    }

    if (auto server = paint.isPaintserver() ? paint.href->getObject() : nullptr) {
        if (is<SPGradient>(server) && cast<SPGradient>(server)->getVector()->isSwatch()) {
            return PaintMode::Swatch;
        }
        else if (is<SPLinearGradient>(server) || is<SPRadialGradient>(server)) {
            return PaintMode::Gradient;
        }
#ifdef WITH_MESH
        else if (is<SPMeshGradient>(server)) {
            return PaintMode::Mesh;
        }
#endif
        else if (is<SPPattern>(server)) {
            return PaintMode::Pattern;
        }
        else if (is<SPHatch>(server)) {
            return PaintMode::Hatch;
        }
        else {}
    }
    else if (paint.isColor() && paint.paintSource == SP_CSS_PAINT_ORIGIN_NORMAL) {
        return PaintMode::Solid;
    }
    else if (paint.isNone()) {
        return PaintMode::None;
    }
    else if (paint.paintSource != SP_CSS_PAINT_ORIGIN_NORMAL) {
        return PaintMode::Derived;
    }

    g_warning("Unexpected paint mode combination\n");
    return PaintMode::Derived;
}

namespace {

const struct Paint { PaintMode mode; const char* icon; const char* name; const char* tip; } paint_modes[] = {
    {PaintMode::Solid,       "paint-solid",           C_("Paint type", "Flat"),     _("Flat color")},
    {PaintMode::Gradient,    "paint-gradient-linear", C_("Paint type", "Gradient"), _("Linear gradient fill")},
#ifdef WITH_MESH
    {PaintMode::Mesh,        "paint-gradient-mesh",   C_("Paint type", "Mesh"),     _("Mesh fill")},
#endif
    // Note: there's no hatch mode; hatches are "patterns"
    {PaintMode::Pattern,     "paint-pattern",         C_("Paint type", "Pattern"),  _("Pattern and hatch fill")},
    {PaintMode::Swatch,      "paint-swatch",          C_("Paint type", "Swatch"),   _("Swatch color")},
    {PaintMode::Derived,     "paint-unknown",         C_("Paint type", "Inherited"),_("Inherited")},
    {PaintMode::None,        "paint-none",            C_("Paint type", "None"),     _("No paint")},
};

class FlatColorEditor : public Gtk::Box {
    const char* _prefs = "/color-editor";
    std::unique_ptr<ColorPickerPanel> _picker;
public:
    FlatColorEditor(Space::Type space, std::shared_ptr<Colors::ColorSet> colors) :
        _picker(ColorPickerPanel::create(space, get_plate_type_preference(_prefs, ColorPickerPanel::Rect), colors)) {
        append(*_picker);
    }
    void set_color_picker_plate(ColorPickerPanel::PlateType type) {
        _picker->set_plate_type(type);
        set_plate_type_preference(_prefs, type);
    }
    ColorPickerPanel::PlateType get_color_picker_plate() const {
        return _picker->get_plate_type();
    }
    ColorPickerPanel& get_picker() {
        return *_picker;
    }
};

} // namespace

Glib::ustring get_paint_mode_icon(PaintMode mode) {
    for (auto&& p : paint_modes) {
        if (p.mode == mode) return p.icon;
    }
    return {};
}

Glib::ustring get_paint_mode_name(PaintMode mode) {
    for (auto&& p : paint_modes) {
        if (p.mode == mode) return p.name;
    }
    return {};
}

PaintSwitch::PaintSwitch():
    Gtk::Box(Gtk::Orientation::VERTICAL) {

    set_name("PaintSwitch");
}

namespace {

Space::Type get_color_type() {
    auto name = Preferences::get()->getString("/color-picker/sel-color-type", "HSL");
    auto space = Manager::get().find(name);
    return space ? space->getType() : Space::Type::HSL;
}

void store_color_type(Space::Type type) {
    if (auto space = Manager::get().find(type)) {
        Preferences::get()->setString("/color-picker/sel-color-type", space->getName());
    }
}

} // namespace

class PaintSwitchImpl : public PaintSwitch {
public:
    PaintSwitchImpl(bool support_no_paint, bool support_fill_rule, bool compact_mode);

    void set_desktop(SPDesktop* desktop) override {

        _desktop = desktop;

        _swatch.set_desktop(desktop);

        if (desktop) {
            _connection_desktop_gradient_stop_selected =
                _desktop->connect_gradient_stop_selected([this] (auto stop) {
                    if (_update.pending()) {
                        return;
                    }
                    auto scoped(_update.block());
                    _gradient.selectStop(stop);
                });
            _connection_gradient_signal_stop_selected =
                _gradient.signal_stop_selected().connect([this, desktop](SPStop *stop) {
                    if (_update.pending()) {
                        return;
                    }
                    auto scoped(_update.block());
                    desktop->emit_gradient_stop_selected(stop);
                });
        } else {
            _connection_gradient_signal_stop_selected.disconnect();
            _connection_desktop_gradient_stop_selected.disconnect();
        }
    }

    void set_document(SPDocument* document) override {
        _document = document;
        _mesh.set_document(document);
        _swatch.set_document(document);
        _pattern.set_document(document);
    }
    void show_placeholder(const Glib::ustring& text, bool reset_toggles) override {
        _placeholder.set_text(text);
        _stack.set_visible_child(_placeholder);
        _mode = PaintMode::None;
        _mode_group.set_active(reset_toggles);
    }

    // called from the outside to update UI
    void set_mode(PaintMode mode) override;
    // internal handler for buttons switching paint mode
    void switch_paint_mode(PaintMode mode);
    void set_color(const Colors::Color& color) override;
    sigc::signal<void (const Colors::Color&)> get_flat_color_changed() override;
    sigc::signal<void (PaintMode)> get_signal_mode_changed() override;
    void update_from_paint(const SPIPaint& paint) override;
    void set_fill_rule(FillRule fill_rule) override;
    void _set_mode(PaintMode mode);

    sigc::signal<void (SPGradient* gradient, SPGradientType type)> get_gradient_changed() override {
        return _signal_gradient_changed;
    }
    sigc::signal<void (SPGradient* mesh)> get_mesh_changed() override {
        return _signal_mesh_changed;
    }
    sigc::signal<void (SPGradient* swatch, EditOperation, SPGradient*, std::optional<Color>, Glib::ustring)> get_swatch_changed() override {
        return _signal_swatch_changed;
    }
    // patterns and hatches
    sigc::signal<void (SPPattern* pattern, std::optional<Color> color, const Glib::ustring& label,
        const Geom::Affine& transform, const Geom::Point& offset, bool uniform_scale, const Geom::Scale& gap)> get_pattern_changed() override {
        return _signal_pattern_changed;
    }
    sigc::signal<void (SPHatch* hatch, std::optional<Colors::Color> color, const Glib::ustring& label,
        const Geom::Affine& transform, const Geom::Point& offset, double pitch, double rotation, double thickness)> get_hatch_changed() override {
        return _signal_hatch_changed;
    }
    sigc::signal<void (FillRule)> get_fill_rule_changed() override {
        return _signal_fill_rule_changed;
    }
    sigc::signal<void (PaintDerivedMode)> get_inherit_mode_changed() override {
        return _signal_inherit_mode_changed;
    }
    void fire_flat_color_changed() {
        if (_update.pending()) return;

        _signal_color_changed.emit(_color->getAverage());
    }
    // get selected pattern/hatch
    SPPaintServer* get_paint() {
        SPPaintServer* paint = nullptr;
        auto id = _pattern.get_selected_doc_pattern();
        if (!id.empty() && _document) {
            paint = cast<SPPaintServer>(_document->getObjectById(id));
        }
        if (!paint) {
            auto [id, stock_doc] = _pattern.get_selected_stock_pattern();
            if (!id.empty() && stock_doc) {
                id = "urn:inkscape:pattern:" + id;
                paint = cast<SPPaintServer>(get_stock_item(id.c_str(), true, stock_doc));
            }
        }
        return paint;
    }
    void fire_pattern_changed() {
        if (_update.pending()) return;

        auto scoped(_update.block());
        auto paint = get_paint();
        if (auto pattern = cast<SPPattern>(paint)) {
            _signal_pattern_changed.emit(pattern,
                _pattern.get_selected_color(), _pattern.get_label(), _pattern.get_selected_transform(),
                _pattern.get_selected_offset(), _pattern.is_selected_scale_uniform(), _pattern.get_selected_gap());
        }
        else if (auto hatch = cast<SPHatch>(paint)) {
            _signal_hatch_changed.emit(hatch,
                _pattern.get_selected_color(), _pattern.get_label(), _pattern.get_selected_transform(),
                _pattern.get_selected_offset(),
                _pattern.get_selected_pitch(), _pattern.get_selected_rotation(), _pattern.get_selected_thickness()
            );
        }
    }
    void fire_gradient_changed(SPGradient* gradient, PaintMode mode) {
        if (_update.pending()) return;

        auto scoped(_update.block());
        auto vector = gradient ? gradient->getVector() : nullptr;
        _signal_gradient_changed.emit(vector, _gradient.get_type());
    }
    void fire_swatch_changed(SPGradient* swatch, EditOperation action, SPGradient* replacement, std::optional<Color> color, Glib::ustring label) {
        if (_update.pending()) return;

        auto scoped(_update.block());
        _signal_swatch_changed.emit(swatch, action, replacement, color, label);
    }
    void fire_mesh_changed(SPGradient* mesh) {
        if (_update.pending()) return;

        auto scoped(_update.block());
        _signal_mesh_changed.emit(mesh);
    }

    // set current page color plate type - circle, rect or none
    void set_plate_type(ColorPickerPanel::PlateType type);
    std::optional<ColorPickerPanel::PlateType> get_plate_type(Gtk::Widget* page) const;
    std::shared_ptr<Colors::ColorSet> _color = std::make_shared<Colors::ColorSet>();
    sigc::signal<void (const Colors::Color&)> _signal_color_changed;

    Glib::RefPtr<Gtk::Builder> _builder;

    sigc::signal<void (PaintMode)> _signal_mode_changed;
    sigc::signal<void (SPGradient* gradient, SPGradientType type)> _signal_gradient_changed;
    sigc::signal<void (SPGradient* mesh)> _signal_mesh_changed;
    sigc::signal<void (SPGradient* swatch, EditOperation, SPGradient*, std::optional<Color>, Glib::ustring)> _signal_swatch_changed;
    sigc::signal<void (SPPattern*, std::optional<Color>, const Glib::ustring&, const Geom::Affine&, const Geom::Point&,
                       bool, const Geom::Scale&)> _signal_pattern_changed;
    sigc::signal<void (SPHatch*, std::optional<Color>, const Glib::ustring&, const Geom::Affine&, const Geom::Point&, double, double, double)> _signal_hatch_changed;
    sigc::signal<void (FillRule)> _signal_fill_rule_changed;
    sigc::signal<void (PaintDerivedMode)> _signal_inherit_mode_changed;

    sigc::connection _connection_gradient_signal_stop_selected;
    sigc::connection _connection_desktop_gradient_stop_selected;

    std::map<PaintMode, Gtk::Widget*> _pages;
    std::map<PaintMode, Gtk::ToggleButton*> _mode_buttons;
    std::map<ColorPickerPanel::PlateType, Gtk::ToggleButton*> _plate_buttons;

    // compact plate buttons
    std::unique_ptr<UI::Widget::PopoverMenu> _menu_popover;
    Gtk::MenuButton& _menu_btn;
    bool _use_compact_mode = false;
    PaintMode _mode = PaintMode::None;
    SPDocument* _document = nullptr;
    Gtk::Stack& _stack;
    FlatColorEditor _flat_color{get_color_type(), _color};
    GradientEditor _gradient{"/gradient-editor", get_color_type(), true, false};
    PatternEditor _pattern{"/pattern-editor", PatternManager::get()};
    SwatchEditor _swatch{get_color_type(), "/swatch-editor"};
    MeshEditor _mesh;
    PaintInherited& _inherited;
    Gtk::Button& _fill_rule_btn;
    FillRule _fill_rule = FillRule::NonZero;
    OperationBlocker _update;
    Gtk::ToggleButton _mode_group;
    WidgetGroup _plate_type;
    SPDesktop* _desktop = nullptr;
    Gtk::Label _placeholder;
};

PaintSwitchImpl::PaintSwitchImpl(bool support_no_paint, bool support_fill_rule, bool compact_mode) :
    _builder(create_builder("paint-switch.ui")),
    _use_compact_mode(compact_mode),
    _stack(get_widget<Gtk::Stack>(_builder, "stack")),
    _inherited(get_derived_widget<PaintInherited>(_builder, "inherited")),
    _fill_rule_btn(get_widget<Gtk::Button>(_builder, "btn-fill-rule")),
    _menu_btn(get_widget<Gtk::MenuButton>(_builder, "btn-menu")),
    _menu_popover(std::make_unique<UI::Widget::PopoverMenu>(Gtk::PositionType::BOTTOM)) {

    _placeholder.set_halign(Gtk::Align::CENTER);
    _placeholder.set_valign(Gtk::Align::CENTER);
    _placeholder.get_style_context()->add_class("dim-label");
    _stack.add(_placeholder, "placeholder");

    if (!support_fill_rule) {
        _fill_rule_btn.hide();
    }
    _color->set(Color(0x000000ff));
    auto& types = get_widget<Gtk::Box>(_builder, "types");

    // add buttons switching paint mode
    for (auto i : paint_modes) {
        if (i.mode == PaintMode::None && !support_no_paint) continue;

        auto btn = Gtk::make_managed<Gtk::ToggleButton>();
        btn->set_icon_name(i.icon);
        btn->set_has_frame(false);
        btn->set_tooltip_text(i.tip);
        btn->set_group(_mode_group);
        auto mode = i.mode;
        btn->signal_toggled().connect([=,this]{
            if (btn->get_active() && !_update.pending()) {
                switch_paint_mode(mode);
            }
        });
        types.append(*btn);
        _mode_buttons[i.mode] = btn;
    }

    if (_use_compact_mode) {
        get_widget<Gtk::Widget>(_builder, "pickers").hide();
        _menu_btn.show();
        _menu_btn.set_popover(*_menu_popover);
    } else {
        _menu_btn.hide();
        get_widget<Gtk::Widget>(_builder, "pickers").show();
    }

    static auto const pickers = std::to_array({
        std::tuple{ 
            ColorPickerPanel::PlateType::Rect,   
            "btn-rect", "color-picker-rect", _("Color Map"), _("Show color map")
        },
        std::tuple{ 
            ColorPickerPanel::PlateType::Circle, 
            "btn-circle", "color-picker-circle", _("Color Wheel"), _("Show color wheel")
        },
        std::tuple{
            ColorPickerPanel::PlateType::None,   
            "btn-input", "color-picker-input", _("Only Sliders"), _("Show color sliders only") 
        }
    });
    for (const auto& [type, button_id, icon, label, tooltip] : pickers) {
        auto& plate_btn = get_widget<Gtk::ToggleButton>(_builder, button_id);
        _plate_buttons[type] = &plate_btn;
        _plate_type.add(&plate_btn);

        if (_use_compact_mode) {
            auto menu_item = Gtk::make_managed<UI::Widget::PopoverMenuItem>(label, false, icon);
            menu_item->set_tooltip_text(tooltip);

            menu_item->signal_activate().connect([this, type, icon] {
                if (!_update.pending()) {
                    set_plate_type(type);
                    _menu_btn.set_icon_name(icon);
                }
            });
            _menu_popover->append(*menu_item);

        } else {
            plate_btn.signal_toggled().connect([this, type, &plate_btn] {
                if (plate_btn.get_active() && !_update.pending()) {
                    set_plate_type(type);
                }
            });
        }
    }

    _flat_color.get_picker().get_color_space_changed().connect([](auto type) {
        store_color_type(type);
    });

    _mesh.signal_changed().connect([this](auto mesh) { fire_mesh_changed(mesh); });

    _swatch.signal_changed().connect([this](auto swatch, auto operation, auto replacement) {
        fire_swatch_changed(swatch, operation, replacement, {}, {});
    });
    _swatch.signal_color_changed().connect([this](auto swatch, auto& color) {
        fire_swatch_changed(swatch, EditOperation::Change, nullptr, color, {});
    });
    _swatch.signal_label_changed().connect([this](auto swatch, auto& label) {
        fire_swatch_changed(swatch, EditOperation::Rename, nullptr, {}, label);
    });
    _swatch.get_picker().get_color_space_changed().connect([](auto type) {
        store_color_type(type);
    });
    _fill_rule_btn.signal_clicked().connect([this] {
        if (_update.pending()) return;

        auto scoped(_update.block());
        _signal_fill_rule_changed.emit(_fill_rule == FillRule::NonZero ? FillRule::EvenOdd : FillRule::NonZero);
    });
    // inherited paint variants
    _inherited.signal_mode_changed().connect([this](auto mode) {
        _signal_inherit_mode_changed.emit(mode);
    });

    _gradient.signal_changed().connect([this](auto gradient) { fire_gradient_changed(gradient, _mode); });
    _gradient.set_margin_top(4);
    _gradient.get_picker().get_color_space_changed().connect([](auto type) {
        store_color_type(type);
    });

    auto& separator = get_widget<Gtk::Separator>(_builder, "separator");
    // this is problematic, but it works: extend the separator
    separator.set_margin_start(-10);
    separator.set_margin_end(-10);
    append(get_widget<Gtk::Box>(_builder, "main"));

    // force height to reveal a list of patterns:
    _pattern.set_name("PatternEditorPopup");
    _pattern.signal_changed().connect([this] { fire_pattern_changed(); });
    _pattern.signal_color_changed().connect([this](auto) { fire_pattern_changed(); });
    _pattern.signal_edit().connect([this] {
        if (_desktop) set_active_tool(_desktop, "Node");
    });
    _pattern.set_margin_top(4);

    _set_mode(PaintMode::None);

    _pages[PaintMode::Solid]    = &_flat_color;
    _pages[PaintMode::Swatch]   = &_swatch;
    _pages[PaintMode::Gradient] = &_gradient;
    _pages[PaintMode::Pattern]  = &_pattern;
    _pages[PaintMode::Hatch]    = &_pattern;
    _pages[PaintMode::Mesh]     = &_mesh;
    _pages[PaintMode::Derived]  = &_inherited;
    for (auto [mode, child] : _pages) {
        if (child && mode != PaintMode::Hatch) {
            _stack.add(*child);
        }
    }

    _color->signal_changed.connect([this] {
        fire_flat_color_changed();
    });
}

void PaintSwitchImpl::switch_paint_mode(PaintMode mode) {
    // fire mode change
    _signal_mode_changed.emit(mode);

    switch (mode) {
    case PaintMode::None:
        break;
    case PaintMode::Solid:
        fire_flat_color_changed();
        break;
    case PaintMode::Pattern:
    case PaintMode::Hatch:
        fire_pattern_changed();
        break;
    case PaintMode::Gradient:
        fire_gradient_changed(nullptr, mode);
        break;
    case PaintMode::Mesh:
        fire_mesh_changed(nullptr);
        break;
    case PaintMode::Swatch:
        //todo: verify: .getGradientSelector()->getVector();
        fire_swatch_changed(_swatch.get_selected_vector(), EditOperation::New, nullptr, {}, {});
        break;
    case PaintMode::Derived:
        break;
    default:
        assert(false);
        break;
    }

    set_mode(mode);
}

void PaintSwitchImpl::set_color(const Colors::Color& color) {
    _flat_color.get_picker().set_color(color);
}

sigc::signal<void (const Colors::Color&)> PaintSwitchImpl::get_flat_color_changed() {
    return _signal_color_changed;
}

sigc::signal<void (PaintMode)> PaintSwitchImpl::get_signal_mode_changed() {
    return _signal_mode_changed;
}

void PaintSwitchImpl::set_mode(PaintMode mode) {
    if (mode == _mode) return;

    _set_mode(mode);
}

void PaintSwitchImpl::_set_mode(PaintMode mode) {
    _mode = mode;
    bool has_color_picker = false;

    // show the corresponding editor page
    if (auto it = _pages.find(mode); it != end(_pages)) {
        _stack.set_visible_child(*it->second);
        // sync plate type buttons with the current page
        auto type = get_plate_type(it->second);
        if (type.has_value()) {
            has_color_picker = true;
            if (auto btn = _plate_buttons[*type]) {
                btn->set_active();
                if (_use_compact_mode) {
                    Glib::ustring icon_name;
                    switch(*type) {
                        case ColorPickerPanel::PlateType::Rect:
                            icon_name = "color-picker-rect";
                            break;
                        case ColorPickerPanel::PlateType::Circle:
                            icon_name = "color-picker-circle";
                            break;
                        case ColorPickerPanel::PlateType::None:
                            icon_name = "color-picker-input";
                            break;
                    }
                    if (!icon_name.empty()) {
                        _menu_btn.set_icon_name(icon_name);
                    }
                }
            }
        }
    }
    if (auto mode_btn = _mode_buttons[mode == PaintMode::Hatch ? PaintMode::Pattern : mode]) {
        mode_btn->set_active();
    }
    // color picker available?
    _plate_type.set_sensitive(has_color_picker);
    if (_use_compact_mode) {
        _menu_btn.set_sensitive(has_color_picker);
    }
}

void PaintSwitchImpl::set_plate_type(ColorPickerPanel::PlateType type) {
    if (auto it = _pages.find(_mode); it != end(_pages)) {
        auto page = it->second;

        if (page == &_flat_color) {
            _flat_color.set_color_picker_plate(type);
        }
        else if (page == &_gradient) {
            _gradient.set_color_picker_plate(type);
        }
        else if (page == &_swatch) {
            _swatch.set_color_picker_plate(type);
        }
    }
}

std::optional<ColorPickerPanel::PlateType> PaintSwitchImpl::get_plate_type(Gtk::Widget* page) const {
    if (page == &_flat_color) {
        return _flat_color.get_color_picker_plate();
    }
    else if (page == &_gradient) {
        return _gradient.get_color_picker_plate();
    }
    else if (page == &_swatch) {
        return _swatch.get_color_picker_plate();
    }

    return {};
}

void PaintSwitchImpl::update_from_paint(const SPIPaint& paint) {
    auto scoped(_update.block());

    if (auto server = paint.isPaintserver() ? paint.href->getObject() : nullptr) {
        if (is<SPGradient>(server) && cast<SPGradient>(server)->getVector()->isSwatch()) {
            // swatch color
            auto vector = cast<SPGradient>(server)->getVector();
            _swatch.select_vector(vector);
        }
        else if (is<SPLinearGradient>(server) || is<SPRadialGradient>(server)) {
            // normal gradient
            auto gradient = cast<SPGradient>(server);
            auto vector = gradient->getVector();
            _gradient.setMode(is<SPLinearGradient>(gradient) ? GradientSelector::MODE_LINEAR : GradientSelector::MODE_RADIAL);
            _gradient.setGradient(gradient);
            _gradient.setVector(vector ? vector->document : nullptr, vector);
            auto stop = cast<SPStop>(const_cast<SPIPaint&>(paint).getTag());
            _gradient.selectStop(stop);
            if (vector) {
                _gradient.setUnits(vector->getUnits());
                _gradient.setSpread(vector->getSpread());
            }
        }
#ifdef WITH_MESH
        else if (is<SPMeshGradient>(server)) {
            // mesh
            auto array = cast<SPGradient>(server)->getArray();
            _mesh.select_mesh(array);
        }
#endif
        else if (is<SPPattern>(server)) {
            // pattern
            _pattern.set_selected(cast<SPPattern>(server));
        }
        else if (is<SPHatch>(server)) {
            // hatch
            _pattern.set_selected(cast<SPHatch>(server));
        }
    }
    else if (auto inherited = get_inherited_paint_mode(paint)) {
        _inherited.set_mode(*inherited);
    }
}

void PaintSwitchImpl::set_fill_rule(FillRule fill_rule) {
    _fill_rule = fill_rule;
    _fill_rule_btn.set_icon_name(fill_rule == FillRule::NonZero ? "fill-rule-nonzero" : "fill-rule-even-odd");
}

std::unique_ptr<PaintSwitch> PaintSwitch::create(bool support_no_paint, bool support_fill_rule, bool compact_mode) {
    return std::make_unique<PaintSwitchImpl>(support_no_paint, support_fill_rule, compact_mode);
}

} // namespace

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
