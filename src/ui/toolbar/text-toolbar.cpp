// SPDX-License-Identifier: GPL-2.0-or-later
/** * @file Text toolbar
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
 *   Tavmjong Bah <tavmjong@free.fr>
 *   Vaibhav Malik <vaibhavmalik2018@gmail.com>
 *
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2003 MenTaLguY
 * Copyright (C) 2001-2002 Ximian, Inc.
 * Copyright (C) 1999-2013 authors
 * Copyright (C) 2017 Tavmjong Bah
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "text-toolbar.h"

#include <ranges>
#include <gtkmm/checkbutton.h>
#include <gtkmm/listbox.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>

#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "libnrtype/font-lister.h"
#include "object/sp-flowdiv.h"
#include "object/sp-flowtext.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "selection-chemistry.h"
#include "selection.h"
#include "svg/css-ostringstream.h"
#include "ui/builder-utils.h"
#include "ui/dialog/dialog-container.h"
#include "ui/icon-names.h"
#include "ui/tools/text-tool.h"
#include "ui/util.h"
#include "ui/widget/combo-box-entry-tool-item.h"
#include "ui/widget/font-size-selector.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-tracker.h"
#include "util/font-collections.h"
#include "widgets/style-utils.h"

using Inkscape::DocumentUndo;
using Inkscape::Util::Unit;
using Inkscape::Util::Quantity;
using Inkscape::UI::Widget::UnitTracker;

constexpr bool DEBUG_TEXT = false;

namespace Inkscape::UI::Toolbar {
namespace {

bool is_relative(Unit const *unit)
{
    return unit->abbr == "" || unit->abbr == "em" || unit->abbr == "ex" || unit->abbr == "%";
}

bool is_relative(SPCSSUnit const unit)
{
    return unit == SP_CSS_UNIT_NONE || unit == SP_CSS_UNIT_EM || unit == SP_CSS_UNIT_EX || unit == SP_CSS_UNIT_PERCENT;
}

// Set property for object, but unset all descendents
// Should probably be moved to desktop_style.cpp
void recursively_set_properties(SPObject *object, SPCSSAttr *css, bool unset_descendents = true)
{
    object->changeCSS(css, "style");

    auto css_unset = sp_repr_css_attr_unset_all(css);
    for (auto i : object->childList(false)) {
        recursively_set_properties(i, unset_descendents ? css_unset : css);
    }
    sp_repr_css_attr_unref(css_unset);
}


// TODO: possibly share with font-selector by moving most code to font-lister (passing family name)
void sp_text_toolbox_select_cb(Gtk::Entry const &entry)
{
    auto const family = entry.get_buffer()->get_text();
    // std::cout << "text_toolbox_missing_font_cb: selecting: " << family << std::endl;

    // Get all items with matching font-family set (not inherited!).
    std::vector<SPItem *> selectList;

    SPDesktop *desktop = SP_ACTIVE_DESKTOP;
    SPDocument *document = desktop->getDocument();
    for (auto item : get_all_items(document->getRoot(), desktop, false, false, true) | std::views::reverse) {
        auto style = item->style;
        if (!style) {
            continue;
        }

        Glib::ustring family_style;
        if (style->font_family.set) {
            family_style = style->font_family.value();
            // std::cout << " family style from font_family: " << family_style << std::endl;
        } else if (style->font_specification.set) {
            family_style = style->font_specification.value();
            // std::cout << " family style from font_spec: " << family_style << std::endl;
        }

        if (family_style.compare(family) == 0) {
            // std::cout << "   found: " << item->getId() << std::endl;
            selectList.push_back(item);
        }
    }

    // Update selection
    auto selection = desktop->getSelection();
    selection->clear();
    // std::cout << "   list length: " << selectList.size() << std::endl;
    selection->setList(selectList);
}

} // namespace

TextToolbar::TextToolbar()
    : TextToolbar{create_builder("toolbar-text.ui")}
{}

TextToolbar::TextToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "text-toolbar")}
    , _tracker{std::make_unique<UnitTracker>(Util::UNIT_TYPE_LINEAR)}
    , _font_collections_list{get_widget<Gtk::ListBox>(builder, "_font_collections_list")}
    , _reset_button{get_widget<Gtk::Button>(builder, "reset_btn")}
    , _line_height_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_line_height_item")}
    , _all_caps_btn{get_widget<Gtk::ToggleButton>(builder, "_all_caps_btn")}
    , _small_caps_btn{get_widget<Gtk::ToggleButton>(builder, "_small_caps_btn")}
    , _superscript_btn{get_widget<Gtk::ToggleButton>(builder, "_superscript_btn")}
    , _subscript_btn{get_widget<Gtk::ToggleButton>(builder, "_subscript_btn")}
    , _word_spacing_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_word_spacing_item")}
    , _letter_spacing_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_letter_spacing_item")}
    , _dx_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_dx_item")}
    , _dy_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_dy_item")}
    , _rotation_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_rotation_item")}
    , _font_size_item{get_widget<UI::Widget::FontSizeSelector>(builder, "font_size")}
{
    // Line height unit tracker.
    auto const &unit_table = Util::UnitTable::get();
    auto lines = Unit::create("lines");
    _tracker->prependUnit(lines.get());
    _tracker->addUnit(unit_table.unit("%"));
    _tracker->addUnit(unit_table.unit("em"));
    _tracker->addUnit(unit_table.unit("ex"));
    _tracker->setActiveUnit(lines.get());

    // Setup the spin buttons.
    // TODO: Take care of the line-height pref settings.
    setup_derived_spin_button(_line_height_item, "line-height", 1.25, &TextToolbar::lineheight_value_changed);
    setup_derived_spin_button(_letter_spacing_item, "letterspacing", 0.0, &TextToolbar::letterspacing_value_changed);
    setup_derived_spin_button(_word_spacing_item, "wordspacing", 0.0, &TextToolbar::wordspacing_value_changed);
    setup_derived_spin_button(_dx_item, "dx", 0.0, &TextToolbar::dx_value_changed);
    setup_derived_spin_button(_dy_item, "dy", 0.0, &TextToolbar::dy_value_changed);
    setup_derived_spin_button(_rotation_item, "rotation", 0.0, &TextToolbar::rotation_value_changed);

    _line_height_item.set_custom_numeric_menu_data({
        {1, _("Single spaced")},
        {1.25, _("Default")},
        {1.5, ""},{
        2, _("Double spaced")}
    });
    _letter_spacing_item.set_custom_numeric_menu_data({{0, C_("Text tool", "Normal")}});
    _word_spacing_item.set_custom_numeric_menu_data({{0, C_("Text tool", "Normal")}});
    _dx_item.set_custom_numeric_menu_data({{0, ""}});
    _dy_item.set_custom_numeric_menu_data({{0, ""}});
    _rotation_item.set_custom_numeric_menu_data({
        {-90, ""},
        {-45, ""},
        {-30, ""},
        {-15, ""},
        {  0, ""},
        { 15, ""},
        { 30, ""},
        { 45, ""},
        { 90, ""}
    });

    // Configure alignment mode buttons
    configure_mode_buttons(_alignment_buttons, get_widget<Gtk::Box>(builder, "alignment_buttons_box"), "align_mode",
                           &TextToolbar::align_mode_changed);
    configure_mode_buttons(_writing_buttons, get_widget<Gtk::Box>(builder, "writing_buttons_box"), "writing_mode",
                           &TextToolbar::writing_mode_changed);
    configure_mode_buttons(_orientation_buttons, get_widget<Gtk::Box>(builder, "orientation_buttons_box"),
                           "orientation_mode", &TextToolbar::orientation_changed);
    configure_mode_buttons(_direction_buttons, get_widget<Gtk::Box>(builder, "direction_buttons_box"),
                           "direction_mode", &TextToolbar::direction_changed);

    auto const fontlister = FontLister::get_instance();

    font_count_changed_connection = fontlister->connectUpdate([this] {
        auto const [all_fonts, _] = FontLister::get_instance()->get_font_count_label();
        _reset_button.set_sensitive(!all_fonts);
    });

    // Font family
    _font_family_item = Gtk::make_managed<UI::Widget::ComboBoxEntryToolItem>(
        "TextFontFamilyAction",
        _("Font Family"),
        _("Select Font Family (Alt-X to access)"),
        fontlister->get_font_list(),
        -1, // Entry width
        50, // Extra list width
        &font_lister_cell_data_func2, // Cell layout
        &font_lister_separator_func
    );

    _font_family_item->popup_enable(); // Enable entry completion
    _font_family_item->set_info(_("Select all text with this font-family")); // Show selection icon
    _font_family_item->set_info_cb(&sp_text_toolbox_select_cb);
    _font_family_item->set_warning(_("Font not found on system")); // Show icon w/ tooltip if font missing
    _font_family_item->set_warning_cb(&sp_text_toolbox_select_cb);
    _font_family_item->focus_on_click(false);
    _font_family_item->connectChanged([this] { fontfamily_value_changed(); });
    _font_family_item->setDefocusTarget(this);

    get_widget<Gtk::Box>(builder, "font_list_box").append(*_font_family_item);

    // Font styles
    _font_style_item = Gtk::manage(new UI::Widget::ComboBoxEntryToolItem(
        "TextFontStyleAction",
        _("Font Style"),
        _("Font style"),
        fontlister->get_style_list(),
        12, // Width in characters
        0, // Extra list width
        {}, // Cell layout
        {} // Separator
    ));

    _font_style_item->connectChanged([this] { fontstyle_value_changed(); });
    _font_style_item->focus_on_click(false);
    _font_style_item->setDefocusTarget(this);

    get_widget<Gtk::Box>(builder, "styles_list_box").append(*_font_style_item);

    // Font size
    _font_size_item.signal_size_changed().connect([this](auto size, int unit) { fontsize_value_changed(size); });

    // Line height units
    _line_height_units_item = _tracker->create_unit_dropdown();
    _line_height_units_item->signal_changed().connect(sigc::mem_fun(*this, &TextToolbar::lineheight_unit_changed));
    get_widget<Gtk::Box>(builder, "line_height_unit_box").append(*_line_height_units_item);

    // PROOF: all caps and small caps; superscript and subscript. They show what the
    // selection is (_selectionChanged), or what text typed next will be.
    _all_caps_btn.signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &TextToolbar::caps_changed), 0));
    _small_caps_btn.signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &TextToolbar::caps_changed), 1));
    _superscript_btn.signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &TextToolbar::script_changed), 0));
    _subscript_btn.signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &TextToolbar::script_changed), 1));

    // Font collections signals.
    auto const font_collections = FontCollections::get();

    get_widget<Gtk::Popover>(builder, "font_collections_popover")
        .signal_show()
        .connect([this] { display_font_collections(); }, false);

    // This signal will keep both the Text and Font dialog and
    // TextToolbar popovers in sync with each other.
    fc_changed_selection = font_collections->connect_selection_update([this] { display_font_collections(); });

    // This one will keep the text toolbar Font Collections
    // updated in case of any change in the Font Collections.
    fc_update = font_collections->connect_update([this] { display_font_collections(); });

    get_widget<Gtk::Button>(builder, "fc_dialog_btn").signal_clicked().connect([this]() {
        TextToolbar::on_fcm_button_pressed();
    });

    _reset_button.signal_clicked().connect([this]() { TextToolbar::on_reset_button_pressed(); });

    _initMenuBtns();

    // Defocus font-size spinbutton on "Esc".
    set_defocus_target(this, this);
}

TextToolbar::~TextToolbar() = default;

void TextToolbar::setDesktop(SPDesktop *desktop)
{
    if (_desktop) {
        _selection_changed_conn.disconnect();
        _selection_modified_conn.disconnect();
        _cursor_moved_conn.disconnect();
        _fonts_updated_conn.disconnect();
    }

    Toolbar::setDesktop(desktop);

    if (_desktop) {
        auto fontlister = FontLister::get_instance();
        fontlister->update_font_list(_desktop->getDocument());

        // Keep font list up to date with document fonts when refreshed.
        _fonts_updated_conn = fontlister->connectNewFonts([=] {
            fontlister->update_font_list(desktop->getDocument());
        });

        auto sel = desktop->getSelection();
        _selection_changed_conn = sel->connectChangedFirst(sigc::mem_fun(*this, &TextToolbar::_selectionChanged));
        _selection_modified_conn = sel->connectModifiedFirst(sigc::mem_fun(*this, &TextToolbar::_selectionModified));
        _cursor_moved_conn = desktop->connect_text_cursor_moved([this] (Tools::TextTool *tool) {
            _cursorMoved(tool);
        });
        _sub_active_item.reset();
        _cursor_numbers = 0;
        _selectionChanged(sel);
    }
}

void TextToolbar::setup_derived_spin_button(UI::Widget::SpinButton &btn, Glib::ustring const &name,
                                            double default_value, ValueChangedMemFun value_changed_mem_fun)
{
    auto const path = "/tools/text/" + name;
    auto const val = Preferences::get()->getDouble(path, default_value);
    auto const adj = btn.get_adjustment();
    adj->set_value(val);
    adj->signal_value_changed().connect(sigc::mem_fun(*this, value_changed_mem_fun));
    btn.setDefocusTarget(this);
}

void TextToolbar::configure_mode_buttons(std::vector<Gtk::ToggleButton *> &buttons, Gtk::Box &box,
                                         Glib::ustring const &name, ModeChangedMemFun mode_changed_mem_fun)
{
    int btn_index = 0;

    for (auto &item : children(box)) {
        auto &btn = dynamic_cast<Gtk::ToggleButton &>(item);
        buttons.push_back(&btn);
        btn.signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, mode_changed_mem_fun), btn_index++));
    }

    // Set the active button after all the buttons have been pushed.
    auto const path = "/tools/text/" + name;
    int const active_button_index = Preferences::get()->getInt(path, 0);
    buttons[active_button_index < buttons.size() ? active_button_index : 0]->set_active(true);
}

void TextToolbar::text_outer_set_style(SPCSSAttr *css)
{
    // Calling sp_desktop_set_style will result in a call to TextTool::_styleSet() which
    // will set the style on selected text inside the <text> element. If we want to set
    // the style on the outer <text> objects we need to bypass this call.
    if (!_outer) {
        return;
    }

    // Apply css to parent text objects directly.
    for (auto item : _desktop->getSelection()->items()) {
        if (is<SPText>(item) || is<SPFlowtext>(item)) {
            // Scale by inverse of accumulated parent transform
            SPCSSAttr *css_set = sp_repr_css_attr_new();
            sp_repr_css_merge(css_set, css);
            auto const local = item->i2doc_affine();
            double const ex = local.descrim();
            if (ex != 0.0 && ex != 1.0) {
                sp_css_attr_scale(css_set, 1 / ex);
            }
            recursively_set_properties(item, css_set);
            sp_repr_css_attr_unref(css_set);
        }
    }
}

void TextToolbar::fontfamily_value_changed()
{
    if constexpr (DEBUG_TEXT) {
        std::cout << std::endl;
        std::cout << "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM" << std::endl;
        std::cout << "sp_text_fontfamily_value_changed: " << std::endl;
    }

     // quit if run by the _changed callbacks
    if (_freeze) {
        if constexpr (DEBUG_TEXT) {
            std::cout << "sp_text_fontfamily_value_changed: frozen... return" << std::endl;
            std::cout << "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM\n" << std::endl;
        }
        return;
    }
    _freeze = true;

    Glib::ustring new_family = _font_family_item->get_active_text();
    css_font_family_unquote( new_family ); // Remove quotes around font family names.

    // TODO: Think about how to handle handle multiple selections. While
    // the font-family may be the same for all, the styles might be different.
    // See: TextEdit::onApply() for example of looping over selected items.
    auto fontlister = FontLister::get_instance();
    if constexpr (DEBUG_TEXT) {
        std::cout << "  Old family: " << fontlister->get_font_family() << std::endl;
        std::cout << "  New family: " << new_family << std::endl;
        std::cout << "  Old active: " << fontlister->get_font_family_row() << std::endl;
        // std::cout << "  New active: " << act->active << std::endl;
    }
    if (new_family.compare(fontlister->get_font_family()) != 0) {
        // Changed font-family

        if (_font_family_item->get_active() == -1) {
            // New font-family, not in document, not on system (could be fallback list)
            fontlister->insert_font_family(new_family);

            // This just sets a variable in the ComboBoxEntryAction object...
            // shouldn't we also set the actual active row in the combobox?
            _font_family_item->set_active(0); // New family is always at top of list.
        }

        fontlister->set_font_family( _font_family_item->get_active() );
        // active text set in sp_text_toolbox_selection_changed()

        auto css = sp_repr_css_attr_new();
        fontlister->fill_css(css);

        if (mergeDefaultStyle(css)) {
            // If there is a selection, update
            DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change font family"), INKSCAPE_ICON("draw-text"));
        }
        sp_repr_css_attr_unref(css);
    }

    // unfreeze
    _freeze = false;

    fontlister->add_document_fonts_at_top(_desktop->getDocument());

    if constexpr (DEBUG_TEXT) {
        std::cout << "sp_text_toolbox_fontfamily_changes: exit"  << std::endl;
        std::cout << "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM" << std::endl;
        std::cout << std::endl;
    }
}

void TextToolbar::fontsize_value_changed(double size)
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    // Set css font size.
    auto css = sp_repr_css_attr_new();
    CSSOStringStream osfs;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    int unit = prefs->getInt("/options/font/unitType", SP_CSS_UNIT_PT);
    if (prefs->getBool("/options/font/textOutputPx", true)) {
        osfs << sp_style_css_size_units_to_px(size, unit) << sp_style_get_css_unit_string(SP_CSS_UNIT_PX);
    } else {
        osfs << size << sp_style_get_css_unit_string(unit);
    }
    sp_repr_css_set_property (css, "font-size", osfs.str().c_str());
    double factor = size / selection_fontsize;

    // Apply font size to selected objects.
    text_outer_set_style(css);

    auto const unit_lh = _tracker->getActiveUnit();
    if (!is_relative(unit_lh) && _outer) {
        double lineheight = _line_height_item.get_adjustment()->get_value();
        _freeze = false;
        _line_height_item.get_adjustment()->set_value(lineheight * factor);
        _freeze = true;
    }

    if (mergeDefaultStyle(css)) {
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:size", RC_("Undo", "Text: Change font size"), INKSCAPE_ICON("draw-text"));
    }

    sp_repr_css_attr_unref(css);

    _freeze = false;
}

void TextToolbar::fontstyle_value_changed()
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    Glib::ustring new_style = _font_style_item->get_active_text();

    auto const fontlister = FontLister::get_instance();

    if (new_style.compare(fontlister->get_font_style()) != 0) {

        fontlister->set_font_style(new_style);
        // active text set in sp_text_toolbox_seletion_changed()

        auto css = sp_repr_css_attr_new();
        fontlister->fill_css(css);

        if (mergeDefaultStyle(css)) {
            DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change font style"), INKSCAPE_ICON("draw-text"));
        }

        sp_repr_css_attr_unref(css);
    }

    _freeze = false;
}

/**
 * PROOF: All Caps (mode 0) and Small Caps (mode 1), as in Illustrator's Character panel.
 *
 * The text keeps what was typed. All caps is text-transform:uppercase; small caps is
 * font-variant-caps:small-caps, which the layout sets with the font's own small capitals
 * or makes of its capitals. Turning one on turns the other off.
 */
