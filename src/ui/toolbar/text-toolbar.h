// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLBAR_TEXT_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_TEXT_TOOLBAR_H

/**
 * @file Text toolbar
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

#include "object/weakptr.h"
#include "style.h"
#include "text-editing.h"
#include "toolbar.h"

namespace Gtk {
class Builder;
class Button;
class ListBox;
class ToggleButton;
} // namespace Gtk

namespace Inkscape {
class Selection;
namespace UI {
namespace Tools {
class ToolBase;
class TextTool;
} // namespace Tools
namespace Widget {
class FontSizeSelector;
class UnitMenu;
class ComboBoxEntryToolItem;
class ComboToolItem;
class SpinButton;
class UnitTracker;
} // namespace Widget
} // namespace UI
} // namespace Inkscape

namespace Inkscape::UI::Toolbar {

class TextToolbar : public Toolbar
{
public:
    TextToolbar();
    ~TextToolbar() override;

    void setDesktop(SPDesktop *desktop) override;

private:
    TextToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    using ValueChangedMemFun = void (TextToolbar::*)();
    using ModeChangedMemFun = void (TextToolbar::*)(int);

    std::unique_ptr<UI::Widget::UnitTracker> _tracker;

    std::vector<Gtk::ToggleButton *> _alignment_buttons;
    std::vector<Gtk::ToggleButton *> _writing_buttons;
    std::vector<Gtk::ToggleButton *> _orientation_buttons;
    std::vector<Gtk::ToggleButton *> _direction_buttons;

    Gtk::ListBox &_font_collections_list;
    Gtk::Button &_reset_button;

    UI::Widget::ComboBoxEntryToolItem *_font_family_item;
    UI::Widget::FontSizeSelector &_font_size_item;
    UI::Widget::ComboBoxEntryToolItem *_font_style_item;
    UI::Widget::UnitMenu*_line_height_units_item;
    UI::Widget::SpinButton &_line_height_item;
    // PROOF: Illustrator's four Character buttons. All caps and small caps are one or
    // neither, and so are superscript and subscript.
    Gtk::ToggleButton &_all_caps_btn;
    Gtk::ToggleButton &_small_caps_btn;
    Gtk::ToggleButton &_superscript_btn;
    Gtk::ToggleButton &_subscript_btn;

    UI::Widget::SpinButton &_word_spacing_item;
    UI::Widget::SpinButton &_letter_spacing_item;
    UI::Widget::SpinButton &_dx_item;
    UI::Widget::SpinButton &_dy_item;
    UI::Widget::SpinButton &_rotation_item;

    bool _freeze = false;
    bool _text_style_from_prefs = false;
    bool _outer = true;
    SPWeakPtr<SPItem> _sub_active_item;
    int _lineheight_unit;
    Text::Layout::iterator wrap_start;
    Text::Layout::iterator wrap_end;
    bool _updating = false;
    int _cursor_numbers = 0;
    SPStyle _query_cursor;
    double selection_fontsize;
    int _previous_unit;

    sigc::scoped_connection fc_changed_selection;
    sigc::scoped_connection fc_update;
    sigc::scoped_connection font_count_changed_connection;
    sigc::connection _selection_changed_conn;
    sigc::connection _selection_modified_conn;
    sigc::connection _cursor_moved_conn;
    sigc::connection _fonts_updated_conn;

    void setup_derived_spin_button(UI::Widget::SpinButton &btn, Glib::ustring const &name, double default_value,
                                   ValueChangedMemFun value_changed_mem_fun);
    void configure_mode_buttons(std::vector<Gtk::ToggleButton *> &buttons, Gtk::Box &box, Glib::ustring const &name,
                                ModeChangedMemFun mode_changed_mem_fun);
    void text_outer_set_style(SPCSSAttr *css);
    void fontfamily_value_changed();
    void fontsize_value_changed(double size);
    void subselection_wrap_toggle(bool start);
    void fontstyle_value_changed();
    void caps_changed(int mode);
    void script_changed(int mode);
    void align_mode_changed(int mode);
    void writing_mode_changed(int mode);
    void orientation_changed(int mode);
    void direction_changed(int mode);
    void lineheight_value_changed();
    void lineheight_unit_changed();
    void wordspacing_value_changed();
    void letterspacing_value_changed();
    void dx_value_changed();
    void dy_value_changed();
    void prepare_inner();
    void focus_text();
    void rotation_value_changed();
    void _selectionChanged(Selection *selection);
    void _selectionModified(Selection *selection, guint flags);
    void _cursorMoved(Tools::TextTool *texttool);
    void set_sizes(int unit);
    void display_font_collections();
    void on_fcm_button_pressed();
    void on_reset_button_pressed();
    XML::Node *unindent_node(XML::Node *repr, XML::Node *before);
    bool mergeDefaultStyle(SPCSSAttr *css);
    SPCSSAttr *cssForTextAlignment(int align_mode, int direction);
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_TEXT_TOOLBAR_H
