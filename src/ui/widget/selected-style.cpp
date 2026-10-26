// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Author:
 *   buliabyak@gmail.com
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2005 author
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "selected-style.h"

#include <glibmm/main.h>
#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/gestureclick.h>

#include "colors/xml-color.h"
#include "desktop-style.h"
#include "document-undo.h"
#include "gradient-chemistry.h"
#include "message-context.h"
#include "object/sp-hatch.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-namedview.h"
#include "object/sp-pattern.h"
#include "object/sp-radial-gradient.h"
#include "selection.h"
#include "style.h"
#include "svg/css-ostringstream.h"
#include "ui/controller.h"
#include "ui/cursor-utils.h"
#include "ui/dialog/dialog-container.h"
#include "ui/dialog/fill-and-stroke.h"
#include "ui/icon-names.h"
#include "ui/tools/tool-base.h"
#include "ui/widget/canvas.h"
#include "ui/widget/color-preview.h"
#include "ui/widget/gradient-image.h"
#include "ui/widget/generic/popover-menu.h"
#include "ui/widget/generic/spin-button.h"
#include "util-string/ustring-format.h"
#include "util/units.h"
#include "util/value-utils.h"
#include "util/variant-visitor.h"

static constexpr int SELECTED_STYLE_SB_WIDTH     =  80;
static constexpr int SELECTED_STYLE_PLACE_WIDTH  =  50;
static constexpr int SELECTED_STYLE_STROKE_WIDTH =  40;
static constexpr int SELECTED_STYLE_FLAG_WIDTH   =  12;
static constexpr int SELECTED_STYLE_WIDTH        = 250;

static constexpr std::array<double, 15> _sw_presets{
    32, 16, 10, 8, 6, 4, 3, 2, 1.5, 1, 0.75, 0.5, 0.25, 0.1};

static const Glib::ustring (*get_type_strings())[2][2] {
    // In order of PaintType enum: fill, stroke; label, tooltip.
    static const Glib::ustring type_strings[][2][2] = {
        // clang-format off
        {{ _("N/A"),                    _("Nothing selected")},
         { _("N/A"),                    _("Nothing selected")}},
        {{C_("Fill", "<i>None</i>"),    _("No fill, middle-click for black fill")},
         {C_("Stroke", "<i>None</i>"),  _("No stroke, middle-click for black stroke")}},
        {{ _("<b>Unset</b>"),           _("Unset fill")},
         { _("<b>Unset</b>"),           _("Unset stroke")}},
        {{ _("≠"),                      _("Different fills")},
         { _("≠"),                      _("Different strokes")}},
        {{ _("Pattern"),                _("Pattern (fill)")},
         { _("Pattern"),                _("Pattern (stroke)")}},
        {{ _("Hatch"),                  _("Pattern (fill)")},
         { _("Hatch"),                  _("Pattern (stroke)")}},
        {{ _("<b>L</b>"),               _("Linear gradient (fill)")},
         { _("<b>L</b>"),               _("Linear gradient (stroke)")}},
        {{ _("<b>R</b>"),               _("Radial gradient (fill)")},
         { _("<b>R</b>"),               _("Radial gradient (stroke)")}},
        {{ _("<b>M</b>"),               _("Mesh gradient (fill)")},
         { _("<b>M</b>"),               _("Mesh gradient (stroke)")}},
        {{ _("<b>C</b>"),               _("Flat color (fill)")},
         { _("<b>C</b>"),               _("Flat color (stroke)")}}
        // clang-format on
    };
    return type_strings;
}

static void
ss_selection_changed (Inkscape::Selection *, gpointer data)
{
    Inkscape::UI::Widget::SelectedStyle *ss = (Inkscape::UI::Widget::SelectedStyle *) data;
    ss->update();
}

static void
ss_selection_modified( Inkscape::Selection *selection, guint flags, gpointer data )
{
    // Don't update the style when dragging or doing non-style related changes
    if (flags & (SP_OBJECT_STYLE_MODIFIED_FLAG)) {
        ss_selection_changed (selection, data);
    }
}

