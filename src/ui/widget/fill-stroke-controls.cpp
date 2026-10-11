// SPDX-License-Identifier: GPL-2.0-or-later
#include "fill-stroke-controls.h"

#include <cmath>
#include <algorithm>
#include <glibmm/i18n.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/popover.h>
#include <gtkmm/stylecontext.h>

#include "desktop.h"
#include "desktop-style.h"
#include "colors/document-colors.h"
#include "document-undo.h"
#include "object/sp-gradient.h"
#include "object/sp-item-group.h"
#include "selection.h"
#include "style.h"
#include "svg/css-ostringstream.h"
#include "ui/controller.h"
#include "ui/icon-names.h"
#include "ui/widget/paint-switch.h"
#include "ui/widget/selection-paint.h"
#include "util/units.h"

namespace Inkscape::UI::Widget {
namespace {
// A tile is as large as a tool's icon and the pair a little larger than one tool, as in
// Illustrator (Illustrator_reference/size_reference.png: 18 px tiles, 28 px the pair).
constexpr int tile = 20;
constexpr int overlap = 10;  // how far the stroke's tile lies down and right of the fill's
constexpr double corner = 3; // a tile's corner radius
constexpr double band = 5;   // width of the stroke tile's ring
unsigned paint_session = 0;

void rounded(Cairo::RefPtr<Cairo::Context> const &cr, double x, double y, double w, double h, double r)
{
    cr->begin_new_sub_path();
    cr->arc(x + w - r, y + r, r, -M_PI_2, 0);
    cr->arc(x + w - r, y + h - r, r, 0, M_PI_2);
    cr->arc(x + r, y + h - r, r, M_PI_2, M_PI);
    cr->arc(x + r, y + r, r, M_PI, 3 * M_PI_2);
    cr->close_path();
}

void rgba(Cairo::RefPtr<Cairo::Context> const &cr, uint32_t c)
{
    cr->set_source_rgba((c >> 24) / 255.0, ((c >> 16) & 255) / 255.0,
                        ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}
} // namespace

FillStrokeControls::FillStrokeControls() : Gtk::Box(Gtk::Orientation::VERTICAL, 3)
{
    set_name("FillStrokeControls");
    add_css_class("proof-paint-controls");
    set_halign(Gtk::Align::CENTER);
    set_margin(3);

    // Scope this to the sidebar: the other tool buttons retain their own sizes.
    static auto css = Gtk::CssProvider::create();
    static bool styled = false;
    if (!styled) {
        css->load_from_data(
            ".proof-paint-controls button { padding: 0; min-width: 0; min-height: 0; }"
            ".proof-paint-controls menubutton > button { background: none; border: none; box-shadow: none; }");
        Gtk::StyleContext::add_provider_for_display(get_display(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
        styled = true;
    }

    _targets.set_size_request(tile + overlap, tile + overlap);
    _targets.set_halign(Gtk::Align::CENTER); // The row of commands under it is wider.
    append(_targets);
    _fill.set_name("FillPaint");
    _stroke.set_name("StrokePaint");
    _fill.set_child(_fill_preview);
    _stroke.set_child(_stroke_preview);
    for (auto area : {&_fill_preview, &_stroke_preview}) area->set_size_request(tile, tile);
    _fill_preview.set_draw_func([this](auto const &cr, int w, int h) { draw(cr, w, h, true); });
    _stroke_preview.set_draw_func([this](auto const &cr, int w, int h) { draw(cr, w, h, false); });
    _targets.put(_stroke, overlap, overlap);
    _targets.put(_fill, 0, 0);
    _fill.set_tooltip_text(_("Fill: click to activate, double-click to edit (X switches target)"));
    _stroke.set_tooltip_text(_("Stroke: click to activate, double-click to edit (X switches target)"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(_fill.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Fill paint"), -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(_stroke.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Stroke paint"), -1);

    for (bool fill : {true, false}) {
        auto &button = fill ? _fill : _stroke;
        auto click = Gtk::GestureClick::create();
        click->set_button(1);
        click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        click->signal_pressed().connect(Controller::use_state(
            [this, fill, &button](auto const &, int n, double, double) {
                setActive(fill);
                if (n == 2) button.popup();
                return Gtk::EventSequenceState::CLAIMED;
            }, *click), true);
        button.add_controller(click);
    }

    auto &manager = PaintPopoverManager::get();
    _fill_registration.emplace(manager.register_button(_fill, true,
        [this] { setupPaint(true); }, [this] { return connectPaint(true); }));
    _stroke_registration.emplace(manager.register_button(_stroke, false,
        [this] { setupPaint(false); }, [this] { return connectPaint(false); }));

    _defaults.set_icon_name("object-fill");
    _swap.set_icon_name("object-flip-horizontal");
    _none.set_icon_name("paint-none");
    _defaults.set_tooltip_text(_("Default fill and stroke (D)"));
    _swap.set_tooltip_text(_("Swap fill and stroke (Shift+X)"));
    _none.set_tooltip_text(_("No paint on the active target (/)"));
    for (auto button : {&_defaults, &_swap, &_none}) {
        button->set_has_frame(false);
        button->set_size_request(16, 20);
        _commands.append(*button);
    }
    _defaults.signal_clicked().connect(sigc::mem_fun(*this, &FillStrokeControls::setDefaults));
    _swap.signal_clicked().connect(sigc::mem_fun(*this, &FillStrokeControls::swapPaint));
    _none.signal_clicked().connect(sigc::mem_fun(*this, &FillStrokeControls::setNone));
    append(_commands);
    set_sensitive(false);
}

FillStrokeControls::~FillStrokeControls()
{
    setDesktop(nullptr);
    _fill_registration.reset();
    _stroke_registration.reset();
}

void FillStrokeControls::setDesktop(SPDesktop *desktop)
{
    if (_desktop == desktop) return;
    closePopovers();
    _selection_changed.disconnect();
    _selection_modified.disconnect();
    _document_changed.disconnect();
    _desktop_destroyed.disconnect();
    _style_changed.disconnect();
    _desktop = desktop;
    set_sensitive(_desktop != nullptr);
    if (!_desktop) return;
    _desktop_destroyed = _desktop->connectDestroy([this](auto) { setDesktop(nullptr); });
    _document_changed = _desktop->connectDocumentReplaced([this](auto, auto) {
        closePopovers();
        bindSelection();
    });
    _style_changed = _desktop->connectSetStyle([this](auto, auto) {
        if (_desktop->getSelection()->isEmpty()) refresh();
        return false; // Let the canvas/tool handle the style change.
    });
    bindSelection();
}

void FillStrokeControls::bindSelection()
{
    _selection_changed.disconnect();
    _selection_modified.disconnect();
    auto selection = _desktop->getSelection();
    _selection_changed = selection->connectChanged([this](auto) { closePopovers(); refresh(); });
    _selection_modified = selection->connectModified([this](auto, unsigned flags) {
        if (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) refresh();
    });
    refresh();
}

void FillStrokeControls::closePopovers()
{
    ++_context; // Pending focus-out commits must not reach a new document/selection.
    auto &manager = PaintPopoverManager::get();
    for (bool fill : {true, false}) {
        auto &registration = fill ? _fill_registration : _stroke_registration;
        if (!registration) continue;
        // get_popover is lazy; avoid creating an editor until it is actually used.
        auto &button = fill ? _fill : _stroke;
        if (auto popover = button.get_popover()) {
            popover->popdown();
            manager.get_switch(fill)->set_desktop(nullptr);
            manager.get_switch(fill)->set_document(nullptr);
        }
    }
}

void FillStrokeControls::setActive(bool fill)
{
    if (_fill_active == fill) return;
    closePopovers();
    _fill_active = fill;
    // The active target is in front, just like the two paint controls in Illustrator.
    auto &button = fill ? _fill : _stroke;
    // Reorder within the same parent. Unparenting on the first press cancels
    // its gesture, so a double-click on an inactive target never reaches n == 2.
    button.insert_after(_targets, fill ? _stroke : _fill);
    _fill_preview.queue_draw();
    _stroke_preview.queue_draw();
}

bool FillStrokeControls::queryStyle(SPStyle &style, bool fill) const
{
    if (!_desktop) return false;
    if (_desktop->getSelection()->isEmpty()) {
        style.mergeCSS(_desktop->current);
        return true;
    }
    auto property = fill ? QUERY_STYLE_PROPERTY_FILL : QUERY_STYLE_PROPERTY_STROKE;
    auto items = _desktop->getSelection()->items_vector();
    auto result = sp_desktop_query_style(_desktop, &style, property);
    if (std::any_of(items.begin(), items.end(), [](auto item) { return is<SPGroup>(item); })) {
        std::vector<SPItem *> leaves;
        auto gather = [&leaves](auto &self, SPItem *item) -> void {
            if (auto group = cast<SPGroup>(item)) {
                for (auto &child : group->children) {
                    if (auto item = cast<SPItem>(&child)) self(self, item);
                }
            } else leaves.push_back(item);
        };
        for (auto item : items) gather(gather, item);
        result = sp_desktop_query_style_from_list(leaves, &style, property);
    }
    return result == QUERY_STYLE_SINGLE || result == QUERY_STYLE_MULTIPLE_SAME;
}

void FillStrokeControls::refresh()
{
    if (!_desktop) return;
    for (bool fill : {true, false}) {
        auto &preview = _preview[fill ? 0 : 1];
        preview.stops.clear();
        SPStyle style(_desktop->getDocument());
        if (!queryStyle(style, fill)) {
            preview.kind = Preview::Kind::Mixed;
        } else {
            auto &paint = *style.getFillOrStroke(fill);
            auto opacity = fill ? style.fill_opacity.as_double() : style.stroke_opacity.as_double();
            if (paint.isNone()) {
                preview.kind = Preview::Kind::None;
            } else if (paint.isColor()) {
                preview.kind = Preview::Kind::Solid;
                preview.rgba = paint.getColor().toRGBA(opacity);
            } else if (paint.isPaintserver() && is<SPGradient>(paint.href->getObject())) {
                preview.kind = Preview::Kind::Gradient;
                auto gradient = cast<SPGradient>(paint.href->getObject());
                gradient->ensureVector();
                for (auto const &stop : gradient->vector.stops) {
                    if (stop.color) preview.stops.emplace_back(stop.offset, stop.color->toRGBA(opacity));
                }
            } else {
                preview.kind = Preview::Kind::Other;
            }
        }
    }
    _fill_preview.queue_draw();
    _stroke_preview.queue_draw();
}

void FillStrokeControls::setupPaint(bool fill)
{
    setActive(fill);
    ++_context;
    auto paint_switch = PaintPopoverManager::get().get_switch(fill);
    paint_switch->set_desktop(_desktop);
    paint_switch->set_document(_desktop ? _desktop->getDocument() : nullptr);
    paint_switch->set_sensitive(_desktop != nullptr);
    if (!_desktop) return;
    SPStyle style(_desktop->getDocument());
    if (!queryStyle(style, fill)) {
        paint_switch->show_placeholder(_("Multiple styles"), true);
        return;
    }
    auto &paint = *style.getFillOrStroke(fill);
    paint_switch->set_mode(get_mode_from_paint(paint));
    paint_switch->update_from_paint(paint);
    if (paint.isColor()) {
        auto color = paint.getColor(); // Preserve its CMYK/ICC/spot space and channel values.
        color.setOpacity(fill ? style.fill_opacity.as_double() : style.stroke_opacity.as_double());
        paint_switch->set_color(color);
    }
    if (fill) paint_switch->set_fill_rule(style.fill_rule.computed == SP_WIND_RULE_EVENODD
        ? FillRule::EvenOdd : FillRule::NonZero);
}

std::vector<sigc::connection> FillStrokeControls::connectPaint(bool fill)
{
    auto desktop = _desktop;
    auto document = desktop ? desktop->getDocument() : nullptr;
    auto context = _context;
    return connect_selection_paint(*PaintPopoverManager::get().get_switch(fill), fill,
        [desktop] { return desktop; },
        [this, desktop, document, context] {
            return !_desktop || _desktop != desktop || _context != context || _desktop->getDocument() != document;
        }, ++paint_session, [this] { refresh(); });
}

void FillStrokeControls::draw(Cairo::RefPtr<Cairo::Context> const &cr, int width, int height, bool fill)
{
    auto const &preview = _preview[fill ? 0 : 1];
    double w = width - 3, h = height - 3;
    cr->save();
    auto outline = [&] {
        rounded(cr, 1.5, 1.5, w, h, corner);
        if (!fill) rounded(cr, 1.5 + band, 1.5 + band, w - 2 * band, h - 2 * band, corner / 2);
    };
    outline();
    cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
    cr->clip();
    // Checkerboard remains visible under transparent paints, including a hollow stroke.
    for (int y = 0; y < height; y += 4) for (int x = 0; x < width; x += 4) {
        auto gray = ((x / 4 + y / 4) % 2) ? 0.72 : 1.0;
        cr->set_source_rgb(gray, gray, gray);
        cr->rectangle(x, y, 4, 4);
        cr->fill();
    }
    if (preview.kind == Preview::Kind::Solid) {
        rgba(cr, preview.rgba);
        cr->paint();
    } else if (preview.kind == Preview::Kind::Gradient && !preview.stops.empty()) {
        auto gradient = Cairo::LinearGradient::create(0, 0, width, height);
        for (auto const &[offset, c] : preview.stops) {
            gradient->add_color_stop_rgba(offset, (c >> 24) / 255.0, ((c >> 16) & 255) / 255.0,
                                          ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
        }
        cr->set_source(gradient);
        cr->paint();
    } else {
        cr->set_source_rgb(0.98, 0.98, 0.98);
        cr->paint();
    }
    cr->restore();
    auto foreground = get_color();
    cr->set_source_rgba(foreground.get_red(), foreground.get_green(), foreground.get_blue(), 1);
    cr->set_line_width(_fill_active == fill ? 2 : 1);
    outline();
    cr->stroke();
    if (preview.kind == Preview::Kind::None) {
        cr->save();
        rounded(cr, 1.5, 1.5, w, h, corner);
        cr->clip();
        cr->set_source_rgb(0.95, 0.05, 0.05);
        cr->set_line_width(2.5);
        cr->move_to(1, height - 1);
        cr->line_to(width - 1, 1);
        cr->stroke();
        cr->restore();
    } else if (preview.kind == Preview::Kind::Mixed || preview.kind == Preview::Kind::Other) {
        // A dash for paints that differ; with two ticks under it, a paint the tile can't show.
        cr->set_source_rgba(foreground.get_red(), foreground.get_green(), foreground.get_blue(), 1);
        cr->set_line_width(1.5);
        cr->move_to(4, 4.5);
        cr->line_to(width - 4, 4.5);
        if (preview.kind == Preview::Kind::Other) {
            cr->move_to(4.5, 3);
            cr->line_to(4.5, 7);
            cr->move_to(8.5, 3);
            cr->line_to(8.5, 7);
        }
        cr->stroke();
    }
}

void FillStrokeControls::setNone()
{
    if (!_desktop) return;
    closePopovers();
    auto css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, _fill_active ? "fill" : "stroke", "none");
    sp_desktop_set_style(_desktop, css);
    sp_repr_css_attr_unref(css);
    remember_default_paint(_desktop, _fill_active);
    DocumentUndo::done(_desktop->getDocument(), _fill_active ? RC_("Undo", "Remove fill") : RC_("Undo", "Remove stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    refresh();
}

void FillStrokeControls::applyStyle(char const *fill, char const *stroke)
{
    auto css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "fill", fill);
    sp_repr_css_set_property(css, "stroke", stroke);
    sp_repr_css_set_property(css, "fill-opacity", "1");
    sp_repr_css_set_property(css, "stroke-opacity", "1");
    // 1 pt, as in Illustrator. The desktop takes a width in px: it divides it by each
    // item's scale to the document, the root's viewBox included.
    CSSOStringStream width;
    width << Util::Quantity::convert(1, "pt", "px");
    sp_repr_css_set_property(css, "stroke-width", width.str().c_str());
    sp_desktop_set_style(_desktop, css);
    sp_repr_css_attr_unref(css);
    remember_default_paint(_desktop, true);
    remember_default_paint(_desktop, false);
    refresh();
}

void FillStrokeControls::setDefaults()
{
    if (!_desktop) return;
    closePopovers();
    if (Colors::DocumentColors::mode(_desktop->getDocument()) == Colors::Space::Type::CMYK) {
        applyStyle("device-cmyk(0 0 0 0)", "device-cmyk(0 0 0 1)");
    } else {
        applyStyle("#ffffff", "#000000");
    }
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Default fill and stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void FillStrokeControls::swapPaint()
{
    if (!_desktop) return;
    closePopovers();
    if (!_desktop->getSelection()->isEmpty()) {
        _desktop->getSelection()->swapFillStroke();
    } else {
        // Copy strings first: changing desktop->current invalidates borrowed values.
        auto fill = std::string(sp_repr_css_property(_desktop->current, "fill", "#000000"));
        auto stroke = std::string(sp_repr_css_property(_desktop->current, "stroke", "none"));
        auto fill_alpha = std::string(sp_repr_css_property(_desktop->current, "fill-opacity", "1"));
        auto stroke_alpha = std::string(sp_repr_css_property(_desktop->current, "stroke-opacity", "1"));
        auto css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "fill", stroke.c_str());
        sp_repr_css_set_property(css, "stroke", fill.c_str());
        sp_repr_css_set_property(css, "fill-opacity", stroke_alpha.c_str());
        sp_repr_css_set_property(css, "stroke-opacity", fill_alpha.c_str());
        sp_desktop_set_style(_desktop, css);
        sp_repr_css_attr_unref(css);
        remember_default_paint(_desktop, true);
        remember_default_paint(_desktop, false);
    }
    refresh();
}

void FillStrokeControls::execute(Command command)
{
    switch (command) {
        case Command::ToggleTarget: setActive(!_fill_active); break;
        case Command::Swap: swapPaint(); break;
        case Command::Defaults: setDefaults(); break;
        case Command::None: setNone(); break;
    }
}
} // namespace Inkscape::UI::Widget
