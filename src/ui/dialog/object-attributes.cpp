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

#include "ui/dialog/object-attributes.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <sstream>
#include <tuple>
#include <2geom/rect.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <gdk/gdkkeysyms.h>
#include <glibmm/i18n.h>
#include <glibmm/ustring.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filterlistmodel.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textview.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/version.h>

#include "css-chemistry.h"
#include "desktop.h"
#include "document-undo.h"
#include "dialog-container.h"
#include "filter-chemistry.h"
#include "filter-enums.h"
#include "filter-effects-dialog.h"
#include "id-clash.h"
#include "layer-manager.h"
#include "livepatheffect-editor.h"
#include "mod360.h"
#include "preferences.h"
#include "selection-chemistry.h"
#include "selection.h"
#include "style.h"
#include "actions/actions-tools.h"
#include "live_effects/effect.h"
#include "live_effects/lpeobject.h"
#include "live_effects/lpeobject-reference.h"
#include "object/filters/gaussian-blur.h"
#include "object/sp-anchor.h"
#include "object/sp-ellipse.h"
#include "object/sp-gradient.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-lpe-item.h"
#include "object/sp-namedview.h"
#include "object/sp-object-iterator.h"
#include "object/sp-object.h"
#include "object/sp-path.h"
#include "object/sp-pattern.h"
#include "object/sp-polygon.h"
#include "object/sp-polyline.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-rect.h"
#include "object/sp-star.h"
#include "object/sp-stop.h"
#include "object/sp-text.h"
#include "object/sp-textpath.h"
#include "object/sp-use.h"
#include "ui/builder-utils.h"
#include "ui/controller.h"
#include "ui/gridview-utils.h"
#include "ui/icon-names.h"
#include "ui/syntax.h"
#include "ui/util.h"
#include "ui/tools/object-picker-tool.h"
#include "ui/tools/text-tool.h"
#include "ui/widget/image-properties.h"
#include "ui/widget/ink-property-grid.h"
#include "ui/widget/object-composite-settings.h"
#include "ui/widget/paint-attribute.h"
#include "util/object-modified-tags.h"
#include "widgets/sp-attribute-widget.h"

