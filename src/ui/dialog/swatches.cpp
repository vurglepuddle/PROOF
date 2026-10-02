// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Jon A. Cruz
 *   John Bintz
 *   Abhishek Sharma
 *   PBS <pbs3141@gmail.com>
 *
 * Copyright (C) 2005 Jon A. Cruz
 * Copyright (C) 2008 John Bintz
 * Copyright (C) 2022 PBS
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "swatches.h"

#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <gtkmm/accelerator.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/searchentry2.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/togglebutton.h>

#include "desktop-style.h"
#include "desktop.h"
#include "document.h"
#include "object/sp-defs.h"
#include "object/sp-gradient-reference.h"
#include "style.h"
#include "ui/builder-utils.h"
#include "ui/column-menu-builder.h"
#include "ui/controller.h"
#include "ui/dialog/color-item.h"
#include "ui/util.h" // ellipsize()
#include "ui/widget/color-palette-preview.h"
#include "ui/widget/color-palette.h"
#include "util/variant-visitor.h"

namespace Inkscape::Colors {
    std::size_t hash_value(Color const& b)
    {
        boost::hash<int> hasher;
        return hasher(b.toRGBA());
    }
}

namespace Inkscape::UI::Dialog {

static constexpr auto auto_id = "Auto";

/*
 * Lifecycle
 */

SwatchesPanel::SwatchesPanel(PanelType panel_type, char const *prefsPath)
    : DialogBase(prefsPath, "Swatches"),
    _builder(create_builder("dialog-swatches.glade")),
    _list_btn(get_widget<Gtk::ToggleButton>(_builder, "list")),
    _grid_btn(get_widget<Gtk::ToggleButton>(_builder, "grid")),
    _selector(get_widget<Gtk::MenuButton>(_builder, "selector")),
    _selector_label(get_widget<Gtk::Label>(_builder, "selector-label")),
    _selector_menu{panel_type == Compact ? nullptr : std::make_unique<UI::Widget::PopoverMenu>(Gtk::PositionType::BOTTOM)},
    // Read the saved custom palette from the dialog path, regardless of our current
    // panel_type/_prefs_path, because that is the only place it is saved, and we want to show
    // the saved palette in other contexts, like the Compact view too.
    _saved_palette_path("/dialogs/swatches/palette-path"),
    _new_btn(get_widget<Gtk::Button>(_builder, "new")),
    _delete_btn(get_widget<Gtk::Button>(_builder, "delete")),
    _import_btn(get_widget<Gtk::Button>(_builder, "import")),
    _open_btn(get_widget<Gtk::Button>(_builder, "open"))
{
    // hide edit buttons
    _new_btn.set_visible(false);
    _import_btn.set_visible(false);
    _delete_btn.set_visible(false);

    _palette = Gtk::make_managed<Inkscape::UI::Widget::ColorPalette>();
    _palette->set_visible();
    if (panel_type == Compact) {
        append(*_palette);
    } else {
        get_widget<Gtk::Box>(_builder, "content").append(*_palette);
        _palette->set_expand();

        _palette->set_settings_visibility(false);

        // Steal popover from colour palette and attach it to our button instead. Fixme: Bad, fragile.
        auto &popover = _palette->get_settings_popover();
        popover.unparent();
        get_widget<Gtk::MenuButton>(_builder, "settings").set_popover(popover);

        _palette->set_filter([this](Dialog::ColorItem const &color){
            return filter_callback(color);
        });
        auto& search = get_widget<Gtk::SearchEntry2>(_builder, "search");
        search.signal_search_changed().connect([this, &search]{
            if (search.get_text().length() == 0) {
                clear_filter();
            } else {
                filter_colors(search.get_text());
            }
        });
    }

    update_palettes(panel_type);

    auto prefs = Inkscape::Preferences::get();
    _current_palette_id = prefs->getString(_prefs_path + "/palette");
    if (auto p = get_palette(_current_palette_id)) {
        _current_palette_id = p->id; // Canonicalise to id, in case name lookup was used.
    } else {
        _current_palette_id = auto_id; // Fall back to auto palette.
    }

    if (panel_type == Dialog) {
        g_assert(_selector_menu);
        setup_selector_menu();
        update_selector_menu();
        update_selector_label(_current_palette_id);
    }

    bool embedded = panel_type == Compact;
    _palette->set_compact(embedded);

    // restore palette settings
    _palette->set_tile_size(prefs->getInt(_prefs_path + "/tile_size", 16));
    _palette->set_aspect(prefs->getDoubleLimited(_prefs_path + "/tile_aspect", 0.0, -2, 2));
    _palette->set_tile_border(prefs->getInt(_prefs_path + "/tile_border", 1));
    _palette->set_rows(prefs->getInt(_prefs_path + "/rows", 2));
    _palette->enable_stretch(prefs->getBool(_prefs_path + "/tile_stretch", true));
    _palette->set_large_pinned_panel(embedded && prefs->getBool(_prefs_path + "/enlarge_pinned", true));
    _palette->enable_labels(!embedded && prefs->getBool(_prefs_path + "/show_labels", true));

    // save settings when they change
    _palette->get_settings_changed_signal().connect([=, this]{
        prefs->setInt(_prefs_path + "/tile_size", _palette->get_tile_size());
        prefs->setDouble(_prefs_path + "/tile_aspect", _palette->get_aspect());
        prefs->setInt(_prefs_path + "/tile_border", _palette->get_tile_border());
        prefs->setInt(_prefs_path + "/rows", _palette->get_rows());
        prefs->setBool(_prefs_path + "/tile_stretch", _palette->is_stretch_enabled());
        prefs->setBool(_prefs_path + "/enlarge_pinned", _palette->is_pinned_panel_large());
        prefs->setBool(_prefs_path + "/show_labels", !embedded && _palette->are_labels_enabled());
    });

    _list_btn.signal_toggled().connect([this]{
        _palette->enable_labels(true);
    });
    _grid_btn.signal_toggled().connect([this]{
        _palette->enable_labels(false);
    });
    (_palette->are_labels_enabled() ? _list_btn : _grid_btn).set_active();

    // Watch for pinned palette options.
    _pinned_observer = prefs->createObserver(_prefs_path + "/pinned/", [this]() {
        rebuild();
    });

    rebuild();

    if (panel_type == Compact) {
        // Respond to requests from the palette widget to change palettes.
        _palette->get_palette_selected_signal().connect([this] (Glib::ustring name) {
            set_palette(name);
        });
        // The Dialog version will update the saved preference underneath us, so watch for that.
        _saved_palette_path.action = [this, panel_type] { update_palettes(panel_type); };
    }
    else if (panel_type == Popup) {
        // swatch fill
        _selector.set_visible(false);
        _current_palette_id = auto_id;
        get_widget<Gtk::Label>(_builder, "swatch-fill").set_visible();
        _palette->show_pinned_colors(false);
        // _palette->enable_color_selection(true);
        _palette->enable_scrollbar(false);
        _palette->show_scrollbar_checkbox(false);
        _palette->enable_stretch(false);
        _palette->show_stretch_checkbox(false);
        auto& header = get_widget<Gtk::Box>(_builder, "header");
        header.set_margin_start(0);
        header.set_margin_end(0);
        header.set_margin_top(3);
        auto& content = get_widget<Gtk::Box>(_builder, "content");
        content.set_margin_start(0);
        content.set_margin_end(0);
        auto& footer = get_widget<Gtk::Box>(_builder, "footer");
        footer.set_visible(false);
        get_widget<Gtk::MenuButton>(_builder, "settings2").set_visible();

        append(get_widget<Gtk::Box>(_builder, "main"));
    }
    else {
        append(get_widget<Gtk::Box>(_builder, "main"));

        _open_btn.signal_clicked().connect([this]{
            // load a color palette file selected by the user
            if (load_swatches()) {
                update_loaded_palette_entry();
                update_selector_menu();
                select_palette(_loaded_palette.id);
            }
        });
    }
}

SwatchesPanel::~SwatchesPanel()
{
    untrack_gradients();
}

void SwatchesPanel::select_vector(SPGradient* vector) {
    //
}

SPGradient* SwatchesPanel::get_selected_vector() const {
    return 0;
}

/*
 * Activation
 */

// Note: The "Auto" palette shows the list of gradients that are swatches. When this palette is
// shown (and we have a document), we therefore need to track both addition/removal of gradients
// and changes to the isSwatch() status to keep the palette up-to-date.

void SwatchesPanel::documentReplaced()
{
    if (getDocument()) {
        if (_current_palette_id == auto_id) {
            track_gradients();
        }
    } else {
        untrack_gradients();
    }

    if (_current_palette_id == auto_id) {
        rebuild();
    }
}

void SwatchesPanel::desktopReplaced()
{
    documentReplaced();
}

void SwatchesPanel::set_palette(const Glib::ustring& id) {
    auto prefs = Preferences::get();
    prefs->setString(_prefs_path + "/palette", id);
    select_palette(id);
}

const PaletteFileData *SwatchesPanel::get_palette(const Glib::ustring& id) {
    if (auto p = GlobalPalettes::get().find_palette(id)) return p;

    if (_loaded_palette.id == id) return &_loaded_palette;

    return nullptr;
}

void SwatchesPanel::select_palette(const Glib::ustring& id) {
    if (_current_palette_id == id) return;

    _current_palette_id = id;

    bool edit = false;
    if (id == auto_id) {
        if (getDocument()) {
            track_gradients();
            edit = false; /*TODO: true; when swatch editing is ready */
        }
    } else {
        untrack_gradients();
    }

    update_selector_label(_current_palette_id);

    _new_btn.set_visible(edit);
    _import_btn.set_visible(edit);
    _delete_btn.set_visible(edit);

    rebuild();
}

void SwatchesPanel::track_gradients()
{
    auto doc = getDocument();

    // Subscribe to the addition and removal of gradients.
    conn_gradients.disconnect();
    conn_gradients = doc->connectResourcesChanged("gradient", [this] {
        gradients_changed = true;
        _scheduleUpdate();
    });

    // Subscribe to child modifications of the defs section. We will use this to monitor
    // each gradient for whether its isSwatch() status changes.
    conn_defs.disconnect();
    conn_defs = doc->getDefs()->connectModified([this] (SPObject*, unsigned flags) {
        if (flags & SP_OBJECT_CHILD_MODIFIED_FLAG) {
            defs_changed = true;
            _scheduleUpdate();
        }
    });

    gradients_changed = false;
    defs_changed = false;
    rebuild_isswatch();
}

void SwatchesPanel::untrack_gradients()
{
    conn_gradients.disconnect();
    conn_defs.disconnect();
    gradients_changed = false;
    defs_changed = false;
}

/*
 * Updating
 */

void SwatchesPanel::selectionChanged(Selection*)
{
    selection_changed = true;
    _scheduleUpdate();
}

void SwatchesPanel::selectionModified(Selection*, guint flags)
{
    if (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) {
        selection_changed = true;
        _scheduleUpdate();
    }
}

void SwatchesPanel::_scheduleUpdate()
{
    if (_tick_callback) {
        return;
    }

    _tick_callback = add_tick_callback([this] (auto &&) {
        _tick_callback = 0;
        _update();
        return false;
    });
}

// Document updates are handled asynchronously by setting a flag and queuing a tick callback.
// This results in the following function being run just before the widget is relayouted,
// so that multiple document updates only result in a single UI update.
void SwatchesPanel::_update()
{
    if (gradients_changed) {
        assert(_current_palette_id == auto_id);
        // We are in the "Auto" palette, and a gradient was added or removed.
        // The list of widgets has therefore changed, and must be completely rebuilt.
        // We must also rebuild the tracking information for each gradient's isSwatch() status.
        rebuild_isswatch();
        rebuild();
    } else if (defs_changed) {
        assert(_current_palette_id == auto_id);
        // We are in the "Auto" palette, and a gradient's isSwatch() status was possibly modified.
        // Check if it has; if so, then the list of widgets has changed, and must be rebuilt.
        if (update_isswatch()) {
            rebuild();
        }
    }

    if (selection_changed) {
        update_fillstroke_indicators();
    }

    selection_changed = false;
    gradients_changed = false;
    defs_changed = false;
}

void SwatchesPanel::rebuild_isswatch()
{
    auto grads = getDocument()->getResourceList("gradient");

    isswatch.resize(grads.size());

    for (int i = 0; i < grads.size(); i++) {
        isswatch[i] = static_cast<SPGradient*>(grads[i])->isSwatch();
    }
}

bool SwatchesPanel::update_isswatch()
{
    auto grads = getDocument()->getResourceList("gradient");

    // Should be guaranteed because we catch all size changes and call rebuild_isswatch() instead.
    assert(isswatch.size() == grads.size());

    bool modified = false;

    for (int i = 0; i < grads.size(); i++) {
        if (isswatch[i] != static_cast<SPGradient*>(grads[i])->isSwatch()) {
            isswatch[i].flip();
            modified = true;
        }
    }

    return modified;
}

void SwatchesPanel::update_fillstroke_indicators()
{
    auto doc = getDocument();
    auto style = SPStyle(doc);

    // Get the current fill or stroke as a ColorKey.
    auto current_color = [&, this] (bool fill) -> std::optional<ColorKey> {
        switch (sp_desktop_query_style(getDesktop(), &style, fill ? QUERY_STYLE_PROPERTY_FILL : QUERY_STYLE_PROPERTY_STROKE))
        {
            case QUERY_STYLE_SINGLE:
            case QUERY_STYLE_MULTIPLE_AVERAGED:
            case QUERY_STYLE_MULTIPLE_SAME:
                break;
            default:
                return {};
        }

        auto attr = style.getFillOrStroke(fill);
        if (!attr->set) {
            return {};
        }

        if (attr->isNone()) {
            return std::monostate{};
        } else if (attr->isColor()) {
            return attr->getColor();
        } else if (attr->isPaintserver()) {
            if (auto grad = cast<SPGradient>(fill ? style.getFillPaintServer() : style.getStrokePaintServer())) {
                if (grad->isSwatch()) {
                    return grad;
                } else if (grad->ref) {
                    if (auto ref = grad->ref->getObject(); ref && ref->isSwatch()) {
                        return ref;
                    }
                }
            }
        }

        return {};
    };

    for (auto w : current_fill) w->set_fill(false);
    for (auto w : current_stroke) w->set_stroke(false);

    current_fill.clear();
    current_stroke.clear();

    if (auto fill = current_color(true)) {
        auto range = widgetmap.equal_range(*fill);
        for (auto it = range.first; it != range.second; ++it) {
            current_fill.emplace_back(it->second);
        }
    }

    if (auto stroke = current_color(false)) {
        auto range = widgetmap.equal_range(*stroke);
        for (auto it = range.first; it != range.second; ++it) {
            current_stroke.emplace_back(it->second);
        }
    }

    for (auto w : current_fill) w->set_fill(true);
    for (auto w : current_stroke) w->set_stroke(true);
}

[[nodiscard]] static auto to_palette_t(PaletteFileData const &p)
{
    UI::Widget::palette_t palette;
    palette.name = p.name;
    palette.id = p.id;
    for (auto const &c : p.colors) {
        std::visit(VariantVisitor {
            [&](const Colors::Color& c) {
                auto rgb = *c.converted(Colors::Space::Type::RGB);
                palette.colors.push_back({rgb[0], rgb[1], rgb[2]});
            },
            [&](const PaletteFileData::SpotColor& s) {
                auto rgb = *s.color.converted(Colors::Space::Type::RGB);
                palette.colors.push_back({rgb[0], rgb[1], rgb[2]});
            },
            [](const PaletteFileData::SpacerItem&) {},
            [](const PaletteFileData::GroupStart&) {}
        }, c);
    }
    return palette;
}

/**
 * Process the list of available palettes and update the list in the _palette widget.
 */
void SwatchesPanel::update_palettes(PanelType panel_type) {
    _palettes.clear();

    // The first palette in the list is always the "Auto" palette. Although this
    // will contain colors when selected, the preview we show for it is empty.
    // TRANSLATORS: A list of swatches in the document
    _palettes.push_back(PaletteLoaded{{_("Document swatches"), auto_id, {}}, false});

    if (panel_type != Popup) {
        // One extra for auto palette above, and one extra for (optional) loaded palette below.
        _palettes.reserve(2 + GlobalPalettes::get().palettes().size());
        // The remaining palettes in the list are the global palettes.
        for (auto &p : GlobalPalettes::get().palettes()) {
            auto palette = to_palette_t(p);
            _palettes.emplace_back(std::move(palette), false);
        }

        // If the _saved_palette_path value changed on us and the loaded palette is currently
        // selected, we'll need to reset the visible palette after we load the new palette path.
        auto reset_to_loaded_palette = (!_current_palette_id.empty() &&
                                        _current_palette_id == _loaded_palette.id);
        if (load_swatches(Glib::filename_from_utf8(_saved_palette_path))) {
            update_loaded_palette_entry();
            if (reset_to_loaded_palette) {
                set_palette(_loaded_palette.id);
            }
        }
    }

    // Save the palettes to our popup button
    std::vector<UI::Widget::palette_t> palettes;
    std::transform(_palettes.begin(), _palettes.end(), std::back_inserter(palettes),
                   [](auto &&pair){ return pair.first; });
    _palette->set_palettes(palettes);
    _palette->set_selected(_current_palette_id);
}

/**
 * Rebuild the list of color items shown by the palette.
 */
void SwatchesPanel::rebuild()
{
    std::vector<std::unique_ptr<ColorItem>> palette;

    // The pointers in widgetmap are to widgets owned by the ColorPalette. It is assumed it will not
    // delete them unless we ask, via the call to set_colors() later in this function.
    widgetmap.clear();
    current_fill.clear();
    current_stroke.clear();

    // Add the "remove-color" color.
    auto w = std::make_unique<ColorItem>(this);
    w->set_pinned_pref(_prefs_path);
    widgetmap.emplace(std::monostate{}, w.get());
    palette.push_back(std::move(w));

    _palette->set_page_size(0);
    if (auto pal = get_palette(_current_palette_id)) {
        _palette->set_page_size(pal->columns);
        palette.reserve(palette.size() + pal->colors.size());
        auto dialog = this;
        for (auto &c : pal->colors) {
            auto w = std::visit(VariantVisitor {
                [](const PaletteFileData::SpacerItem&) {
                    return std::make_unique<ColorItem>("");
                },
                [](const PaletteFileData::GroupStart& g) {
                    return std::make_unique<ColorItem>(g.name);
                },
                [=, this](const Colors::Color& c) {
                    auto w = std::make_unique<ColorItem>(c, dialog);
                    w->set_pinned_pref(_prefs_path);
                    widgetmap.emplace(c, w.get());
                    return w;
                },
                [=, this](const PaletteFileData::SpotColor& s) {
                    auto w = std::make_unique<ColorItem>(s.color, dialog, true, s.tint);
                    w->set_pinned_pref(_prefs_path);
                    widgetmap.emplace(s.color, w.get());
                    return w;
                },
            }, c);
            palette.push_back(std::move(w));
        }
    } else if (_current_palette_id == auto_id && getDocument()) {
        auto grads = getDocument()->getResourceList("gradient");
        for (auto obj : grads) {
            auto grad = cast_unsafe<SPGradient>(obj);
            if (grad->isSwatch()) {
                auto w = std::make_unique<ColorItem>(grad, this);
                widgetmap.emplace(grad, w.get());
                // Rebuild if the gradient gets pinned or unpinned
                w->signal_pinned().connect([this]{
                    rebuild();
                });
                palette.push_back(std::move(w));
            }
        }
    }

    if (getDocument()) {
        update_fillstroke_indicators();
    }

    _palette->set_colors(std::move(palette));
    _palette->set_selected(_current_palette_id);
}

bool SwatchesPanel::load_swatches() {
    auto window = dynamic_cast<Gtk::Window *>(get_root());
    auto file = choose_palette_file(window);
    auto loaded = false;
    if (file && load_swatches(file->get_path())) {
        auto prefs = Preferences::get();
        prefs->setString(_prefs_path + "/palette", _loaded_palette.id);
        prefs->setString(_prefs_path + "/palette-path", file->get_path());
        loaded = true;
    }
    return loaded;
}

bool SwatchesPanel::load_swatches(std::string const &path)
{
    if (path.empty()) {
        return false;
    }

    // load colors
    auto res = load_palette(path);
    if (res.palette) {
        // use loaded palette
        _loaded_palette = std::move(*res.palette);
        return true;
    } else if (auto desktop = getDesktop()) {
        desktop->showNotice(res.error_message);
    }

    return false;
}

void SwatchesPanel::update_loaded_palette_entry() {
    // add or update last entry in a store to match loaded palette
    if (_palettes.empty() || !_palettes.back().second) { // last palette !loaded
        _palettes.emplace_back();
    }
    auto &[palette, loaded] = _palettes.back();
    palette = to_palette_t(_loaded_palette);
    loaded = true;
}

void SwatchesPanel::setup_selector_menu()
{
    auto const key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(sigc::mem_fun(*this, &SwatchesPanel::on_selector_key_pressed), true);
    _selector.set_popover(*_selector_menu);
    _selector.add_controller(key);
}

bool SwatchesPanel::on_selector_key_pressed(unsigned const keyval, unsigned /*keycode*/,
                                            Gdk::ModifierType const state)
{
    // We act like GtkComboBox in that we only move the active item if no modifier key was pressed:
    if (Controller::has_flag(state, Gtk::Accelerator::get_default_mod_mask())) return false;

    auto const begin = _palettes.cbegin(), end = _palettes.cend();
    auto it = std::find_if(begin, end, [&](auto &p){ return p.first.id == _current_palette_id; });
    if (it == end) return false;

    int const old_index = std::distance(begin, it), back = _palettes.size() - 1;
    int       new_index = old_index;

    switch (keyval) {
        case GDK_KEY_Up  : --new_index       ; break;
        case GDK_KEY_Down: ++new_index       ; break;
        case GDK_KEY_Home:   new_index = 0   ; break;
        case GDK_KEY_End :   new_index = back; break;
        default: return false;
    }

    new_index = std::clamp(new_index, 0, back);
    if (new_index != old_index) {
        it = begin + new_index;
        set_palette(it->first.id);
    }
    return true;
}

[[nodiscard]] static auto make_selector_item(UI::Widget::palette_t const &palette)
{
    static constexpr int max_chars = 35; // Make PopoverMenuItems ellipsize long labels, in middle.

    auto const label = Gtk::make_managed<Gtk::Label>(palette.name, true);
    label->set_xalign(0.0);
    UI::ellipsize(*label, max_chars, Pango::EllipsizeMode::MIDDLE);

    auto const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    box->append(*label);
    box->append(*Gtk::make_managed<UI::Widget::ColorPalettePreview>(palette.colors));

    auto const item = Gtk::make_managed<UI::Widget::PopoverMenuItem>();
    item->set_child(*box);

    return std::pair{item, label};
}

void SwatchesPanel::update_selector_menu()
{
    g_assert(_selector_menu);

    _selector.set_sensitive(false);
    _selector_label.set_label({});
    _selector_menu->remove_all();

    if (_palettes.empty()) return;

    // TODO: GTK4: probably nicer to use GtkGridView.
    Inkscape::UI::ColumnMenuBuilder builder{*_selector_menu, 2};
    // Items are put in a SizeGroup to keep the two columnsʼ widths homogeneous
    auto const size_group = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL);
    auto const add_item = [&](UI::Widget::palette_t const &palette){
        auto const [item, label] = make_selector_item(palette);
        item->signal_activate().connect([id = palette.id, this]{ set_palette(id); });
        size_group->add_widget(*label);
        builder.add_item(*item);
    };
    // Acrobatics are done to sort down columns, not over rows
    auto const size = _palettes.size(), half = (size + 1) / 2;
    for (std::size_t left = 0; left < half; ++left) {
        add_item(_palettes.at(left).first);
        if (auto const right = left + half; right < size) {
            add_item(_palettes.at(right).first);
        }
    }

    _selector.set_sensitive(true);
    size_group->add_widget(_selector_label);
}

void SwatchesPanel::update_selector_label(Glib::ustring const &active_id)
{
    // Set the new paletteʼs name as label of the selector menubutton.
    auto const it = std::find_if(_palettes.cbegin(), _palettes.cend(),
                                 [&](auto const &pair){ return pair.first.id == active_id; });
    g_assert(it != _palettes.cend());
    _selector_label.set_label(it->first.name);
}

void SwatchesPanel::clear_filter() {
    if (_color_filter_text.empty()) return;

    _color_filter_text.erase();
    _palette->apply_filter();
}

void SwatchesPanel::filter_colors(const Glib::ustring& text) {
    auto search = text.lowercase();
    if (_color_filter_text == search) return;

    _color_filter_text = search;
    _palette->apply_filter();
}

bool SwatchesPanel::filter_callback(const Dialog::ColorItem& color) const {
    if (_color_filter_text.empty()) return true;

    // let's hide group headers and fillers when searching for a matching color
    if (color.is_filler() || color.is_group()) return false;

    auto text = color.get_description().lowercase();
    return text.find(_color_filter_text) != Glib::ustring::npos;
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