void TextToolbar::caps_changed(int mode)
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    bool const on = (mode == 0 ? _all_caps_btn : _small_caps_btn).get_active();
    (mode == 0 ? _small_caps_btn : _all_caps_btn).set_active(false);

    // Both are said either way: a span inside text that has one must be able to have none.
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "text-transform", on && mode == 0 ? "uppercase" : "none");
    sp_repr_css_set_property(css, "font-variant-caps", on && mode == 1 ? "small-caps" : "normal");
    if (mergeDefaultStyle(css)) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change capitals"), INKSCAPE_ICON("draw-text"));
    }
    sp_repr_css_attr_unref(css);

    _freeze = false;
}

/**
 * PROOF: Superscript (mode 0) and Subscript (mode 1), as in Illustrator's Character panel.
 *
 * The type says where it stands (font-variant-position) and keeps its size; the layout
 * sets it smaller and off the baseline by what the document says (type-settings.h).
 * Turning one on turns the other off.
 */
void TextToolbar::script_changed(int mode)
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    bool const on = (mode == 0 ? _superscript_btn : _subscript_btn).get_active();
    (mode == 0 ? _subscript_btn : _superscript_btn).set_active(false);

    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "font-variant-position", !on ? "normal" : mode == 0 ? "super" : "sub");

    // Text that Inkscape made a superscript or subscript says so in its baseline shift, at
    // 65% of the size. It is put back on its line at its size, to be the one thing or neither.
    SPStyle query(_desktop->getDocument());
    int const result_baseline = sp_desktop_query_style(_desktop, &query, QUERY_STYLE_PROPERTY_BASELINES);
    if (result_baseline != QUERY_STYLE_NOTHING && query.baseline_shift.set &&
        query.baseline_shift.type == SP_BASELINE_SHIFT_LITERAL &&
        (query.baseline_shift.literal == SP_CSS_BASELINE_SHIFT_SUPER || query.baseline_shift.literal == SP_CSS_BASELINE_SHIFT_SUB)) {
        sp_repr_css_set_property(css, "baseline-shift", "baseline");
        sp_repr_css_set_property(css, "font-size", "");
    }

    if (mergeDefaultStyle(css)) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change superscript or subscript"), INKSCAPE_ICON("draw-text"));
    }
    sp_repr_css_attr_unref(css);

    _freeze = false;
}