namespace Inkscape::UI::Dialog {

using namespace Inkscape::UI::Utils;
using Parts = Widget::PaintAttribute::Parts;

const auto TAG = get_next_object_modified_tag();
constexpr int MARGIN = 4;
// Some panels are not ready and kept behind this flag
constexpr bool INCLUDE_EXPERIMENTAL_PANELS = false;

namespace {

void enter_group(SPDesktop* desktop, SPGroup* group) {
    if (!desktop || !group) return;

    desktop->layerManager().setCurrentLayer(group);
    desktop->getSelection()->clear();
}

const PathEffectList* get_item_lpe_list(const SPObject* object) {
    auto lpe = cast<SPLPEItem>(object);
    if (!lpe || !lpe->path_effect_list) return nullptr;

    // todo: do hidden or incomplete lpe items need special attention?
    // simple approach at first:
    return lpe->path_effect_list;
}

bool is_row_filtered_in(const Glib::RefPtr<LPEMetadata>& item, const Glib::ustring& text) {// Gtk::ListBoxRow* row, const Glib::ustring& text) {
    if (!item) return false;

    if (text.empty()) return true;

    return item->label.lowercase().find(text.lowercase()) != std::string::npos;
}

void apply_lpeffect(SPItem* item, LivePathEffect::EffectType type) {
    if (!item) return;

    auto key = LivePathEffect::LPETypeConverter.get_key(type);
    LivePathEffect::Effect::createAndApply(key.c_str(), item->document, item);
    DocumentUndo::done(item->document, RC_("Undo", "Create and apply path effect"), INKSCAPE_ICON("dialog-path-effects"));
}

void remove_lpeffect(SPObject* object, int index) {
    auto list = get_item_lpe_list(object);
    if (!list) return;

    auto lpe_item = cast<SPLPEItem>(object);
    int i = 0;
    for (auto&& lpe : *list) {
        if (index == i) {
            if (auto effect = lpe->lpeobject ? lpe->lpeobject->get_lpe() : nullptr) {
                lpe_item->removePathEffect(effect, false);
                DocumentUndo::done(object->document, RC_("Undo", "Removed live path effect"), INKSCAPE_ICON("dialog-path-effects"));
            }
            break;
        }
        i++;
    }
}

} // namespace

struct SPAttrDesc {
    char const *label;
    char const *attribute;
};

static const SPAttrDesc anchor_desc[] = {
    { N_("Href:"), "xlink:href"},
    { N_("Target:"), "target"},
    { N_("Type:"), "xlink:type"},
    // TRANSLATORS: for info, see http://www.w3.org/TR/2000/CR-SVG-20000802/linking.html#AElementXLinkRoleAttribute
    // Identifies the type of the related resource with an absolute URI
    { N_("Role:"), "xlink:role"},
    // TRANSLATORS: for info, see http://www.w3.org/TR/2000/CR-SVG-20000802/linking.html#AElementXLinkArcRoleAttribute
    // For situations where the nature/role alone isn't enough, this offers an additional URI defining the purpose of the link.
    { N_("Arcrole:"), "xlink:arcrole"},
    // TRANSLATORS: for info, see http://www.w3.org/TR/2000/CR-SVG-20000802/linking.html#AElementXLinkTitleAttribute
    { N_("Title:"), "xlink:title"},
    { N_("Show:"), "xlink:show"},
    // TRANSLATORS: for info, see http://www.w3.org/TR/2000/CR-SVG-20000802/linking.html#AElementXLinkActuateAttribute
    { N_("Actuate:"), "xlink:actuate"},
    { nullptr, nullptr}
};


ObjectAttributes::ObjectAttributes()
    : DialogBase(details::dlg_pref_path.c_str(), "ObjectProperties"),
    _builder(create_builder("object-attributes.glade")),
    _main_panel(get_widget<Gtk::Box>(_builder, "main-panel"))
{
    auto& main = get_widget<Gtk::Box>(_builder, "main-widget");
    append(main);

    // install observer to catch sodipodi:insensitive attribute change, not reported by selection modified
    _observer.signal_changed().connect([this](auto change, auto str) {
        if (change == XML::SignalObserver::Attribute) {
            if (_update.pending() || !getDesktop() || !_current_panel || !_current_item) return;

            _current_panel->update_lock(_current_item);
        }
    });
}

void ObjectAttributes::widget_setup() {
    if (_update.pending() || !getDesktop()) return;

    auto selection = getDesktop()->getSelection();
    auto item = selection->singleItem();

    if (item != _current_item) {
        _observer.set(item);
    }

    auto scoped(_update.block());

    auto panel = get_panel(selection);

    if (panel != _current_panel && _current_panel) {
        _current_panel->update_panel(nullptr, nullptr, false);
        _main_panel.remove(_current_panel->widget());
    }

    _current_panel = panel;
    _current_item = nullptr;

    if (panel) {
        if (_main_panel.get_children().empty()) {
            auto& w = panel->widget();
            w.set_expand();
            _main_panel.append(w);
        }
        panel->update_panel(item, getDesktop(), false);
        panel->widget().set_visible(true);
    }

    _current_item = item;
}

void ObjectAttributes::desktopReplaced() {
    if (_current_panel) {
        _current_panel->set_desktop(getDesktop());
    }
    if (auto desktop = getDesktop()) {
        _cursor_move = desktop->connect_text_cursor_moved([this](auto tool) {
            cursor_moved(tool);
        });
    }
}

void ObjectAttributes::cursor_moved(Tools::TextTool* tool) {
    if (_current_panel) {
        auto s = tool->get_subselection(false);
        _current_panel->subselection_changed(s);
    }
    //TODO: text panel
}

void ObjectAttributes::documentReplaced() {
    auto doc = getDocument();
    for (auto& kv : _panels) {
        if (kv.second) kv.second->set_document(doc);
    }
    if (_multi_obj_panel) _multi_obj_panel->set_document(doc);
    //todo: watch doc modified to update locked state of current obj
}

void ObjectAttributes::selectionChanged(Selection* selection) {
    widget_setup();
}

void ObjectAttributes::selectionModified(Selection* _selection, guint flags) {
    if (_update.pending() || !getDesktop() || !_current_panel) return;

    auto selection = getDesktop()->getSelection();
    if (flags & (SP_OBJECT_MODIFIED_FLAG |
                 SP_OBJECT_CHILD_MODIFIED_FLAG |
                 SP_OBJECT_PARENT_MODIFIED_FLAG |
                 SP_OBJECT_STYLE_MODIFIED_FLAG)) {

        auto item = selection->singleItem();
        if (item == _current_item) {
            _current_panel->update_panel(item, getDesktop(), !!(flags & TAG));
        }
        else {
            g_warning("ObjectAttributes: missed selection change?");
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

namespace {

std::tuple<bool, double, double> round_values(double x, double y) {
    auto a = std::round(x);
    auto b = std::round(y);
    return std::make_tuple(a != x || b != y, a, b);
}

std::tuple<bool, double, double> round_values(Widget::InkSpinButton& x, Widget::InkSpinButton& y) {
    return round_values(x.get_adjustment()->get_value(), y.get_adjustment()->get_value());
}

const LivePathEffectObject* find_lpeffect(SPLPEItem* item, LivePathEffect::EffectType etype) {
    if (!item) return nullptr;

    auto lpe = item->getFirstPathEffectOfType(Inkscape::LivePathEffect::FILLET_CHAMFER);
    if (!lpe) return nullptr;
    return lpe->getLPEObj();
}

void remove_lpeffect(SPLPEItem* item, LivePathEffect::EffectType type) {
    if (auto effect = find_lpeffect(item, type)) {
        item->setCurrentPathEffect(effect);
        auto document = item->document;
        item->removeCurrentPathEffect(false);
        DocumentUndo::done(document, RC_("Undo", "Remove live path effect"), INKSCAPE_ICON("dialog-path-effects"));
    }
}

std::optional<double> get_number(SPItem* item, const char* attribute) {
    if (!item) return {};

    auto val = item->getAttribute(attribute);
    if (!val) return {};

    return item->getRepr()->getAttributeDouble(attribute);
}

void set_dimension_adj(Widget::InkSpinButton& btn) {
    btn.set_adjustment(Gtk::Adjustment::create(0, 0, 1'000'000, 1, 5));
}

void set_location_adj(Widget::InkSpinButton& btn) {
    btn.set_adjustment(Gtk::Adjustment::create(0, -1'000'000, 1'000'000, 1, 5));
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

details::AttributesPanel::AttributesPanel()
    : _builder(create_builder("object-properties.ui"))
    , _x(get_widget<Widget::InkSpinButton>(_builder, "obj-x"))
    , _y(get_widget<Widget::InkSpinButton>(_builder, "obj-y"))
    , _width(get_widget<Widget::InkSpinButton>(_builder, "obj-width"))
    , _height(get_widget<Widget::InkSpinButton>(_builder, "obj-height"))
    , _round_loc(get_widget<Gtk::Button>(_builder, "round-location"))
    , _round_size(get_widget<Gtk::Button>(_builder, "round-size"))
    , _obj_label(get_widget<Gtk::Entry>(_builder, "obj-label"))
    , _locked(get_widget<Gtk::Button>(_builder, "obj-lock"))
    , _obj_title(get_widget<Gtk::Entry>(_builder, "obj-title"))
    , _obj_id(get_widget<Gtk::Entry>(_builder, "obj-id"))
    , _obj_set_id(get_widget<Gtk::Button>(_builder, "obj-set-id"))
    , _obj_description(get_widget<Gtk::TextView>(_builder, "obj-description"))
    , _clear_filters(get_widget<Gtk::Button>(_builder, "clear-filters"))
    , _clear_blur(get_widget<Gtk::Button>(_builder, "clear-blur"))
    , _add_blur(get_widget<Gtk::Button>(_builder, "add-blur"))
    , _add_filter(get_widget<Gtk::Button>(_builder, "add-filter"))
    , _blur(get_widget<Widget::InkSpinButton>(_builder, "filter-blur"))
    , _lpe_menu(get_widget<Gtk::ListBox>(_builder, "lpe-menu"))
    , _lpe_search(get_widget<Gtk::SearchEntry2>(_builder, "lpe-search"))
    , _lpe_list(get_widget<Gtk::ListBox>(_builder, "lpe-list"))
    , _lpe_list_wnd(get_widget<Gtk::ScrolledWindow>(_builder, "lpe-list-wnd"))
    , _add_lpe(get_widget<Gtk::MenuButton>(_builder, "add-lpe"))
    {

    _grid.set_indent(MARGIN);
    _widget = &_grid;
    _tracker = std::make_unique<UI::Widget::UnitTracker>(Inkscape::Util::UNIT_TYPE_LINEAR);
    //todo: is this needed?
    // auto init_units = desktop->getNamedView()->display_units;
    // _tracker->setActiveUnit(init_units);
#if GTKMM_CHECK_VERSION(4, 18, 0)
    _lpe_menu.set_tab_behavior(Gtk::ListTabBehavior::ITEM);
#endif
}

void details::AttributesPanel::add_fill_and_stroke(Parts parts) {
    _paint.reset(new Widget::PaintAttribute(parts, TAG));
    _paint->insert_widgets(_grid);
    _show_fill_stroke = true;
}

void details::AttributesPanel::transform() {
    if (!_document || _update.pending()) return;

    auto scoped(_update.block());
    // todo: expose the units?
    auto unit = _document->getDisplayUnit();
    auto prefs = Preferences::get();
    bool transform_stroke = prefs->getBool("/options/transform/stroke", true);
    bool preserve_transform = prefs->getBool("/options/preservetransform/value", false);
    auto use_visual_box = prefs->getInt("/tools/bounding_box") == 0;
    auto rect = Geom::Rect::from_xywh(_x.get_value(), _y.get_value(), _width.get_value(), _height.get_value());
    sp_transform_selected_items(_desktop, rect, unit, "object-properties-", transform_stroke, preserve_transform, use_visual_box);
}

void details::AttributesPanel::update_label(SPObject* object, Inkscape::Selection* selection) {
    if (!_show_obj_label) return;

    _obj_label.set_sensitive(object != nullptr);
    // if a user-edited label is present, use it
    _obj_label.set_text(object && object->label() ? object->label() : "");

    auto title = get_title(selection);
    if (object) {
        _obj_label.set_placeholder_text(title);
    }
    else {
        // label is disabled; placeholder is barely visible; set text instead
        _obj_label.set_placeholder_text("");
        _obj_label.set_text(title);
    }
}

void details::AttributesPanel::add_size_properties() {
    _show_size_location = true;

    _round_loc.signal_clicked().connect([this]{
        auto [changed, x, y] = round_values(_x, _y);
        if (changed) {
            _x.get_adjustment()->set_value(x);
            _y.get_adjustment()->set_value(y);
        }
    });

    _round_size.signal_clicked().connect([this]{
        auto [changed, x, y] = round_values(_width, _height);
        if (changed) {
            _width.get_adjustment()->set_value(x);
            _height.get_adjustment()->set_value(y);
        }
    });

    _x.signal_value_changed().connect([this](auto){ transform(); });
    _y.signal_value_changed().connect([this](auto){ transform(); });
    _width.signal_value_changed().connect([this](auto){ transform(); });
    _height.signal_value_changed().connect([this](auto){ transform(); });

    Widget::reparent_properties(get_widget<Gtk::Grid>(_builder, "size-props"), _grid);
}

void details::AttributesPanel::add_name_properties() {
    if (_show_names) return;

    _show_names = true;
    _name_toggle = _grid.add_section(_("Description"));
    _name_group = Widget::reparent_properties(get_widget<Gtk::Grid>(_builder, "name-props"), _grid, true, false);
    _grid.add_section_divider();

    _name_toggle->signal_clicked().connect([this] {
        bool show = !_name_props_visibility;
        show_name_properties(show);
        Preferences::get()->setBool(_name_props_visibility.observed_path, show);
    });
    show_name_properties(_name_props_visibility);
    _name_props_visibility.action = [this] {
        show_name_properties(_name_props_visibility);
    };

    _obj_set_id.signal_clicked().connect([this] {
        if (_update.pending() || !_current_object || !_current_object->document) return;

        auto id = _obj_id.get_text();
        auto [valid, warning] = is_object_id_valid(id.raw());
        if (!valid) return;

        auto scoped(_update.block());
        _current_object->setAttribute("id", id);
        DocumentUndo::done(_current_object->document, RC_("Undo", "Set object ID"), INKSCAPE_ICON("dialog-object-properties"));
    });
    _obj_id.signal_changed().connect([this] {
        if (_update.pending() || !_current_object || !_document) return;

        // check entered ID and show the warning icon as needed
        validate_obj_id();
    });

    _obj_title.signal_changed().connect([this] {
        if (_update.pending() || !_current_object || !_current_object->document) return;

        auto scoped(_update.block());
        if (_current_object->setTitle(_obj_title.get_text().c_str())) {
            _current_object->requestModified(SP_OBJECT_MODIFIED_FLAG | TAG);
            DocumentUndo::maybeDone(_current_object->document, "set-obj-title", RC_("Undo", "Set object title"), INKSCAPE_ICON("dialog-object-properties"));
        }
    });
    _obj_description.get_buffer()->signal_changed().connect([this] {
        if (_update.pending() || !_current_object || !_current_object->document) return;

        auto scoped(_update.block());
        if (_current_object->setDesc(_obj_description.get_buffer()->get_text().c_str())) {
            _current_object->requestModified(SP_OBJECT_MODIFIED_FLAG | TAG);
            DocumentUndo::maybeDone(_current_object->document, "set-obj-desc", RC_("Undo", "Set object description"), INKSCAPE_ICON("dialog-object-properties"));
        }
    });
}

void details::AttributesPanel::add_interactivity_properties() {
    if (_show_interactivity) return;

    _obj_interactivity = std::make_unique<ObjectProperties>();
    _obj_interactivity->get_attr_table()->set_modified_tag(TAG);
    _show_interactivity = true;

    _inter_toggle = _grid.add_section(_("Interactivity"));
    _inter_group = Widget::reparent_properties(_obj_interactivity->get_grid(), _grid, false, true);
    _grid.add_section_divider();

    _grid.add_gap();
    auto js = Gtk::make_managed<Gtk::Label>();
    js->set_markup(_("<small><i>Enter JavaScript code for interactive behavior in a browser.</i></small>"));
    js->set_ellipsize(Pango::EllipsizeMode::END);
    js->set_xalign(0.0);
    _grid.add_row(js);
    _inter_group.add(js);

    _inter_toggle->signal_clicked().connect([this] {
        bool show = !_inter_props_visibility;
        show_interactivity_properties(show);
        Preferences::get()->setBool(_inter_props_visibility.observed_path, show);
    });
    show_interactivity_properties(_inter_props_visibility);
    _inter_props_visibility.action = [this] {
        show_interactivity_properties(_inter_props_visibility);
    };
}

void details::AttributesPanel::add_header(const Glib::ustring& title) {
    auto label = Gtk::make_managed<Gtk::Label>(title);
    label->set_halign(Gtk::Align::START);
    label->set_xalign(0.0f);
    label->add_css_class("grid-section-title");
    _grid.add_row(label);
}

void details::AttributesPanel::select_lpe_row(int dir) {
    if (!_lpe_selection_model) return;

    int selected = _lpe_selection_model->get_selected();
    auto n = _lpe_selection_model->get_n_items();
    auto new_selection = selected >= n || selected < 0 ? 0 : selected + dir;
    if (new_selection >= 0 && new_selection < n) {
        // new selection
        _lpe_selection_model->set_selected(new_selection);

        auto row = _lpe_menu.get_row_at_index(new_selection);
        if (!row) return;
        _lpe_menu.select_row(*row);

        // scroll into view
        Gdk::Graphene::Point pt(0.0f, 0.0f);
        auto location = row->compute_point(_lpe_menu, pt);
        auto adj = _lpe_menu.get_adjustment();
        if (adj && location.has_value()) {
            adj->set_value(location->get_y() - (adj->get_page_size() - row->get_preferred_size().natural.get_height()) / 2);
        }
    }
}

void details::AttributesPanel::apply_selected_lpe() {
    auto selected = _lpe_selection_model->get_selected_item();
    if (selected && _current_object) {
        auto lpe = std::dynamic_pointer_cast<LPEMetadata>(selected);
        apply_lpeffect(cast<SPItem>(_current_object), lpe->type);
    }
    _add_lpe.popdown();
}

bool details::AttributesPanel::on_key_pressed(guint keyval, guint keycode, Gdk::ModifierType state) {
    switch (keyval) {
    case GDK_KEY_Escape: // Cancel
        _add_lpe.popdown();
        return true;
    case GDK_KEY_Up:
        select_lpe_row(-1);
        return true;
    case GDK_KEY_Down:
        select_lpe_row(1);
        return true;
    case GDK_KEY_Return:
        apply_selected_lpe();
        return true;
    default:
        return false;
    }
}

void details::AttributesPanel::refilter_lpes() {
    auto expression = Gtk::ClosureExpression<bool>::create([this](auto& item) {
        auto text = _lpe_search.get_text();
        auto lpe = std::dynamic_pointer_cast<LPEMetadata>(item);
        return is_row_filtered_in(lpe, text);
    });
    // filter results
    _lpe_filter->set_expression(expression);
    // enforce selection after filtering
    select_lpe_row();
}


void details::AttributesPanel::add_lpes(bool clone) {
    Widget::reparent_properties(get_widget<Gtk::Grid>(_builder, "lpe-box"), _grid);
    _grid.add_section_divider();
    _show_lpes = true;

    _lpe_search.set_key_capture_widget(_lpe_menu);
    auto key_entry = Gtk::EventControllerKey::create();
    key_entry->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    key_entry->signal_key_pressed().connect(sigc::mem_fun(*this, &AttributesPanel::on_key_pressed), false); // Before default handler.
    _add_lpe.get_popover()->add_controller(key_entry);
    _lpe_filter = Gtk::BoolFilter::create({});

    _add_lpe.get_popover()->signal_show().connect([this, clone]{
        if (_lpe_selection_model) return;
        // before opening a popup, create a list of LPEs
        auto store = Gio::ListStore<LPEMetadata>::create();
        bool experimental = Preferences::get()->getBool("/dialogs/livepatheffect/showexperimental", false);
        auto list = get_list_of_applicable_lpes(cast<SPLPEItem>(_current_object), clone, experimental);
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            // sort by name only
            return a->label < b->label;
        });
        for (auto lpe : list) {
            if (lpe->sensitive) store->append(lpe);
        }
        auto filtered_model = Gtk::FilterListModel::create(store, _lpe_filter);
        _lpe_selection_model = Gtk::SingleSelection::create(filtered_model);
        _lpe_menu.bind_model(_lpe_selection_model, [](const Glib::RefPtr<Glib::Object>& item) {
            auto lpe = std::dynamic_pointer_cast<LPEMetadata>(item);
            auto box = Gtk::make_managed<Gtk::Box>();
            box->set_spacing(4);
            box->set_margin(1);
            auto icon = Gtk::make_managed<Gtk::Image>();
            icon->set_from_icon_name(lpe->icon_name);
            box->append(*icon);
            auto label = Gtk::make_managed<Gtk::Label>(lpe->label);
            label->set_halign(Gtk::Align::START);
            box->append(*label);
            box->set_tooltip_text(lpe->tooltip);
            return box;
        });
        select_lpe_row();
    });

    refilter_lpes();

    _lpe_search.signal_search_changed().connect([this] {
        refilter_lpes();
    });

    // menu row clicked
    _lpe_menu.signal_row_activated().connect([this](auto row) {
        if (row) {
            _lpe_menu.select_row(*row);
            _lpe_selection_model->set_selected(row->get_index());
            apply_selected_lpe();
        }
    });

    // list of applied LPEs - row double-click
    _lpe_list.signal_row_activated().connect([this](auto row) {
        // go to the LPE editor
        if (auto container = _desktop->getContainer()) {
            container->new_dialog("LivePathEffect");
        }
    });
}

void details::AttributesPanel::add_filters(bool separate) {
    if (separate) {
        _grid.add_gap();
        _grid.add_section_divider();
    }
    Widget::reparent_properties(get_widget<Gtk::Grid>(_builder, "filter-box"), _grid);
    _grid.add_section_divider();
    _show_filters = true;
    _filter_dropdown.set_hexpand();
    get_widget<Gtk::Box>(_builder, "filter-dropdown").append(_filter_dropdown);

    _clear_filters.signal_clicked().connect([this] {
        if (!can_update()) return;

        auto item = cast<SPItem>(_current_object);
        if (!item) return;
        auto filter = item->style ? item->style->getFilter() : nullptr;
        auto scoped(_update.block());
        remove_filter(_current_object, false);
        if (filter) {
            filter->collectOrphan();
        }
        DocumentUndo::done(_current_object->document, RC_("Undo", "Remove filter"), "dialog-fill-and-stroke", TAG);
        update_filters(_current_object);
    });
    _add_blur.signal_clicked().connect([this] {
        if (!can_update()) return;

        auto scoped(_update.block());
        if (modify_filter_gaussian_blur_amount(cast<SPItem>(_current_object), 10.0)) {
            DocumentUndo::done(_current_object->document, RC_("Undo", "Add blur filter"), "dialog-fill-and-stroke", TAG);
            update_filters(_current_object);
        }
    });
    _clear_blur.signal_clicked().connect([this] {
        if (!can_update()) return;

        auto scoped(_update.block());
        if (remove_filter_gaussian_blur(_current_object)) {
            DocumentUndo::done(_current_object->document, RC_("Undo", "Remove blur filter"), "dialog-fill-and-stroke", TAG);
            update_filters(_current_object);
        }
    });
    _add_filter.signal_clicked().connect([this] {
        if (!_desktop || !_document) return;

        auto item = cast<SPItem>(_current_object);
        if (!item) return;
        auto filter = item->style ? item->style->getFilter() : nullptr;
        if (!filter) {
            auto scoped(_update.block());
            filter = new_filter(_document);
            auto count = _document->getResourceList("filter").size();
            std::ostringstream os;
            os << _("filter") << count;
            filter->setLabel(os.str().c_str());
            sp_style_set_property_url(item, "filter", filter, false);
            DocumentUndo::done(_document, RC_("Undo", "Add filter"), "dialog-fill-and-stroke", TAG);
            update_filters(_current_object);
        }
        if (auto container = _desktop->getContainer()) {
            container->new_dialog("FilterEffects");
            if (auto dialog = dynamic_cast<FilterEffectsDialog*>(container->get_dialog("FilterEffects"))) {
                dialog->select_filter(filter);
            }
        }
    });
    _filter_dropdown.signal_changed().connect([this] {
      if (!can_update()) return;

      auto pos = _filter_dropdown.get_selected();
      if (pos >= _filter_list.size()) return;
      auto filter = _filter_list[pos];

      auto item = cast<SPItem>(_current_object);
      if (!filter || !item || !filter->valid_for(item)) return;

      auto scoped(_update.block());
      sp_style_set_property_url(item, "filter", filter, false);
      DocumentUndo::done(_current_object->document, RC_("Undo", "Change filter"), "dialog-fill-and-stroke", TAG);
      update_filters(_current_object, false);
    });
    _blur.signal_value_changed().connect([this](auto value) {
         if (!can_update()) return;

         auto scoped(_update.block());
         if (modify_filter_gaussian_blur_amount(cast<SPItem>(_current_object), value * 100)) {
             DocumentUndo::maybeDone(_current_object->document, "change-blur-radius", RC_("Undo", "Change blur filter"), "dialog-fill-and-stroke", TAG);
        }
    });
  
}

void details::AttributesPanel::populate_filter_menu() {
    auto scoped(_update.block());
    _filter_dropdown.remove_all();
    _filter_list.clear();
    
    if (!_document || !_current_object) return;

    auto item = cast<SPItem>(_current_object);
    auto current = item && item->style ? item->style->getFilter() : nullptr;
    int current_pos = -1;

    for (auto obj : _document->getResourceList("filter")) {
        auto filter = cast<SPFilter>(obj);
        if (!filter) continue;

        auto label = filter->label();
        auto id = filter->getId();
        Glib::ustring name = label ? label : (id ? id : _("Filter"));

        if (filter == current) {
            current_pos = static_cast<int>(_filter_list.size());
        }
        _filter_dropdown.append(name);
        _filter_list.push_back(filter);
    }
     
    if (current_pos >= 0) {
        _filter_dropdown.set_selected(current_pos);
    }
}

void details::AttributesPanel::set_document(SPDocument* document) {
    _document = document;
    if (_show_fill_stroke) {
        _paint->set_document(document);
    }
    _resource_changed.disconnect();
    if (document) {
        _resource_changed = document->connectResourcesChanged("filter", [this]{
            update_filters(_current_object);
        });
    }
}

void details::AttributesPanel::set_desktop(SPDesktop* desktop) {
    _desktop = desktop;
    if (_show_fill_stroke) {
        _paint->set_desktop(desktop);
    }
}

void details::AttributesPanel::update_panel(SPObject* object, SPDesktop* desktop, bool tagged) {
    if (object && object->document) {
        auto scoped(_update.block());
        auto units = object->document->getNamedView() ? object->document->getNamedView()->display_units : nullptr;
        if (units) _tracker->setActiveUnit(units);
    }

    set_desktop(desktop);
    _current_object = object;

    if (!_update.pending()) {
        if (tagged) {
            // tagged updates originate from this dialog, so ignore them, but refresh size,
            // as it depends on visual bounding box impacted by stroke width among other attributes
            update_size_location();
        }
        else {
            // "Selection" at the top (a label)
            update_label(object, desktop ? desktop->getSelection() : nullptr);
            // update object's lock state
            update_lock(object);
            // fill and stroke
            update_paint(object);
            // location and size
            update_size_location();
            // update current filter
            update_filters(object);
            // update list of live path effects
            update_lpes(object);
            // title and description
            update_names(object);
            // JavaScript event handlers
            update_interactive_props(object);
            // element-specific properties
            update(object);
        }
    }
}

Glib::ustring details::AttributesPanel::get_title(Selection* selection) const {
    if (!selection) return _title;

    if (auto item = selection->singleItem()) {
        return get_synthetic_object_name(item);
    }
    // no selection or multiple selected
    return _title;
}

void details::AttributesPanel::update_lock(SPObject* object) {
    if (!_show_obj_label) return;

    if (auto item = cast<SPItem>(object)) {
        _locked.set_visible();
        _locked.set_icon_name(!item->sensitive ? "object-locked" : "object-unlocked");
    }
    else {
        _locked.set_visible(false);
    }
}

void details::AttributesPanel::update_paint(SPObject* object) {
    if (_show_fill_stroke) {
        _paint->update_visibility(object);
        _paint->update_from_object(object);
    }
}

bool details::AttributesPanel::can_update() const {
    return _current_object && _current_object->style && !_update.pending();
}

void details::AttributesPanel::update_size_location() {
    if (!_show_size_location || !_document) return;

    auto scoped(_update.block());

    auto use_visual_box = Preferences::get()->getInt("/tools/bounding_box") == 0;
    auto rect = sp_selection_get_xywh(_desktop, _document->getDisplayUnit(), use_visual_box);
    _x.set_value(rect.min().x());
    _y.set_value(rect.min().y());
    _width.set_value(rect.width());
    _height.set_value(rect.height());
}

void details::AttributesPanel::update_filters(SPObject* object, bool update_menu) {
    // Stop UI from changing filters
    auto scoped(_update.block());

    auto item = cast<SPItem>(object);
    auto filter = item && item->style ? item->style->getFilter() : nullptr;

    bool gaussian_blur = false;
    bool other_filter = false;
    size_t other_filter_count = 0;

    if (filter) {
        for (auto &primitive : filter->children) {
            if (cast<SPGaussianBlur>(&primitive)) {
                gaussian_blur = true;
            } else {
                other_filter = true;
                ++other_filter_count;
            }
        }
    }

    // Update blur value
    if (gaussian_blur) {
        double blur = 0;

        if (auto radius = object_query_blur_filter(item)) {
            if (auto bbox = item->desktopGeometricBounds()) {
                double perimeter = bbox->dimensions()[Geom::X] + bbox->dimensions()[Geom::Y];
                blur = std::sqrt(*radius * Widget::BLUR_MULTIPLIER / perimeter);
            }
        }

        _blur.set_value(blur);
    } else {
        _blur.set_value(0);
    }

   // Update visibility
   _blur.set_visible(gaussian_blur);
   _blur.set_sensitive(gaussian_blur);
   _clear_blur.set_visible(gaussian_blur);

   _clear_filters.set_visible(filter != nullptr);
 
   _add_blur.set_visible(!gaussian_blur);
   if(update_menu) {
      populate_filter_menu();
   }
   _filter_dropdown.set_visible(filter && !_filter_list.empty());
   _add_filter.set_visible(true);
   _add_filter.set_icon_name(filter ? "edit" : "plus");
   _add_filter.set_tooltip_text(filter ? _("Edit filter") : _("Add filter"));
   
}

void details::AttributesPanel::update_lpes(SPObject* object) {
    if (!_show_lpes) return;

    // auto lpe_count = get_lpe_count(object);
    auto list = get_item_lpe_list(object);
    if (list && !list->empty()) {
        _lpe_list.remove_all();
        // list LPEs
        int index = 0;
        for (auto&& lpe : *list) {
            if (auto effect = lpe->lpeobject ? lpe->lpeobject->get_lpe() : nullptr) {
                auto icon_name = LivePathEffect::LPETypeConverter.get_icon(effect->effectType());
                auto box = Gtk::make_managed<Gtk::Box>();
                box->set_spacing(4);
                auto icon = Gtk::make_managed<Gtk::Image>();
                icon->set_from_icon_name(icon_name);
                box->append(*icon);
                auto label = Gtk::make_managed<Gtk::Label>(effect->getName());
                label->set_halign(Gtk::Align::START);
                label->set_hexpand();
                label->set_xalign(0);
                label->set_ellipsize(Pango::EllipsizeMode::END);
                box->append(*label);
                auto close = Gtk::make_managed<Gtk::Button>();
                close->set_has_frame(false);
                close->set_icon_name("minus");
                close->add_css_class("reduced-padding");
                close->set_tooltip_text("Remove effect");
                close->signal_clicked().connect([this, index] {
                    // remove the LPE
                    remove_lpeffect(_current_object, index);
                });
                box->append(*close);
                _lpe_list.append(*box);
                index++;
            }
        }
        _lpe_list_wnd.set_visible(true);
    }
    else {
        _lpe_list_wnd.set_visible(false);
        // _lpe_widgets.set_visible(false);
    }
}

void details::AttributesPanel::update_names(SPObject* object) {
    if (!_show_names || !object || !_document) return;

    auto scoped(_update.block());

    auto title = object->title();
    _obj_title.set_text(title ? title : "");
    g_free(title);
    auto description = object->desc();
    _obj_description.get_buffer()->set_text(description ? description : "");
    g_free(description);
    auto id = object->getId();
    _obj_id.set_text(id ? id : "");
}

void details::AttributesPanel::update_interactive_props(SPObject* object) {
    if (!_show_interactivity || !object || !_document) return;

    auto scoped(_update.block());

    _obj_interactivity->get_attr_table()->change_object(object);
}

void details::AttributesPanel::validate_obj_id() {
    auto id = _obj_id.get_text();
    auto [valid, warning] = is_object_id_valid(id.raw());
    if (valid) {
        auto current = _current_object->getId();
        if (!current) current = "";
        if (id != current && _document->getObjectById(id.raw())) {
            valid = false;
            warning = _("This ID is already in use");
        }
    }
    _obj_id.property_secondary_icon_name().set_value(valid ? "" : "dialog-warning");
    _obj_id.set_icon_tooltip_text(warning, Gtk::Entry::IconPosition::SECONDARY);

    _obj_set_id.set_sensitive(valid);
}

void details::AttributesPanel::show_name_properties(bool expand) {
    _name_group.set_visible(expand);
    _grid.open_section(_name_toggle, expand);
}

void details::AttributesPanel::show_interactivity_properties(bool expand) {
    _inter_group.set_visible(expand);
    _grid.open_section(_inter_toggle, expand);
}

void details::AttributesPanel::change_value_px(SPObject* object, const char* key, double input, const char* attr, std::function<void (double)>&& setter) {
    if (_update.pending() || !object) return;

    auto scoped(_update.block());

    const auto unit = _tracker->getActiveUnit();
    auto value = Util::Quantity::convert(input, unit, "px");
    if (value != 0 || attr == nullptr) {
        setter(value);
    }
    else if (attr) {
        object->removeAttribute(attr);
    }

    DocumentUndo::maybeDone(object->document, key, RC_("Undo", "Change object attribute"), ""); //TODO INKSCAPE_ICON("draw-rectangle"));
}

void details::AttributesPanel::change_angle(SPObject* object, const char* key, double angle, std::function<void (double)>&& setter) {
    if (_update.pending() || !object) return;

    auto scoped(_update.block());

    auto value = degree_to_radians_mod2pi(angle);
    setter(value);

    DocumentUndo::maybeDone(object->document, key, RC_("Undo", "Change object attribute"), ""); //TODO INKSCAPE_ICON("draw-rectangle"));
}

void details::AttributesPanel::change_value(SPObject* object, const Glib::RefPtr<Gtk::Adjustment>& adj, std::function<void (double)>&& setter) {
    if (_update.pending() || !object) return;

    auto scoped(_update.block());

    auto value = adj ? adj->get_value() : 0;
    setter(value);

    DocumentUndo::done(object->document, RC_("Undo", "Change object attribute"), ""); //TODO INKSCAPE_ICON("draw-rectangle"));
}

void details::AttributesPanel::add_object_label() {
    Widget::reparent_properties(get_widget<Gtk::Grid>(_builder, "label-props"), _grid);
    _grid.add_gap();
    _show_obj_label = true;

    _obj_label.signal_changed().connect([this] {
        if (_update.pending() || !_current_object || !_current_object->document) return;

        auto scoped(_update.block());
        auto new_label = _obj_label.get_text();
        auto current_label = _current_object->label();
        if (new_label.compare(current_label ? current_label : "") != 0) {
            _current_object->setLabel(new_label.c_str());
            _current_object->requestModified(SP_OBJECT_MODIFIED_FLAG | TAG);
            DocumentUndo::maybeDone(_current_object->document, "set-obj-label", RC_("Undo", "Set object label"), INKSCAPE_ICON("dialog-object-properties"));
        }
    });

    _locked.signal_clicked().connect([this] {
        auto item = cast<SPItem>(_current_object);
        if (_update.pending() || !item) return;

        bool lock = item->sensitive;
        item->setLocked(lock);
        DocumentUndo::done(item->document, lock ? RC_("Undo", "Lock object") : RC_("Undo", "Unlock object"), "dialog-object-properties");
    });
}

///////////////////////////////////////////////////////////////////////////////

class ImagePanel : public details::AttributesPanel {
public:
    ImagePanel() {
        add_object_label();
        add_size_properties();
        _grid.add_gap();
        // Add attributes that apply to images
        add_fill_and_stroke(static_cast<Parts>(Parts::Opacity | Parts::BlendMode));

        add_header(_("Image"));
        _panel = std::make_unique<Widget::ImageProperties>();
        Widget::reparent_properties(_panel->get_main(), _grid, true, false);
        add_filters();
        // no LPEs work on image currently, so no path effect section here
        add_name_properties();
        add_interactivity_properties();
    }
    ~ImagePanel() override = default;

    void update(SPObject* object) override { _panel->update(cast<SPImage>(object)); }

private:
    std::unique_ptr<Widget::ImageProperties> _panel;
};

/**
 * @class AnchorPanel
 * @brief A layout container that enables anchoring of its child elements
 *        relative to its boundaries.
 *
 * The AnchorPanel allows you to specify the positioning of child elements
 * by setting their anchor properties. Anchoring defines how child elements
 * maintain their positions and sizes relative to the panel’s edges when
 * the panel is resized. This container is useful for creating resizable UI layouts.
 *
 * Features:
 * - Supports anchoring to top, bottom, left, right, or any combination.
 * - Automatically adjusts child elements' sizes and positions based on anchors.
 * - Provides flexibility in designing dynamic user interfaces.
 *
 * Usage Notes:
 * - Child elements need to define anchor settings specifying their positional
 *   relationship to the panel's edges.
 * - Proper anchoring ensures consistent layout behavior, even when the
 *   application window or panel resizes.
 *
 * Limitations:
 * - Child elements without defined anchors will not resize or reposition
 *   during panel layout updates.
 * - Overlapping anchored elements may require careful configuration to avoid
 *   unexpected visual results.
 */

class AnchorPanel : public details::AttributesPanel {
public:
    AnchorPanel() {
        _title = _("Anchor");
        _table = std::make_unique<SPAttributeTable>();
        _table->set_modified_tag(TAG);
        _table->set_visible(true);
        _table->set_hexpand();
        _table->set_vexpand(false);
        _table->set_margin_start(6);
        _widget = _table.get();

        std::vector<Glib::ustring> labels;
        std::vector<Glib::ustring> attrs;
        int len = 0;
        while (anchor_desc[len].label) {
            labels.emplace_back(anchor_desc[len].label);
            attrs.emplace_back(anchor_desc[len].attribute);
            len += 1;
        }
        _table->create(labels, attrs);
    }

    ~AnchorPanel() override = default;

    void update(SPObject* object) override {
        auto anchor = cast<SPAnchor>(object);
        auto changed = _anchor != anchor;
        _anchor = anchor;
        if (!anchor) {
            _picker.disconnect();
            return;
        }

        if (changed) {
            _table->change_object(anchor);

            if (auto grid = dynamic_cast<Gtk::Grid*>(_table->get_first_child())) {
                auto op_button = Gtk::make_managed<Gtk::ToggleButton>();
                op_button->set_active(false);
                op_button->set_tooltip_markup(_("<b>Picker Tool</b>\nSelect objects on canvas"));
                op_button->set_margin_start(4);
                op_button->set_image_from_icon_name("object-pick");
                op_button->set_has_frame(false);

                op_button->signal_toggled().connect([=, this] {
                    // Use operation blocker to block the toggle signal
                    // emitted when the object has been picked and the
                    // button is toggled.
                    if (!_desktop || _update.pending()) {
                        return;
                    }

                    // Disconnect the picker signal if the button state is
                    // toggled to inactive.
                    if (!op_button->get_active()) {
                        _picker.disconnect();
                        set_active_tool(_desktop, _desktop->getTool()->get_last_active_tool());
                        return;
                    }

                    auto active_tool = get_active_tool(_desktop);
                    if (active_tool != "Picker") {
                        // activate the object picker tool
                        set_active_tool(_desktop, "Picker");
                    }
                    if (auto tool = dynamic_cast<Inkscape::UI::Tools::ObjectPickerTool*>(_desktop->getTool())) {
                        _picker = tool->signal_object_picked.connect([grid, this](SPObject* item){
                            // set anchor href
                            auto edit = dynamic_cast<Gtk::Entry*>(grid->get_child_at(1, 0));
                            if (edit && item) {
                                Glib::ustring id = "#";
                                edit->set_text(id + item->getId());
                            }
                            _picker.disconnect();
                            return false; // no more object picking
                        });

                        _tool_switched = tool->signal_tool_switched.connect([=, this] {
                            if (op_button->get_active()) {
                                auto scoped(_update.block());
                                op_button->set_active(false);
                            }
                            _tool_switched.disconnect();
                        });
                    }
                });
                grid->attach(*op_button, 2, 0);
            }
        }
        else {
            _table->reread_properties();
        }
    }

private:
    std::unique_ptr<SPAttributeTable> _table;
    SPAnchor* _anchor = nullptr;
    sigc::scoped_connection _picker;
    sigc::scoped_connection _tool_switched;
    bool _first_time_update = true;
};

///////////////////////////////////////////////////////////////////////////////

class RectPanel : public details::AttributesPanel {
public:
    RectPanel(Glib::RefPtr<Gtk::Builder> builder) :
        _rx(get_widget<Widget::InkSpinButton>(builder, "rect-rx")),
        _ry(get_widget<Widget::InkSpinButton>(builder, "rect-ry")),
        _sharp(get_widget<Gtk::Button>(builder, "rect-sharp")),
        _corners(get_widget<Gtk::DropDown>(builder, "rect-corners"))
    {
        _rx.signal_value_changed().connect([this](auto value){
            change_value_px(_rect, "corner-rx", value, "rx", [this](double rx){ _rect->setVisibleRx(rx); });
        });
        _ry.signal_value_changed().connect([this](auto value){
            change_value_px(_rect, "corner-ry", value, "ry", [this](double ry){ _rect->setVisibleRy(ry); });
        });

        _sharp.signal_clicked().connect([this]{
            if (!_rect) return;

            // remove rounded corners if LPE is there (first one found)
            remove_lpeffect(_rect, LivePathEffect::FILLET_CHAMFER);
            _rx.set_value(0);
            _ry.set_value(0);
        });
        _corners.property_selected().signal_changed().connect([this]{
            if (!_rect || !_desktop) return;

            auto selected_index = _corners.get_selected();
            if (selected_index == WITHOUT_LPE) {
                remove_lpeffect(_rect, LivePathEffect::FILLET_CHAMFER);
            } else if (selected_index == WITH_LPE) {
                // switch to the node tool to show handles
                set_active_tool(_desktop, "Node");
                // rx/ry need to be reset first, LPE doesn't handle them too well
                _rx.set_value(0);
                _ry.set_value(0);
                // add flexible corners effect if not yet present
                if (!find_lpeffect(_rect, LivePathEffect::FILLET_CHAMFER)) {
                    LivePathEffect::Effect::createAndApply("fillet_chamfer", _rect->document, _rect);
                    DocumentUndo::done(_rect->document, RC_("Undo", "Add fillet/chamfer effect"), INKSCAPE_ICON("dialog-path-effects"));
                }
            }
        });

        add_object_label();
        add_size_properties();
        _grid.add_gap();
        add_fill_and_stroke();
        add_header(_("Rectangle"));
        reparent_properties(get_widget<Gtk::Grid>(builder, "rect-main"), _grid, true, false);
        add_filters();
        add_lpes();
        add_name_properties();
        add_interactivity_properties();
    }

    ~RectPanel() override = default;

    void document_replaced(SPDocument* document) override {
        _paint->set_document(document);
    }

    void update(SPObject* object) override {
        _rect = cast<SPRect>(object);
        if (!_rect) return;

        auto scoped(_update.block());
        _rx.set_value(_rect->getVisibleRx());
        _ry.set_value(_rect->getVisibleRy());
        auto lpe = find_lpeffect(_rect, LivePathEffect::FILLET_CHAMFER);
        _sharp.set_sensitive(_rect->getVisibleRx() > 0 || _rect->getVisibleRy() > 0 || lpe);
        _corners.set_selected(lpe ? WITH_LPE : WITHOUT_LPE);
    }

private:
    SPRect* _rect = nullptr;
    Widget::InkSpinButton& _rx;
    Widget::InkSpinButton& _ry;
    Gtk::Button& _sharp;
    Gtk::DropDown& _corners;

    // Indices for the values in the _corners drop down
    static const int WITHOUT_LPE = 0;
    static const int WITH_LPE = 1;
};

///////////////////////////////////////////////////////////////////////////////

class EllipsePanel : public details::AttributesPanel {
public:
    EllipsePanel(Glib::RefPtr<Gtk::Builder> builder) :
        _cx(get_widget<Widget::InkSpinButton>(builder, "el-cx")),
        _cy(get_widget<Widget::InkSpinButton>(builder, "el-cy")),
        _rx(get_widget<Widget::InkSpinButton>(builder, "el-rx")),
        _ry(get_widget<Widget::InkSpinButton>(builder, "el-ry")),
        _start(get_widget<Widget::InkSpinButton>(builder, "el-start")),
        _end(get_widget<Widget::InkSpinButton>(builder, "el-end")),
        _slice(get_widget<Gtk::ToggleButton>(builder, "el-slice")),
        _arc(get_widget<Gtk::ToggleButton>(builder, "el-arc")),
        _chord(get_widget<Gtk::ToggleButton>(builder, "el-chord")),
        _whole(get_widget<Gtk::ToggleButton>(builder, "el-whole")),
        _round(get_widget<Gtk::Button>(builder, "el-round"))
    {
        _type[0] = &_slice;
        _type[1] = &_arc;
        _type[2] = &_chord;

        auto normalize = [this]{
            _ellipse->normalize();
            _ellipse->updateRepr();
            _ellipse->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | TAG);
        };

        int type = 0;
        for (auto btn : _type) {
            btn->signal_toggled().connect([=, this] {
                if (_update.pending() || !_ellipse || !btn->get_active()) return;

                auto scoped(_update.block());
                if (_ellipse->is_whole()) {
                    // set some initial angles; there's nothing else to change the whole ellipse into a slice
                    double s = 30, e = -30;
                    _start.set_value(s);
                    _end.set_value(e);
                    _ellipse->start = degree_to_radians_mod2pi(s);
                    _ellipse->end = degree_to_radians_mod2pi(e);
                    normalize();
                }
                set_type(type);
            });
            type++;
        }

        _whole.signal_toggled().connect([=, this]{
            if (_update.pending() || !_ellipse || !_whole.get_active()) return;

            auto scoped(_update.block());
            // back to the whole ellipse from slice:
            _start.set_value(0);
            _end.set_value(0);
            _ellipse->start = _ellipse->end = 0;
            normalize();
            DocumentUndo::done(_ellipse->document, RC_("Undo", "Change ellipse type"), "");
        });

        _cx.signal_value_changed().connect([=,this](auto value){
            change_value_px(_ellipse, "ellipse-center-x", value, nullptr, [=,this](double cx) {
                _ellipse->setVisibleCx(cx); normalize();
            });
        });
        _cy.signal_value_changed().connect([=,this](auto value){
            change_value_px(_ellipse, "ellipse-center-y", value, nullptr, [=,this](double cy) {
                _ellipse->setVisibleCy(cy); normalize();
            });
        });
        _rx.signal_value_changed().connect([=,this](auto value){
            change_value_px(_ellipse, "ellipse-radius-x", value, nullptr, [=,this](double rx) {
                _ellipse->setVisibleRx(rx); normalize();
            });
        });
        _ry.signal_value_changed().connect([=,this](auto value){
            change_value_px(_ellipse, "ellipse-radius-y", value, nullptr, [=,this](double ry) {
                _ellipse->setVisibleRy(ry); normalize();
            });
        });
        _start.signal_value_changed().connect([=,this](auto angle){
            change_angle(_ellipse, "ellipse-start-angle", angle, [=,this](double s) {
                _ellipse->start = s;
                normalize();
                update_ellipse_type();
            });
        });
        _end.signal_value_changed().connect([=,this](auto angle){
            change_angle(_ellipse, "ellipse-end-angle", angle, [=,this](double e) {
                _ellipse->end = e;
                normalize();
                update_ellipse_type();
            });
        });

        _round.signal_clicked().connect([this]{
            auto [changed, x, y] = round_values(_rx, _ry);
            if (changed && x > 0 && y > 0) {
                _rx.set_value(x);
                _ry.set_value(y);
            }
        });

        add_object_label();
        add_size_properties();
        _grid.add_gap();
        add_fill_and_stroke();
        add_header(_("Ellipse"));
        reparent_properties(get_widget<Gtk::Grid>(builder, "ellipse-main"), _grid, true, false);
        add_filters();
        add_lpes();
        add_name_properties();
        add_interactivity_properties();
    }

    ~EllipsePanel() override = default;

    void update(SPObject* object) override {
        _ellipse = cast<SPGenericEllipse>(object);
        if (!_ellipse) return;

        auto scoped(_update.block());
        _cx.set_value(_ellipse->getVisibleCx());
        _cy.set_value(_ellipse->getVisibleCy());
        _rx.set_value(_ellipse->getVisibleRx());
        _ry.set_value(_ellipse->getVisibleRy());
        _start.set_value(radians_to_degree_mod360(_ellipse->start));
        _end.set_value(radians_to_degree_mod360(_ellipse->end));

        update_ellipse_type();
    }

    void set_type(int type) {
        if (!_ellipse) return;

        auto scoped(_update.block());

        Glib::ustring arc_type = "slice";
        bool open = false;
        switch (type) {
            case 0:
                arc_type = "slice";
                open = false;
                break;
            case 1:
                arc_type = "arc";
                open = true;
                break;
            case 2:
                arc_type = "chord";
                open = true; // For backward compat, not truly open but chord most like arc.
                break;
            default:
                std::cerr << "Ellipse type change - bad arc type: " << type << std::endl;
                break;
        }
        _ellipse->setAttribute("sodipodi:open", open ? "true" : nullptr);
        _ellipse->setAttribute("sodipodi:arc-type", arc_type.c_str());
        _ellipse->updateRepr();
        DocumentUndo::done(_ellipse->document, RC_("Undo", "Change arc type"), INKSCAPE_ICON("draw-ellipse"));
    }

private:
    void update_ellipse_type() {
        _slice.set_active(_ellipse->arc_type == SP_GENERIC_ELLIPSE_ARC_TYPE_SLICE);
        _arc.set_active(_ellipse->arc_type == SP_GENERIC_ELLIPSE_ARC_TYPE_ARC);
        _chord.set_active(_ellipse->arc_type == SP_GENERIC_ELLIPSE_ARC_TYPE_CHORD);
        _whole.set_active(_ellipse->is_whole());
    }

    SPGenericEllipse* _ellipse = nullptr;
    Widget::InkSpinButton& _cx;
    Widget::InkSpinButton& _cy;
    Widget::InkSpinButton& _rx;
    Widget::InkSpinButton& _ry;
    Widget::InkSpinButton& _start;
    Widget::InkSpinButton& _end;
    Gtk::ToggleButton& _slice;
    Gtk::ToggleButton& _arc;
    Gtk::ToggleButton& _chord;
    Gtk::ToggleButton& _whole;
    Gtk::ToggleButton* _type[3];
    Gtk::Button& _round;
};

///////////////////////////////////////////////////////////////////////////////

class StarPanel : public details::AttributesPanel {
public:
    StarPanel(Glib::RefPtr<Gtk::Builder> builder) :
        _corners(get_widget<Widget::InkSpinButton>(builder, "star-corners")),
        _ratio(get_widget<Widget::InkSpinButton>(builder, "star-spoke")),
        _rounded(get_widget<Widget::InkSpinButton>(builder, "star-round")),
        _rand(get_widget<Widget::InkSpinButton>(builder, "star-rnd")),
        _align(get_widget<Gtk::Button>(builder, "star-align")),
        _poly(get_widget<Gtk::ToggleButton>(builder, "star-poly")),
        _star(get_widget<Gtk::ToggleButton>(builder, "star-star")),
        _reset_ratio(get_widget<Gtk::Button>(builder, "star-def-ratio")),
        _reset_rounded(get_widget<Gtk::Button>(builder, "star-sharp")),
        _reset_randomized(get_widget<Gtk::Button>(builder, "star-no-rnd"))
    {
        _corners.signal_value_changed().connect([this](auto){
            change_value(_path, _corners.get_adjustment(), [this](double sides) {
                _path->setAttributeDouble("sodipodi:sides", (int)sides);
                auto arg1 = get_number(_path, "sodipodi:arg1").value_or(0.5);
                _path->setAttributeDouble("sodipodi:arg2", arg1 + M_PI / sides);
                _path->updateRepr();
            });
        });
        _rounded.signal_value_changed().connect([this](auto){
            change_value(_path, _rounded.get_adjustment(), [this](double rounded) {
                _path->setAttributeDouble("inkscape:rounded", rounded);
                _path->updateRepr();
            });
        });
        _ratio.signal_value_changed().connect([this](auto){
            change_value(_path, _ratio.get_adjustment(), [this](double ratio){
                auto r1 = get_number(_path, "sodipodi:r1").value_or(1.0);
                auto r2 = get_number(_path, "sodipodi:r2").value_or(1.0);
                if (r2 < r1) {
                    _path->setAttributeDouble("sodipodi:r2", r1 * ratio);
                } else {
                    _path->setAttributeDouble("sodipodi:r1", r2 * ratio);
                }
                _path->updateRepr();
            });
        });
        _rand.signal_value_changed().connect([this](auto){
            change_value(_path, _rand.get_adjustment(), [this](double rnd){
                _path->setAttributeDouble("inkscape:randomized", rnd);
                _path->updateRepr();
            });
        });

        add_object_label();
        add_size_properties();
        _grid.add_gap();
        add_fill_and_stroke();
        add_header(_("Star"));
        reparent_properties(get_widget<Gtk::Grid>(builder, "star-main"), _grid, true, false);

        _reset_ratio.signal_clicked().connect([this]{ _ratio.set_value(0.5); });
        _reset_rounded.signal_clicked().connect([this]{ _rounded.set_value(0); });
        _reset_randomized.signal_clicked().connect([this]{ _rand.set_value(0); });

        _poly.signal_toggled().connect([this]{ set_flat(true); });
        _star.signal_toggled().connect([this]{ set_flat(false); });
        _align.signal_clicked().connect([this]{
            change_value(_path, {}, [this](double) { _path->turn_upright(); });
        });

        add_filters();
        add_lpes();
        add_name_properties();
        add_interactivity_properties();
    }

    ~StarPanel() override = default;

    void update(SPObject* object) override {
        _path = cast<SPStar>(object);
        if (!_path) return;

        auto scoped(_update.block());
        _corners.set_value(_path->sides);
        double r1 = get_number(_path, "sodipodi:r1").value_or(0.5);
        double r2 = get_number(_path, "sodipodi:r2").value_or(0.5);
        if (r2 < r1) {
            _ratio.set_value(r1 > 0 ? r2 / r1 : 0.5);
        } else {
            _ratio.set_value(r2 > 0 ? r1 / r2 : 0.5);
        }
        _rounded.set_value(_path->rounded);
        _rand.set_value(_path->randomized);
        _reset_randomized.set_visible(_path->randomized != 0);
        _reset_rounded.set_visible(_path->rounded != 0);
        _reset_ratio.set_visible(std::abs(_ratio.get_value() - 0.5) > 0.0005);

        _poly.set_active(_path->flatsided);
        _star.set_active(!_path->flatsided);
        _ratio.set_sensitive(!_path->flatsided);
    }

    void set_flat(bool flat) {
        change_value(_path, {}, [flat, this](double){
            _path->setAttribute("inkscape:flatsided", flat ? "true" : "false");
            _path->updateRepr();
        });
        // adjust corners/sides
        _corners.get_adjustment()->set_lower(flat ? 3 : 2);
        if (flat && _corners.get_value() < 3) {
            _corners.get_adjustment()->set_value(3);
        }
        _ratio.set_sensitive(!flat);
    }

private:
    SPStar* _path = nullptr;
    Widget::InkSpinButton& _corners;
    Widget::InkSpinButton& _ratio;
    Widget::InkSpinButton& _rounded;
    Widget::InkSpinButton& _rand;
    Gtk::Button& _align;
    Gtk::ToggleButton& _poly;
    Gtk::ToggleButton& _star;
    Gtk::Button& _reset_ratio;
    Gtk::Button& _reset_rounded;
    Gtk::Button& _reset_randomized;
};

///////////////////////////////////////////////////////////////////////////////

namespace {

struct PaintKey {
    // paint mode
    Widget::PaintMode mode = Widget::PaintMode::None;
    // for flat colors and swatches
    std::optional<Colors::Color> color;
    std::string id;
    // display only label
    std::string label;
    // gradient or pattern, if any
    SPObject* server = nullptr;
    SPObject* vector = nullptr;

    bool operator < (const PaintKey& p) const {
        if (mode != p.mode) return mode < p.mode;

        // ignore color, server and vector; it's a payload
        // ignore label too for now

        return id < p.id;
    }
};

PaintKey get_paint(SPIPaint* paint) {
    auto mode = paint ? Widget::get_mode_from_paint(*paint) : Widget::PaintMode::Derived;
    PaintKey key;
    key.mode = mode;
    if (mode == Widget::PaintMode::Solid) {
        key.id = paint->getColor().toString(false);
        key.color = paint->getColor();
    }
    else if (mode != Widget::PaintMode::Derived && mode != Widget::PaintMode::None) {
        if (auto server = paint->href ? paint->href->getObject() : nullptr) {
            if (auto gradient = cast<SPGradient>(server)) {
                // gradients, meshes
                key.vector = gradient->getVector(false);
            }
            else if (auto pattern = cast<SPPattern>(server)) {
                key.vector = pattern->rootPattern();
            }
            auto s = key.vector ? key.vector : server;
            key.id = s->getId() ? s->getId() : "";
            key.label = s->defaultLabel();
            key.server = server;
        }
    }
    return key;
};

// paint servers, colors, or no paint
auto paint_to_item(const PaintKey& paint) {
    auto mode_name = get_paint_mode_name(paint.mode);
    auto tooltip = paint.vector || !paint.color ? mode_name : Glib::ustring(paint.color->toString(false));
    if (paint.vector) tooltip = tooltip + " " + paint.vector->defaultLabel();
    auto label = paint.label.empty() ? paint.id : paint.label;
    if (label.empty()) label = mode_name;
    if (paint.mode == Widget::PaintMode::Swatch) {
        Colors::Color color{0};
        auto swatch = cast<SPGradient>(paint.vector);
        if (swatch && swatch->hasStops()) {
            color = swatch->getFirstStop()->getColor();
        }
        return GridViewList::create_item(paint.id, 0, label, {}, tooltip, color, {}, true);
    }
    else if (paint.mode == Widget::PaintMode::Solid) {
        return GridViewList::create_item(paint.id, 0, label, {}, tooltip, paint.color, {}, false);
    }
    else if (paint.mode == Widget::PaintMode::Gradient) {
        // todo: pattern size needs to match tile size
        auto pat_t = cast<SPGradient>(paint.vector)->create_preview_pattern(16);
        auto pat = Cairo::RefPtr<Cairo::Pattern>(new Cairo::Pattern(pat_t, true));
        return GridViewList::create_item(paint.id, 0, label, {}, tooltip, {}, pat, false, is<SPRadialGradient>(paint.server));
    }
    else {
        auto icon = get_paint_mode_icon(paint.mode);
        return GridViewList::create_item(paint.id, 0, label, icon, tooltip, {}, {}, false);
    }
}

} // namespace

// Note: This text panel used to have a fair amount of incomplete extra UI options. Text properties
//  like font scaling, line height, styles, etc. But the UI never got properly hooked up to code
//  and so it was removed. If you want to expand the features of the Text panel, you might be able
//  to find some useful code in https://gitlab.com/inkscape/inkscape/-/merge_requests/7946
class TextPanel : public details::AttributesPanel {
public:
    TextPanel(Glib::RefPtr<Gtk::Builder> builder) {
        add_object_label();
        add_size_properties();
        _grid.add_gap();
        // add F&S for the main text element
        add_fill_and_stroke();
        _grid.add_section_divider();
        add_filters(false);
        add_name_properties();
        add_interactivity_properties();
    }

private:
    void update(SPObject* object) override {
        auto text = cast<SPText>(object);
        if (text) {
            // set title; there are various "text" types
            //todo: is text-in-a-shape a flow text?
            _title = text->displayName();
            if (SP_IS_TEXT_TEXTPATH(text)) {
                // sp-text description uses similar (and translation dubious) concatenation approach
                _title += " ";
                _title += C_("<text> on path", "on path");
            }
        }
    }
};

///////////////////////////////////////////////////////////////////////////////

class PointsPanel : public details::AttributesPanel {
public:
    PointsPanel(const Glib::RefPtr<Gtk::Builder>& builder, const char* points_section_name, Syntax::SyntaxMode syntax) :
        _svgd_edit(Syntax::TextEditView::create(syntax)),
        _main(get_widget<Gtk::Grid>(builder, "path-main")),
        _info(get_widget<Gtk::Label>(builder, "path-info")),
        _data(_svgd_edit->getTextView())
    {
        add_object_label();
        add_size_properties();
        _grid.add_gap();
        add_fill_and_stroke();

        _grid.add_gap();
        _data_toggle = _grid.add_section(points_section_name);
        _grid.add_row(&_main);
        _grid.add_section_divider();

        add_filters(false);
        add_lpes();
        add_name_properties();
        add_interactivity_properties();

        auto pref_path = details::dlg_pref_path + "path-panel/";

        auto theme = Preferences::get()->getString("/theme/syntax-color-theme", "-none-");
        _svgd_edit->setStyle(theme);
        _data.set_wrap_mode(Gtk::WrapMode::WORD);

        auto const key = Gtk::EventControllerKey::create();
        key->signal_key_pressed().connect(sigc::mem_fun(*this, &PointsPanel::on_key_pressed), true);
        _data.add_controller(key);

        auto& wnd = get_widget<Gtk::ScrolledWindow>(builder, "path-data-wnd");
        wnd.set_child(_data);

        auto set_precision = [=,this](int const n) {
            _precision = n;
            auto& menu_button = get_widget<Gtk::MenuButton>(builder, "path-menu");
            auto menu = menu_button.get_menu_model();
            auto section = menu->get_item_link(0, Gio::MenuModel::Link::SECTION);
            auto type = Glib::VariantType{g_variant_type_new("s")};
            auto variant = section->get_item_attribute(n, Gio::MenuModel::Attribute::LABEL, type);
            auto label = ' ' + static_cast<const Glib::Variant<Glib::ustring>&>(variant).get();
            get_widget<Gtk::Label>(builder, "path-precision").set_label(label);
            Preferences::get()->setInt(pref_path + "precision", n);
            menu_button.set_active(false);
        };

        const int N = 5;
        _precision = Preferences::get()->getIntLimited(pref_path + "precision", 2, 0, N);
        set_precision(_precision);
        auto group = Gio::SimpleActionGroup::create();
        auto action = group->add_action_radio_integer("precision", _precision);
        action->property_state().signal_changed().connect([=,this]{ int n; action->get_state(n);
                                                                    set_precision(n); });
        _main.insert_action_group("attrdialog", std::move(group));

        get_widget<Gtk::Button>(builder, "path-data-round").signal_clicked().connect([this]{
            truncate_digits(_data.get_buffer(), _precision);
            commit_d();
        });
        get_widget<Gtk::Button>(builder, "path-enter").signal_clicked().connect([this]{ commit_d(); });

        _data_toggle->signal_clicked().connect([this] {
            bool show = !_data_props_visibility;
            show_data_properties(show);
            Preferences::get()->setBool(_data_props_visibility.observed_path, show);
            if (_sel_changed) {
                _sel_changed = false;
                update_ui();
            }
        });
        show_data_properties(_data_props_visibility);
        _data_props_visibility.action = [this] {
            show_data_properties(_data_props_visibility);
        };
    }

    ~PointsPanel() override = default;

    void update(SPObject* object) override {
        auto item = update_item(object);
        _sel_changed = item != _item;
        _item = item;
        if (!_item) {
            _update_data.disconnect();
            return;
        }

        if (!_sel_changed) {
            // throttle UI refresh, it is expensive
            _update_data = Glib::signal_timeout().connect([this]{ update_ui(); return false; }, 250, Glib::PRIORITY_DEFAULT_IDLE);
        }
        else {
            _update_data.disconnect();
            // new path; update right away
            update_ui();
        }
    }

private:
    virtual SPShape* update_item(SPObject* object) = 0;
    virtual const char* get_points() = 0;
    virtual void set_points(const Glib::ustring& points) = 0;

    virtual std::size_t get_point_count() const {
        if (!_item) return 0;

        auto curve = _item->curveBeforeLPE();
        if (!curve) curve = _item->curve();
        std::size_t node_count = 0;
        if (curve) {
            node_count = curve->curveCount();
        }
        return node_count;
    }

    void show_data_properties(bool expand) {
        _main.set_visible(expand);
        _grid.open_section(_data_toggle, expand);
    }

    void update_ui() {
        if (_update.pending() || !_document || !_desktop) return;

        auto scoped(_update.block());
        auto d = get_points();

        if (_data_props_visibility) {
            _svgd_edit->setText(d ? d : "");
        }

        auto node_count = get_point_count();
        _info.set_text(C_("Number of path nodes follows", "Nodes: ") + std::to_string(node_count));

        //TODO: we can consider adding more stats, like perimeter, area, etc.
    }

    bool on_key_pressed(unsigned keyval, unsigned keycode, Gdk::ModifierType state) {
        switch (keyval) {
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            return Controller::has_flag(state, Gdk::ModifierType::SHIFT_MASK) ? commit_d() : false;
        }
        return false;
    }

    bool commit_d() {
        if (!_item || !_data.is_visible()) return false;

        auto scoped(_update.block());
        auto d = _svgd_edit->getText();
        set_points(d);
        return true;
    }

    SPShape* _item = nullptr;
    Gtk::Grid& _main;
    Gtk::Label& _info;
    std::unique_ptr<Syntax::TextEditView> _svgd_edit;
    Gtk::TextView& _data;
    int _precision = 2;
    bool _sel_changed = false;
    sigc::scoped_connection _update_data;
    Gtk::Button* _data_toggle;
    Pref<bool> _data_props_visibility = {details::dlg_pref_path + "/options/show_path_data"};
};

class PathPanel : public PointsPanel {
public:
    PathPanel(const Glib::RefPtr<Gtk::Builder>& builder) : PointsPanel(builder, _("Path data"), Syntax::SyntaxMode::SvgPathData) {}
    ~PathPanel() override = default;

private:
    SPShape* update_item(SPObject* object) override {
        _path = cast<SPPath>(object);
        return _path;
    }

    const char* get_points() override {
        auto d = _path->getAttribute("inkscape:original-d");
        if (d && _path->hasPathEffect()) {
            _original = true;
        }
        else {
            _original = false;
            d = _path->getAttribute("d");
        }
        return d;
    }

    void set_points(const Glib::ustring& points) override {
        _path->setAttribute(_original ? "inkscape:original-d" : "d", points);
        DocumentUndo::maybeDone(_path->document, "path-data", RC_("Undo", "Change path"), "");
    }

    SPPath* _path = nullptr;
    bool _original = false;
};

class PolylinePanel : public PointsPanel {
public:
    PolylinePanel(const Glib::RefPtr<Gtk::Builder>& builder) : PointsPanel(builder, _("Polyline points"), Syntax::SyntaxMode::SvgPolyPoints) {}
    ~PolylinePanel() override = default;

private:
    SPShape* update_item(SPObject* object) override {
        _polyline = cast<SPPolyLine>(object);
        return _polyline;
    }

    const char* get_points() override {
        return _polyline ? _polyline->getAttribute("points") : nullptr;
    }

    void set_points(const Glib::ustring& points) override {
        _polyline->setAttribute("points", points);
        DocumentUndo::maybeDone(_polyline->document, "polyline-data", RC_("Undo", "Change polyline"), "");
    }

    SPPolyLine* _polyline = nullptr;
};

class PolygonPanel : public PointsPanel {
public:
    PolygonPanel(const Glib::RefPtr<Gtk::Builder>& builder) : PointsPanel(builder, _("Polygon points"), Syntax::SyntaxMode::SvgPolyPoints) {}
    ~PolygonPanel() override = default;

private:
    SPShape* update_item(SPObject* object) override {
        _polygon = cast<SPPolygon>(object);
        return _polygon;
    }

    const char* get_points() override {
        return _polygon ? _polygon->getAttribute("points") : nullptr;
    }

    void set_points(const Glib::ustring& points) override {
        _polygon->setAttribute("points", points);
        DocumentUndo::maybeDone(_polygon->document, "polyline-data", RC_("Undo", "Change polyline"), "");
    }

    std::size_t get_point_count() const override {
        return 0;
    }

    SPPolygon* _polygon = nullptr;
};


///////////////////////////////////////////////////////////////////////////////

class GroupPanel : public details::AttributesPanel {
public:
    GroupPanel(Glib::RefPtr<Gtk::Builder> builder) {
        add_object_label();
        add_size_properties();
        _grid.add_gap();
        add_fill_and_stroke();

        add_header(_("Group"));
        auto enter = Gtk::make_managed<Gtk::Button>(_("Enter group"));
        enter->set_can_focus(false);
        enter->set_tooltip_text(_("Enter into this group to select objects"));
        enter->signal_clicked().connect([this] {
            enter_group(_desktop, _group);
        });
        _grid.add_row(_("Elements"), enter);

if constexpr (INCLUDE_EXPERIMENTAL_PANELS) {
        //TODO: would that be useful?
        auto remove = Gtk::make_managed<Gtk::Button>(_("Remove style"));
        remove->set_tooltip_text(_("Remove style from group elements\nto override it with group style"));
        remove->signal_clicked().connect([this] {
            // remove style from the group's children
            remove_styles(_group);
        });
        //
        //TODO: would that be useful?
        auto enter = Gtk::make_managed<Gtk::Button>(_("Enter group"));
        enter->set_tooltip_text(_("Enter into this group to select objects"));
        enter->signal_clicked().connect([this] {
            enter_group(_desktop, _group);
        });
        _grid.add_property(_("Elements"), nullptr, remove, enter);
}
        add_filters();
        add_lpes();
        add_name_properties();
        add_interactivity_properties();
    }

private:
    void update(SPObject* object) override {
        _group = cast<SPGroup>(object);
    }

    void remove_styles(SPObject* parent) {
        if (!parent) return;

        if (remove_children_styles(parent, true)) {
            DocumentUndo::done(parent->document, RC_("Undo", "Removed style"), "");
        }
    }

    bool remove_children_styles(SPObject* parent, bool recursive) {
        auto changed = false;
        for (auto obj = parent->firstChild(); obj; obj = obj->getNext()) {
            if (Css::remove_item_style(obj)) {
                changed = true;
            }
            if (recursive && remove_children_styles(obj, true)) {
                changed = true;
            }
        }
        return changed;
    }

    SPGroup* _group = nullptr;
};

///////////////////////////////////////////////////////////////////////////////

class ClonePanel : public details::AttributesPanel {
public:
    ClonePanel(Glib::RefPtr<Gtk::Builder> builder) {
        add_object_label();
        add_size_properties();
        _grid.add_gap();
        add_fill_and_stroke();

        add_header(_("Clone"));
        auto go_to = create_button(_("Go to"), "object-pick");
        go_to->set_can_focus(false);
        go_to->set_tooltip_text(_("Select original object"));
        go_to->signal_clicked().connect([this] {
            if (_desktop) {
                // go to original; this method should take clone as input
                //todo: go to true original
                _desktop->getSelection()->cloneOriginal();
            }
        });
        _grid.add_row(_("Original"), go_to);

if constexpr (INCLUDE_EXPERIMENTAL_PANELS) {
        auto remove = Gtk::make_managed<Gtk::Button>(_("Steal style"));
        remove->set_tooltip_text(_("Remove style from the original element\nand place it on this clone"));
        remove->signal_clicked().connect([this] {
            // remove style from the original element
            remove_styles(_clone);
        });

        auto link = Gtk::make_managed<Gtk::Button>(_("Original"));
        link->set_tooltip_text(_("Link this clone to original element"));
        link->signal_clicked().connect([this] {
            // link clone to the original object if it points to another <use> element
            link_to_original(_clone);
        });
        _link = link;

        auto go_to = create_button(_("Go to"), "object-pick");
        go_to->set_tooltip_text(_("Select original object"));
        go_to->signal_clicked().connect([this] {
            if (_desktop) {
                // go to original; this method should take clone as input
                //todo: go to true original
                _desktop->getSelection()->cloneOriginal();
            }
        });

        _grid.add_gap();
        _grid.add_property(_("Original"), nullptr, remove, go_to);
        _grid.add_property(_("Link to"), nullptr, link, nullptr);
}
        add_filters();
        //TODO: commented out for now; clones need special treatment (clone original lpe?)
        // add_lpes(true);
        add_name_properties();
        add_interactivity_properties();
    }

private:
    void update(SPObject* object) override {
        _clone = cast<SPUse>(object);
        if (_link) {
            _link->set_sensitive(_clone && _clone->trueOriginal() != _clone->get_original());
        }
    }

    void link_to_original(SPUse* clone) {
        if (!clone) return;

        if (auto original = clone->trueOriginal()) {
            if (auto id = original->getId()) {
                std::string url = "#";
                url += id;
                // re-link
                clone->setAttribute("xlink:href", url.c_str());
            }
        }
    }

    void remove_styles(SPUse* clone) {
        if (!clone) return;

        auto original = clone->get_original();
        if (Css::transfer_item_style(original, clone)) {
            DocumentUndo::done(clone->document, RC_("Undo", "Transferred style"), "");
        }
    }

    bool remove_children_styles(SPObject* parent, bool recursive) {
        auto changed = false;
        for (auto obj = parent->firstChild(); obj; obj = obj->getNext()) {
            auto style = obj->getAttribute("style");
            if (style && *style) {
                obj->removeAttribute("style");
                changed = true;
            }
            if (recursive && remove_children_styles(obj, true)) {
                changed = true;
            }
        }
        return changed;
    }

    SPUse* _clone = nullptr;
    Gtk::Button* _link = nullptr;
};

///////////////////////////////////////////////////////////////////////////////

namespace {

template<typename F>
void visit_objects(SPObject* object, F f) {
    auto visit_children_fn = [&](SPItem* item, auto& self) -> void {
        f(item);
        for (auto& child : item->children) {
            if (auto i = cast<SPItem>(&child)) {
                self(i, self);
            }
        }
    };

    auto visit_objects_fn = [&](SPObject* object, auto& self) -> void {
        if (auto group = cast<SPGroup>(object)) {
            f(group);
            for (auto& child : group->children) {
                self(&child, self);
            }
        }
        else if (auto clone = cast<SPUse>(object)) {
            f(clone);
            if (auto original = clone->trueOriginal()) {
                f(original);
            }
        }
        else if (auto text = cast<SPText>(object)) {
            visit_children_fn(text, visit_children_fn);
        }
        else if (object) {
            f(object);
        }
    };

    visit_objects_fn(object, visit_objects_fn);
}

} // namespace

class MultiObjPanel : public details::AttributesPanel {
public:
    MultiObjPanel(Glib::RefPtr<Gtk::Builder> builder) {
        add_object_label();
        add_size_properties();

if constexpr (INCLUDE_EXPERIMENTAL_PANELS) {
        //todo: should those options be exposed? =======================
        // auto box = Gtk::make_managed<Gtk::Box>();
        // box->set_spacing(4);
        // auto enter = Gtk::make_managed<Gtk::CheckButton>(_("Enter groups"));
        // enter->set_tooltip_text(_("Scan objects inside groups"));
        // auto original = Gtk::make_managed<Gtk::CheckButton>(_("Scan originals"));
        // original->set_tooltip_text(_("Scan originals pointed to be clones"));
        // box->append(*enter);
        // box->append(*original);
        // _grid.add_row(box);
        // _grid.add_property(_("Fill"), nullptr, )
        //todo: end ====================================================

        _types.set_hexpand();
        _grid.add_row(_("Types"), &_types);
        _grid.add_row(Gtk::make_managed<Gtk::Separator>(), nullptr, true);

        _fill_paint.set_hexpand();
        _grid.add_row(_("Fills"), &_fill_paint);
        _grid.add_row(Gtk::make_managed<Gtk::Separator>(), nullptr, true);

        _stroke_paint.set_hexpand();
        _grid.add_row(_("Strokes"), &_stroke_paint);
        _grid.add_row(Gtk::make_managed<Gtk::Separator>(), nullptr, true);

        _stroke_width.set_hexpand();
        _grid.add_row(_("Stroke widths"), &_stroke_width);
        _stroke_width.get_signal_value_changed().connect([this](auto id, auto orig, auto value) {
            printf("val chg: %s %.8f -> %.8f\n", id.c_str(), orig, value);
            auto selection = _desktop->getSelection();
            bool changed = false;
            for (auto obj : selection->objects()) {
                visit_objects(obj, [&](SPObject* o) {
                    if (auto item = cast<SPItem>(o)) {
                        if (item->style->stroke_width.computed == orig) {
                            printf("stroke match %s\n", o->getId());
                            changed = true;
    //todo: this is a test
    auto css = boost::intrusive_ptr(sp_repr_css_attr_new(), false);
    sp_repr_css_set_property_double(css.get(), "stroke-width", value);
    item->changeCSS(css.get(), "style");
    // end of test
                        }
                        else {
                            printf("stroke no match %.8f, %s\n", item->style->stroke_width.computed, o->getId());
                        }
                    }
                });
            }
            if (changed) {
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "stroke width"), "");
            }
        });
}
    }

private:
    Glib::ustring get_title(Selection* selection) const override {
        if (!selection) return _title;

        auto n = selection->size();
        return Glib::ustring::compose(ngettext("%1 Object", "%1 Objects", n), n);
    }

    void update(SPObject* object) override {
        if (!_desktop) return;

        auto selection = _desktop->getSelection();

        return; // not used for now

        std::set<std::string> types;
        std::set<PaintKey> fills; // fill paints
        std::set<PaintKey> strokes;
        std::set<double> stroke_widths;

        //todo: test code ------------------
        // auto get_paint = [](SPIPaint* paint) {
        //     auto mode = paint ? Widget::get_mode_from_paint(*paint) : Widget::PaintMode::NotSet;
        //     PaintKey key;
        //     key.mode = mode;
        //     if (mode == Widget::PaintMode::Solid) {
        //         key.id = paint->getColor().toString(false);
        //         key.color = paint->getColor();
        //     }
        //     else if (auto server = paint->href ? paint->href->getObject() : nullptr) {
        //         if (auto gradient = cast<SPGradient>(server)) {
        //             // gradients, meshes
        //             key.vector = gradient->getVector(false);
        //         }
        //         else if (auto pattern = cast<SPPattern>(server)) {
        //             key.vector = pattern->rootPattern();
        //         }
        //         auto s = key.vector ? key.vector : server;
        //         key.id = s->getId() ? s->getId() : "";
        //         key.label = s->defaultLabel();
        //         key.server = server;
        //     }
        //     return key;
        // };

        auto collect_attr = [&](SPObject* obj) {
            if (auto repr = obj->getRepr()) {
                types.insert(repr->name());
            }
            if (auto item = cast<SPItem>(obj)) {
                auto fill = item->style->getFillOrStroke(true);
                fills.insert(get_paint(fill));

                auto stroke = item->style->getFillOrStroke(false);
                strokes.insert(get_paint(stroke));

                stroke_widths.insert(item->style->stroke_width.computed);
            }
            //todo: groups and text
        };

        for (auto obj : selection->objects()) {
            visit_objects(obj, collect_attr);
        }

        {
            auto it = types.begin();
            _types.update_store(types.size(), [&](auto i) {
                auto&& name = *it;
                ++it;
                return GridViewList::create_item(name, 0, name, {}, {}, {}, {}, false);
            });
        }
        {
            auto it = stroke_widths.begin();
            _stroke_width.update_store(stroke_widths.size(), [&](auto i) {
                auto width = *it++;
                auto id = std::to_string(i);
                return GridViewList::create_item(id, width, {}, {}, {}, {}, {}, false);
            });
        }

        {
            //todo: experiments -------------------
            // paint servers, colors, or no paint
            // auto paint_to_item = [](const PaintKey& paint) {
            //     auto mode_name = get_paint_mode_name(paint.mode);
            //     auto tooltip = paint.vector || !paint.color ? mode_name : Glib::ustring(paint.color->toString(false));
            //     if (paint.vector) tooltip = tooltip + " " + paint.vector->defaultLabel();
            //     auto label = paint.label.empty() ? paint.id : paint.label;
            //     if (label.empty()) label = mode_name;
            //     if (paint.mode == Widget::PaintMode::Swatch) {
            //         Colors::Color color{0};
            //         auto swatch = cast<SPGradient>(paint.vector);
            //         if (swatch && swatch->hasStops()) {
            //             color = swatch->getFirstStop()->getColor();
            //         }
            //         return GridViewList::create_item(paint.id, 0, label, {}, tooltip, color, {}, true);
            //     }
            //     else if (paint.mode == Widget::PaintMode::Solid) {
            //         return GridViewList::create_item(paint.id, 0, label, {}, tooltip, paint.color, {}, false);
            //     }
            //     else if (paint.mode == Widget::PaintMode::Gradient) {
            //         // todo: pattern size needs to match tile size
            //         auto pat_t = cast<SPGradient>(paint.vector)->create_preview_pattern(16);
            //         auto pat = Cairo::RefPtr<Cairo::Pattern>(new Cairo::Pattern(pat_t, true));
            //         return GridViewList::create_item(paint.id, 0, label, {}, tooltip, {}, pat, false, is<SPRadialGradient>(paint.server));
            //     }
            //     else {
            //         auto icon = get_paint_mode_icon(paint.mode);
            //         return GridViewList::create_item(paint.id, 0, label, icon, tooltip, {}, {}, false);
            //     }
            // };
            {
                auto it = fills.begin();
                _fill_paint.update_store(fills.size(), [&](auto index) {
                    return paint_to_item(*it++);
                });
            }
            {
                auto it = strokes.begin();
                _stroke_paint.update_store(strokes.size(), [&](auto index) {
                    return paint_to_item(*it++);
                });
            }
        }
    }

    GridViewList _types{GridViewList::Label};
    GridViewList _fill_paint{GridViewList::ColorLong};
    GridViewList _stroke_paint{GridViewList::ColorLong};
    GridViewList _stroke_width{Gtk::Adjustment::create(0, 0, 1e5, 0.1, 1), 8};
};

///////////////////////////////////////////////////////////////////////////////

class EmptyPanel : public details::AttributesPanel {
public:
    EmptyPanel(Glib::RefPtr<Gtk::Builder> builder) {
        Widget::reparent_properties(get_widget<Gtk::Grid>(builder, "empty-panel"), _grid);

if constexpr (INCLUDE_EXPERIMENTAL_PANELS) {
        // TODO: panel with default paint and other style attributes
        _grid.add_property(_("Defaults"), nullptr, nullptr, nullptr);
        add_fill_and_stroke(Parts::FillPaint);
}
    }

