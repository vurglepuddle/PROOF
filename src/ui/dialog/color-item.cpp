// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Color item used in palettes and swatches UI.
 */
/* Authors: PBS <pbs3141@gmail.com>
 * Copyright (C) 2022 PBS
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "color-item.h"

#include <glibmm/i18n.h>
#include <gdkmm/general.h>
#include <gtkmm/binlayout.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/iconpaintable.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/snapshot.h>

#include "colors/spaces/base.h"
#include "colors/document-colors.h"
#include "desktop-style.h"
#include "document.h"
#include "document-undo.h"
#include "message-context.h"
#include "preferences.h"
#include "selection.h"
#include "actions/actions-tools.h"
#include "io/resource.h"
#include "style.h"
#include "ui/containerize.h"
#include "ui/controller.h"
#include "ui/dialog/dialog-base.h"
#include "ui/dialog/dialog-container.h"
#include "ui/icon-names.h"
#include "ui/snapshot-utils.h"
#include "ui/util.h"
#include "util/value-utils.h"
#include "util/variant-visitor.h"
#include "colors/xml-color.h"
#include "spot-ink.h"

namespace Inkscape::UI::Dialog {
namespace {

/// Get the "remove-color" image.
Glib::RefPtr<Gtk::IconPaintable> get_removecolor(int size)
{   
    static Glib::RefPtr<Gtk::IconPaintable> cached_icon;
    static int cached_size = 0;

    if (size > cached_size) {
        size *= 2; // step up 2x to avoid repeated redraws
        auto path = IO::Resource::get_path(IO::Resource::SYSTEM, IO::Resource::UIS, "resources", "remove-color.svg");
        auto icon = Gtk::IconPaintable::create(Gio::File::create_for_path(path.pointer()), size);
        if (icon) {
            cached_icon = std::move(icon);
            cached_size = size;
        } else {
            std::cerr << "Null icon for " << Glib::filename_to_utf8(path.pointer()) << std::endl;
        }

    }

    return cached_icon;
}

} // namespace

ColorItem::ColorItem(DialogBase *dialog)
    : dialog(dialog)
{
    data = PaintNone();
    pinned_default = true;
    add_css_class("paint-none");
    description = C_("Paint", "None");
    color_id = "none";
    common_setup();
}

ColorItem::ColorItem(Colors::Color color, DialogBase *dialog, bool spot, double spot_tint)
    : dialog(dialog)
    , spot(spot)
    , spot_tint(spot_tint)
{
    description = color.getName();
    color_id = color_to_id(color);
    if (spot) {
        tooltip = _("Spot ink: applied as a linked document swatch");
    }
    data = std::move(color);
    common_setup();
}

ColorItem::ColorItem(SPGradient *gradient, DialogBase *dialog)
    : dialog(dialog)
{
    data = GradientData{gradient};
    description = gradient->defaultLabel();
    color_id = gradient->getId();

    gradient->connectRelease(sigc::track_object([this] (SPObject*) {
        std::get<GradientData>(data).gradient = nullptr;
    }, *this));

    gradient->connectModified(sigc::track_object([this] (SPObject *obj, unsigned flags) {
        if (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) {
            queue_draw();
        }
        description = obj->defaultLabel();
        _signal_modified.emit();
        if (is_pinned() != was_grad_pinned) {
            was_grad_pinned = is_pinned();
            _signal_pinned.emit();
        }
    }, *this));

    was_grad_pinned = is_pinned();
    common_setup();
}

ColorItem::ColorItem(Glib::ustring name)
    : description(std::move(name))
{
    bool group = !description.empty();
    set_name("ColorItem");
    set_tooltip_text(description);
    color_id = "-";
    add_css_class(group ? "group" : "filler");
}

ColorItem::~ColorItem() = default;

bool ColorItem::is_group() const {
    return !dialog && color_id == "-" && !description.empty();
}

bool ColorItem::is_spot() const
{
    if (spot) {
        return true;
    }
    if (auto const graddata = std::get_if<GradientData>(&data); graddata && graddata->gradient) {
        return SpotInk::read(graddata->gradient->getRepr()).has_value();
    }
    return false;
}

bool ColorItem::is_filler() const {
    return !dialog && color_id == "-" && description.empty();
}

void ColorItem::common_setup()
{
    containerize(*this);
    set_layout_manager(Gtk::BinLayout::create());
    set_name("ColorItem");
    set_tooltip_text(description + (tooltip.empty() ? tooltip : "\n" + tooltip));

    auto const drag = Gtk::DragSource::create();
    drag->set_button(1); // left
    drag->set_actions(Gdk::DragAction::MOVE | Gdk::DragAction::COPY);
    drag->signal_prepare().connect([this](auto &&...) { return on_drag_prepare(); }, false); // before
    drag->signal_drag_begin().connect([this, &drag = *drag](auto &&...) { on_drag_begin(drag); });
    add_controller(drag);

    auto const motion = Gtk::EventControllerMotion::create();
    motion->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    motion->signal_enter().connect([this](auto &&...) { on_motion_enter(); });
    motion->signal_leave().connect([this](auto &&...) { on_motion_leave(); });
    add_controller(motion);

    auto const click = Gtk::GestureClick::create();
    click->set_button(0); // any
    click->signal_pressed().connect(Controller::use_state([this](auto& controller, auto &&...) { return on_click_pressed(controller); }, *click));
    click->signal_released().connect(Controller::use_state([this](auto& controller, auto &&...) { return on_click_released(controller); }, *click));
    add_controller(click);
}

void ColorItem::set_pinned_pref(const std::string &path)
{
    pinned_pref = path + "/pinned/" + color_id;
}

void ColorItem::draw_color(Glib::RefPtr<Gtk::Snapshot> const &snapshot, int w, int h) const
{
    std::visit(VariantVisitor{
    [&] (Undefined) {
        // there's no color to paint; indicate clearly that there is nothing to select:
        auto y = h / 2;
        auto width = w / 4;
        auto x = (w - width) / 2;
        auto fg = ::get_color(*this);
        fg.alpha = 0.5;
        gtk_snapshot_append_color(snapshot->gobj(), &fg, pass_in(Geom::IntRect::from_xywh(x, y, width, 1)));
    },
    [&] (PaintNone) {
        if (auto const icon = get_removecolor(w * get_scale_factor())) {
            icon->snapshot(snapshot, w, h);
        }
    },
    [&] (Colors::Color const &data) {
        // PROOF: a tint swatch of a spot ink shows its tinted colour.
        auto preview = spot && spot_tint < 1.0 ? SpotInk::display_color(data, spot_tint) : data;
        if (!spot && dialog && dialog->getDesktop())
            preview = Colors::DocumentColors::interpret(dialog->getDesktop()->getDocument(), preview);
        auto const col = to_gtk(preview);
        // there's no way to query background color to check if a color item stands out,
        // so we apply faint outline to let users make out color shapes blending with a background
        auto const fg = ::get_color(*this);
        constexpr float blend = 0.07;
        auto const blended = GdkRGBA(
            fg.red   * blend + col.red   * (1 - blend),
            fg.green * blend + col.green * (1 - blend),
            fg.blue  * blend + col.blue  * (1 - blend),
            1
        );
        gtk_snapshot_append_color(snapshot->gobj(), &blended, pass_in(Geom::IntRect::from_xywh(0, 0, w, h)));
        gtk_snapshot_append_color(snapshot->gobj(), &col, pass_in(Geom::IntRect::from_xywh(1, 1, w - 2, h - 2)));
    },
    [&] (GradientData data) {
        // Gradient pointer may be null if the gradient was destroyed.
        auto grad = data.gradient;
        if (!grad) {
            return;
        }

        auto checkerboard = create_checkerboard_node(Geom::IntRect::from_xywh(0, 0, w, h));
        gtk_snapshot_append_node(snapshot->gobj(), checkerboard.get());

        auto const stops = createPreviewStops(grad);
        gtk_snapshot_append_linear_gradient(
            snapshot->gobj(),
            pass_in(Geom::IntRect::from_xywh(0, 0, w, h)),
            pass_in(Geom::IntPoint{}),
            pass_in(Geom::IntPoint{w, 0}),
            stops.data(),
            stops.size()
        );
    }}, data);
}

void ColorItem::snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const &snapshot)
{
    int w = get_width();
    int h = get_height();

    draw_color(snapshot, w, h);

    // PROOF: spot-ink marker, a white corner triangle with a dot (as in Illustrator).
    if (is_spot()) {
        auto const s = std::min(w, h) * 0.5;
        auto builder = gsk_path_builder_new();
        gsk_path_builder_move_to(builder, w, h - s);
        gsk_path_builder_line_to(builder, w, h);
        gsk_path_builder_line_to(builder, w - s, h);
        gsk_path_builder_close(builder);
        auto triangle = gsk_path_builder_free_to_path(builder);
        auto const white = GdkRGBA{1, 1, 1, 0.95};
        gtk_snapshot_append_fill(snapshot->gobj(), triangle, GSK_FILL_RULE_WINDING, &white);
        gsk_path_unref(triangle);

        builder = gsk_path_builder_new();
        gsk_path_builder_add_circle(builder, pass_in(Geom::Point(w - s * 0.3, h - s * 0.3)), s * 0.12);
        auto dot = gsk_path_builder_free_to_path(builder);
        auto const black = GdkRGBA{0, 0, 0, 0.9};
        gtk_snapshot_append_fill(snapshot->gobj(), dot, GSK_FILL_RULE_WINDING, &black);
        gsk_path_unref(dot);
    }

    // Draw fill/stroke indicators.
    if (is_fill || is_stroke) {
        double const lightness = Colors::get_perceptual_lightness(getColor());
        auto [grey, alpha] = Colors::get_contrasting_color(lightness);
        auto col = GdkRGBA(grey, grey, grey, alpha);

        // Scale so that the square -1...1 is the biggest possible square centred in the widget.
        auto minwh = std::min(w, h);
        snapshot->save();
        gtk_snapshot_translate(snapshot->gobj(), pass_in(Geom::Point(w - minwh, h - minwh) / 2));
        snapshot->scale(minwh / 2.0, minwh / 2.0);
        gtk_snapshot_translate(snapshot->gobj(), pass_in(Geom::IntPoint(1, 1)));

        if (is_fill) {
            static auto const path = [] {
                auto builder = gsk_path_builder_new();
                gsk_path_builder_add_circle(builder, pass_in(Geom::Point{}), 0.35);
                return gsk_path_builder_free_to_path(builder);
            }();
            gtk_snapshot_append_fill(snapshot->gobj(), path, GSK_FILL_RULE_EVEN_ODD, &col);
        }

        if (is_stroke) {
            static auto const path = [] {
                auto builder = gsk_path_builder_new();
                gsk_path_builder_add_circle(builder, pass_in(Geom::Point{}), 0.65);
                gsk_path_builder_add_circle(builder, pass_in(Geom::Point{}), 0.5);
                return gsk_path_builder_free_to_path(builder);
            }();
            gtk_snapshot_append_fill(snapshot->gobj(), path, GSK_FILL_RULE_EVEN_ODD, &col);
        }

        snapshot->restore();
    }
}

void ColorItem::on_motion_enter()
{
    assert(dialog);

    mouse_inside = true;
    if (auto desktop = dialog->getDesktop()) {
        auto msg = Glib::ustring::compose(_("Color: <b>%1</b>; <b>Click</b> to set fill, <b>Shift+click</b> to set stroke"), description);
        desktop->tipsMessageContext()->set(Inkscape::INFORMATION_MESSAGE, msg.c_str());
    }
}

void ColorItem::on_motion_leave()
{
    assert(dialog);

    mouse_inside = false;
    if (auto desktop = dialog->getDesktop()) {
        desktop->tipsMessageContext()->clear();
    }
}

Gtk::EventSequenceState ColorItem::on_click_pressed(Gtk::GestureClick const &click)
{
    assert(dialog);

    if (click.get_current_button() == 3) {
        on_rightclick();
        return Gtk::EventSequenceState::CLAIMED;
    }
    // Return true necessary to avoid stealing the canvas focus.
    return Gtk::EventSequenceState::CLAIMED;
}

Gtk::EventSequenceState ColorItem::on_click_released(Gtk::GestureClick const &click)
{
    assert(dialog);

    auto const button = click.get_current_button();
    if (mouse_inside && (button == 1 || button == 2)) {
        auto const state = click.get_current_event_state();
        auto const stroke = button == 2 || Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK);
        on_click(stroke);
        return Gtk::EventSequenceState::CLAIMED;
    }
    return Gtk::EventSequenceState::NONE;
}

void ColorItem::on_click(bool stroke)
{
    assert(dialog);

    auto desktop = dialog->getDesktop();
    if (!desktop) return;

    auto attr_name = stroke ? "stroke" : "fill";
    auto css = std::unique_ptr<SPCSSAttr, void(*)(SPCSSAttr*)>(sp_repr_css_attr_new(), [] (auto p) {sp_repr_css_attr_unref(p);});

    std::optional<Inkscape::Util::Internal::ContextString> description;
    if (is_paint_none()) {
        sp_repr_css_set_property(css.get(), attr_name, "none");
        description = stroke ? RC_("Undo", "Set stroke color to none") : RC_("Undo", "Set fill color to none");
    } else if (auto const color = std::get_if<Colors::Color>(&data); color && spot) {
        // PROOF: link to the ink's document swatch, creating it on first use.
        auto name = color->getName().empty() ? std::string(_("Unnamed ink")) : color->getName();
        auto id = SpotInk::ensure(desktop->getDocument(), SpotInk::Ink{name, *color, spot_tint});
        sp_repr_css_set_property_string(css.get(), attr_name, "url(#" + id + ")");
        description = stroke ? RC_("Undo", "Set stroke to spot ink") : RC_("Undo", "Set fill to spot ink");
    } else if (auto const color = std::get_if<Colors::Color>(&data)) {
        sp_repr_css_set_property_string(css.get(), attr_name, color->toString());
        description = stroke ? RC_("Undo", "Set stroke color from swatch") : RC_("Undo", "Set fill color from swatch");
    } else if (auto const graddata = std::get_if<GradientData>(&data)) {
        auto grad = graddata->gradient;
        if (!grad) return;
        auto colorspec = "url(#" + Glib::ustring(grad->getId()) + ")";
        sp_repr_css_set_property(css.get(), attr_name, colorspec.c_str());
        description = stroke ? RC_("Undo", "Set stroke color from swatch") : RC_("Undo", "Set fill color from swatch");
    }

    sp_desktop_set_style(desktop, css.get());

    DocumentUndo::done(desktop->getDocument(), *description, INKSCAPE_ICON("swatches"));
}

void ColorItem::on_rightclick()
{
    // Only re/insert actions on click, not in ctor, to avoid performance hit on rebuilding palette
    auto const main_actions = Gio::SimpleActionGroup::create();
    main_actions->add_action("set-fill"  , sigc::mem_fun(*this, &ColorItem::action_set_fill  ));
    main_actions->add_action("set-stroke", sigc::mem_fun(*this, &ColorItem::action_set_stroke));
    main_actions->add_action("delete"    , sigc::mem_fun(*this, &ColorItem::action_delete    ));
    main_actions->add_action("edit"      , sigc::mem_fun(*this, &ColorItem::action_edit      ));
    main_actions->add_action("toggle-pin", sigc::mem_fun(*this, &ColorItem::action_toggle_pin));
    insert_action_group("color-item", main_actions);

    auto const menu = Gio::Menu::create();

    // TRANSLATORS: An item in context menu on a colour in the swatches
    menu->append(_("Set Fill"), "color-item.set-fill");
    menu->append(_("Set Stroke"), "color-item.set-stroke");

    auto section = menu;

    if (auto const graddata = std::get_if<GradientData>(&data)) {
        section = Gio::Menu::create();
        menu->append_section(section);
        section->append(_("Delete" ), "color-item.delete");
        section->append(_("Edit..."), "color-item.edit");
        section = Gio::Menu::create();
        menu->append_section(section);
    }

    section->append(is_pinned() ? _("Unpin Color") : _("Pin Color"), "color-item.toggle-pin");

    // If document has gradients, add Convert section w/ actions to convert them to swatches.
    auto grad_names = std::vector<Glib::ustring>{};
    for (auto const obj : dialog->getDesktop()->getDocument()->getResourceList("gradient")) {
        auto const grad = static_cast<SPGradient *>(obj);
        if (grad->hasStops() && !grad->isSwatch()) {
            grad_names.emplace_back(grad->getId());
        }
    }
    if (!grad_names.empty()) {
        auto const convert_actions = Gio::SimpleActionGroup::create();
        auto const convert_submenu = Gio::Menu::create();

        std::sort(grad_names.begin(), grad_names.end());
        for (auto const &name: grad_names) {
            convert_actions->add_action(name, sigc::bind(sigc::mem_fun(*this, &ColorItem::action_convert), name));
            convert_submenu->append(name, "color-item-convert." + name);
        }

        insert_action_group("color-item-convert", convert_actions);

        section = Gio::Menu::create();
        section->append_submenu(_("Convert"), convert_submenu);
        menu->append_section(section);
    }


    if (_popover) {
        _popover->unparent();
    }

    _popover = std::make_unique<Gtk::PopoverMenu>(menu, Gtk::PopoverMenu::Flags::NESTED);
    _popover->set_parent(*this);

    _popover->popup();
}

void ColorItem::action_set_fill()
{
    on_click(false);
}

void ColorItem::action_set_stroke()
{
    on_click(true);
}

void ColorItem::action_delete()
{
    auto const grad = std::get<GradientData>(data).gradient;
    if (!grad) return;

    grad->setSwatch(false);
    DocumentUndo::done(grad->document, RC_("Undo", "Delete swatch"), INKSCAPE_ICON("color-gradient"));
}

void ColorItem::action_edit()
{
    auto const grad = std::get<GradientData>(data).gradient;
    if (!grad) return;

    auto desktop = dialog->getDesktop();
    auto selection = desktop->getSelection();
    auto items = selection->items_vector();

    if (!items.empty()) {
        auto query = SPStyle(desktop->doc());
        int result = objects_query_fillstroke(items, &query, true);
        if (result == QUERY_STYLE_MULTIPLE_SAME || result == QUERY_STYLE_SINGLE) {
            if (query.fill.isPaintserver()) {
                if (cast<SPGradient>(query.getFillPaintServer()) == grad) {
                    desktop->getContainer()->new_dialog("FillStroke");
                    return;
                }
            }
        }
    }

    // Otherwise, invoke the gradient tool.
    set_active_tool(desktop, "Gradient");
}

void ColorItem::action_toggle_pin()
{
    if (auto const graddata = std::get_if<GradientData>(&data)) {
        auto const grad = graddata->gradient;
        if (!grad) return;

        grad->setPinned(!is_pinned());
        DocumentUndo::done(grad->document, is_pinned() ? RC_("Undo", "Pin swatch") : RC_("Undo", "Unpin swatch"), INKSCAPE_ICON("color-gradient"));
    } else {
        Inkscape::Preferences::get()->setBool(pinned_pref, !is_pinned());
    }
}

void ColorItem::action_convert(Glib::ustring const &name)
{
    // This will not be needed until next menu
    remove_action_group("color-item-convert");

    auto const doc = dialog->getDesktop()->getDocument();
    auto const resources = doc->getResourceList("gradient");
    auto const it = std::find_if(resources.cbegin(), resources.cend(),
                                 [&](auto &_){ return _->getId() == name; });
    if (it == resources.cend()) return;

    auto const grad = static_cast<SPGradient *>(*it);
    grad->setSwatch();
    DocumentUndo::done(doc, RC_("Undo", "Add gradient stop"), INKSCAPE_ICON("color-gradient"));
}

Glib::RefPtr<Gdk::ContentProvider> ColorItem::on_drag_prepare()
{
    if (!dialog) return {};

    Colors::Paint paint;
    if (!is_paint_none()) {
        paint = getColor();
    }

    return Gdk::ContentProvider::create(Util::GlibValue::create<Colors::Paint>(std::move(paint)));
}

void ColorItem::on_drag_begin(Gtk::DragSource &source)
{
    constexpr int w = 32;
    constexpr int h = 24;

    auto snapshot = Gtk::Snapshot::create();
    draw_color(snapshot, w, h);
    source.set_icon(snapshot->to_paintable(), 0, 0);
}

void ColorItem::set_fill(bool b)
{
    is_fill = b;
    queue_draw();
}

void ColorItem::set_stroke(bool b)
{
    is_stroke = b;
    queue_draw();
}

bool ColorItem::is_pinned() const
{
    if (auto const graddata = std::get_if<GradientData>(&data)) {
        auto const grad = graddata->gradient;
        return grad && grad->isPinned();
    } else {
        return Inkscape::Preferences::get()->getBool(pinned_pref, pinned_default);
    }
}

/**
 * Return the average color for this color item. If none, returns white
 * but if a gradient an average of the gradient in RGB is returned.
 */
Colors::Color ColorItem::getColor() const
{
    return std::visit(VariantVisitor{
    [] (Undefined) {
        assert(false);
        return Colors::Color{0xffffffff};
    },
    [] (PaintNone) {
        return Colors::Color(0xffffffff);
    },
    [] (Colors::Color const &color) {
        return color;
    },
    [] (GradientData graddata) {
        auto grad = graddata.gradient;
        if (!grad) {
            return Color{0x0};
        }
        auto color = grad->getPreviewAverageColor();
        color.setName(grad->getId());
        return color;
    }}, data);
}

bool ColorItem::is_paint_none() const {
    return std::holds_alternative<PaintNone>(data);
}

} // namespace Inkscape::UI::Dialog

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