// Any text alignment in, right/center/left/justify out.
SPCSSTextAlign text_align_to_side(SPCSSTextAlign const &align, SPCSSDirection const &direction)
{
    auto new_align = align;

    if ((align == SP_CSS_TEXT_ALIGN_START && direction == SP_CSS_DIRECTION_LTR) ||
        (align == SP_CSS_TEXT_ALIGN_END   && direction == SP_CSS_DIRECTION_RTL)) {
        new_align = SP_CSS_TEXT_ALIGN_LEFT;
    }

    if ((align == SP_CSS_TEXT_ALIGN_START && direction == SP_CSS_DIRECTION_RTL) ||
        (align == SP_CSS_TEXT_ALIGN_END   && direction == SP_CSS_DIRECTION_LTR)) {
        new_align = SP_CSS_TEXT_ALIGN_RIGHT;
    }

    return new_align;
}

SPCSSAttr *TextToolbar::cssForTextAlignment(int align_mode, int direction)
{
    auto css = sp_repr_css_attr_new();

    if ((align_mode == 0 && direction == SP_CSS_DIRECTION_LTR) ||
        (align_mode == 2 && direction == SP_CSS_DIRECTION_RTL)) {
        sp_repr_css_set_property(css, "text-anchor", "start");
        sp_repr_css_set_property(css, "text-align",  "start");
    }
    if ((align_mode == 0 && direction == SP_CSS_DIRECTION_RTL) ||
        (align_mode == 2 && direction == SP_CSS_DIRECTION_LTR)) {
        sp_repr_css_set_property(css, "text-anchor", "end");
        sp_repr_css_set_property(css, "text-align",  "end");
    }
    if (align_mode == 1) {
        sp_repr_css_set_property(css, "text-anchor", "middle");
        sp_repr_css_set_property(css, "text-align",  "center");
    }
    if (align_mode == 3) {
        sp_repr_css_set_property(css, "text-anchor", "start");
        sp_repr_css_set_property(css, "text-align",  "justify");
    }

    return css;
}

void TextToolbar::align_mode_changed(int align_mode)
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    auto prefs = Preferences::get();
    prefs->setInt("/tools/text/align_mode", align_mode);

    // Move the alignment point of all texts to preserve the same bbox.
    // Because align_mode interpretation depends on text direction, we don't simply us
    // mergeDefaultStyle like other "button changed" handlers. We need to query each selection.
    bool changed = false;
    Selection *selection = _desktop->getSelection();
    for (auto i : selection->items()) {
        auto text = cast<SPText>(i);
        // auto flowtext = cast<SPFlowtext>(i);
        if (text) {

            // Below, variable names suggest horizontal move, but we check the writing direction
            // and move in the corresponding axis.
            Geom::Dim2 axis;
            unsigned writing_mode = text->style->writing_mode.value;
            if (writing_mode == SP_CSS_WRITING_MODE_LR_TB || writing_mode == SP_CSS_WRITING_MODE_RL_TB) {
                axis = Geom::X;
            } else {
                axis = Geom::Y;
            }

            // Find current text inline-size (width for horizontal text, height for vertical text.
            Geom::OptRect bbox = text->get_frame(); // 'inline-size' or rectangle frame.
            if (!bbox) {
                bbox = text->geometricBounds();
            }
            if (!bbox) {
                continue; // No bounding box, no joy!
            }
            double width = bbox->dimensions()[axis];

            double move = 0;
            auto direction = text->style->direction.value;

            // Switch alignment point
            auto old_side = text_align_to_side(text->style->text_align.value, direction);
            switch (old_side) {
                case SP_CSS_TEXT_ALIGN_LEFT:
                    switch (align_mode) {
                        case 0:
                            break;
                        case 1:
                            move = width/2;
                            break;
                        case 2:
                            move = width;
                            break;
                        case 3:
                            break; // Justify
                        default:
                            std::cerr << "TextToolbar::align_mode_changed() Unexpected value (mode): " << align_mode << std::endl;
                    }
                    break;
                case SP_CSS_TEXT_ALIGN_CENTER:
                    switch (align_mode) {
                        case 0:
                            move = -width/2;
                            break;
                        case 1:
                            break;
                        case 2:
                            move = width/2;
                            break;
                        case 3:
                            break; // Justify
                        default:
                            std::cerr << "TextToolbar::align_mode_changed() Unexpected value (mode): " << align_mode << std::endl;
                    }
                    break;
                case SP_CSS_TEXT_ALIGN_RIGHT:
                    switch (align_mode) {
                        case 0:
                            move = -width;
                            break;
                        case 1:
                            move = -width/2;
                            break;
                        case 2:
                            break;
                        case 3:
                            break; // Justify
                        default:
                            std::cerr << "TextToolbar::align_mode_changed() Unexpected value (mode): " << align_mode << std::endl;
                    }
                    break;
                case SP_CSS_TEXT_ALIGN_JUSTIFY:
                    // Do nothing
                    break;
                default:
                    std::cerr << "TextToolbar::align_mode_changed() Unexpected value (old_side): " << old_side << std::endl;
            }

            if (std::abs(move) > 0) {
                changed = true;
            }

            auto css = cssForTextAlignment(align_mode, direction);
            text->changeCSS(css, "style");
            sp_repr_css_attr_unref(css);

            Geom::Point XY = text->attributes.firstXY();
            if (axis == Geom::X) {
                XY = XY + Geom::Point (move, 0);
            } else {
                XY = XY + Geom::Point (0, move);
            }
            text->attributes.setFirstXY(XY);
            text->updateRepr();
            text->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        } // if(text)
    }

    // Update the default style css if nothing is selected, based off of the default direction.
    if (selection->isEmpty()) {
        auto direction = prefs->getInt("/tools/text/direction_mode", SP_CSS_DIRECTION_LTR);
        auto css = cssForTextAlignment(align_mode, direction);
        changed = mergeDefaultStyle(css);
        sp_repr_css_attr_unref(css);
    }

    if (changed) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change alignment"), INKSCAPE_ICON("draw-text"));
    }

    onDefocus();

    _freeze = false;
}

void TextToolbar::writing_mode_changed(int mode)
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    Preferences::get()->setInt("/tools/text/writing_mode", mode);

    auto css = sp_repr_css_attr_new();
    switch (mode) {
        case 0:
            sp_repr_css_set_property (css, "writing-mode", "lr-tb");
            break;
        case 1:
            sp_repr_css_set_property (css, "writing-mode", "tb-rl");
            break;
        case 2:
            sp_repr_css_set_property (css, "writing-mode", "vertical-lr");
            break;
        default:
            break;
    }

    if (mergeDefaultStyle(css)) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change writing mode"), INKSCAPE_ICON("draw-text"));
    }
    sp_repr_css_attr_unref(css);

    onDefocus();

    _freeze = false;
}

void TextToolbar::orientation_changed(int mode)
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    Preferences::get()->setInt("/tools/text/orientation_mode", mode);

    auto css = sp_repr_css_attr_new();
    switch (mode) {
        case 0:
            sp_repr_css_set_property(css, "text-orientation", "auto");
            break;
        case 1:
            sp_repr_css_set_property(css, "text-orientation", "upright");
            break;
        case 2:
            sp_repr_css_set_property(css, "text-orientation", "sideways");
            break;
        default:
            break;
    }

    if (mergeDefaultStyle(css)) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change orientation"), INKSCAPE_ICON("draw-text"));
    }
    sp_repr_css_attr_unref (css);
    onDefocus();

    _freeze = false;
}

