// SPDX-License-Identifier: GPL-2.0-or-later

#include "paint-attribute.h"

#include <numeric>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <glibmm/ustring.h>
#include <gtkmm/enums.h>
#include <gtkmm/binlayout.h>

#include "desktop-style.h"
#include "document-undo.h"
#include "filter-chemistry.h"
#include "filter-effect-chooser.h"
#include "filter-enums.h"
#include "gradient-chemistry.h"
#include "inkscape.h"
#include "pattern-manager.h"
#include "pattern-manipulation.h"
#include "property-utils.h"
#include "stroke-style.h"
#include "object/sp-gradient.h"
#include "object/sp-hatch.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-flowtext.h"
#include "object/sp-paint-server.h"
#include "object/sp-pattern.h"
#include "object/sp-text.h"
#include "style.h"
#include "actions/actions-tools.h"
#include "colors/spaces/base.h"
#include "colors/spaces/gamut.h"
#include "live_effects/effect.h"
#include "object/sp-namedview.h"
#include "object/sp-stop.h"
#include "svg/css-ostringstream.h"
#include "ui/builder-utils.h"
#include "ui/util.h"
#include "ui/dialog/dialog-container.h"
#include "ui/tools/marker-tool.h"
#include "ui/widget/paint-switch.h"
#include "ui/widget/popover-utils.h"
#include "util/expression-evaluator.h"
#include "xml/attribute-record.h"
#include "xml/sp-css-attr.h"

