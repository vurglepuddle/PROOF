// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Generic object attribute editor
 *//*
 * Authors:
 * see git history
 * Kris De Gussem <Kris.DeGussem@gmail.com>
 * Michael Kowalski
 *
 * Copyright (C) 2018-2025 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_DIALOGS_OBJECT_ATTRIBUTES_H
#define SEEN_DIALOGS_OBJECT_ATTRIBUTES_H

#include <glibmm/ustring.h>
#include <gtkmm/boolfilter.h>
#include <gtkmm/listbox.h>
#include <gtkmm/searchentry2.h>
#include <gtkmm/widget.h>
#include <memory>
#include <map>

#include "desktop.h"
#include "object/sp-object.h"
#include "preferences.h"
#include "ui/dialog/dialog-base.h"
#include "ui/dialog/object-properties.h"
#include "ui/operation-blocker.h"
#include "ui/widget/paint-attribute.h"
#include "ui/widget/unit-tracker.h"
#include "ui/widget/drop-down-list.h"
#include "xml/helper-observer.h"

class SPAttributeTable;
class SPItem;

namespace Inkscape::UI::Dialog {
class LPEMetadata;

namespace details {

const auto dlg_pref_path = Glib::ustring("/dialogs/object-properties/");

class AttributesPanel {
public:
    AttributesPanel();
    virtual ~AttributesPanel() = default;

    void set_document(SPDocument* document);
    void set_desktop(SPDesktop* desktop);
    void update_panel(SPObject* object, SPDesktop* desktop, bool tagged);
    virtual void subselection_changed(const std::vector<SPItem*>& items) {}
    Gtk::Widget& widget() { if(!_widget) throw "missing widget in attributes panel"; return *_widget; }
    virtual Glib::ustring get_title(Selection* selection) const;
    void update_lock(SPObject* object);

protected:
    virtual void update(SPObject* object) = 0;
    virtual void update_paint(SPObject* object);
    bool can_update() const;
    virtual void document_replaced(SPDocument* document) {}
    // value with units changed by the user; modify the current object
    void change_value_px(SPObject* object, const char* key, double input, const char* attr, std::function<void (double)>&& setter);
    // angle in degrees changed by the user; modify the current object
    void change_angle(SPObject* object, const char* key, double angle, std::function<void (double)>&& setter);
    // modify the current object
    void change_value(SPObject* object, const Glib::RefPtr<Gtk::Adjustment>& adj, std::function<void (double)>&& setter);
    // add top-level object label
    void add_object_label();
    // add fill and stroke attributes
    void add_fill_and_stroke(Widget::PaintAttribute::Parts parts = Widget::PaintAttribute::AllParts);
    // add x, y and width, height property editing
    void add_size_properties();
    // add a section with title and description
    void add_name_properties();
    // add JavaScript interactivity properties
    void add_interactivity_properties();
    // add a header label
    void add_header(const Glib::ustring& title);
    // add live path effects info
    void add_lpes(bool clone = false);
    // add filter info
    void add_filters(bool separate = true);

    SPDesktop* _desktop = nullptr;
    SPDocument* _document = nullptr;
    SPObject* _current_object = nullptr;
    sigc::scoped_connection _resource_changed;
    OperationBlocker _update;
    Glib::ustring _title;
    Gtk::Widget* _widget = nullptr;
    std::unique_ptr<UI::Widget::UnitTracker> _tracker;
    std::unique_ptr<Widget::PaintAttribute> _paint;
    Widget::InkPropertyGrid _grid;
private:
    // transform the current selection (use x/y/width/height)
    void transform();
    // PROOF: turn the selection to the angle in the Rotate field, or make its box upright
    void rotate();
    void reset_box();
    void update_label(SPObject* object, Inkscape::Selection* selection);
    void update_size_location();
    void update_filters(SPObject* object, bool update_menu = true);
    void populate_filter_menu();
    void update_lpes(SPObject* object);
    void update_names(SPObject* object);
    void update_interactive_props(SPObject* object);
    void show_name_properties(bool expand);
    void show_interactivity_properties(bool expand);
    bool on_key_pressed(guint keyval, guint keycode, Gdk::ModifierType state);
    void select_lpe_row(int dir = 0);
    void apply_selected_lpe();
    void refilter_lpes();
    void validate_obj_id();