void TextToolbar::direction_changed(int mode)
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    auto prefs = Preferences::get();
    auto has_new_default_direction = prefs->getInt("/tools/text/direction_mode", SP_CSS_DIRECTION_LTR) != mode;
    prefs->setInt("/tools/text/direction_mode", mode);

    auto css = sp_repr_css_attr_new();
    switch (mode) {
        case 0:
            sp_repr_css_set_property (css, "direction", "ltr");
            break;
        case 1:
            sp_repr_css_set_property (css, "direction", "rtl");
            break;
        default:
            break;
    }

    // Because align_mode interpretation depends on direction_mode, we update the global default
    // style's align_mode if nothing is selected (just like a selected text's style would
    // swap alignment as direction changes).
    if (_desktop->getSelection()->isEmpty() && has_new_default_direction) {
        auto alignment = prefs->getInt("/tools/text/align_mode", 0);
        auto new_alignment = alignment;
        if (alignment == 0) {
            new_alignment = 2;
        } else if (alignment == 2) {
            new_alignment = 0;
        }
        if (alignment != new_alignment) {
            prefs->setInt("/tools/text/align_mode", new_alignment);
            _alignment_buttons[new_alignment]->set_active(true);
        }
    }

    if (mergeDefaultStyle(css)) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Text: Change direction"), INKSCAPE_ICON("draw-text"));
    }
    sp_repr_css_attr_unref(css);

    onDefocus();

    _freeze = false;
}

void TextToolbar::lineheight_value_changed()
{
    // quit if run by the _changed callbacks or is not text tool
    if (_freeze || !SP_TEXT_CONTEXT(_desktop->getTool())) {
        return;
    }

    _freeze = true;

    // Get user selected unit and save as preference
    auto const unit = _tracker->getActiveUnit();

    // This nonsense is to get SP_CSS_UNIT_xx value corresponding to unit so
    // we can save it (allows us to adjust line height value when unit changes).

    // Set css line height.
    auto css = sp_repr_css_attr_new();
    CSSOStringStream osfs;
    if (is_relative(unit)) {
        osfs << _line_height_item.get_adjustment()->get_value() << unit->abbr;
    } else {
        // Inside SVG file, always use "px" for absolute units.
        osfs << Quantity::convert(_line_height_item.get_adjustment()->get_value(), unit, "px") << "px";
    }

    sp_repr_css_set_property (css, "line-height", osfs.str().c_str());

    auto selection = _desktop->getSelection();
    auto itemlist = selection->items();
    if (_outer) {
        // Special else makes this different from other uses of text_outer_set_style
        text_outer_set_style(css);
    } else {
        auto parent = itemlist.front();
        SPStyle *parent_style = parent->style;
        SPCSSAttr *parent_cssatr = sp_css_attr_from_style(parent_style, SP_STYLE_FLAG_IFSET);
        Glib::ustring parent_lineheight = sp_repr_css_property(parent_cssatr, "line-height", "1.25");
        SPCSSAttr *cssfit = sp_repr_css_attr_new();
        sp_repr_css_set_property(cssfit, "line-height", parent_lineheight.c_str());
        double minheight = 0;
        if (parent_style) {
            minheight = parent_style->line_height.computed;
        }
        if (minheight) {
            for (auto i : parent->childList(false)) {
                auto child = cast<SPItem>(i);
                if (!child) {
                    continue;
                }
                recursively_set_properties(child, cssfit);
            }
        }
        sp_repr_css_set_property(cssfit, "line-height", "0");
        parent->changeCSS(cssfit, "style");
        subselection_wrap_toggle(true);
        sp_desktop_set_style(_desktop, css, true, true);
        subselection_wrap_toggle(false);
        sp_repr_css_attr_unref(cssfit);
    }
    // Only need to save for undo if a text item has been changed.
    itemlist = selection->items();
    bool modified = false;
    for (auto item : itemlist) {
        if (is<SPText>(item) || is<SPFlowtext>(item)) {
            modified = true;
            break;
        }
    }

    // Save for undo
    if (modified) {
        // Call ensureUpToDate() causes rebuild of text layout (with all proper style
        // cascading, etc.). For multi-line text with sodipodi::role="line", we must explicitly
        // save new <tspan> 'x' and 'y' attribute values by calling updateRepr().
        // Partial fix for bug #1590141.

        _desktop->getDocument()->ensureUpToDate();
        for (auto item : itemlist) {
            if (is<SPText>(item) || is<SPFlowtext>(item)) {
                item->updateRepr();
            }
        }
        if (!_outer) {
            prepare_inner();
        }
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:line-height", RC_("Undo", "Text: Change line-height"), INKSCAPE_ICON("draw-text"));
    }

    mergeDefaultStyle(css);

    sp_repr_css_attr_unref(css);

    _freeze = false;
}

/**
 * Merge the style into either the tool or the desktop style depending on
 * which one the user has decided to use in the preferences.
 *
 * @returns true if style was set to an object.
 */
bool TextToolbar::mergeDefaultStyle(SPCSSAttr *css)
{
    // If no selected objects, set default.
    auto query = SPStyle{_desktop->getDocument()};
    int result_numbers = sp_desktop_query_style(_desktop, &query, QUERY_STYLE_PROPERTY_FONTNUMBERS);
    if (result_numbers == QUERY_STYLE_NOTHING) {
        Preferences::get()->mergeStyle("/tools/text/style", css);
    }
    // This updates the global style
    sp_desktop_set_style(_desktop, css, true, true);
    return result_numbers != QUERY_STYLE_NOTHING;
}

void TextToolbar::lineheight_unit_changed()
{
    // quit if run by the _changed callbacks or is not text tool
    if (_freeze || !SP_TEXT_CONTEXT(_desktop->getTool())) {
        return;
    }
    _freeze = true;

    // Get old saved unit
    int old_unit = _lineheight_unit;

    // Get user selected unit and save as preference
    auto const unit = _tracker->getActiveUnit();

    // This nonsense is to get SP_CSS_UNIT_xx value corresponding to unit.
    SPILength temp_length;
    CSSOStringStream temp_stream;
    temp_stream << 1 << unit->abbr;
    temp_length.read(temp_stream.str().c_str());
    Preferences::get()->setInt("/tools/text/lineheight/display_unit", temp_length.unit);
    if (old_unit == temp_length.unit) {
        _freeze = false;
        return;
    } else {
        _lineheight_unit = temp_length.unit;
    }

    // Read current line height value
    auto line_height_adj = _line_height_item.get_adjustment();
    double line_height = line_height_adj->get_value();
    SPDesktop *desktop = _desktop;
    Selection *selection = desktop->getSelection();
    auto itemlist = selection->items();

    // Convert between units
    double font_size = 0;
    double doc_scale = 1;
    int count = 0;

    for (auto item : itemlist) {
        if (is<SPText>(item) || is<SPFlowtext>(item)) {
            doc_scale = Geom::Affine(item->i2dt_affine()).descrim();
            font_size += item->style->font_size.computed * doc_scale;
            ++count;
        }
    }
    if (count > 0) {
        font_size /= count;
    } else {
        // ideally use default font-size.
        font_size = 20;
    }
    if ((unit->abbr == "" || unit->abbr == "em") && (old_unit == SP_CSS_UNIT_NONE || old_unit == SP_CSS_UNIT_EM)) {
        // Do nothing
    } else if ((unit->abbr == "" || unit->abbr == "em") && old_unit == SP_CSS_UNIT_EX) {
        line_height *= 0.5;
    } else if ((unit->abbr) == "ex" && (old_unit == SP_CSS_UNIT_EM || old_unit == SP_CSS_UNIT_NONE)) {
        line_height *= 2.0;
    } else if ((unit->abbr == "" || unit->abbr == "em") && old_unit == SP_CSS_UNIT_PERCENT) {
        line_height /= 100.0;
    } else if ((unit->abbr) == "%" && (old_unit == SP_CSS_UNIT_EM || old_unit == SP_CSS_UNIT_NONE)) {
        line_height *= 100;
    } else if ((unit->abbr) == "ex" && old_unit == SP_CSS_UNIT_PERCENT) {
        line_height /= 50.0;
    } else if ((unit->abbr) == "%" && old_unit == SP_CSS_UNIT_EX) {
        line_height *= 50;
    } else if (is_relative(unit)) {
        // Convert absolute to relative... for the moment use average font-size
        if (old_unit == SP_CSS_UNIT_NONE) old_unit = SP_CSS_UNIT_EM;
        line_height = Quantity::convert(line_height, sp_style_get_css_unit_string(old_unit), "px");

        if (font_size > 0) {
            line_height /= font_size;
        }
        if (unit->abbr == "%") {
            line_height *= 100;
        } else if (unit->abbr == "ex") {
            line_height *= 2;
        }
    } else if (old_unit == SP_CSS_UNIT_NONE || old_unit == SP_CSS_UNIT_PERCENT || old_unit == SP_CSS_UNIT_EM ||
            old_unit == SP_CSS_UNIT_EX) {
        // Convert relative to absolute... for the moment use average font-size
        if (old_unit == SP_CSS_UNIT_PERCENT) {
            line_height /= 100.0;
        } else if (old_unit == SP_CSS_UNIT_EX) {
            line_height /= 2.0;
        }
        line_height *= font_size;
        line_height = Quantity::convert(line_height, "px", unit);
    } else {
        // Convert between different absolute units (only used in GUI)
        line_height = Quantity::convert(line_height, sp_style_get_css_unit_string(old_unit), unit);
    }
    // Set css line height.
    auto css = sp_repr_css_attr_new();
    CSSOStringStream osfs;
    // Set css line height.
    if ( is_relative(unit) ) {
        osfs << line_height << unit->abbr;
    } else {
        osfs << Quantity::convert(line_height, unit, "px") << "px";
    }
    sp_repr_css_set_property(css, "line-height", osfs.str().c_str());

    // Update GUI with line_height value.
    line_height_adj->set_value(line_height);
    // Update "climb rate"  The custom action has a step property but no way to set it.
    if (unit->abbr == "%") {
        line_height_adj->set_step_increment(1.0);
        line_height_adj->set_page_increment(10.0);
    } else {
        line_height_adj->set_step_increment(0.1);
        line_height_adj->set_page_increment(1.0);
    }
    // Internal function to set line-height which is spacing mode dependent.
    SPItem *parent = itemlist.empty() ? nullptr : itemlist.front();
    SPStyle *parent_style = nullptr;
    if (parent) {
        parent_style = parent->style;
    }
    bool inside = false;
    if (_outer) {
        if (!selection->singleItem() || !parent_style || parent_style->line_height.computed != 0) {
            for (auto item : itemlist) {
                if (is<SPText>(item) || is<SPFlowtext>(item)) {
                    // Scale by inverse of accumulated parent transform
                    auto css_set = sp_repr_css_attr_new();
                    sp_repr_css_merge(css_set, css);
                    auto const local = item->i2doc_affine();
                    double const ex = local.descrim();
                    if (ex != 0.0 && ex != 1.0) {
                        sp_css_attr_scale(css_set, 1 / ex);
                    }
                    recursively_set_properties(item, css_set);
                    sp_repr_css_attr_unref(css_set);
                }
            }
        } else {
            inside = true;
        }
    }
    if (!_outer || inside) {
        SPCSSAttr *parent_cssatr = sp_css_attr_from_style(parent_style, SP_STYLE_FLAG_IFSET);
        Glib::ustring parent_lineheight = sp_repr_css_property(parent_cssatr, "line-height", "1.25");
        SPCSSAttr *cssfit = sp_repr_css_attr_new();
        sp_repr_css_set_property(cssfit, "line-height", parent_lineheight.c_str());
        double minheight = 0;
        if (parent_style) {
            minheight = parent_style->line_height.computed;
        }
        if (minheight) {
            for (auto child : parent->childList(false)) {
                if (is<SPItem>(child)) {
                    recursively_set_properties(child, cssfit);
                }
            }
        }
        sp_repr_css_set_property(cssfit, "line-height", "0");
        parent->changeCSS(cssfit, "style");
        subselection_wrap_toggle(true);
        sp_desktop_set_style(desktop, css, true, true);
        subselection_wrap_toggle(false);
        sp_repr_css_attr_unref(cssfit);
    }
    itemlist = selection->items();
    // Only need to save for undo if a text item has been changed.
    bool modified = false;
    for (auto item : itemlist) {
        if (is<SPText>(item) || is<SPFlowtext>(item)) {
            modified = true;
            break;
        }
    }
    // Save for undo
    if (modified) {
        // Call ensureUpToDate() causes rebuild of text layout (with all proper style
        // cascading, etc.). For multi-line text with sodipodi::role="line", we must explicitly
        // save new <tspan> 'x' and 'y' attribute values by calling updateRepr().
        // Partial fix for bug #1590141.

        desktop->getDocument()->ensureUpToDate();
        for (auto item : itemlist) {
            if (is<SPText>(item) || is<SPFlowtext>(item)) {
                item->updateRepr();
            }
        }
        if (_outer) {
            prepare_inner();
        }
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:line-height", RC_("Undo", "Text: Change line-height unit"), INKSCAPE_ICON("draw-text"));
    }

    mergeDefaultStyle(css);

    sp_repr_css_attr_unref(css);

    _freeze = false;
}