namespace Inkscape::UI::Widget {

struct SelectedStyleDropTracker final {
    SelectedStyle* parent;
    int item;
};

/* Drag and Drop */
enum ui_drop_target_info {
    APP_OSWB_COLOR
};

/* convenience function */
static Dialog::FillAndStroke *get_fill_and_stroke_panel(SPDesktop *desktop);

SelectedStyle::SelectedStyle()
{
    set_name("SelectedStyle");
    set_size_request (SELECTED_STYLE_WIDTH, -1);

    grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_size_request(SELECTED_STYLE_WIDTH, -1);

    // Fill and stroke
    for (int i = 0; i <2; i++) {
        label[i] = Gtk::make_managed<Gtk::Label>(i == 0 ? _("Fill:") : _("Stroke:"));
        label[i]->set_halign(Gtk::Align::END);

        // Multiple, Average, or Single
        tag[i] = Gtk::make_managed<Gtk::Label>(); // "m", "a", or empty
        tag[i]->set_size_request(SELECTED_STYLE_FLAG_WIDTH, -1);
        tag[i]->set_name("Tag");

        // Type of fill
        type_label[i] = std::make_unique<Gtk::Label>(get_type_strings()[0][i][0]);
        type_label[i]->set_hexpand(true);

        // CSS sets width to 54.
        gradient_preview[i] = std::make_unique<GradientImage>(nullptr);
        gradient_preview[i]->set_visible(false);

        color_preview[i] = std::make_unique<Inkscape::UI::Widget::ColorPreview>(0);
        color_preview[i]->set_size_request(SELECTED_STYLE_PLACE_WIDTH, -1);
        color_preview[i]->set_hexpand(true);
        color_preview[i]->set_visible(false);

        // Shows one or two children at a time.
        swatch[i] = Gtk::make_managed<RotateableSwatch>(this, i);
        swatch[i]->set_orientation(Gtk::Orientation::HORIZONTAL);
        swatch[i]->set_hexpand(false);
        swatch[i]->append(*type_label[i]);
        swatch[i]->append(*gradient_preview[i]);
        swatch[i]->append(*color_preview[i]);
        swatch[i]->set_tooltip_text(get_type_strings()[0][i][1]);
        swatch[i]->set_size_request(SELECTED_STYLE_PLACE_WIDTH, -1);

        // Drag color from color palette, for example.
        drop[i] = std::make_unique<SelectedStyleDropTracker>();
        drop[i]->parent = this;
        drop[i]->item = i;
        auto target = Gtk::DropTarget::create(Util::GlibValue::type<Colors::Paint>(), Gdk::DragAction::COPY | Gdk::DragAction::MOVE);
        target->signal_drop().connect([this, i] (Glib::ValueBase const &value, double, double) {
            if (!dropEnabled[i]) {
                return false;
            }

            auto const &tracker = *drop[i];
            auto const paint = Util::GlibValue::get<Colors::Paint>(value);
            auto const colorspec = std::visit(VariantVisitor{
                [] (Colors::Color const &color) { return color.toString(false); },
                [] (Colors::NoColor) -> std::string { return "none"; }
            }, *paint);

            auto const css = sp_repr_css_attr_new();
            sp_repr_css_set_property_string(css, tracker.item == SS_FILL ? "fill" : "stroke", colorspec);
            sp_desktop_set_style(tracker.parent->_desktop, css);
            sp_repr_css_attr_unref(css);

            DocumentUndo::done(tracker.parent->_desktop->getDocument(), RC_("Undo", "Drop color"), "");
            return true;
        }, true);
        swatch[i]->add_controller(target);

        auto const click = Gtk::GestureClick::create();
        auto const callback = i == 0 ? sigc::mem_fun(*this, &SelectedStyle::on_fill_click)
                                     : sigc::mem_fun(*this, &SelectedStyle::on_stroke_click);
        click->set_button(0); // any
        click->signal_released().connect(Controller::use_state(std::move(callback), *click));
        swatch[i]->add_controller(click);

        grid->attach(*label[i],  0, i, 1, 1);
        grid->attach(*tag[i],    1, i, 1, 1);
        grid->attach(*swatch[i], 2, i, 1, 1);

        make_popup(static_cast<FillOrStroke>(i));
        _mode[i] = SS_NA;
    }

    // Stroke width
    stroke_width = Gtk::make_managed<Gtk::Label>("1");
    stroke_width_rotateable = Gtk::make_managed<RotateableStrokeWidth>(this);
    stroke_width_rotateable->append(*stroke_width);
    stroke_width_rotateable->set_size_request(SELECTED_STYLE_STROKE_WIDTH, -1);
    {
        auto const click = Gtk::GestureClick::create();
        click->set_button(0); // any
        click->signal_released().connect(Controller::use_state(sigc::mem_fun(*this, &SelectedStyle::on_sw_click), *click));
        stroke_width_rotateable->add_controller(click);
    }
    grid->attach(*stroke_width_rotateable, 3, 1, 1, 1);

    // Opacity
    make_popup_opacity();
    opacity_adjustment = Gtk::Adjustment::create(100, 0.0, 100, 1.0, 10.0);
    opacity_sb = Gtk::make_managed<Inkscape::UI::Widget::InkSpinButton>();
    opacity_sb->set_step(0.02);
    opacity_sb->set_digits(0);
    opacity_sb->set_icon("transparency");
    opacity_sb->add_css_class("symbolic");
    opacity_sb->set_suffix(_("%"));
    opacity_sb->set_adjustment(opacity_adjustment);
    opacity_sb->set_size_request(SELECTED_STYLE_SB_WIDTH);
    opacity_sb->set_sensitive(false);
    opacity_sb->setDefocusTarget(this);
    opacity_sb->set_valign(Gtk::Align::CENTER);

    auto opacity_box = Gtk::make_managed<Gtk::Box>();
    opacity_box->append(*opacity_sb);

    auto const click = Gtk::GestureClick::create();
    click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    click->set_button(2); // middle
    click->signal_pressed().connect([&click = *click](auto &&...) { click.set_state(Gtk::EventSequenceState::CLAIMED); });
    click->signal_released().connect(Controller::use_state(sigc::mem_fun(*this, &SelectedStyle::on_opacity_click), *click));
    opacity_box->add_controller(click);

    on_popup_menu(*opacity_box, sigc::mem_fun(*this, &SelectedStyle::on_opacity_popup));
    opacity_sb->signal_value_changed().connect(sigc::mem_fun(*this, &SelectedStyle::on_opacity_changed));

    grid->attach(*opacity_box, 4, 0, 1, 2);

    grid->set_column_spacing(4);
    setChild(grid);

    make_popup_units();
}

SelectedStyle::~SelectedStyle() = default;

void SelectedStyle::setDesktop(SPDesktop *desktop)
{
    if (_desktop) {
        selection_changed_connection.disconnect();
        selection_modified_connection.disconnect();
        stop_selected_connection.disconnect();
        stop_refresh_connection.disconnect();
    }

    _desktop = desktop;

    if (_desktop) {
        auto selection = desktop->getSelection();

        selection_changed_connection = selection->connectChanged(
            sigc::bind(&ss_selection_changed, this)
        );
        selection_modified_connection = selection->connectModified(
            sigc::bind(&ss_selection_modified, this)
        );
        // A gradient or mesh point picked by a tool shows its own colour; once the tool is done (it
        // announces this midway through its own work on its points).
        stop_selected_connection = desktop->connect_gradient_stop_selected([this](auto) {
            if (stop_refresh_connection.connected()) return;
            stop_refresh_connection = Glib::signal_idle().connect([this] { update(); return false; });
        });
        update();

        _sw_unit = desktop->getNamedView()->display_units;
    }
}

void SelectedStyle::on_fill_remove() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "fill", "none");
    sp_desktop_set_style (_desktop, css, true, true);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Remove fill"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_stroke_remove() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "stroke", "none");
    sp_repr_css_unset_property (css, "vector-effect");
    sp_repr_css_unset_property (css, "-inkscape-stroke");
    sp_desktop_set_style (_desktop, css, true, true);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Remove stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_unset() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_unset_property (css, "fill");
    sp_desktop_set_style (_desktop, css, true, true);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Unset fill"), INKSCAPE_ICON("dialog-fill-and-stroke"));

}

