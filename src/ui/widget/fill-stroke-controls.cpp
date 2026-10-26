// SPDX-License-Identifier: GPL-2.0-or-later
#include "fill-stroke-controls.h"

#include <cmath>
#include <algorithm>
#include <glibmm/main.h>
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
#include "util/scope_exit.h"
#include "util/units.h"

namespace Inkscape::UI::Widget {
namespace {
// A tile is as large as a tool's icon and the pair a little larger than one tool, as in
// Illustrator (Illustrator_reference/size_reference.png: 18 px tiles, 28 px the pair).
constexpr int tile = 20;
constexpr int overlap = 10;  // how far the stroke's tile lies down and right of the fill's
constexpr double band = 5;   // width of the stroke tile's ring
// Over the tiles, the default paints at the left and swap at the right, 12 px as in Illustrator;
// under them, the paint kinds: 8 px squares 2 px apart. Nothing is wider than the tile pair, so
// the tools column keeps its width (the owner, 2026-10-20: three buttons in a row widened it).
constexpr int glyph = 12;
constexpr int kind = 10;                         // a paint kind's square and the gap after it
constexpr int above = glyph + 2;                 // where the tiles start
constexpr int below = above + tile + overlap + 4; // where the paint kinds start
unsigned paint_session = 0;

// Square corners, as Illustrator's tiles (the owner, 2026-10-20: "rounded corners look weird").
void square(Cairo::RefPtr<Cairo::Context> const &cr, double x, double y, double w, double h)
{
    cr->rectangle(x, y, w, h);
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
            ".proof-paint-controls button { padding: 0; min-width: 0; min-height: 0; border-radius: 0; }"
            ".proof-paint-controls menubutton > button { background: none; border: none; box-shadow: none; }");
        Gtk::StyleContext::add_provider_for_display(get_display(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
        styled = true;
    }

    _targets.set_size_request(tile + overlap, below + kind);
    _targets.set_halign(Gtk::Align::CENTER);
    append(_targets);
    _fill.set_name("FillPaint");
    _stroke.set_name("StrokePaint");
    _fill.set_child(_fill_preview);
    _stroke.set_child(_stroke_preview);
    for (auto area : {&_fill_preview, &_stroke_preview}) area->set_size_request(tile, tile);
    _fill_preview.set_draw_func([this](auto const &cr, int w, int h) { draw(cr, w, h, true); });
    _stroke_preview.set_draw_func([this](auto const &cr, int w, int h) { draw(cr, w, h, false); });
    _targets.put(_stroke, overlap, above + overlap);
    _targets.put(_fill, 0, above);
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

    struct Place {
        Glyph glyph;
        int x, y, size;
        char const *tooltip, *label;
    };
    Place const places[] = {
        {Glyph::Defaults, 0, 0, glyph, N_("Default fill and stroke (D)"), N_("Default fill and stroke")},
        {Glyph::Swap, tile + overlap - glyph, 0, glyph, N_("Swap fill and stroke (Shift+X)"), N_("Swap fill and stroke")},
        // Illustrator's , and . : the last solid colour and the last gradient of the active
        // target. Not remembered yet (todo.md), so not offered yet.
        {Glyph::Color, 0, below, kind, N_("Color: the active target's last solid color (not available yet)"), N_("Color")},
        {Glyph::Gradient, kind, below, kind, N_("Gradient: the active target's last gradient (not available yet)"), N_("Gradient")},
        {Glyph::None, 2 * kind, below, kind, N_("None: no paint on the active target (/)"), N_("None")},
    };
    for (auto const &place : places) {
        auto &[button, area] = _small[static_cast<int>(place.glyph)];
        area.set_size_request(place.size, place.size);
        area.set_draw_func([this, which = place.glyph](auto const &cr, int, int) { drawGlyph(cr, which); });
        button.set_child(area);
        button.set_has_frame(false);
        button.set_focus_on_click(false);
        button.set_tooltip_text(_(place.tooltip));
        gtk_accessible_update_property(GTK_ACCESSIBLE(button.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, _(place.label), -1);
        _targets.put(button, place.x, place.y);
    }
    _small[static_cast<int>(Glyph::Color)].button.set_sensitive(false);
    _small[static_cast<int>(Glyph::Gradient)].button.set_sensitive(false);
    _small[static_cast<int>(Glyph::Defaults)].button.signal_clicked().connect(sigc::mem_fun(*this, &FillStrokeControls::setDefaults));
    _small[static_cast<int>(Glyph::Swap)].button.signal_clicked().connect(sigc::mem_fun(*this, &FillStrokeControls::swapPaint));
    _small[static_cast<int>(Glyph::None)].button.signal_clicked().connect(sigc::mem_fun(*this, &FillStrokeControls::setNone));
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
    _stop_selected.disconnect();
    _stop_refresh.disconnect();
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
    // A gradient or mesh point picked (or dropped) by a tool: its colour is what the tiles show and
    // an open editor edits. Without this they kept the last colour set (owner, 2026-10-26). Once the
    // tool is done: it announces this midway through its own work on its points.
    _stop_selected = _desktop->connect_gradient_stop_selected([this](auto) {
        if (_stop_refresh.connected()) return;
        _stop_refresh = Glib::signal_idle().connect([this] {
            refresh();
            showOpenPaint();
            return false;
        });
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
        result = sp_desktop_query_style_from_list(sp_desktop_paint_sources(leaves), &style, property);
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
    showPaint(fill);
}

void FillStrokeControls::showOpenPaint()
{
    for (bool fill : {true, false}) {
        auto popover = (fill ? _fill : _stroke).get_popover();
        if (!popover || !popover->get_visible()) continue;
        _showing = true;
        auto done = scope_exit([this] { _showing = false; });
        showPaint(fill);
    }
}

void FillStrokeControls::showPaint(bool fill)
{
    auto paint_switch = PaintPopoverManager::get().get_switch(fill);
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
            return _showing || !_desktop || _desktop != desktop || _context != context || _desktop->getDocument() != document;
        }, ++paint_session, [this] { refresh(); });
}

void FillStrokeControls::draw(Cairo::RefPtr<Cairo::Context> const &cr, int width, int height, bool fill)
{
    auto const &preview = _preview[fill ? 0 : 1];
    double w = width - 3, h = height - 3;
    cr->save();
    auto outline = [&] {
        square(cr, 1.5, 1.5, w, h);
        if (!fill) square(cr, 1.5 + band, 1.5 + band, w - 2 * band, h - 2 * band);
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
        square(cr, 1.5, 1.5, w, h);
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

void FillStrokeControls::drawGlyph(Cairo::RefPtr<Cairo::Context> const &cr, Glyph which)
{
    auto const foreground = get_color();
    auto ink = [&] { cr->set_source_rgba(foreground.get_red(), foreground.get_green(), foreground.get_blue(), 1); };
    auto paper = [&] { cr->set_source_rgb(0.98, 0.98, 0.98); };
    // Not offered yet: drawn faint.
    bool const faint = !_small[static_cast<int>(which)].button.get_sensitive();
    if (faint) cr->push_group();
    cr->set_line_width(1);
    switch (which) {
        case Glyph::Defaults:
            // A white fill in front of a black stroke's ring, as the tiles show them.
            ink();
            cr->set_line_width(2);
            cr->rectangle(5, 5, 6, 6);
            cr->stroke();
            cr->set_line_width(1);
            cr->rectangle(0.5, 0.5, 7, 7);
            paper();
            cr->fill_preserve();
            ink();
            cr->stroke();
            break;
        case Glyph::Swap:
            // Two curved arrows chasing each other (Illustrator_reference/color_flip.png).
            ink();
            cr->set_line_width(1.3);
            cr->move_to(1.5, 7);
            cr->curve_to(1.5, 3.5, 3.5, 2, 8, 2);
            cr->move_to(10.5, 5);
            cr->curve_to(10.5, 8.5, 8.5, 10, 4, 10);
            cr->stroke();
            cr->move_to(11.5, 2);
            cr->line_to(8, -0.5);
            cr->line_to(8, 4.5);
            cr->close_path();
            cr->move_to(0.5, 10);
            cr->line_to(4, 7.5);
            cr->line_to(4, 12.5);
            cr->close_path();
            cr->fill();
            break;
        case Glyph::Color:
        case Glyph::Gradient:
        case Glyph::None:
            // An 8 px square, Illustrator's: white, a ramp, white crossed out in red.
            cr->rectangle(0.5, 0.5, 7, 7);
            if (which == Glyph::Gradient) {
                auto ramp = Cairo::LinearGradient::create(0.5, 0, 7.5, 0);
                ramp->add_color_stop_rgb(0, 1, 1, 1);
                ramp->add_color_stop_rgb(1, 0, 0, 0);
                cr->set_source(ramp);
            } else {
                paper();
            }
            cr->fill_preserve();
            ink();
            cr->stroke();
            if (which == Glyph::None) {
                cr->set_source_rgb(0.95, 0.05, 0.05);
                cr->set_line_width(1.5);
                cr->move_to(1, 7);
                cr->line_to(7, 1);
                cr->stroke();
            }
            break;
    }
    if (faint) {
        cr->pop_group_to_source();
        cr->paint_with_alpha(0.35);
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
