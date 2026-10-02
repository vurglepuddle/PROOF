// SPDX-License-Identifier: GPL-2.0-or-later
//
// Created by Michael Kowalski on 9/7/24.
//

#ifndef SWATCH_EDITOR_H
#define SWATCH_EDITOR_H

#include <gtkmm/boolfilter.h>
#include <gtkmm/box.h>
#include <gtkmm/builder.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/gridview.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry2.h>
#include <gtkmm/singleselection.h>
#include <ui/operation-blocker.h>

#include "color-picker-panel.h"
#include "edit-operation.h"
#include "resizing-separator.h"
#include "object/sp-gradient.h"
#include "ui/filtered-store.h"

namespace Inkscape::UI::Widget {

class SwatchEditor : public Gtk::Box {
public:
    SwatchEditor(Colors::Space::Type space, Glib::ustring prefs_path);

    void set_desktop(SPDesktop* desktop);
    void set_document(SPDocument* document);
    void select_vector(SPGradient* vector);
    SPGradient* get_selected_vector() const;
    void set_color_picker_plate(ColorPickerPanel::PlateType type);
    ColorPickerPanel::PlateType get_color_picker_plate() const;
    sigc::signal<void (SPGradient*, EditOperation, SPGradient*)> signal_changed() const { return _signal_changed; }
    sigc::signal<void (SPGradient*, const Colors::Color&)> signal_color_changed() const { return _signal_color_changed; }
    sigc::signal<void (SPGradient*, const Glib::ustring&)> signal_label_changed() const { return _signal_label_changed; }
    ColorPickerPanel& get_picker() { return *_color_picker; }

private:
    void build_grid();
    void build_settings();
    void import_swatches();
    void export_swatches();
    void schedule_update();
    void update_store();
    bool is_item_visible(const Glib::RefPtr<Glib::ObjectBase>& item) const;
    void refilter();
    void rebuild();
    void set_view_list_mode(bool list);
    void update_selection(const std::string& id);
    // PROOF: spot-ink switch and description for the selected swatch.
    void update_spot_ui(SPGradient* swatch);
    void toggle_spot();

    Glib::RefPtr<Gtk::Builder> _builder;
    SPDesktop* _desktop = nullptr;
    SPDocument* _document = nullptr;
    Gtk::Box& _main;
    Gtk::ScrolledWindow& _scroll;
    Gtk::GridView& _gridview;
    Glib::RefPtr<Gio::ListStoreBase> _store;
    Glib::RefPtr<Gtk::SingleSelection> _selection_model;
    Glib::RefPtr<Gtk::BoolFilter> _filter;
    bool _show_labels = true;
    std::shared_ptr<Colors::ColorSet> _colors;
    std::unique_ptr<ColorPickerPanel> _color_picker;
    Gtk::SearchEntry2& _search;
    Gtk::Entry& _label;
    Gtk::Button& _new_btn;
    Gtk::Button& _del_btn;
    Gtk::Button& _import_btn;
    Gtk::Button& _export_btn;
    Gtk::Button& _clean_btn;
    Gtk::Popover& _settings;
    std::string _cur_swatch_id;
    sigc::signal<void (SPGradient*, EditOperation, SPGradient*)> _signal_changed;
    sigc::signal<void (SPGradient*, const Colors::Color&)> _signal_color_changed;
    sigc::signal<void (SPGradient*, const Glib::ustring&)> _signal_label_changed;
    sigc::scoped_connection _defs_changed;
    sigc::scoped_connection _rsrc_changed;
    guint _delayed_update = 0;
    int _tile_size = 16;
    int _list_height = 200;
    Glib::ustring _prefs_path;
    OperationBlocker _update;
    ResizingSeparator& _separator;
    Gtk::Box _spot_box;
    Gtk::CheckButton _spot_toggle;
    Gtk::Label _spot_info;
};

} // namespace

#endif //SWATCH_EDITOR_H