namespace Inkscape::UI::Widget {

using namespace Inkscape::UI::Utils;

PaintAttribute::PaintAttribute(Parts add_parts, unsigned int tag) :
    _fill(create_builder("paint-strip.ui"), _("Fill"), true, tag),
    _stroke(create_builder("paint-strip.ui"), _("Stroke"), false, tag),
    _builder(create_builder("paint-attribute.ui")),
    _stroke_width(get_widget<InkSpinButton>(_builder, "stroke-width")),
    _markers(get_widget<Gtk::Box>(_builder, "stroke-markers")),
    _blend(get_blendmode_combo_converter(), SPAttr::INVALID, false, "BlendMode"),
    _unit_selector(get_derived_widget<UnitMenu>(_builder, "stroke-unit")),
    _dash_selector(get_derived_widget<DashSelector>(_builder, "stroke-dash-selector", true)),
    _stroke_icons(get_widget<Gtk::Box>(_builder, "stroke-icons")),
    _stroke_presets(get_widget<Gtk::MenuButton>(_builder, "stroke-presets")),
    _stroke_popup(get_widget<Gtk::Popover>(_builder, "stroke-popup")),
    _opacity(get_derived_widget<SpinScale>(_builder, "obj-opacity")),
    _reset_blend(get_widget<Gtk::Button>(_builder, "reset-blend-mode")),
    _visible(get_widget<Gtk::Button>(_builder, "visible-btn")),
    _added_parts(add_parts),
    _modified_tag(tag)
{
    _opacity.set_max_block_count(20); // 100/20 -> 5% increments
    _opacity.set_suffix("%", false);
    _opacity.set_scaling_factor(100.0);

    _marker_start.set_flat(true);
    _marker_mid.set_flat(true);
    _marker_end.set_flat(true);

    _fill._update = &_update;
    _stroke._update = &_update;

    // when stroke fill is toggled (any paint vs. none), change a set of visible widgets
    _stroke._toggle_definition.connect([this](bool defined){
        show_stroke(defined);
    });

    _visible.signal_clicked().connect([this] {
        if (_update.pending() || !_current_item) return;

        bool hide = !_current_item->isExplicitlyHidden();
        _current_item->setExplicitlyHidden(hide);
        DocumentUndo::done(_current_item->document, hide ? RC_("Undo", "Hide object") : RC_("Undo", "Unhide object"), "dialog-object-properties");
    });
}

void PaintAttribute::PaintStrip::hide() {
    _paint_btn.set_visible(false);
    _alpha.set_visible(false);
    _define.set_visible();
    _clear.set_visible(false);
}

void PaintAttribute::PaintStrip::show() {
    _paint_btn.set_visible();
    _alpha.set_visible();
    _define.set_visible(false);
    _clear.set_visible();
}

bool PaintAttribute::PaintStrip::can_update() const {
    return _current_item && _update && !_update->pending();
}

namespace {

void request_item_update(SPObject* item, unsigned int tag) {
    if (!item) return;

    item->updateRepr();
    item->requestModified(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG | tag);
    item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
}

boost::intrusive_ptr<SPCSSAttr> new_css_attr() {
    return boost::intrusive_ptr(sp_repr_css_attr_new(), false);
}

bool is_text(const SPObject* object) {
    return is<SPText>(object) || is<SPFlowtext>(object);
}

// PROOF: the style a text object's fill or stroke shows with. Type whose own paint is unset
// draws with what its spans carry (imported type, or text styled in part); the first span
// that sets the paint then stands for the object, in place of "unset".
const SPStyle* shown_style(const SPObject* object, bool fill) {
    if (!object || !object->style) return nullptr;
    if (!is_text(object) || object->style->getFillOrStroke(fill)->set) return object->style;

    const SPStyle* found = nullptr;
    auto search = [&](auto& self, const SPObject* parent) -> void {
        for (auto& child : parent->children) {
            if (found) return;
            if (child.style && child.style->getFillOrStroke(fill)->set) {
                found = child.style;
            }
            else {
                self(self, &child);
            }
        }
    };
    search(search, object);
    return found ? found : object->style;
}

// PROOF: spans of a text object may set paint of their own (imported type, or text styled
// in part), and then a style set on the object changes nothing that shows. What is set on
// the object is the whole object's: its spans stop setting those properties themselves.
void spans_inherit(SPItem* item, SPCSSAttr* properties) {
    if (!is_text(item)) return;

    auto unset = new_css_attr();
    for (auto const& property : properties->attributeList()) {
        sp_repr_css_unset_property(unset.get(), g_quark_to_string(property.key));
    }
    auto visit = [&](auto& self, SPObject* parent) -> void {
        for (auto& child : parent->children) {
            if (child.getRepr() && child.getRepr()->attribute("style")) {
                child.changeCSS(unset.get(), "style");
            }
            self(self, &child);
        }
    };
    visit(visit, item);
}

void spans_inherit(SPItem* item, std::initializer_list<const char*> properties) {
    auto css = new_css_attr();
    for (auto property : properties) {
        sp_repr_css_set_property(css.get(), property, "");
    }
    spans_inherit(item, css.get());
}

void spans_inherit_paint(SPItem* item, bool fill) {
    if (fill) {
        spans_inherit(item, {"fill", "fill-opacity"});
    }
    else {
        spans_inherit(item, {"stroke", "stroke-opacity"});
    }
}

void set_item_style(SPItem* item, SPCSSAttr* css) {
    double scale = item->i2doc_affine().descrim();
    if (scale != 0 && scale != 1) {
        sp_css_attr_scale(css, 1 / scale);
    }
    item->changeCSS(css, "style");
    spans_inherit(item, css);
}

void set_item_style_str(SPItem* item, const char* attr, const char* value) {
    auto css = new_css_attr();
    sp_repr_css_set_property(css.get(), attr, value);
    set_item_style(item, css.get());
}

void set_item_style_dbl(SPItem* item, const char* attr, double value) {
    CSSOStringStream os;
    os << value;
    set_item_style_str(item, attr, os.str().c_str());
}

void set_stroke_width(SPItem* item, double width_typed, bool hairline, const Unit* unit) {
    auto css = new_css_attr();
    if (hairline) {
        // For renderers that don't understand -inkscape-stroke:hairline, fall back to 1px non-scaling
        width_typed = 1;
        sp_repr_css_set_property(css.get(), "vector-effect", "non-scaling-stroke");
        sp_repr_css_set_property(css.get(), "-inkscape-stroke", "hairline");
    }
    else {
        sp_repr_css_unset_property(css.get(), "vector-effect");
        sp_repr_css_unset_property(css.get(), "-inkscape-stroke");
    }

    double width = calc_scale_line_width(width_typed, item, unit);
    sp_repr_css_set_property_double(css.get(), "stroke-width", width);

    if (Preferences::get()->getBool("/options/dash/scale", true)) {
        // This will read the old stroke-width to unscale the pattern.
        auto [dash, offset] = getDashFromStyle(item->style);
        set_scaled_dash(css.get(), dash.size(), dash.data(), offset, width);
    }
    // item->style->stroke_dasharray.values = ;
    set_item_style(item, css.get());
}

void set_item_marker(SPItem* item, int location, const char* attr, const std::string& uri) {
    set_item_style_str(item, attr, uri.c_str());
    //TODO: verify if any of the below lines are needed
    // item->requestModified(SP_OBJECT_MODIFIED_FLAG);
    // item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
    // needed?
    item->document->ensureUpToDate();
}

void edit_marker(int location, SPDesktop* desktop) {
    if (!desktop) return;

    set_active_tool(desktop, "Marker");
    if (auto marker_tool = dynamic_cast<Tools::MarkerTool*>(desktop->getTool())) {
        marker_tool->editMarkerMode = location;
        marker_tool->selection_changed(desktop->getSelection());
    }
}

std::optional<Colors::Color> get_item_color(SPItem* item, bool fill) {
    if (!item || !item->style) return {};

    auto paint = item->style->getFillOrStroke(fill);
    return paint && paint->isColor() ? std::optional(paint->getColor()) : std::nullopt;
}

void swatch_operation(SPItem* item, SPGradient* vector, SPDesktop* desktop, bool fill, EditOperation operation, SPGradient* replacement, std::optional<Color> color, Glib::ustring label, unsigned int tag) {
    auto kind = fill ? FILL : STROKE;

    switch (operation) {
    case EditOperation::New:
        // try to find an existing swatch with matching color definition:
        if (auto clr = get_item_color(item, fill)) {
            vector = sp_find_matching_swatch(item->document, *clr);
        }
        else {
            // create a new swatch
            vector = nullptr;
        }
        sp_item_apply_gradient(item, vector, desktop, SP_GRADIENT_TYPE_LINEAR, true, kind);
        spans_inherit_paint(item, fill);
        DocumentUndo::done(item->document, fill ? RC_("Undo", "Set swatch on fill") : RC_("Undo", "Set swatch on stroke"), "dialog-fill-and-stroke", tag);
        break;
    case EditOperation::Change:
        if (color.has_value()) {
            sp_change_swatch_color(vector, *color);
            DocumentUndo::maybeDone(item->document, "swatch-color", RC_("Undo", "Change swatch color"), "dialog-fill-and-stroke", tag);
        }
        else {
            sp_item_apply_gradient(item, vector, desktop, SP_GRADIENT_TYPE_LINEAR, true, kind);
            spans_inherit_paint(item, fill);
            DocumentUndo::maybeDone(
                item->document,
                fill ? "fill-swatch-change" : "stroke-swatch-change",
                fill ? RC_("Undo", "Set swatch on fill") : RC_("Undo", "Set swatch on stroke"),
                "dialog-fill-and-stroke",
                tag);
        }
        break;
    case EditOperation::Delete:
        sp_delete_item_swatch(item, kind, vector, replacement);
        DocumentUndo::done(item->document, RC_("Undo", "Delete swatch"), "dialog-fill-and-stroke", tag);
        break;
    case EditOperation::Rename:
        sp_rename_swatch(vector, label);
        DocumentUndo::maybeDone(item->document, "swatch-rename", RC_("Undo", "Rename swatch"), "dialog-fill-and-stroke", tag);
        break;
    default:
        break;
    }
}

} // namespace

// min size of color preview tiles
constexpr int COLOR_TILE = 16;

PaintAttribute::PaintStrip::PaintStrip(Glib::RefPtr<Gtk::Builder> builder, const Glib::ustring& title, bool fill, unsigned int tag) :
    _builder(builder),
    _is_fill(fill),
    _modified_tag(tag),
    _main(get_widget<Gtk::Grid>(builder, "paint-strip")),
    _paint_btn(get_widget<Gtk::MenuButton>(builder, "paint-btn")),
    _color_preview(get_derived_widget<ColorPreview>(builder, "paint-color-preview")),
    _paint_icon(get_widget<Gtk::Image>(builder, "paint-icon-preview")),
    _label(get_widget<Gtk::Label>(builder, "paint-label")),
    _alpha(get_widget<InkSpinButton>(builder, "paint-alpha")),
    _define(get_widget<Gtk::Button>(builder, "paint-add")),
    _clear(get_widget<Gtk::Button>(builder, "paint-clear")),
    _box(get_widget<Gtk::Box>(builder, "paint-buttons")),
    _connection(PaintPopoverManager::get().register_button(_paint_btn, fill,
        [this]() { set_paint(_current_item); },
        [this]() { return connect_signals(); }
    ))
{
    _label.set_text(title);
    _switch = PaintPopoverManager::get().get_switch(fill);
    _paint_btn.set_tooltip_text(fill ?_("Fill paint") : _("Stroke paint"));

    _color_preview.setStyle(ColorPreview::Simple);
    _color_preview.set_frame(true);
    _color_preview.set_border_radius(0);
    _color_preview.set_size_request(COLOR_TILE, COLOR_TILE);
    _color_preview.set_checkerboard_tile_size(4);
    _color_preview.set_halign(Gtk::Align::FILL);
    _color_preview.set_valign(Gtk::Align::CENTER);
    _paint_icon.set_layout_manager(Gtk::BinLayout::create());

    _define.set_tooltip_text(fill ? _("Add fill") : _("Add stroke"));
    _clear.set_tooltip_text(fill ? _("No fill") : _("No stroke"));
    _clear.set_visible(false);

    _clear.signal_clicked().connect([this, fill, tag]() {
        if (!can_update()) return;

        // deleting fill or stroke; remove all related attributes as well
        auto css = new_css_attr();
        if (fill) {
            sp_repr_css_set_property(css.get(), "fill", "none");
            sp_repr_css_unset_property(css.get(), "fill-opacity");
        }
        else {
            for (auto attr : {
                "stroke", "stroke-opacity", "stroke-width", "stroke-miterlimit", "stroke-linejoin",
                "stroke-linecap", "stroke-dashoffset", "stroke-dasharray"}) {
                sp_repr_css_unset_property(css.get(), attr);
            }
            sp_repr_css_set_property(css.get(), "stroke", "none");
        }
        set_item_style(cast<SPItem>(_current_item), css.get());
        request_update(true);

        DocumentUndo::done(_current_item->document, fill ? RC_("Undo", "Remove fill") : RC_("Undo", "Remove stroke"), "dialog-fill-and-stroke", tag);
        // paint removed
        _toggle_definition.emit(false);
    });

    _define.signal_clicked().connect([=,this]() {
        if (!can_update()) return;

        // add fill or stroke
        set_flat_color(Color(0x909090ff));
        // paint defined
        _toggle_definition.emit(true);
    });

    _alpha.signal_value_changed().connect([this, fill, tag](auto alpha) {
        if (!can_update()) return;

        if (fill) {
            _current_item->style->fill_opacity.set_double(alpha);
        }
        else {
            _current_item->style->stroke_opacity.set_double(alpha);
        }
        spans_inherit(_current_item, {fill ? "fill-opacity" : "stroke-opacity"});
        request_update(true);
        //todo: alternative approach to updating fill/stroke opacity:
        /*
        auto css = new_css_attr();
        sp_repr_css_set_property_double(css.get(), fill ? "fill-opacity" : "stroke-opacity", alpha);
        set_item_style(_current_item, css.get());
        */
        DocumentUndo::maybeDone(_current_item->document, fill ? "undo_fill_alpha" : "undo_stroke_alpha",
            fill ? RC_("Undo", "Set fill opacity") : RC_("Undo", "Set stroke opacity"), "dialog-fill-and-stroke", tag);
    });
}

void PaintAttribute::PaintStrip::set_fill_rule(FillRule rule) {
    if (!can_update()) return;

    set_item_style_str(_current_item, "fill-rule", rule == FillRule::EvenOdd ? "evenodd" : "nonzero");
    request_update(true);
    _switch->set_fill_rule(rule);

    DocumentUndo::maybeDone(_current_item->document, "change-fill-rule", RC_("Undo", "Change fill rule"), "dialog-fill-and-stroke");
};

void PaintAttribute::PaintStrip::set_flat_color(const Color& color) {
    if (!can_update()) return;

    auto c = color;
    if (Colors::out_of_gamut(color, color.getSpace())) {
        c = Colors::to_gamut_css(color, color.getSpace());
    }

    //TODO: paint selection should be remembered
    // sp_desktop_set_color(_desktop, color, false, fill);

    c.enableOpacity(false);
    if (_is_fill) {
        _current_item->style->fill.clear();
        _current_item->style->fill.setColor(c);
        _current_item->style->fill_opacity.set_double(color.getOpacity());
    }
    else {
        _current_item->style->stroke.clear();
        _current_item->style->stroke.setColor(c);
        _current_item->style->stroke_opacity.set_double(color.getOpacity());
    }
    spans_inherit_paint(_current_item, _is_fill);
    request_update(true);

    //todo: this is alternative approach to changing style:
    /*
    auto css = new_css_attr();
    auto c = color.toString(false);
    sp_repr_css_set_property_string(css.get(), fill ? "fill" : "stroke", c);//color.toString(false));
    sp_repr_css_set_property_double(css.get(), fill ? "fill-opacity" : "stroke-opacity", color.getOpacity());
    set_item_style(_current_item, css.get());
    */
    DocumentUndo::maybeDone(_current_item->document, _is_fill ? "change-fill" : "change-stroke",
        _is_fill ? RC_("Undo", "Set fill color") : RC_("Undo", "Set stroke color"), "dialog-fill-and-stroke", _modified_tag);
};

std::vector<sigc::connection> PaintAttribute::PaintStrip::connect_signals() {
    std::vector<sigc::connection> conns;
    bool fill = _is_fill;
    unsigned int tag = _modified_tag;

    if (!_switch) return conns;


    conns.push_back(_switch->get_pattern_changed().connect([this, fill, tag](auto pattern, auto color, auto label, auto transform, auto offset, auto uniform, auto gap) {
        if (!can_update()) return;

        if (auto item = cast<SPItem>(_current_item)) {
            auto kind = fill ? FILL : STROKE;
            sp_item_apply_pattern(item, pattern, kind, color, label, transform, offset, uniform, gap);
            spans_inherit_paint(item, fill);
            DocumentUndo::maybeDone(item->document, fill ? "fill-pattern-change" : "stroke-pattern-change", fill ? RC_("Undo", "Set pattern on fill") : RC_("Undo", "Set pattern on stroke"), "dialog-fill-and-stroke", tag);
            update_preview_indicators(_current_item);
            set_paint(_current_item);
        }
    }));

    conns.push_back(_switch->get_hatch_changed().connect([this, fill, tag](auto hatch, auto color, auto label, auto transform, auto offset, auto pitch, auto rotation, auto stroke) {
        if (!can_update()) return;

        if (auto item = cast<SPItem>(_current_item)) {
            auto kind = fill ? FILL : STROKE;
            sp_item_apply_hatch(item, hatch, kind, color, label, transform, offset, pitch, rotation, stroke);
            spans_inherit_paint(item, fill);
            DocumentUndo::maybeDone(item->document, fill ? "fill-pattern-change" : "stroke-pattern-change", fill ? RC_("Undo", "Set pattern on fill") : RC_("Undo", "Set pattern on stroke"), "dialog-fill-and-stroke", tag);
            update_preview_indicators(_current_item);
            set_paint(_current_item);
        }
    }));

    conns.push_back(_switch->get_gradient_changed().connect([this, fill, tag](auto vector, auto gradient_type) {
        if (!can_update()) return;

        if (auto item = cast<SPItem>(_current_item)) {
            auto kind = fill ? FILL : STROKE;
            sp_item_apply_gradient(item, vector, _desktop, gradient_type, false, kind);
            spans_inherit_paint(item, fill);
            DocumentUndo::maybeDone(item->document, fill ? "fill-gradient-change" : "stroke-gradient-change", fill ? RC_("Undo", "Set gradient on fill") : RC_("Undo", "Set gradient on stroke"), "dialog-fill-and-stroke", tag);
            update_preview_indicators(_current_item);
            set_paint(_current_item);
        }
    }));

    conns.push_back(_switch->get_mesh_changed().connect([this, fill, tag](auto mesh) {
        if (!can_update()) return;

        if (auto item = cast<SPItem>(_current_item)) {
            auto kind = fill ? FILL : STROKE;
            sp_item_apply_mesh(item, mesh, _current_item->document, kind);
            spans_inherit_paint(item, fill);
            DocumentUndo::maybeDone(item->document, fill ? "fill-mesh-change" : "stroke-mesh-change", fill ? RC_("Undo", "Set mesh on fill") : RC_("Undo", "Set mesh on stroke"), "dialog-fill-and-stroke", tag);
            update_preview_indicators(_current_item);
            set_paint(_current_item);
        }
    }));

    conns.push_back(_switch->get_swatch_changed().connect([this, fill, tag](auto vector, auto operation, auto replacement, std::optional<Color> color, auto label) {
        if (!can_update()) return;

        if (auto item = cast<SPItem>(_current_item)) {
            swatch_operation(item, vector, _desktop, fill, operation, replacement, color, label, _modified_tag);
            update_preview_indicators(_current_item);
            set_paint(_current_item);
        }
    }));

    conns.push_back(_switch->get_flat_color_changed().connect([=,this](auto& color) {
        set_flat_color(color);
    }));

    conns.push_back(_switch->get_fill_rule_changed().connect([=,this](auto fill_rule) {
        set_fill_rule(fill_rule);
    }));

    conns.push_back(_switch->get_inherit_mode_changed().connect([=,this](auto mode) {
        if (!can_update()) return;

        auto css = new_css_attr();
        auto attr = fill ? "fill" : "stroke";
        switch (mode) {
        case PaintDerivedMode::Unset:
            sp_repr_css_unset_property(css.get(), attr);
            break;
        case PaintDerivedMode::Inherit:
            sp_repr_css_set_property(css.get(), attr, "inherit");
            break;
        case PaintDerivedMode::ContextFill:
            sp_repr_css_set_property(css.get(), attr, "context-fill");
            break;
        case PaintDerivedMode::ContextStroke:
            sp_repr_css_set_property(css.get(), attr, "context-stroke");
            break;
        case PaintDerivedMode::CurrentColor:
            sp_repr_css_set_property(css.get(), attr, "currentColor");
            break;
        default:
            g_warning("Unknown PaintUnsetMode");
            break;
        }
        set_item_style(cast<SPItem>(_current_item), css.get());
        DocumentUndo::done(_current_item->document,  fill ? RC_("Undo", "Inherit fill") : RC_("Undo", "Inherit stroke"), "dialog-fill-and-stroke", tag);
        update_preview_indicators(_current_item);
    }));

    conns.push_back(_switch->get_signal_mode_changed().connect([this, fill, tag](auto mode) {
        if (!can_update()) return;

        if (mode == PaintMode::Derived) {
            auto css = new_css_attr();
            sp_repr_css_unset_property(css.get(), fill ? "fill" : "stroke");
            set_item_style(cast<SPItem>(_current_item), css.get());
            DocumentUndo::done(_current_item->document,  fill ? RC_("Undo", "Unset fill") : RC_("Undo", "Unset stroke"), "dialog-fill-and-stroke", tag);
            if (auto paint = _current_item->style->getFillOrStroke(fill)) {
                _switch->update_from_paint(*paint);
            }
            update_preview_indicators(_current_item);
        }
    }));
    return conns;
}

void PaintAttribute::PaintStrip::request_update(bool update_preview) {
    if (!_current_item) return;

    request_item_update(_current_item, _modified_tag);

    if (update_preview) {
        update_preview_indicators(_current_item);
    }
}

// set the correct icon for the current fill / stroke type
void PaintAttribute::PaintStrip::set_preview(const SPIPaint& paint, double paint_opacity, PaintMode mode) {
    if (mode == PaintMode::None) {
        hide();
        return;
    }

    // in the simple solid case, make it more clear we are editing the color alpha here, otherwise it's opacity
    if (mode == PaintMode::Solid) {
        _alpha.set_icon("");
        _alpha.set_label(C_("Short for 'Alpha'", "A"));
        _alpha.set_tooltip_text(_("Alpha"));
    } else {
        _alpha.set_label("");
        _alpha.set_icon("opacity");
        _alpha.set_tooltip_text(_("Opacity"));
    }

    if (mode == PaintMode::Solid || mode == PaintMode::Swatch || mode == PaintMode::Gradient || mode == PaintMode::Pattern || mode == PaintMode::Hatch) {
        _alpha.set_value(paint_opacity);
        _paint_icon.set_visible(false);
        _color_preview.set_visible();

        if (mode == PaintMode::Solid) {
            auto color = paint.getColor();
            color.setOpacity(paint_opacity);
            _color_preview.setRgba32(color.toRGBA());
            _color_preview.setIndicator(ColorPreview::None);
        }
        else if (mode == PaintMode::Swatch) {
            // swatch
            auto server = paint.href->getObject();
            auto swatch = cast<SPGradient>(server);
            assert(swatch);
            auto vect = swatch->getVector();
            auto color = paint.getColor();
            if (auto stop = vect->getFirstStop()) {
                // swatch color is in the first (and only) stop
                color = stop->getColor();
            }
            color.setOpacity(paint_opacity);
            _color_preview.setRgba32(color.toRGBA());
            _color_preview.setIndicator(ColorPreview::Swatch);
        }
        else if (mode == PaintMode::Pattern || mode == PaintMode::Hatch) {
            // patterns and hatches
            auto server = cast<SPPaintServer>(paint.href->getObject());
            unsigned int background = 0xffffffff; // use white background for patterns
            // create a pattern preview with arbitrarily selected width
            auto surface = PatternManager::get().get_preview(server, 200, COLOR_TILE, background, _color_preview.get_scale_factor());
            auto pat = Cairo::SurfacePattern::create(surface);
            pat->set_extend(Cairo::Pattern::Extend::REPEAT);
            _color_preview.setPattern(pat);
            _color_preview.setIndicator(ColorPreview::None);
        }
        else {
            // gradients
            auto server = cast<SPGradient>(paint.href->getObject());
            std::vector<ColorPreview::GradientStops> gradient;
            server->ensureVector();
            for (auto& stop : server->vector.stops) {
                if (stop.color.has_value()) {
                    double opacity = 1;
                    auto c = stop.color->toRGBA(opacity);
                    gradient.push_back({stop.offset, SP_RGBA32_R_F(c), SP_RGBA32_G_F(c), SP_RGBA32_B_F(c), SP_RGBA32_A_F(c)});
                }
            }
            _paint_icon.set_from_icon_name(is<SPRadialGradient>(server) ? "paint-gradient-radial" : "paint-gradient-linear");
            _paint_icon.set_visible();
            _color_preview.set_gradient(gradient);
            _color_preview.setIndicator(ColorPreview::None);
        }
        show();
    }
    else {
        auto icon = get_paint_mode_icon(mode);
        _paint_icon.set_from_icon_name(icon);
        _paint_icon.set_visible();
        _color_preview.set_visible(false);
        show();
    }
}

PaintMode PaintAttribute::PaintStrip::update_preview_indicators(const SPObject* object) {
    if (!object || !object->style) return PaintMode::None;

    auto style = shown_style(object, _is_fill);
    auto& paint = *style->getFillOrStroke(_is_fill);
    auto mode = get_mode_from_paint(paint);
    auto opacity = _is_fill ? style->fill_opacity.as_double() : style->stroke_opacity.as_double();
    set_preview(paint, opacity, mode);
    return mode;
}

void PaintAttribute::PaintStrip::set_paint(const SPObject* object) {
    if (!object || !object->style) return;

    auto style = shown_style(object, _is_fill);
    if (_is_fill) {
        if (auto fill = style->getFillOrStroke(true)) {
            auto fill_rule = style->fill_rule.computed == SP_WIND_RULE_NONZERO ? FillRule::NonZero : FillRule::EvenOdd;
            set_paint(*fill, style->fill_opacity.as_double(), fill_rule);
        }
    }
    else {
        if (auto stroke = style->getFillOrStroke(false)) {
            set_paint(*stroke, style->stroke_opacity.as_double(), FillRule::NonZero);
        }
    }
}

void PaintAttribute::PaintStrip::set_paint(const SPIPaint& paint, double opacity, FillRule fill_rule) {
    auto scoped(_update->block());

    auto mode = get_mode_from_paint(paint);
    _switch->set_mode(mode);
    if (paint.isColor()) {
        auto color = paint.getColor();
        color.setOpacity(opacity);
        _switch->set_color(color);
    }
    _switch->update_from_paint(paint);
    _switch->set_fill_rule(fill_rule);
}

void PaintAttribute::insert_widgets(InkPropertyGrid& grid) {
    _markers.append(_marker_start);
    _markers.append(_marker_mid);
    _markers.append(_marker_end);

    auto set_marker = [this](int location, const char* id, const std::string& uri) {
        if (!can_update()) return;

        set_item_marker(_current_item, location, id, uri);
        DocumentUndo::maybeDone(_current_item->document, "marker-change", RC_("Undo", "Set marker"), "dialog-fill-and-stroke", _modified_tag);
    };

    for (auto combo : {&_marker_start, &_marker_mid, &_marker_end}) {
        combo->connect_changed([=] {
            if (!combo->in_update()) {
                set_marker(combo->get_loc(), combo->get_id(), combo->get_active_marker_uri());
            }
        });

        // request to edit the current marker on the canvas
        combo->connect_edit([this,combo] { edit_marker(combo->get_loc(), _desktop); });
    }

    //TODO: unit-specific adj?
    _stroke_width.set_evaluator_function([this](auto& text) {
        auto unit = _unit_selector.getUnit();
        auto result = ExpressionEvaluator(text.c_str(), unit).evaluate();
        // check if the output dimension corresponds to the input unit
        if (result.dimension != (unit->isAbsolute() ? 1 : 0) ) {
            throw EvaluatorException("Input dimensions do not match with parameter dimensions.", "");
        }
        return result.value;
    });
    auto set_stroke = [this](double width) {
        if (!can_update()) return;

        auto scoped(_update.block());
        auto hairline = _unit_selector.get_selected() == _hairline_item;
        auto unit = _unit_selector.getUnit();
        set_stroke_width(_current_item, width, hairline, unit);
        update_stroke(_current_item);
        DocumentUndo::maybeDone(_current_item->document, "set-stroke-width", RC_("Undo", "Set stroke width"), "dialog-fill-and-stroke", _modified_tag);
    };
    auto set_stroke_unit = [=,this] {
        if (!can_update()) return;

        auto new_unit = _unit_selector.getUnit();
        if (new_unit == _current_unit) return;

        auto hairline = _unit_selector.get_selected() == _hairline_item;
        auto width = _stroke_width.get_value();
        if (hairline) {
            auto scoped(_update.block());
            _current_unit = new_unit;
            set_stroke_width(_current_item, 1, hairline, new_unit);
            DocumentUndo::maybeDone(_current_item->document, "set-stroke-unit", RC_("Undo", "Set stroke unit"), "dialog-fill-and-stroke", _modified_tag);
        }
        else {
            // if the current unit is empty, then it's a hairline, b/c it's not in a unit table
            if (_current_unit->abbr.empty()) {
                _current_unit = UnitTable::get().getUnit("px");
            }
            width = Quantity::convert(width, _current_unit, new_unit);
            _current_unit = new_unit;
            {
                auto scoped(_update.block());
                _stroke_width.set_value(width);
            }
            set_stroke(width);
        }
        update_stroke(_current_item);
    };
    auto set_stroke_style = [this](const char* attr, const char* value) {
        if (!can_update()) return;

        auto scoped(_update.block());
        set_item_style_str(_current_item, attr, value);
        DocumentUndo::maybeDone(_current_item->document, "set-stroke-style", RC_("Undo", "Set stroke style"), "dialog-fill-and-stroke", _modified_tag);
        update_stroke(_current_item);
    };
    auto set_stroke_miter_limit = [this](double limit) {
        if (!can_update()) return;

        auto scoped(_update.block());
        set_item_style_dbl(_current_item, "stroke-miterlimit", limit);
        DocumentUndo::maybeDone(_current_item->document, "set-stroke-miter-limit", RC_("Undo", "Set stroke miter"), "dialog-fill-and-stroke", _modified_tag);
    };
    _stroke_width.signal_value_changed().connect([=,this](auto value) {
        set_stroke(value);
    });
    _unit_selector.setUnitType(UNIT_TYPE_LINEAR);
    _hairline_item = _unit_selector.append(_("Hairline"));
    _unit_selector.signal_changed().connect([=,this] {
        set_stroke_unit();
    });
    _stroke_popup.set_child(_stroke_options);
    _stroke_options._join_changed.connect([=](auto style) {
        set_stroke_style("stroke-linejoin", style);
    });
    _stroke_options._cap_changed.connect([=](auto style) {
        set_stroke_style("stroke-linecap", style);
    });
    _stroke_options._order_changed.connect([=](auto style) {
        set_stroke_style("paint-order", style);
    });
    _stroke_options._miter_changed.connect([=](auto value) {
        set_stroke_miter_limit(value);
    });

    if (_added_parts & FillPaint) {
        reparent_properties(_fill._main, grid);
        _stroke_widgets.add(grid.add_gap());
    }
    if (_added_parts & StrokePaint) {
        reparent_properties(_stroke._main, grid);
    }
    if (_added_parts & StrokeAttributes) {
        _stroke_widgets.add(reparent_properties(get_widget<Gtk::Grid>(_builder, "stroke-attributes"), grid));
        _stroke_widgets.add(grid.add_gap());
    }

    auto set_dash = [this](bool pattern_edit) {
        if (!can_update()) return;

        auto scoped(_update.block());
        auto item = _current_item;
        auto& dash = pattern_edit ? _dash_selector.get_custom_dash_pattern() : _dash_selector.get_dash_pattern();
        auto offset = _dash_selector.get_offset();
        double scale = item->i2doc_affine().descrim();
        if (Preferences::get()->getBool("/options/dash/scale", true)) {
            scale = item->style->stroke_width.computed * scale;
        }
        auto css = new_css_attr();
        set_scaled_dash(css.get(), dash.size(), dash.data(), offset, scale);
        set_item_style(item, css.get());
        _stroke.request_update(false);
        // update menu selection if the user edits a dash pattern
        auto [vec, offset2] = getDashFromStyle(item->style);
        _dash_selector.set_dash_pattern(vec, offset2);
        DocumentUndo::maybeDone(_current_item->document, "set-dash-pattern", RC_("Undo", "Set stroke dash pattern"), "dialog-fill-and-stroke", _modified_tag);
    };
    _dash_selector.changed_signal.connect([=](auto change) {
        set_dash(change == DashSelector::Pattern);
    });

    if (_added_parts & Opacity) {
        reparent_properties(get_widget<Gtk::Grid>(_builder, "opacity-box"), grid);
    }
    if (_added_parts & BlendMode) {
        _blend.set_hexpand();
        get_widget<Gtk::Box>(_builder, "blend-mode").append(_blend);
        reparent_properties(get_widget<Gtk::Grid>(_builder, "blend-box"), grid);
    }

    auto set_object_opacity = [this](double opacity, bool clear) {
        if (!can_update()) return;

        auto item = _current_item;
        auto scoped(_update.block());
        if (clear) {
            item->style->opacity.clear();
            _opacity.set_value(item->style->opacity.as_double());
        }
        else {
            item->style->opacity.set_double(opacity);
        }
        update_reset_opacity_button();
        request_item_update(item, _modified_tag);
        DocumentUndo::done(item->document, clear ? RC_("Undo", "Clear opacity") : RC_("Undo", "Set opacity"), "dialog-fill-and-stroke", _modified_tag);
    };
    _opacity.signal_value_changed().connect([=,this](auto value){ set_object_opacity(value, false); });
    //TODO: there's no place for opacity reset button, so it's been removed
    // _reset_opacity.signal_clicked().connect([=,this] { set_object_opacity(1.0, true); });

    auto set_blend_mode = [=,this](SPBlendMode mode, bool clear) {
        if (!can_update()) return;

        auto scoped(_update.block());
        if ( clear && ::clear_blend_mode(_current_item) ||
            !clear && ::set_blend_mode(_current_item, mode)) {

            if (clear) {
                _blend.set_active_by_id(SP_CSS_BLEND_NORMAL);
            }
            update_reset_blend_button();
            DocumentUndo::done(_current_item->document, clear ? RC_("Undo", "Clear blending mode") : RC_("Undo", "Set blending mode"), "dialog-fill-and-stroke", _modified_tag);
        }
    };
    _blend.signal_changed().connect([=,this] {
        if (auto id = _blend.get_selected_id()) {
            set_blend_mode(*id, false);
        }
    });
    _reset_blend.signal_clicked().connect([=,this] {
        set_blend_mode(SP_CSS_BLEND_NORMAL, true);
    });
}

void PaintAttribute::set_document(SPDocument* document) {
    for (auto combo : {&_marker_start, &_marker_mid, &_marker_end}) {
        combo->setDocument(document);
    }
    if (_fill._switch) _fill._switch->set_document(document);
    if (_stroke._switch) _stroke._switch->set_document(document);
}

void PaintAttribute::set_desktop(SPDesktop* desktop) {
    if (_desktop != desktop && desktop) {
        auto unit = desktop->getNamedView()->display_units;
        if (unit != _unit_selector.getUnit()) {
            auto scoped(_update.block());
            _unit_selector.setUnit(unit->abbr);
        }
        _current_unit = unit;
    }
    _desktop = desktop;
    if (_fill._switch) _fill._switch->set_desktop(desktop);
    if (_stroke._switch) _stroke._switch->set_desktop(desktop);
}

void PaintAttribute::set_paint(const SPObject* object, bool fill) {
    auto& strip = fill ? _fill : _stroke;
    strip.set_paint(object);
}

void PaintAttribute::update_markers(SPIMarker* markers[], SPObject* object) {
    for (auto combo : {&_marker_start, &_marker_mid, &_marker_end}) {
        if (combo->in_update()) continue;

        SPObject* marker = nullptr;
        if (auto value = markers[combo->get_loc()]->value()) {
            marker = getMarkerObj(value, object->document);
        }
        combo->setDocument(object->document);
        combo->set_current(marker);
    }
}

void PaintAttribute::show_stroke(bool show) {
    _stroke_widgets.set_visible(show);
}

// stroke update from element to UI
void PaintAttribute::update_stroke(SPItem* item) {
    if (!item || !item->style) return;

    SPStyle* style = item->style;
    if (style->stroke_extensions.hairline) {
        _stroke_width.set_sensitive(false);
        _stroke_width.set_value(1);
        //todo: F&S dialog disables dash selector; should it be?
        _dash_selector.set_sensitive(false);
        _stroke_presets.set_sensitive(false);
        _markers.set_sensitive(false);
        _unit_selector.set_selected(_hairline_item);
    }
    else {
        if (_unit_selector.get_selected() == _hairline_item) {
            auto unit = _desktop->getNamedView()->display_units;
            _unit_selector.setUnit(unit->abbr);
        }
        auto unit = _unit_selector.getUnit();
        auto i2dt = item->i2dt_affine();
        double width = style->stroke_width.computed * i2dt.descrim();
        if (!std::isnan(width)) {
            width = Quantity::convert(width, "px", unit);
            _stroke_width.set_value(width);
            _stroke_width.set_sensitive();
            _dash_selector.set_sensitive();
            _stroke_presets.set_sensitive();
            _markers.set_sensitive();
        }
    }

    auto [vec, offset] = getDashFromStyle(style);
    _dash_selector.set_dash_pattern(vec, offset);

    // stroke options - update icons only
    auto icons = _stroke_icons.get_children();
    Glib::ustring name;

    auto join = style->stroke_linejoin.value;
    name = "stroke-join-miter";
    if (join == SP_STROKE_LINEJOIN_BEVEL) {
        name = "stroke-join-bevel";
    }
    else if (join == SP_STROKE_LINEJOIN_ROUND) {
        name = "stroke-join-round";
    }
    dynamic_cast<Gtk::Image*>(icons.at(0))->set_from_icon_name(name);
    auto cap = style->stroke_linecap.value;
    name = "stroke-cap-butt";
    if (cap == SP_STROKE_LINECAP_SQUARE) {
        name = "stroke-cap-square";
    }
    else if (cap == SP_STROKE_LINECAP_ROUND) {
        name = "stroke-cap-round";
    }
    dynamic_cast<Gtk::Image*>(icons.at(1))->set_from_icon_name(name);
    SPIPaintOrder order;
    order.read(style->paint_order.set ? style->paint_order.value : "normal");
    name = "paint-order-fsm"; // "normal" order
    if (order.layer[0] != SP_CSS_PAINT_ORDER_NORMAL) {
        if (order.layer[0] == SP_CSS_PAINT_ORDER_FILL) {
            if (order.layer[1] == SP_CSS_PAINT_ORDER_STROKE) {
                name = "paint-order-fsm";
            }
            else {
                name = "paint-order-fms";
            }
        }
        else if (order.layer[0] == SP_CSS_PAINT_ORDER_STROKE) {
            if (order.layer[1] == SP_CSS_PAINT_ORDER_FILL) {
                name = "paint-order-sfm";
            }
            else {
                name = "paint-order-smf";
            }
        }
        else {
            if (order.layer[1] == SP_CSS_PAINT_ORDER_STROKE) {
                name = "paint-order-msf";
            }
            else {
                name = "paint-order-mfs";
            }
        }
    }
    dynamic_cast<Gtk::Image*>(icons.at(2))->set_from_icon_name(name);
}

bool PaintAttribute::can_update() const {
    return _current_item && _current_item->style && !_update.pending();
}

void PaintAttribute::update_reset_opacity_button() {
    if (!_current_item || !_current_item->style) return;

    auto& opacity = _current_item->style->opacity;
    // no reset btn available
    //TODO: find place for reset btn
    // _reset_opacity.set_visible(opacity.inherit || (opacity.set && static_cast<double>(opacity) < 1.0));
}

void PaintAttribute::update_reset_blend_button() {
    if (!_current_item || !_current_item->style) return;

    auto blend_mode = _current_item->style->mix_blend_mode.set ? _current_item->style->mix_blend_mode.value : SP_CSS_BLEND_NORMAL;
    _reset_blend.set_visible(blend_mode != SP_CSS_BLEND_NORMAL);
}

void PaintAttribute::update_from_object(SPObject* object) {
    if (_update.pending()) return;

    auto scoped(_update.block());

    _current_object = object;
    _current_item = cast<SPItem>(object);
    _fill._current_item = _current_item;
    _stroke._current_item = _current_item;
    _fill._desktop = _desktop;
    _stroke._desktop = _desktop;

    if (!_current_object || !_current_object->style) {
        // hide
        _fill.hide();
        _stroke.hide();
        //todo: reset document in marker combo?
    }
    else {
        auto& style = object->style;
        _fill.update_preview_indicators(object);
        if (auto pop = _fill._paint_btn.get_popover(); pop && pop->is_visible()) {
            set_paint(_current_object, true);
        }

        auto stroke_mode = _stroke.update_preview_indicators(object);
        if (auto pop = _stroke._paint_btn.get_popover(); pop && pop->is_visible()) {
            set_paint(_current_object, false);
        }
        update_stroke(_current_item);
        update_markers(style->marker_ptrs, object);
        if (stroke_mode != PaintMode::None) {
            _stroke_options.update_widgets(*style);
            show_stroke(true);
        }
        else {
            show_stroke(false);
        }

        double opacity = style->opacity.as_double();
        _opacity.set_value(opacity);
        update_reset_opacity_button();

        auto blend_mode = style->mix_blend_mode.set ? style->mix_blend_mode.value : SP_CSS_BLEND_NORMAL;
        _blend.set_active_by_id(blend_mode);
        update_reset_blend_button();
    }
}

void PaintAttribute::update_visibility(SPObject* object) {
    bool show = false;
    if (auto item = cast<SPItem>(object)) {
        show = true;
        _visible.set_icon_name(item->isExplicitlyHidden() ? "object-hidden" : "object-visible");
    }
    // don't hide buttons, it shifts everything
    _visible.set_opacity(show ? 1 : 0);
    _visible.set_sensitive(show);
}

} // namespace