void TextToolbar::wordspacing_value_changed()
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    // At the moment this handles only numerical values (i.e. no em unit).
    // Set css word-spacing
    SPCSSAttr *css = sp_repr_css_attr_new();
    CSSOStringStream osfs;
    osfs << _word_spacing_item.get_adjustment()->get_value() << "px"; // For now always use px
    sp_repr_css_set_property (css, "word-spacing", osfs.str().c_str());
    text_outer_set_style(css);

    if (mergeDefaultStyle(css)) {
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:word-spacing", RC_("Undo", "Text: Change word-spacing"), INKSCAPE_ICON("draw-text"));
    }

    sp_repr_css_attr_unref(css);

    _freeze = false;
}

void TextToolbar::letterspacing_value_changed()
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    // At the moment this handles only numerical values (i.e. no em unit).
    // Set css letter-spacing
    auto css = sp_repr_css_attr_new();
    CSSOStringStream osfs;
    osfs << _letter_spacing_item.get_adjustment()->get_value() << "px"; // For now always use px
    sp_repr_css_set_property(css, "letter-spacing", osfs.str().c_str());
    text_outer_set_style(css);

    if (mergeDefaultStyle(css)) {
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:letter-spacing", RC_("Undo", "Text: Change letter-spacing"), INKSCAPE_ICON("draw-text"));
    }

    sp_repr_css_attr_unref(css);

    _freeze = false;
}

void TextToolbar::dx_value_changed()
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    double new_dx = _dx_item.get_adjustment()->get_value();
    bool modified = false;

    if (auto tc = SP_TEXT_CONTEXT(_desktop->getTool())) {
        unsigned char_index = -1;
        if (auto attributes = text_tag_attributes_at_position(tc->textItem(), std::min(tc->text_sel_start, tc->text_sel_end), &char_index)) {
            double old_dx = attributes->getDx(char_index);
            double delta_dx = new_dx - old_dx;
            sp_te_adjust_dx(tc->textItem(), tc->text_sel_start, tc->text_sel_end, _desktop, delta_dx);
            modified = true;
        }
    }

    if (modified) {
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:dx", RC_("Undo", "Text: Change dx (kern)"), INKSCAPE_ICON("draw-text"));
    }

    _freeze = false;
}

void TextToolbar::dy_value_changed()
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    double new_dy = _dy_item.get_adjustment()->get_value();
    bool modified = false;

    if (auto tc = SP_TEXT_CONTEXT(_desktop->getTool())) {
        unsigned char_index = -1;
        if (auto attributes = text_tag_attributes_at_position(tc->textItem(), std::min(tc->text_sel_start, tc->text_sel_end), &char_index)) {
            double old_dy = attributes->getDy(char_index);
            double delta_dy = new_dy - old_dy;
            sp_te_adjust_dy(tc->textItem(), tc->text_sel_start, tc->text_sel_end, _desktop, delta_dy);
            modified = true;
        }
    }

    if (modified) {
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:dy", RC_("Undo", "Text: Change dy"), INKSCAPE_ICON("draw-text"));
    }

    _freeze = false;
}

void TextToolbar::rotation_value_changed()
{
    // quit if run by the _changed callbacks
    if (_freeze) {
        return;
    }
    _freeze = true;

    double new_degrees = _rotation_item.get_adjustment()->get_value();

    bool modified = false;
    if (auto tc = SP_TEXT_CONTEXT(_desktop->getTool())) {
        unsigned char_index = -1;
        if (auto attributes = text_tag_attributes_at_position(tc->textItem(), std::min(tc->text_sel_start, tc->text_sel_end), &char_index)) {
            double old_degrees = attributes->getRotate(char_index);
            double delta_deg = new_degrees - old_degrees;
            sp_te_adjust_rotation(tc->textItem(), tc->text_sel_start, tc->text_sel_end, _desktop, delta_deg);
            modified = true;
        }
    }

    if (modified) {
        DocumentUndo::maybeDone(_desktop->getDocument(), "ttb:rotate", RC_("Undo", "Text: Change rotate"), INKSCAPE_ICON("draw-text"));
    }

    _freeze = false;
}