void SelectedStyle::on_stroke_unset() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_unset_property (css, "stroke");
    sp_repr_css_unset_property (css, "stroke-opacity");
    sp_repr_css_unset_property (css, "stroke-width");
    sp_repr_css_unset_property (css, "stroke-miterlimit");
    sp_repr_css_unset_property (css, "stroke-linejoin");
    sp_repr_css_unset_property (css, "stroke-linecap");
    sp_repr_css_unset_property (css, "stroke-dashoffset");
    sp_repr_css_unset_property (css, "stroke-dasharray");
    sp_repr_css_unset_property (css, "vector-effect");
    sp_repr_css_unset_property (css, "-inkscape-stroke");
    sp_desktop_set_style (_desktop, css, true, true);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Unset stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_opaque() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "fill-opacity", "1");
    sp_desktop_set_style (_desktop, css, true);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Make fill opaque"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_stroke_opaque() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "stroke-opacity", "1");
    sp_desktop_set_style (_desktop, css, true);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Make fill opaque"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_lastused() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    auto color = sp_desktop_get_color(_desktop, true);
    sp_repr_css_set_property_string(css, "fill", color ? color->toString() : "none");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Apply last set color to fill"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_stroke_lastused() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    auto color = sp_desktop_get_color(_desktop, false);
    sp_repr_css_set_property_string(css, "fill", color ? color->toString() : "none");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Apply last set color to stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_lastselected() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property_string(css, "fill", _lastselected[SS_FILL] ? _lastselected[SS_FILL]->toString() : "");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Apply last selected color to fill"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_stroke_lastselected() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property_string(css, "stroke", _lastselected[SS_STROKE] ? _lastselected[SS_STROKE]->toString() : "");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Apply last selected color to stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_invert() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    auto color = _thisselected[SS_FILL];
    if (_mode[SS_FILL] == SS_LGRADIENT || _mode[SS_FILL] == SS_RGRADIENT) {
        sp_gradient_invert_selected_gradients(_desktop, Inkscape::FOR_FILL);
        return;

    }

    if (_mode[SS_FILL] != SS_COLOR) return;
    color->invert();
    sp_repr_css_set_property_string(css, "fill", color->toString());
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Invert fill"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_stroke_invert() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    auto color = _thisselected[SS_STROKE];
    if (_mode[SS_STROKE] == SS_LGRADIENT || _mode[SS_STROKE] == SS_RGRADIENT) {
        sp_gradient_invert_selected_gradients(_desktop, Inkscape::FOR_STROKE);
        return;
    }
    if (_mode[SS_STROKE] != SS_COLOR) return;
    color->invert();
    sp_repr_css_set_property_string(css, "stroke", color->toString());
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Invert stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_white() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "fill", "#ffffff");
    sp_repr_css_set_property (css, "fill-opacity", "1");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "White fill"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_stroke_white() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "stroke", "#ffffff");
    sp_repr_css_set_property (css, "stroke-opacity", "1");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "White stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_black() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "fill", "#000000");
    sp_repr_css_set_property (css, "fill-opacity", "1.0");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Black fill"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_stroke_black() {
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "stroke", "#000000");
    sp_repr_css_set_property (css, "stroke-opacity", "1.0");
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Black stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
}

void SelectedStyle::on_fill_copy() {
    if (_mode[SS_FILL] == SS_COLOR) {
        auto text = _thisselected[SS_FILL]->toString();
        if (!text.empty()) {
            auto const display = Gdk::Display::get_default();
            display->get_primary_clipboard()->set_text(text);
        }
    }
}

void SelectedStyle::on_stroke_copy() {
    if (_mode[SS_STROKE] == SS_COLOR) {
        auto text = _thisselected[SS_STROKE]->toString();
        if (!text.empty()) {
            auto const display = Gdk::Display::get_default();
            display->get_primary_clipboard()->set_text(text);
        }
    }
}