    Glib::RefPtr<Gtk::Builder> _builder;
    Widget::InkSpinButton& _x;
    Widget::InkSpinButton& _y;
    Widget::InkSpinButton& _width;
    Widget::InkSpinButton& _height;
    Gtk::Button& _round_loc;
    Gtk::Button& _round_size;
    Widget::InkSpinButton& _angle;
    Gtk::Button& _reset_box;
    bool _show_obj_label = false;
    bool _show_fill_stroke = false;
    bool _show_size_location = false;
    bool _show_filters = false;
    bool _show_lpes = false;
    bool _show_names = false;
    bool _show_interactivity = false;
    Gtk::Button* _name_toggle = nullptr;
    Widget::WidgetGroup _name_group;
    Gtk::Entry& _obj_title;
    Gtk::Entry& _obj_id;
    Gtk::Button& _obj_set_id;
    Gtk::TextView& _obj_description;
    Gtk::Entry& _obj_label;
    Gtk::Button& _locked;
    Pref<bool> _name_props_visibility = {dlg_pref_path + "/options/show_name_props"};
    Gtk::Button* _inter_toggle = nullptr;
    Widget::WidgetGroup _inter_group;
    std::unique_ptr<ObjectProperties> _obj_interactivity;
    Pref<bool> _inter_props_visibility = {dlg_pref_path + "/options/show_interactivity_props"};
    Widget::InkSpinButton& _blur;
    Gtk::Button& _clear_filters;
    Gtk::Button& _clear_blur;
    Gtk::Button& _add_blur;
    Gtk::Button& _add_filter;
    Inkscape::UI::Widget::DropDownList _filter_dropdown;
    std::vector<SPFilter*> _filter_list; 
    Gtk::ListBox& _lpe_menu;
    Gtk::ListBox& _lpe_list;
    Gtk::MenuButton& _add_lpe;
    Gtk::ScrolledWindow& _lpe_list_wnd;
    // Glib::RefPtr<Gio::ListStore<LPEMetadata>> _lp_effects;
    Glib::RefPtr<Gtk::BoolFilter> _lpe_filter;
    Glib::RefPtr<Gtk::SingleSelection> _lpe_selection_model;
    Gtk::SearchEntry2& _lpe_search;
};

} // namespace details

/**
 * A dialog widget to show object attributes (currently for images and links).
 */
class ObjectAttributes : public DialogBase
{
public:
    ObjectAttributes();
    ~ObjectAttributes() override = default;

    void selectionChanged(Selection *selection) override;
    void selectionModified(Selection *selection, guint flags) override;

    void desktopReplaced() override;
    void documentReplaced() override;

    /**
     * Updates entries and other child widgets on selection change, object modification, etc.
     */
    void widget_setup();

private:
    Glib::RefPtr<Gtk::Builder> _builder;

    std::unique_ptr<details::AttributesPanel> create_panel(int key);
    std::map<int, std::unique_ptr<details::AttributesPanel>> _panels;
    std::unique_ptr<details::AttributesPanel> _multi_obj_panel;
    std::unique_ptr<details::AttributesPanel> _empty_panel;
    details::AttributesPanel* get_panel(Selection* selection);
    details::AttributesPanel* prepare_panel(int tag);
    void cursor_moved(Tools::TextTool* tool);
    // PROOF: build the sections of common objects ahead of time, while idle.
    void schedule_prebuild();
    bool prebuild_step();
    sigc::scoped_connection _prebuild;
    int _paint_prebuilt = 0; ///< Steps done towards the shared paint editors.

    details::AttributesPanel* _current_panel = nullptr;
    OperationBlocker _update;
    Gtk::Box& _main_panel;
    // Contains a pointer to the currently selected item (NULL in case nothing is or multiple objects are selected).
    SPItem* _current_item = nullptr;
    XML::SignalObserver _observer;
    sigc::scoped_connection _cursor_move;
    sigc::scoped_connection _document_modified;
};

} // namespace Inkscape::UI::Dialog

#endif // SEEN_DIALOGS_OBJECT_ATTRIBUTES_H

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