void TextToolbar::_selectionChanged(Selection *selection) // don't bother to update font list if subsel changed
{
    static int count = 0; // for DEBUG_TEXT

    if constexpr (DEBUG_TEXT) {
        ++count;
        std::cout << std::endl;
        std::cout << "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&" << std::endl;
        std::cout << "sp_text_toolbox_selection_changed: start " << count << std::endl;
    }

    // quit if run by the _changed callbacks
    if (_freeze) {
        if constexpr (DEBUG_TEXT) {
            std::cout << "    Frozen, returning" << std::endl;
            std::cout << "sp_text_toolbox_selection_changed: exit " << count << std::endl;
            std::cout << "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&" << std::endl;
            std::cout << std::endl;
        }
        return;
    }
    _freeze = true;

    // selection defined as argument but not used, argh!!!
    SPDesktop *desktop = _desktop;
    SPDocument *document = _desktop->getDocument();
    selection = desktop->getSelection();
    auto itemlist = selection->items();

    if constexpr (DEBUG_TEXT) {
        for (auto i : itemlist) {
            std::cout << "    " << i->getId() << std::endl;
        }
        if (auto text_tool = dynamic_cast<Tools::TextTool const *>(_desktop->getTool())) {
            auto const selected_text = get_selected_text(*text_tool);
            std::cout << "  Selected text: |" << selected_text << "|" << std::endl;
        }
    }

    // Only flowed text can be justified, only normal text can be kerned...
    // Find out if we have flowed text now so we can use it several places
    bool isFlow = false;
    std::vector<SPItem *> to_work;
    for (auto i : itemlist) {
        auto text = cast<SPText>(i);
        auto flowtext = cast<SPFlowtext>(i);
        if (text || flowtext) {
            to_work.push_back(i);
        }
        if (flowtext ||
            !is_kerning_supported(text)) {
            isFlow = true;
        }
    }
    bool outside = false;
    if (selection && to_work.empty()) {
        outside = true;
    }

    auto fontlister = FontLister::get_instance();
    fontlister->selection_update();
    // Update font list, but only if widget already created.
    _font_family_item->set_active_text(fontlister->get_font_family().c_str(), fontlister->get_font_family_row());
    _font_style_item->set_active_text(fontlister->get_font_style().c_str());

    /*
     * Query from current selection:
     *   Font family (font-family)
     *   Style (font-weight, font-style, font-stretch, font-variant, font-align)
     *   Numbers (font-size, letter-spacing, word-spacing, line-height, text-anchor, writing-mode)
     *   Font specification (Inkscape private attribute)
     */
    SPStyle query(document);
    SPStyle query_fallback(document);
    int result_family = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_FONTFAMILY);
    int result_style = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_FONTSTYLE);
    int result_baseline = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_BASELINES);
    int result_wmode = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_WRITINGMODES);
    // PROOF: capitals and position, for the Character buttons.
    int result_variants = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_FONTVARIANTS);

    // Calling sp_desktop_query_style will result in a call to TextTool::_styleQueried().
    // This returns the style of the selected text inside the <text> element... which
    // is often the style of one or more <tspan>s. If we want the style of the outer
    // <text> objects then we need to bypass the call to TextTool::_styleQueried().
    // The desktop selection never includes the elements inside the <text> element.
    int result_numbers = 0;
    int result_numbers_fallback = 0;
    if (!outside) {
        auto sub_active_item = _sub_active_item.get();
        if (_outer && sub_active_item) {
            auto parent = cast<SPItem>(sub_active_item->parent);
            result_numbers = objects_query_fontnumbers({sub_active_item}, &query);
            result_numbers_fallback = objects_query_fontnumbers({parent}, &query_fallback);
        } else if (_outer) {
            result_numbers = objects_query_fontnumbers(to_work, &query);
        } else {
            result_numbers = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_FONTNUMBERS);
        }
    } else {
        result_numbers = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_FONTNUMBERS);
    }

    auto prefs = Preferences::get();
    /*
     * If no text in selection (querying returned nothing), read the style from
     * the /tools/text preferences (default style for new texts). Return if
     * tool bar already set to these preferences.
     */
    if (result_family  == QUERY_STYLE_NOTHING ||
        result_style   == QUERY_STYLE_NOTHING ||
        result_numbers == QUERY_STYLE_NOTHING ||
        result_wmode   == QUERY_STYLE_NOTHING)
    {
        // There are no texts in selection, read from preferences.
        if (SPCSSAttr *css = SP_ACTIVE_DESKTOP->getCurrentOrToolStyle("/tools/text", true)) {
            query.mergeCSS(css);
            sp_repr_css_attr_unref(css);
        }

        if constexpr (DEBUG_TEXT) {
            std::cout << "    read style from prefs:" << std::endl;
            std::cout << "    Family set? " << query.font_family.set
                      << "    Style set? "  << query.font_style.set
                      << "    FontSpec set? " << query.font_specification.set
                      << std::endl;
        }
        if (_text_style_from_prefs) {
            // Do not reset the toolbar style from prefs if we already did it last time
            _freeze = false;
            if constexpr (DEBUG_TEXT) {
                std::cout << "    text_style_from_prefs: toolbar already set" << std:: endl;
                std::cout << "sp_text_toolbox_selection_changed: exit " << count << std::endl;
                std::cout << "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&" << std::endl;
                std::cout << std::endl;
            }
            return;
        }

        // To ensure the value of the combobox is properly set on start-up, only mark
        // the prefs set if the combobox has already been constructed.
        _text_style_from_prefs = true;
    } else {
        _text_style_from_prefs = false;
    }

    // If we have valid query data for text (font-family, font-specification) set toolbar accordingly.
    {
        // Size (average of text selected)

        int unit = prefs->getInt("/options/font/unitType", SP_CSS_UNIT_PT);
        double size = 0;
        if (!size && _cursor_numbers != QUERY_STYLE_NOTHING) {
            size = sp_style_css_size_px_to_units(_query_cursor.font_size.computed, unit);
        }
        if (!size && result_numbers != QUERY_STYLE_NOTHING) {
            size = sp_style_css_size_px_to_units(query.font_size.computed, unit);
        }
        if (!size && result_numbers_fallback != QUERY_STYLE_NOTHING) {
            size = sp_style_css_size_px_to_units(query_fallback.font_size.computed, unit);
        }
        if (!size && _text_style_from_prefs) {
            size = sp_style_css_size_px_to_units(query.font_size.computed, unit);
        }

        // Avoid conversion oddities by rounding
        selection_fontsize = sp_style_css_size_round_for_user_display(size);
        _font_size_item.setSize(selection_fontsize);

        // Superscript
        bool superscriptSet =
            ((result_baseline == QUERY_STYLE_SINGLE || result_baseline == QUERY_STYLE_MULTIPLE_SAME ) &&
             query.baseline_shift.set &&
             query.baseline_shift.type == SP_BASELINE_SHIFT_LITERAL &&
             query.baseline_shift.literal == SP_CSS_BASELINE_SHIFT_SUPER );

        // Subscript
        bool subscriptSet =
            ((result_baseline == QUERY_STYLE_SINGLE || result_baseline == QUERY_STYLE_MULTIPLE_SAME ) &&
             query.baseline_shift.set &&
             query.baseline_shift.type == SP_BASELINE_SHIFT_LITERAL &&
             query.baseline_shift.literal == SP_CSS_BASELINE_SHIFT_SUB );

        // PROOF: the four Character buttons. A button is down when all the text selected
        // is that; with nothing selected, when the text typed next will be.
        // The query leaves in `value` what differs between the texts, and in `set` whether
        // they agree on text-transform; the style of text to come is one style.
        bool const one_style = result_variants == QUERY_STYLE_NOTHING;
        bool const same_position = one_style || query.font_variant_position.value == 0;
        bool const same_caps = one_style || query.font_variant_caps.value == 0;
        bool const same_transform = one_style || query.text_transform.set;
        _superscript_btn.set_active(superscriptSet ||
                                    (same_position && query.font_variant_position.computed == SP_CSS_FONT_VARIANT_POSITION_SUPER));
        _subscript_btn.set_active(subscriptSet ||
                                  (same_position && query.font_variant_position.computed == SP_CSS_FONT_VARIANT_POSITION_SUB));
        bool const all_caps = same_transform && query.text_transform.computed == SP_CSS_TEXT_TRANSFORM_UPPERCASE;
        _all_caps_btn.set_active(all_caps);
        _small_caps_btn.set_active(!all_caps && same_caps && query.font_variant_caps.computed == SP_CSS_FONT_VARIANT_CAPS_SMALL);

        // Alignment

        // Note: SVG 1.1 doesn't include text-align, SVG 1.2 Tiny doesn't include text-align="justify"
        // text-align="justify" was a draft SVG 1.2 item (along with flowed text).
        // Only flowed text can be left and right justified at the same time.
        // Disable button if we don't have flowed text.

        _alignment_buttons[3]->set_sensitive(isFlow);

        int activeButton = prefs->getInt("/tools/text/align_mode", 0);
        bool r2l = (query.direction.computed == SP_CSS_DIRECTION_RTL);

        if ((query.text_align.computed == SP_CSS_TEXT_ALIGN_START && !r2l) ||
            (query.text_align.computed == SP_CSS_TEXT_ALIGN_END   &&  r2l) ||
             query.text_align.computed == SP_CSS_TEXT_ALIGN_LEFT) {
            activeButton = 0;
        } else if (query.text_align.computed == SP_CSS_TEXT_ALIGN_CENTER) {
            activeButton = 1;
        } else if ((query.text_align.computed == SP_CSS_TEXT_ALIGN_START &&  r2l) ||
                   (query.text_align.computed == SP_CSS_TEXT_ALIGN_END   && !r2l) ||
                    query.text_align.computed == SP_CSS_TEXT_ALIGN_RIGHT) {
            activeButton = 2;
        } else if (query.text_align.computed == SP_CSS_TEXT_ALIGN_JUSTIFY) {
            activeButton = 3;
        }
        _alignment_buttons[activeButton]->set_active(true);

        double height = 0;
        gint line_height_unit = 0;

        if (!height && _cursor_numbers != QUERY_STYLE_NOTHING) {
            height = _query_cursor.line_height.value;
            line_height_unit = _query_cursor.line_height.unit;
        }

        if (!height && result_numbers != QUERY_STYLE_NOTHING) {
            height = query.line_height.value;
            line_height_unit = query.line_height.unit;
        }

        if (!height && result_numbers_fallback != QUERY_STYLE_NOTHING) {
            height = query_fallback.line_height.value;
            line_height_unit = query_fallback.line_height.unit;
        }

        if (!height && _text_style_from_prefs) {
            height = query.line_height.value;
            line_height_unit = query.line_height.unit;
        }

        if (line_height_unit == SP_CSS_UNIT_PERCENT) {
            height *= 100.0; // Inkscape store % as fraction in .value
        }

        // We dot want to parse values just show
        if (!is_relative(SPCSSUnit(line_height_unit))) {
            int curunit = prefs->getInt("/tools/text/lineheight/display_unit", 1);
            // For backwards comaptibility
            if (is_relative(SPCSSUnit(curunit))) {
                prefs->setInt("/tools/text/lineheight/display_unit", 1);
                curunit = 1;
            }
            height = Quantity::convert(height, "px", sp_style_get_css_unit_string(curunit));
            line_height_unit = curunit;
        }
        auto line_height_adj = _line_height_item.get_adjustment();
        line_height_adj->set_value(height);

        // Update "climb rate"
        if (line_height_unit == SP_CSS_UNIT_PERCENT) {
            line_height_adj->set_step_increment(1.0);
            line_height_adj->set_page_increment(10.0);
        } else {
            line_height_adj->set_step_increment(0.1);
            line_height_adj->set_page_increment(1.0);
        }

        if (line_height_unit == SP_CSS_UNIT_NONE) {
            // Function 'sp_style_get_css_unit_string' returns 'px' for unit none.
            // We need to avoid this.
            _tracker->setActiveUnitByAbbr("lines");
        } else {
            _tracker->setActiveUnitByAbbr(sp_style_get_css_unit_string(line_height_unit));
        }

        // Save unit so we can do conversions between new/old units.
        _lineheight_unit = line_height_unit;
        // Word spacing
        double wordSpacing;
        if (query.word_spacing.normal) {
            wordSpacing = 0.0;
        } else {
            wordSpacing = query.word_spacing.computed; // Assume no units (change in desktop-style.cpp)
        }

        _word_spacing_item.get_adjustment()->set_value(wordSpacing);

        // Letter spacing
        double letterSpacing;
        if (query.letter_spacing.normal) {
            letterSpacing = 0.0;
        } else {
            letterSpacing = query.letter_spacing.computed; // Assume no units (change in desktop-style.cpp)
        }

        _letter_spacing_item.get_adjustment()->set_value(letterSpacing);

        // Writing mode
        int activeButton2 = 0;
        if (query.writing_mode.computed == SP_CSS_WRITING_MODE_LR_TB) activeButton2 = 0;
        if (query.writing_mode.computed == SP_CSS_WRITING_MODE_TB_RL) activeButton2 = 1;
        if (query.writing_mode.computed == SP_CSS_WRITING_MODE_TB_LR) activeButton2 = 2;

        _writing_buttons[activeButton2]->set_active(true);

        // Orientation
        int activeButton3 = 0;
        if (query.text_orientation.computed == SP_CSS_TEXT_ORIENTATION_MIXED   ) activeButton3 = 0;
        if (query.text_orientation.computed == SP_CSS_TEXT_ORIENTATION_UPRIGHT ) activeButton3 = 1;
        if (query.text_orientation.computed == SP_CSS_TEXT_ORIENTATION_SIDEWAYS) activeButton3 = 2;

        _orientation_buttons[activeButton3]->set_active(true);

        // Disable text orientation for horizontal text...
        for (auto btn : _orientation_buttons) {
            btn->set_sensitive(activeButton2 != 0);
        }

        // Direction
        int activeButton4 = 0;
        if (query.direction.computed == SP_CSS_DIRECTION_LTR) activeButton4 = 0;
        if (query.direction.computed == SP_CSS_DIRECTION_RTL) activeButton4 = 1;
        _direction_buttons[activeButton4]->set_active(true);
    }

    if constexpr (DEBUG_TEXT) {
        std::cout << "    GUI: fontfamily.value: "     << query.font_family.value()  << std::endl;
        std::cout << "    GUI: font_size.computed: "   << query.font_size.computed   << std::endl;
        std::cout << "    GUI: font_weight.computed: " << query.font_weight.computed << std::endl;
        std::cout << "    GUI: font_style.computed: "  << query.font_style.computed  << std::endl;
        std::cout << "    GUI: text_anchor.computed: " << query.text_anchor.computed << std::endl;
        std::cout << "    GUI: text_align.computed:  " << query.text_align.computed  << std::endl;
        std::cout << "    GUI: line_height.computed: " << query.line_height.computed
                  << "  line_height.value: "    << query.line_height.value
                  << "  line_height.unit: "     << query.line_height.unit  << std::endl;
        std::cout << "    GUI: word_spacing.computed: " << query.word_spacing.computed
                  << "  word_spacing.value: "    << query.word_spacing.value
                  << "  word_spacing.unit: "     << query.word_spacing.unit  << std::endl;
        std::cout << "    GUI: letter_spacing.computed: " << query.letter_spacing.computed
                  << "  letter_spacing.value: "    << query.letter_spacing.value
                  << "  letter_spacing.unit: "     << query.letter_spacing.unit  << std::endl;
        std::cout << "    GUI: writing_mode.computed: " << query.writing_mode.computed << std::endl;
    }

    // Kerning (xshift), yshift, rotation.  NB: These are not CSS attributes.
    if (auto tc = SP_TEXT_CONTEXT(_desktop->getTool())) {
        unsigned char_index = -1;
        if (auto attributes = text_tag_attributes_at_position(tc->textItem(), std::min(tc->text_sel_start, tc->text_sel_end), &char_index)) {
            // Dx
            double dx = attributes->getDx(char_index);
            _dx_item.get_adjustment()->set_value(dx);

            // Dy
            double dy = attributes->getDy(char_index);
            _dy_item.get_adjustment()->set_value(dy);

            // Rotation
            double rotation = attributes->getRotate(char_index);
            /* SVG value is between 0 and 360 but we're using -180 to 180 in widget */
            if (rotation > 180.0) {
                rotation -= 360.0;
            }
            _rotation_item.get_adjustment()->set_value(rotation);

            if constexpr (DEBUG_TEXT) {
                std::cout << "    GUI: Dx: " << dx << std::endl;
                std::cout << "    GUI: Dy: " << dy << std::endl;
                std::cout << "    GUI: Rotation: " << rotation << std::endl;
            }
        }
    }

    // Set these here as we don't always have kerning/rotating attributes
    _dx_item.set_sensitive(!isFlow);
    _dy_item.set_sensitive(!isFlow);
    _rotation_item.set_sensitive(!isFlow);

    if constexpr (DEBUG_TEXT) {
        std::cout << "sp_text_toolbox_selection_changed: exit " << count << std::endl;
        std::cout << "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&" << std::endl;
        std::cout << std::endl;
    }

    _freeze = false;
}