    void update(SPObject* object) override {
if constexpr (INCLUDE_EXPERIMENTAL_PANELS) {
        if (!_desktop || !_desktop->getDocument()) return;

        if (auto view = _desktop->getDocument()->getNamedView()) {
            if (view->style) {
                update_paint(view);
            }
        }
}
    }
};

///////////////////////////////////////////////////////////////////////////////

details::AttributesPanel* ObjectAttributes::get_panel(Selection* selection) {
    if (auto item = selection->singleItem()) {
        int tag = item->tag();
        auto it = _panels.find(tag);
        auto panel = it == _panels.end() ? nullptr : it->second.get();
        if (!panel) {
            // create a panel
            auto obj_panel = create_panel(tag);
            panel = obj_panel.get();
            _panels[tag] = std::move(obj_panel);
            if (panel) {
                panel->set_document(getDocument());
                for_each_descendant(panel->widget(), [this](auto& widget) {
                    if (auto sb = dynamic_cast<UI::Widget::InkSpinButton*>(&widget)) {
                        sb->setDefocusTarget(this);
                    }
                    return ForEachResult::_continue;
                });
            }
        }
        return panel;
    }

    if (selection->isEmpty()) {
        if (!_empty_panel) {
            _empty_panel = std::make_unique<EmptyPanel>(_builder);
        }
        return _empty_panel.get();
    }

    if (selection->size() > 1) {
        if (!_multi_obj_panel) {
            _multi_obj_panel = std::make_unique<MultiObjPanel>(_builder);
            _multi_obj_panel->set_document(getDocument());
        }
        return _multi_obj_panel.get();
    }
    return nullptr;
}

std::unique_ptr<details::AttributesPanel> ObjectAttributes::create_panel(int key) {
    switch (key) {
        case tag_of<SPImage>:    return std::make_unique<ImagePanel>();
        case tag_of<SPRect>:     return std::make_unique<RectPanel>(_builder);
        case tag_of<SPGenericEllipse>: return std::make_unique<EllipsePanel>(_builder);
        case tag_of<SPStar>:     return std::make_unique<StarPanel>(_builder);
        case tag_of<SPAnchor>:   return std::make_unique<AnchorPanel>();
        case tag_of<SPPath>:     return std::make_unique<PathPanel>(_builder);
        case tag_of<SPPolyLine>: return std::make_unique<PolylinePanel>(_builder);
        case tag_of<SPPolygon>:  return std::make_unique<PolygonPanel>(_builder);
        case tag_of<SPGroup>:    return std::make_unique<GroupPanel>(_builder);
        case tag_of<SPText>:     return std::make_unique<TextPanel>(_builder);
        case tag_of<SPUse>:      return std::make_unique<ClonePanel>(_builder);
    }

    return {};
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