void SelectedStyle::_on_paste_callback(Glib::RefPtr<Gio::AsyncResult>& result, Glib::ustring typepaste)
{
    auto const display = Gdk::Display::get_default();
    Glib::RefPtr<Gdk::Clipboard> refClipboard = display->get_primary_clipboard();
    // Parse the clipboard text as if it was a color string.
    Glib::ustring text;
    try {
        text = refClipboard->read_text_finish(result);
    } catch (Glib::Error const &err) {
        std::cerr << "Pasting text failed: " << err.what() << std::endl;
        return;
    }
    if (auto color = Inkscape::Colors::Color::parse(text)) {
        SPCSSAttr *css = sp_repr_css_attr_new ();
        sp_repr_css_set_property_string(css, "fill", color->toString());
        sp_desktop_set_style (_desktop, css);
        sp_repr_css_attr_unref (css);
        DocumentUndo::done(_desktop->getDocument(), typepaste == "fill" ? RC_("Undo", "Paste fill") : RC_("Undo", "Paste stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

void SelectedStyle::on_fill_paste() {
    auto const display = Gdk::Display::get_default();
    Glib::RefPtr<Gdk::Clipboard> refClipboard = display->get_primary_clipboard();
    refClipboard->read_text_async(sigc::bind(sigc::mem_fun(*this, &SelectedStyle::_on_paste_callback), "fill"));
}

void SelectedStyle::on_stroke_paste() {
    auto const display = Gdk::Display::get_default();
    Glib::RefPtr<Gdk::Clipboard> refClipboard = display->get_primary_clipboard();
    refClipboard->read_text_async(sigc::bind(sigc::mem_fun(*this, &SelectedStyle::_on_paste_callback), "stroke"));
}

void SelectedStyle::on_fillstroke_swap() {
    _desktop->getSelection()->swapFillStroke();
}

void SelectedStyle::on_fill_edit() {
    if (Dialog::FillAndStroke *fs = get_fill_and_stroke_panel(_desktop))
        fs->showPageFill();
}

void SelectedStyle::on_stroke_edit() {
    if (Dialog::FillAndStroke *fs = get_fill_and_stroke_panel(_desktop))
        fs->showPageStrokePaint();
}

Gtk::EventSequenceState SelectedStyle::on_fill_click(Gtk::GestureClick const &click, int n_press, double /*x*/,
                                                     double /*y*/)
{
    auto const button = click.get_current_button();
    if (button == 1) { // click, open fill&stroke
        if (Dialog::FillAndStroke *fs = get_fill_and_stroke_panel(_desktop))
            fs->showPageFill();
    } else if (button == 3) { // right-click, popup menu
        setPopover(_popup[SS_FILL].get());
        _popup[SS_FILL]->popup_at_center(*swatch[SS_FILL]);
    } else if (button == 2) { // middle click, toggle none/lastcolor
        if (_mode[SS_FILL] == SS_NONE) {
            on_fill_lastused();
        } else {
            on_fill_remove();
        }
    }
    return Gtk::EventSequenceState::CLAIMED;
}

Gtk::EventSequenceState SelectedStyle::on_stroke_click(Gtk::GestureClick const &click, int n_press, double /*x*/,
                                                       double /*y*/)
{
    auto const button = click.get_current_button();
    if (button == 1) { // click, open fill&stroke
        if (Dialog::FillAndStroke *fs = get_fill_and_stroke_panel(_desktop))
            fs->showPageStrokePaint();
    } else if (button == 3) { // right-click, popup menu
        setPopover(_popup[SS_STROKE].get());
        _popup[SS_STROKE]->popup_at_center(*swatch[SS_STROKE]);
    } else if (button == 2) { // middle click, toggle none/lastcolor
        if (_mode[SS_STROKE] == SS_NONE) {
            on_stroke_lastused();
        } else {
            on_stroke_remove();
        }
    }
    return Gtk::EventSequenceState::CLAIMED;
}

Gtk::EventSequenceState SelectedStyle::on_sw_click(Gtk::GestureClick const &click, int n_press, double, double)
{
    auto const button = click.get_current_button();
    if (button == 1) { // click, open fill&stroke
        if (Dialog::FillAndStroke *fs = get_fill_and_stroke_panel(_desktop))
            fs->showPageStrokeStyle();
    } else if (button == 3) { // right-click, popup menu
        auto const it = std::find_if(_unit_mis.cbegin(), _unit_mis.cend(), [this] (auto mi) {
            return mi->get_label() == _sw_unit->abbr;
        });
        if (it != _unit_mis.cend()) (*it)->set_active(true);

        setPopover(_popup_sw.get());
        _popup_sw->popup_at_center(*stroke_width);
    } else if (button == 2) { // middle click, toggle none/lastwidth?
        //
    }
    return Gtk::EventSequenceState::CLAIMED;
}

Gtk::EventSequenceState
SelectedStyle::on_opacity_click(Gtk::GestureClick const & /*click*/,
                                int /*n_press*/, double /*x*/, double /*y*/)
{
    const char* opacity = opacity_sb->get_value() < 50? "0.5" : (opacity_sb->get_value() == 100? "0" : "1");
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property (css, "opacity", opacity);
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Change opacity"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    return Gtk::EventSequenceState::CLAIMED;
}

template <typename Slot, typename ...Args>
static UI::Widget::PopoverMenuItem *make_menu_item(Glib::ustring const &label, Slot slot,
                                            Args &&...args)
{
    auto const item = Gtk::make_managed<UI::Widget::PopoverMenuItem>(std::forward<Args>(args)...);
    item->set_child(*Gtk::make_managed<Gtk::Label>(label, Gtk::Align::START, Gtk::Align::START));
    item->signal_activate().connect(std::move(slot));
    return item;
};

void SelectedStyle::make_popup(FillOrStroke const i)
{
    _popup[i] = std::make_unique<UI::Widget::PopoverMenu>(Gtk::PositionType::TOP);

    auto const add_item = [&](Glib::ustring const &  fill_label, auto const   fill_method,
                              Glib::ustring const &stroke_label, auto const stroke_method)
    {
        auto const &label = i == SS_FILL || stroke_label.empty()     ? fill_label  : stroke_label ;
        auto const method = i == SS_FILL || stroke_method == nullptr ? fill_method : stroke_method;
        auto const item = make_menu_item(label, sigc::mem_fun(*this, method));
        _popup[i]->append(*item);
        return item;
    };

    add_item(_("Edit Fill..."  )      , &SelectedStyle::  on_fill_edit        ,
             _("Edit Stroke...")      , &SelectedStyle::on_stroke_edit        );

    _popup[i]->append_separator();


    add_item(_("Last Set Color")      , &SelectedStyle::  on_fill_lastused    ,
             {}                       , &SelectedStyle::on_stroke_lastused    );
    add_item(_("Last Selected Color") , &SelectedStyle::  on_fill_lastselected,
             {}                       , &SelectedStyle::on_stroke_lastselected);

    _popup[i]->append_separator();

    add_item(_("Invert")              , &SelectedStyle::  on_fill_invert      ,
             {}                       , &SelectedStyle::on_stroke_invert      );

    _popup[i]->append_separator();

    add_item(_("White")               , &SelectedStyle::  on_fill_white       ,
             {}                       , &SelectedStyle::on_stroke_white       );
    add_item(_("Black")               , &SelectedStyle::  on_fill_black       ,
             {}                       , &SelectedStyle::on_stroke_black       );

    _popup[i]->append_separator();

    _popup_copy[i] = add_item(
             _("Copy Color")          , &SelectedStyle::  on_fill_copy        ,
             {}                       , &SelectedStyle::on_stroke_copy        );
    _popup_copy[i]->set_sensitive(false);

    add_item(_("Paste Color")         , &SelectedStyle::  on_fill_paste       ,
             {}                       , &SelectedStyle::on_stroke_paste       );
    add_item(_("Swap Fill and Stroke"), &SelectedStyle::on_fillstroke_swap    ,
             {}                       , nullptr                               );

    _popup[i]->append_separator();

    add_item(_("Make Fill Opaque"  )  , &SelectedStyle::  on_fill_opaque      ,
             _("Make Stroke Opaque")  , &SelectedStyle::on_stroke_opaque      );
    //TRANSLATORS COMMENT: unset is a verb here
    add_item(_("Unset Fill"  )        , &SelectedStyle::  on_fill_unset       ,
             _("Unset Stroke")        , &SelectedStyle::on_stroke_unset       );
    add_item(_("Remove Fill"  )       , &SelectedStyle::  on_fill_remove      ,
             _("Remove Stroke")       , &SelectedStyle::on_stroke_remove      );
}

void SelectedStyle::make_popup_units()
{
    _popup_sw = std::make_unique<UI::Widget::PopoverMenu>(Gtk::PositionType::TOP);

    _popup_sw->append_section_label(_("<b>Stroke Width</b>"));

    _popup_sw->append_separator();

    _popup_sw->append_section_label(_("Unit"));
    Gtk::CheckButton *group = nullptr;
    auto const &unit_table = Util::UnitTable::get();
    for (auto unit : unit_table.units(Inkscape::Util::UNIT_TYPE_LINEAR)) {
        auto key = unit->abbr;
        auto const item = Gtk::make_managed<UI::Widget::PopoverMenuItem>();
        auto const radio = Gtk::make_managed<Gtk::CheckButton>(key);
        if (!group) {
            group = radio;
        } else {
            radio->set_group(*group);
        }
        item->set_child(*radio);
        _unit_mis.push_back(radio);
        auto const u = unit_table.getUnit(key);
        item->signal_activate().connect(
            sigc::bind(sigc::mem_fun(*this, &SelectedStyle::on_popup_units), u));
        _popup_sw->append(*item);
    }

    _popup_sw->append_separator();

    _popup_sw->append_section_label(_("Width"));
    for (std::size_t i = 0; i < _sw_presets.size(); ++i) {
        _popup_sw->append(*make_menu_item(Inkscape::ustring::format_classic(_sw_presets[i]),
            sigc::bind(sigc::mem_fun(*this, &SelectedStyle::on_popup_preset), i)));
    }
    _popup_sw->append(*make_menu_item("Hairline",
        sigc::bind(sigc::mem_fun(*this, &SelectedStyle::on_popup_preset), _sw_presets.size())));

    _popup_sw->append_separator();

    _popup_sw->append(*make_menu_item(_("Remove Stroke"),
          sigc::mem_fun(*this, &SelectedStyle::on_stroke_remove)));
}

void SelectedStyle::on_popup_units(Inkscape::Util::Unit const *unit) {
    _sw_unit = unit;
    update();
}

void SelectedStyle::on_popup_preset(int i) {
    SPCSSAttr *css = sp_repr_css_attr_new ();

    // Note: "vector-effect as define in SVG2 allows only one value in the list.
    // It was intended to allow multiple values, if the spec changes, this code
    // will need to be updated (probably best to add sp_repr_css_add_list_value()
    // and sp_repr_css_remove_list_value() functions).
    if (i == _sw_presets.size()) {
        // Hairline
        sp_repr_css_set_property (css, "stroke-width", "1");
        sp_repr_css_set_property (css, "vector-effect", "non-scaling-stroke");
        sp_repr_css_set_property (css, "-inkscape-stroke", "hairline");
    } else {
        // Not hairline
        gdouble w;
        if (_sw_unit) {
            w = Inkscape::Util::Quantity::convert(_sw_presets[i], _sw_unit, "px");
        } else {
            w = _sw_presets[i];
        }
        sp_repr_css_set_property_double (css, "stroke-width", w);
        sp_repr_css_unset_property (css, "vector-effect");
        sp_repr_css_unset_property (css, "-inkscape-stroke");
    }

    // FIXME: update dash patterns!
    sp_desktop_set_style (_desktop, css, true);
    sp_repr_css_attr_unref (css);
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Change stroke width"), INKSCAPE_ICON("swatches"));
}

void SelectedStyle::onDefocus()
{
    if (_desktop) {
        _desktop->getCanvas()->grab_focus();
    }
}

void
SelectedStyle::update()
{
    if (_desktop == nullptr)
        return;

    // Create temporary style
    SPStyle query(_desktop->getDocument());

    for (int i = SS_FILL; i <= SS_STROKE; i++) {

        // New
        type_label[i]->show(); // Used by all types except solid color.
        gradient_preview[i]->set_visible(false);
        color_preview[i]->set_visible(false);

        _mode[i] = SS_NA;
        _paintserver_id[i].clear();
        _popup_copy[i]->set_sensitive(false);

        // Query style from desktop. This returns a result flag and fills query with the style of
        // subselection, if any, or selection.
        int result = sp_desktop_query_style (_desktop, &query,
                                             (i == SS_FILL) ?
                                             QUERY_STYLE_PROPERTY_FILL :
                                             QUERY_STYLE_PROPERTY_STROKE);

        switch (result) {
        case QUERY_STYLE_NOTHING:

            tag[i]->set_markup("");

            type_label[i]->set_markup(get_type_strings()[SS_NA][i][0]);
            swatch[i]->set_tooltip_text(get_type_strings()[SS_NA][i][1]);

            if (dropEnabled[i]) {
                dropEnabled[i] = false;
            }
            break;
        case QUERY_STYLE_SINGLE:
        case QUERY_STYLE_MULTIPLE_AVERAGED:
        case QUERY_STYLE_MULTIPLE_SAME: {
            dropEnabled[i] = true;

            auto paint = i == SS_FILL ? query.fill.upcast() : query.stroke.upcast();
            double opacity = i == SS_FILL ? query.fill_opacity.as_double() : query.stroke_opacity.as_double();
            if (paint->set && paint->isPaintserver()) {
                SPPaintServer *server = (i == SS_FILL)? SP_STYLE_FILL_SERVER (&query) : SP_STYLE_STROKE_SERVER (&query);
                if ( server ) {
                    Inkscape::XML::Node *srepr = server->getRepr();
                    _paintserver_id[i] += "url(#";
                    _paintserver_id[i] += srepr->attribute("id");
                    _paintserver_id[i] += ")";

                    if (is<SPLinearGradient>(server)) {
                        auto vector = cast<SPGradient>(server)->getVector();

                        type_label[i]->set_markup(  get_type_strings()[SS_LGRADIENT][i][0]);
                        swatch[i]->set_tooltip_text(get_type_strings()[SS_LGRADIENT][i][1]);
                        gradient_preview[i]->set_gradient(vector);
                        gradient_preview[i]->show();

                        _mode[i] = SS_LGRADIENT;
                    } else if (is<SPRadialGradient>(server)) {
                        auto vector = cast<SPGradient>(server)->getVector();

                        type_label[i]->set_markup(  get_type_strings()[SS_RGRADIENT][i][0]);
                        swatch[i]->set_tooltip_text(get_type_strings()[SS_RGRADIENT][i][1]);
                        gradient_preview[i]->set_gradient(vector);
                        gradient_preview[i]->show();

                        _mode[i] = SS_RGRADIENT;
                    } else if (is<SPMeshGradient>(server)) {
                        auto array = cast<SPGradient>(server)->getArray();

                        type_label[i]->set_markup(  get_type_strings()[SS_MGRADIENT][i][0]);
                        swatch[i]->set_tooltip_text(get_type_strings()[SS_MGRADIENT][i][1]);
                        gradient_preview[i]->set_gradient(array);
                        gradient_preview[i]->show();

                        _mode[i] = SS_MGRADIENT;
                    } else if (is<SPPattern>(server)) {
                        type_label[i]->set_markup(  get_type_strings()[SS_PATTERN][i][0]);
                        swatch[i]->set_tooltip_text(get_type_strings()[SS_PATTERN][i][1]);

                        _mode[i] = SS_PATTERN;
                    } else if (is<SPHatch>(server)) {
                        type_label[i]->set_markup(  get_type_strings()[SS_HATCH][i][0]);
                        swatch[i]->set_tooltip_text(get_type_strings()[SS_HATCH][i][1]);

                        _mode[i] = SS_HATCH;
                    }
                } else {
                    g_warning ("file %s: line %d: Unknown paint server", __FILE__, __LINE__);
                }
            } else if (paint->set && paint->isColor()) {
                auto color = paint->getColor();
                color.addOpacity(opacity);

                _lastselected[i] = _thisselected[i];
                _thisselected[i] = color; // include opacity

                // No type_label.
                swatch[i]->set_tooltip_text(get_type_strings()[SS_COLOR][i][1] + ": " + color.toString() +
                                            _(", drag to adjust, middle-click to remove"));
                type_label[i]->set_visible(false);
                color_preview[i]->setRgba32(color.toRGBA());
                color_preview[i]->show();

                _mode[i] = SS_COLOR;
                _popup_copy[i]->set_sensitive(true);
            } else if (paint->set && paint->isNone()) {
                type_label[i]->set_markup(get_type_strings()[  SS_NONE][i][0]);
                swatch[i]->set_tooltip_text(get_type_strings()[SS_NONE][i][1]);
                _mode[i] = SS_NONE;
            } else if (!paint->set) {
                type_label[i]->set_markup(get_type_strings()[  SS_UNSET][i][0]);
                swatch[i]->set_tooltip_text(get_type_strings()[SS_UNSET][i][1]);

                _mode[i] = SS_UNSET;
            }

            if (result == QUERY_STYLE_MULTIPLE_AVERAGED) {
                // TRANSLATORS: A means "Averaged"
                tag[i]->set_markup("<b>a</b>");
                tag[i]->set_tooltip_text(i == 0 ?
                                         _("Fill is averaged over selected objects") :
                                         _("Stroke is averaged over selected objects"));

            } else if (result == QUERY_STYLE_MULTIPLE_SAME) {
                // TRANSLATORS: M means "Multiple"
                tag[i]->set_markup("<b>m</b>");
                tag[i]->set_tooltip_text(i == 0 ?
                                         _("Multiple selected objects have same fill") :
                                         _("Multiple selected objects have same stroke"));
            } else {
                tag[i]->set_markup("");
                tag[i]->set_tooltip_text("");
            }
            break;
        }

        case QUERY_STYLE_MULTIPLE_DIFFERENT:
            type_label[i]->set_markup(get_type_strings()[  SS_MANY][i][0]);
            swatch[i]->set_tooltip_text(get_type_strings()[SS_MANY][i][1]);

            _mode[i] = SS_MANY;
            break;
        default:
            break;
        }
    }

// Now query opacity
    int result = sp_desktop_query_style (_desktop, &query, QUERY_STYLE_PROPERTY_MASTEROPACITY);

    switch (result) {
    case QUERY_STYLE_NOTHING:
        opacity_sb->set_tooltip_text(_("Nothing selected"));
        opacity_sb->set_sensitive(false);
        break;
    case QUERY_STYLE_SINGLE:
    case QUERY_STYLE_MULTIPLE_AVERAGED:
    case QUERY_STYLE_MULTIPLE_SAME:
        opacity_sb->set_tooltip_markup(_("<b>Opacity (%)</b>\nMiddle-click cycles through 0%, 50%, 100%"));

        if (_opacity_blocked) break;

        _opacity_blocked = true;
        opacity_sb->set_sensitive(true);
        opacity_adjustment->set_value(query.opacity.as_double() * 100);
        _opacity_blocked = false;
        break;
    }

// Now query stroke_width
    int result_sw = sp_desktop_query_style (_desktop, &query, QUERY_STYLE_PROPERTY_STROKEWIDTH);
    switch (result_sw) {
    case QUERY_STYLE_NOTHING:
        stroke_width->set_markup("");
        current_stroke_width = 0;
        break;
    case QUERY_STYLE_SINGLE:
    case QUERY_STYLE_MULTIPLE_AVERAGED:
    case QUERY_STYLE_MULTIPLE_SAME:
    {
        if (query.stroke_extensions.hairline) {
            stroke_width->set_markup(_("Hairline"));
            stroke_width->set_tooltip_text(_("Stroke width: Hairline"));
        } else {
            double w;
            if (_sw_unit) {
                w = Inkscape::Util::Quantity::convert(query.stroke_width.computed, "px", _sw_unit);
            } else {
                w = query.stroke_width.computed;
            }
            current_stroke_width = w;

            {
                gchar *str = g_strdup_printf(" %#.3g", w);
                if (str[strlen(str) - 1] == ',' || str[strlen(str) - 1] == '.') {
                    str[strlen(str)-1] = '\0';
                }
                stroke_width->set_markup(str);
                g_free (str);
            }
            {
                gchar *str = g_strdup_printf(_("Stroke width: %.5g%s%s"),
                                             w,
                                             _sw_unit? _sw_unit->abbr.c_str() : "px",
                                             (result_sw == QUERY_STYLE_MULTIPLE_AVERAGED)?
                                             _(" (averaged)") : "");
                stroke_width->set_tooltip_text(str);
                g_free (str);
            }
        }
        break;
    }
    default:
        break;
    }
}

void SelectedStyle::opacity_0()   {opacity_sb->set_value(0);}
void SelectedStyle::opacity_025() {opacity_sb->set_value(25);}
void SelectedStyle::opacity_05()  {opacity_sb->set_value(50);}
void SelectedStyle::opacity_075() {opacity_sb->set_value(75);}
void SelectedStyle::opacity_1()   {opacity_sb->set_value(100);}

void SelectedStyle::make_popup_opacity()
{
    _popup_opacity = std::make_unique<UI::Widget::PopoverMenu>(Gtk::PositionType::TOP);
    auto const add_item = [&] (Glib::ustring const &label, auto method) {
        _popup_opacity->append(*make_menu_item(label, sigc::mem_fun(*this, method)));
    };
    add_item(_("0% (Transparent)"), &SelectedStyle::opacity_0  );
    add_item("25%", &SelectedStyle::opacity_025);
    add_item("50%", &SelectedStyle::opacity_05 );
    add_item("75%", &SelectedStyle::opacity_075);
    add_item(_("100% (Opaque)"  ), &SelectedStyle::opacity_1  );
}

bool SelectedStyle::on_opacity_popup(PopupMenuOptionalClick)
{
    setPopover(_popup_opacity.get());
    _popup_opacity->popup_at_center(*opacity_sb);
    return true;
}

void SelectedStyle::on_opacity_changed(double value)
{
    g_return_if_fail(_desktop); // TODO this shouldn't happen!
    if (_opacity_blocked) {
        return;
    }
    _opacity_blocked = true;
    SPCSSAttr *css = sp_repr_css_attr_new ();
    sp_repr_css_set_property_double (css, "opacity", std::clamp(value / 100, 0.0, 1.0));
    sp_desktop_set_style (_desktop, css);
    sp_repr_css_attr_unref (css);
    DocumentUndo::maybeDone(_desktop->getDocument(), "fillstroke:opacity", RC_("Undo", "Change opacity"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    _opacity_blocked = false;
}

/* =============================================  RotateableSwatch  */

RotateableSwatch::RotateableSwatch(SelectedStyle *parent, guint mode)
    : fillstroke(mode)
    , parent(parent)
{
    set_name("RotatableSwatch");
}

RotateableSwatch::~RotateableSwatch() = default;

std::pair<double, double> RotateableSwatch::color_adjust(Colors::Color const &cc, double by, guint modifier)
{
    static int map[4] = {0,2,1,3};
    auto hsl = *cc.converted(Colors::Space::Type::HSL);
    int ch = map[modifier];
    double old = hsl[ch];

    hsl.set(ch, old + by * (by > 0 ? (1 - hsl[ch]) : hsl[ch]));
    hsl.normalize();
    double diff = hsl[ch] - old;
    hsl.convert(cc.getSpace());

    SPCSSAttr *css = sp_repr_css_attr_new ();
    if (modifier == 3) { // alpha
        sp_repr_css_set_property_double(css, (fillstroke == SS_FILL) ? "fill-opacity" : "stroke-opacity", hsl.getOpacity());
    } else {
        sp_repr_css_set_property_string(css, (fillstroke == SS_FILL) ? "fill" : "stroke", hsl.toString(false));
    }
    sp_desktop_set_style (parent->getDesktop(), css);
    sp_repr_css_attr_unref (css);
    return {old, diff};
}

void RotateableSwatch::do_motion(double by, guint modifier)
{
    if (parent->_mode[fillstroke] != SS_COLOR) {
        return;
    }

    if (!scrolling && modifier != cursor_state) {
        std::string cursor_filename = "adjust_hue.svg";
        if (modifier == 2) {
            cursor_filename = "adjust_saturation.svg";
        } else if (modifier == 1) {
            cursor_filename = "adjust_lightness.svg";
        } else if (modifier == 3) {
            cursor_filename = "adjust_alpha.svg";
        }
        set_svg_cursor(*this, cursor_filename);

        cursor_state = modifier;
    }

    if (!startcolor) {
        startcolor = parent->_thisselected[fillstroke];
    }

    auto ret = color_adjust(*startcolor, by, modifier);

    if (modifier == 3) { // alpha
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust alpha"), INKSCAPE_ICON("dialog-fill-and-stroke"));
        parent->getDesktop()->getTool()->message_context->setF(
            Inkscape::IMMEDIATE_MESSAGE,
            _("Adjusting <b>alpha</b>: was %.3g, now <b>%.3g</b> (diff %.3g); with <b>Ctrl</b> to adjust lightness, with <b>Shift</b> to adjust saturation, without modifiers to adjust hue"),
            ret.first, ret.first + ret.second, ret.second);

    } else if (modifier == 2) { // saturation
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust saturation"), INKSCAPE_ICON("dialog-fill-and-stroke"));
        parent->getDesktop()->getTool()->message_context->setF(
            Inkscape::IMMEDIATE_MESSAGE,
            _("Adjusting <b>saturation</b>: was %.3g, now <b>%.3g</b> (diff %.3g); with <b>Ctrl</b> to adjust lightness, with <b>Alt</b> to adjust alpha, without modifiers to adjust hue"),
            ret.first, ret.first + ret.second, ret.second);

    } else if (modifier == 1) { // lightness
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust lightness"), INKSCAPE_ICON("dialog-fill-and-stroke"));
        parent->getDesktop()->getTool()->message_context->setF(
            Inkscape::IMMEDIATE_MESSAGE,
            _("Adjusting <b>lightness</b>: was %.3g, now <b>%.3g</b> (diff %.3g); with <b>Shift</b> to adjust saturation, with <b>Alt</b> to adjust alpha, without modifiers to adjust hue"),
            ret.first, ret.first + ret.second, ret.second);

    } else { // hue
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust hue"), INKSCAPE_ICON("dialog-fill-and-stroke"));
        parent->getDesktop()->getTool()->message_context->setF(
            Inkscape::IMMEDIATE_MESSAGE,
            _("Adjusting <b>hue</b>: was %.3g, now <b>%.3g</b> (diff %.3g); with <b>Shift</b> to adjust saturation, with <b>Alt</b> to adjust alpha, with <b>Ctrl</b> to adjust lightness"),
            ret.first, ret.first + ret.second, ret.second);
    }
}

void RotateableSwatch::do_scroll(double by, guint modifier)
{
    do_motion(by/30.0, modifier);
    do_release(by/30.0, modifier);
}

void RotateableSwatch::do_release(double by, guint modifier)
{
    if (parent->_mode[fillstroke] != SS_COLOR)
        return;

    if (startcolor) {
        color_adjust(*startcolor, by, modifier);
    }

    if (cursor_state != -1) {
        set_cursor();
        cursor_state = -1;
    }

    if (modifier == 3) { // alpha
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust alpha"), INKSCAPE_ICON("dialog-fill-and-stroke"));

    } else if (modifier == 2) { // saturation
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust saturation"), INKSCAPE_ICON("dialog-fill-and-stroke"));

    } else if (modifier == 1) { // lightness
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust lightness"), INKSCAPE_ICON("dialog-fill-and-stroke"));

    } else { // hue
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust hue"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }

    if (!strcmp(undokey, "ssrot1")) {
        undokey = "ssrot2";
    } else {
        undokey = "ssrot1";
    }

    parent->getDesktop()->getTool()->message_context->clear();
    startcolor.reset();
}

/* =============================================  RotateableStrokeWidth  */

RotateableStrokeWidth::RotateableStrokeWidth(SelectedStyle *parent) :
    parent(parent),
    startvalue(0),
    startvalue_set(false),
    undokey("swrot1")
{
}

RotateableStrokeWidth::~RotateableStrokeWidth() = default;

double
RotateableStrokeWidth::value_adjust(double current, double by, guint /*modifier*/, bool final)
{
    double newval;
    // by is -1..1
    double max_f = 50;  // maximum width is (current * max_f), minimum - zero
    newval = current * (std::exp(std::log(max_f-1) * (by+1)) - 1) / (max_f-2);

    SPCSSAttr *css = sp_repr_css_attr_new ();
    if (final && newval < 1e-6) {
        // if dragged into zero and this is the final adjust on mouse release, delete stroke;
        // if it's not final, leave it a chance to increase again (which is not possible with "none")
        sp_repr_css_set_property (css, "stroke", "none");
    } else {
        newval = Inkscape::Util::Quantity::convert(newval, parent->_sw_unit, "px");
        sp_repr_css_set_property_double (css, "stroke-width", newval);
    }

    sp_desktop_set_style (parent->getDesktop(), css);
    sp_repr_css_attr_unref (css);
    return newval - current;
}

void RotateableStrokeWidth::do_motion(double by, guint modifier)
{
    // if this is the first motion after a mouse grab, remember the current width
    if (!startvalue_set) {
        startvalue = parent->current_stroke_width;
        // if it's 0, adjusting (which uses multiplication) will not be able to change it, so we
        // cheat and provide a non-zero value
        if (startvalue == 0)
            startvalue = 1;
        startvalue_set = true;
    }

    if (modifier == 3) { // Alt, do nothing
    } else {
        double diff = value_adjust(startvalue, by, modifier, false);
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust stroke width"), INKSCAPE_ICON("dialog-fill-and-stroke"));
        parent->getDesktop()->getTool()->message_context->setF(Inkscape::IMMEDIATE_MESSAGE, _("Adjusting <b>stroke width</b>: was %.3g, now <b>%.3g</b> (diff %.3g)"), startvalue, startvalue + diff, diff);
    }
}

void RotateableStrokeWidth::do_release(double by, guint modifier)
{
    if (modifier == 3) { // do nothing

    } else {
        value_adjust(startvalue, by, modifier, true);
        startvalue_set = false;
        DocumentUndo::maybeDone(parent->getDesktop()->getDocument(), undokey, RC_("Undo", "Adjust stroke width"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }

    if (!strcmp(undokey, "swrot1")) {
        undokey = "swrot2";
    } else {
        undokey = "swrot1";
    }
    parent->getDesktop()->getTool()->message_context->clear();
}

void RotateableStrokeWidth::do_scroll(double by, guint modifier)
{
    do_motion(by/10.0, modifier);
    do_release(by / 10.0, modifier);
    startvalue_set = false;
}

Dialog::FillAndStroke *get_fill_and_stroke_panel(SPDesktop *desktop)
{
    desktop->getContainer()->new_dialog("FillStroke");
    return dynamic_cast<Dialog::FillAndStroke *>(desktop->getContainer()->get_dialog("FillStroke"));
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