void TextToolbar::_selectionModified(Selection *selection, unsigned)
{
    _sub_active_item.reset();
    _selectionChanged(selection);
}

void TextToolbar::subselection_wrap_toggle(bool start)
{
    if (auto tc = SP_TEXT_CONTEXT(_desktop->getTool())) {
        _updating = true;
        if (te_get_layout(tc->textItem())) {
            std::swap(tc->text_sel_start, wrap_start);
            std::swap(tc->text_sel_end, wrap_end);
        }
        _updating = start;
    }
}

/*
* This function parses the just created line height in one or more lines of a text subselection.
* It can describe 2 kinds of input because when we store a text element we apply a fallback that change
* structure. This visually is not reflected but user maybe want to change a part of this subselection
* once the fallback is created, so we need more complex logic here to fill the gap.
* Basically, we have a line height changed in the new wrapper element/s between wrap_start and wrap_end.
* These variables store starting iterator of first char in line and last char in line in a subselection.
* These elements are styled well but we can have orphaned text nodes before and after the subselection.
* So, normally 3 elements are inside a container as direct child of a text element.
* We need to apply the container style to the optional first and last text nodes,
* wrapping into a new element that gets the container style (this is not part to the sub-selection).
* After wrapping, we unindent all children of the container and remove the container.
*
*/
void TextToolbar::prepare_inner()
{
    auto const tc = SP_TEXT_CONTEXT(_desktop->getTool());
    if (!tc) {
        return;
    }
    auto const layout = te_get_layout(tc->textItem());
    if (!layout) {
      return;
    }
    auto doc = _desktop->getDocument();
    auto spobject = tc->textItem();
    auto spitem = tc->textItem();
    auto text = cast<SPText>(tc->textItem());
    auto flowtext = cast<SPFlowtext>(tc->textItem());
    XML::Document *xml_doc = doc->getReprDoc();
    if (!spobject) {
        return;
    }

    // We check for external files with text nodes direct children of text element
    // and wrap it into a tspan elements as inkscape do.
    if (text) {
        bool changed = false;
        std::vector<SPObject *> childs = spitem->childList(false);
        for (auto child : childs) {
            auto spstring = cast<SPString>(child);
            if (spstring) {
                Glib::ustring content = spstring->string;
                if (content != "\n") {
                    XML::Node *rstring = xml_doc->createTextNode(content.c_str());
                    XML::Node *rtspan  = xml_doc->createElement("svg:tspan");
                    //XML::Node *rnl     = xml_doc->createTextNode("\n");
                    rtspan->setAttribute("sodipodi:role", "line");
                    rtspan->addChild(rstring, nullptr);
                    text->getRepr()->addChild(rtspan, child->getRepr());
                    GC::release(rstring);
                    GC::release(rtspan);
                    text->getRepr()->removeChild(spstring->getRepr());
                    changed = true;
                }
            }
        }
        if (changed) {
            // proper rebuild happens later,
            // this just updates layout to use now, avoids use after free
            text->rebuildLayout();
        }
    }

    std::vector<SPObject *> containers;
    {
        // populate `containers` with objects that will be modified.

        // Temporarily remove the shape so Layout calculates
        // the position of wrap_end and wrap_start, even if
        // one of these are hidden because the previous line height was changed
        if (text) {
            text->hide_shape_inside();
        } else if (flowtext) {
            flowtext->fix_overflow_flowregion(false);
        }
        SPObject *rawptr_start = nullptr;
        SPObject *rawptr_end = nullptr;
        layout->validateIterator(&wrap_start);
        layout->validateIterator(&wrap_end);
        layout->getSourceOfCharacter(wrap_start, &rawptr_start);
        layout->getSourceOfCharacter(wrap_end, &rawptr_end);
        if (text) {
            text->show_shape_inside();
        } else if (flowtext) {
            flowtext->fix_overflow_flowregion(true);
        }
        if (!rawptr_start || !rawptr_end) {
            return;
        }

        // Loop through parents of start and end till we reach
        // first children of the text element.
        // Get all objects between start and end (inclusive)
        SPObject *start = rawptr_start;
        SPObject *end   = rawptr_end;
        while (start->parent != spobject) {
            start = start->parent;
        }
        while (end->parent != spobject) {
            end = end->parent;
        }

        while (start && start != end) {
            containers.push_back(start);
            start = start->getNext();
        }
        if (start) {
            containers.push_back(start);
        }
    }

    for (auto container : containers) {
        XML::Node *prevchild = container->getRepr();
        std::vector<SPObject*> childs = container->childList(false);
        for (auto child : childs) {
            auto spstring = cast<SPString>(child);
            auto flowtspan = cast<SPFlowtspan>(child);
            auto tspan = cast<SPTSpan>(child);
            // we need to upper all flowtspans to container level
            // to do this we need to change the element from flowspan to flowpara
            if (flowtspan) {
                XML::Node *flowpara = xml_doc->createElement("svg:flowPara");
                std::vector<SPObject*> fts_childs = flowtspan->childList(false);
                bool hascontent = false;
                // we need to move the contents to the new created element
                // maybe we can move directly but it is safer for me to duplicate,
                // inject into the new element and delete original
                for (auto fts_child : fts_childs) {
                    // is this check necessary?
                    if (fts_child) {
                        XML::Node *fts_child_node = fts_child->getRepr()->duplicate(xml_doc);
                        flowtspan->getRepr()->removeChild(fts_child->getRepr());
                        flowpara->addChild(fts_child_node, nullptr);
                        GC::release(fts_child_node);
                        hascontent = true;
                    }
                }
                // if no contents we dont want to add
                if (hascontent) {
                    flowpara->setAttribute("style", flowtspan->getRepr()->attribute("style"));
                    spobject->getRepr()->addChild(flowpara, prevchild);
                    GC::release(flowpara);
                    prevchild = flowpara;
                }
                container->getRepr()->removeChild(flowtspan->getRepr());
            } else if (tspan) {
                if (child->childList(false).size()) {
                    child->getRepr()->setAttribute("sodipodi:role", "line");
                    // maybe we need to move unindent function here
                    // to be the same as other here
                    prevchild = unindent_node(child->getRepr(), prevchild);
                } else {
                    // if no contents we dont want to add
                    container->getRepr()->removeChild(child->getRepr());
                }
            } else if (spstring) {
                // we are on a text node, we act different if in a text or flowtext.
                // wrap a duplicate of the element and unindent after the prevchild
                // and finally delete original
                XML::Node *string_node = xml_doc->createTextNode(spstring->string.c_str());
                if (text) {
                    XML::Node *tspan_node = xml_doc->createElement("svg:tspan");
                    tspan_node->setAttribute("style", container->getRepr()->attribute("style"));
                    tspan_node->addChild(string_node, nullptr);
                    tspan_node->setAttribute("sodipodi:role", "line");
                    text->getRepr()->addChild(tspan_node, prevchild);
                    GC::release(string_node);
                    GC::release(tspan_node);
                    prevchild = tspan_node;
                } else if (flowtext) {
                    XML::Node *flowpara_node = xml_doc->createElement("svg:flowPara");
                    flowpara_node->setAttribute("style", container->getRepr()->attribute("style"));
                    flowpara_node->addChild(string_node, nullptr);
                    flowtext->getRepr()->addChild(flowpara_node, prevchild);
                    GC::release(string_node);
                    GC::release(flowpara_node);
                    prevchild = flowpara_node;
                }
                container->getRepr()->removeChild(spstring->getRepr());
            }
        }
        tc->textItem()->getRepr()->removeChild(container->getRepr());
    }
}

XML::Node *TextToolbar::unindent_node(XML::Node *repr, XML::Node *prevchild)
{
    g_assert(repr != nullptr);

    XML::Node *parent = repr->parent();
    if (parent) {
        XML::Node *grandparent = parent->parent();
        if (grandparent) {
            SPDocument *doc = _desktop->getDocument();
            XML::Document *xml_doc = doc->getReprDoc();
            XML::Node *newrepr = repr->duplicate(xml_doc);
            parent->removeChild(repr);
            grandparent->addChild(newrepr, prevchild);
            GC::release(newrepr);
            newrepr->setAttribute("sodipodi:role", "line");
            return newrepr;
        }
    }
    std::cerr << "TextToolbar::unindent_node error: node has no (grand)parent, nothing done.\n";
    return repr;
}

void TextToolbar::display_font_collections()
{
    UI::remove_all_children(_font_collections_list);

    auto font_collections = FontCollections::get();

    // Insert system collections.
    for(auto const &col: font_collections->get_collections(true)) {
        auto const btn = Gtk::make_managed<Gtk::CheckButton>(col);
        btn->set_margin_bottom(2);
        btn->set_active(font_collections->is_collection_selected(col));
        btn->signal_toggled().connect([font_collections, col](){
            // toggle font system collection
            font_collections->update_selected_collections(col);
        });
// g_message("tag: %s", tag.display_name.c_str());
        auto const row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_focusable(false);
        row->set_child(*btn);
        _font_collections_list.append(*row);
    }

    // Insert row separator.
    auto const sep = Gtk::make_managed<Gtk::Separator>();
    sep->set_margin_bottom(2);
    auto const sep_row = Gtk::make_managed<Gtk::ListBoxRow>();
    sep_row->set_focusable(false);
    sep_row->set_child(*sep);
    _font_collections_list.append(*sep_row);

    // Insert user collections.
    for (auto const& col: font_collections->get_collections()) {
        auto const btn = Gtk::make_managed<Gtk::CheckButton>(col);
        btn->set_margin_bottom(2);
        btn->set_active(font_collections->is_collection_selected(col));
        btn->signal_toggled().connect([font_collections, col](){
            // toggle font collection
            font_collections->update_selected_collections(col);
        });
// g_message("tag: %s", tag.display_name.c_str());
        auto const row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_focusable(false);
        row->set_child(*btn);
        _font_collections_list.append(*row);
    }
}

void TextToolbar::on_fcm_button_pressed()
{
    if (_desktop) {
        if (auto container = _desktop->getContainer()) {
            container->new_floating_dialog("FontCollections");
        }
    }
}

void TextToolbar::on_reset_button_pressed()
{
    auto font_collections = FontCollections::get();
    font_collections->clear_selected_collections();

    auto font_lister = FontLister::get_instance();
    font_lister->init_font_families();
    font_lister->init_default_styles();

    auto document = _desktop->getDocument();
    font_lister->add_document_fonts_at_top(document);
}

void TextToolbar::_cursorMoved(Tools::TextTool* tc)
{
    if constexpr (DEBUG_TEXT) {
        std::cout << std::endl;
        std::cout << "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&" << std::endl;
        std::cout << "subselection_changed: start " << std::endl;
    }
    // quit if run by the _changed callbacks
    _sub_active_item.reset();
    if (_updating) {
        return;
    }
    if (tc) {
        Text::Layout const *layout = te_get_layout(tc->textItem());
        if (layout) {
            Text::Layout::iterator start           = layout->begin();
            Text::Layout::iterator end             = layout->end();
            Text::Layout::iterator start_selection = tc->text_sel_start;
            Text::Layout::iterator end_selection   = tc->text_sel_end;
            if constexpr (DEBUG_TEXT) {
                std::cout << "    GUI: Start of text: " << layout->iteratorToCharIndex(start) << std::endl;
                std::cout << "    GUI: End of text: " << layout->iteratorToCharIndex(end) << std::endl;
                std::cout << "    GUI: Start of selection: " << layout->iteratorToCharIndex(start_selection) << std::endl;
                std::cout << "    GUI: End of selection: " << layout->iteratorToCharIndex(end_selection) << std::endl;
                std::cout << "    GUI: Loop Subelements: " << std::endl;
                std::cout << "    ::::::::::::::::::::::::::::::::::::::::::::: " << std::endl;
            }
            gint startline = layout->paragraphIndex(start_selection);
            if (start_selection == end_selection) {
                _outer = true;
                int counter = 0;
                for (auto child : tc->textItem()->childList(false)) {
                    auto item = cast<SPItem>(child);
                    if (item && counter == startline) {
                        _sub_active_item = item;
                        int origin_selection = layout->iteratorToCharIndex(start_selection);
                        Text::Layout::iterator next = layout->charIndexToIterator(origin_selection + 1);
                        Text::Layout::iterator prev = layout->charIndexToIterator(origin_selection - 1);
                        //TODO: find a better way to init
                        _updating = true;
                        auto query = SPStyle{_desktop->getDocument()};
                        _query_cursor = query;
                        Text::Layout::iterator start_line = tc->text_sel_start;
                        start_line.thisStartOfLine();
                        if (tc->text_sel_start == start_line) {
                            tc->text_sel_start = next;
                        } else {
                            tc->text_sel_start = prev;
                        }
                        _cursor_numbers = sp_desktop_query_style(_desktop, &_query_cursor, QUERY_STYLE_PROPERTY_FONTNUMBERS);
                        tc->text_sel_start = start_selection;
                        wrap_start = tc->text_sel_start;
                        wrap_end = tc->text_sel_end;
                        wrap_start.thisStartOfLine();
                        wrap_end.thisEndOfLine();
                        _updating = false;
                        break;
                    }
                    ++counter;
                }
                _selectionChanged(nullptr);
            } else if ((start_selection == start && end_selection == end) ||
                       (start_selection == end && end_selection == start)) {
                // full subselection
                _cursor_numbers = 0;
                _outer = true;
                _selectionChanged(nullptr);
            } else {
                _cursor_numbers = 0;
                _outer = false;
                wrap_start = tc->text_sel_start;
                wrap_end = tc->text_sel_end;
                if (tc->text_sel_start > tc->text_sel_end) {
                    wrap_start.thisEndOfLine();
                    wrap_end.thisStartOfLine();
                } else {
                    wrap_start.thisStartOfLine();
                    wrap_end.thisEndOfLine();
                }
                _selectionChanged(nullptr);
            }
        }
    }
    if constexpr (DEBUG_TEXT) {
        std::cout << "subselection_changed: exit " << std::endl;
        std::cout << "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&" << std::endl;
        std::cout << std::endl;
    }
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8 :
