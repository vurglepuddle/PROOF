// SPDX-License-Identifier: GPL-2.0-or-later
#include "recolor-art.h"

#include <glibmm/i18n.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gridlayout.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/layoutmanager.h>
#include <gtkmm/overlay.h>

#include "actions/actions-tools.h"
#include "color-notebook.h"
#include "desktop.h"
#include "document-undo.h"
#include "multi-marker-color-plate.h"
#include "ui/builder-utils.h"
#include "ui/icon-names.h"
#include "ui/widget/color-preview.h"
#include "selcue.h"
#include "seltrans.h"
#include "selection.h" 
#include "ui/tools/select-tool.h"

namespace Inkscape::UI::Widget {

/*
 * this class is showed by paint-selector class that has a button
 * to trigger the popover that has this widget as its child
 *
 * related classes :
 * 1- ink-colorwheel for the multimarkercolorwheel in the colorwheel page
 * 2- multi-marker-color-wheel-plate that manges the multimarkercolorwheel and sliders under it
 * 3- object-colors manages data and extract objects colors
 */

RecolorArt::RecolorArt()
    : RecolorArt{create_builder("widget-recolor.ui")}
{}

RecolorArt::RecolorArt(Glib::RefPtr<Gtk::Builder> const &builder)
    : _color_picker_container{get_widget<Gtk::Box>(builder, "color-picker")}
    , _notebook(get_widget<Gtk::Notebook>(builder, "list-wheel-box"))
    , _color_wheel_page(get_widget<Gtk::Box>(builder, "color-wheel-page"))
    , _color_wheel(Gtk::make_managed<MultiMarkerColorPlate>(Colors::ColorSet{}))
    , _color_list(get_widget<Gtk::Box>(builder, "colors-list"))
    , _reset(get_widget<Gtk::Button>(builder, "reset"))
    , _live_preview(get_widget<Gtk::CheckButton>(builder, "liveP"))
    , _list_view{get_widget<Gtk::ListView>(builder, "recolor-art-list")}
{
    set_name("RecolorArt");
    append(get_widget<Gtk::Box>(builder, "recolor-art"));
    _solid_colors->set(Color(0x000000ff));
    // when recolor widget is closed it detaches from desktop, ending session
    signal_unmap().connect([this] { setDesktop(nullptr); });
    // PROOF: hide the selection boxes only while the widget is on screen. Fill and Stroke attaches
    // it to each new desktop even when the dialog is hidden, which left the boxes hidden for good.
    signal_map().connect([this] {
        if (_desktop) {
            _desktop->setHideSelectionBoxes(true);
        }
    });

    _color_wheel->connect_color_changed(static_cast<sigc::slot<void()>>([this]() {
        if(_blocker.pending()) {
            return; // to stop recursive calling to signal if changed from the color list page
        }
        uint32_t cc =  _color_wheel->getColor().toRGBA();
        Color c(cc,true); 
        if(_color_wheel->getActiveIndex() != -1) {
            int index = _color_wheel->getActiveIndex();
            if (!_manager.getColor(index)) {
                return;
            }
            _current_color_id = _manager.getColor(index)->toRGBA();
            auto idx = findColorItemByKey(_current_color_id);
            _selection_model->set_selected(idx.second);
            onColorPickerChanged(c, true);
            onOriginalColorClicked(_current_color_id);
            if (_color_wheel->getHueLock()) {
                if (_manager.isColorsEmpty()) {
                    return;
                }
                std::vector<Colors::Color> new_colors = _color_wheel->getColors();
                _manager.setSelectedNewColor(new_colors);
                updateColorModel(new_colors);
                if (_is_preview)
                    _manager.convertToRecoloredColors();
            }
        }
    }));
    _color_wheel->setRecolorWidget(this);
    // add hover opacity effect when hovering over markers in the wheel
    _color_wheel->connect_color_hovered([this] {
        uint32_t cc =  _color_wheel->getColor().toRGBA();
        Color c(cc,true); 
        if (_color_wheel->getHoverIndex() != -1) {
            int index = _color_wheel->getHoverIndex();
            if (!_manager.getColor(index)) {
                return;
            }
            _current_color_id = _manager.getColor(index)->toRGBA();
        }
    });

    layoutColorPicker();
    _live_preview.set_active(true);
    _live_preview.signal_toggled().connect(sigc::mem_fun(*this, &RecolorArt::onLivePreviewToggled));
    _reset.signal_clicked().connect(sigc::mem_fun(*this, &RecolorArt::onResetClicked));

    // setting up list view for the color list
    _color_model = Gio::ListStore<ColorItem>::create();
    _selection_model = Gtk::SingleSelection::create(_color_model);
    _color_factory = Gtk::SignalListItemFactory::create();

    // setup how the list item should look 
    _color_factory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const &list_item) {
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        auto original = Gtk::make_managed<Gtk::Box>();
        auto arrow = Gtk::make_managed<Gtk::Image>();
        auto recolored = Gtk::make_managed<Gtk::Box>();

        auto original_preview = Gtk::make_managed<ColorPreview>(Color(0x00000000).toRGBA());
        auto recolored_preview = Gtk::make_managed<ColorPreview>(Color(0x00000000).toRGBA());

        auto type_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        type_box->set_spacing(2);
        type_box->set_margin_start(4);
        type_box->set_hexpand(false);
        type_box->set_vexpand(false);
        type_box->set_halign(Gtk::Align::START);
        type_box->set_valign(Gtk::Align::CENTER);
        type_box->get_style_context()->add_class("type_box");

        original_preview->set_hexpand(true);
        recolored_preview->set_hexpand(true);

        original_preview->set_vexpand(true);
        recolored_preview->set_vexpand(true);

        auto original_overlay = Gtk::make_managed<Gtk::Overlay>();

        original_overlay->set_child(*original_preview);
        original_overlay->add_overlay(*type_box);

        original->append(*original_overlay);
        recolored->append(*recolored_preview);

        original->set_hexpand(true);

        recolored->set_hexpand(true);

        arrow->set_from_icon_name(INKSCAPE_ICON("go-right"));
        arrow->set_halign(Gtk::Align::CENTER);
        arrow->set_valign(Gtk::Align::CENTER);
        arrow->set_margin_top(3);
        arrow->set_margin_start(6);
        arrow->set_margin_end(6);

        box->set_name("original-recolor-box");
        box->append(*original);
        box->append(*arrow);
        box->append(*recolored);
        list_item->set_data("typebox", type_box);
        list_item->set_child(*box);
    });

    // setup signals for the list item children after they are created
    _color_factory->signal_bind().connect([this](Glib::RefPtr<Gtk::ListItem> const &list_item) {
        auto item = std::dynamic_pointer_cast<ColorItem>(list_item->get_item());
        if (!item) {
            return;
        }

        auto box = dynamic_cast<Gtk::Box *>(list_item->get_child());
        if (!box || !box->get_first_child() || !box->get_last_child()) {
            return;
        }

        auto original = dynamic_cast<Gtk::Box *>(box->get_first_child());
        auto recolored = dynamic_cast<Gtk::Box *>(box->get_last_child());

        if (original && recolored) {
            colorButtons(original, item->old_color, true);
            colorButtons(recolored, item->new_color);
            setUpTypeBox(static_cast<Gtk::Box *>(list_item->get_data("typebox")), item->old_color);

            original->set_name("original");
            recolored->set_name("recolored");

            auto original_click = Gtk::GestureClick::create();
            auto recolored_click = Gtk::GestureClick::create();

            original_click->signal_pressed().connect(
                [this, item, index = list_item->get_position()](int n_press, double x, double y) {
                    _selection_model->set_selected(index);
                    onOriginalColorClicked(item->key);
                });

            recolored_click->signal_pressed().connect(
                [this, item, index = list_item->get_position()](int n_press, double x, double y) {
                    _selection_model->set_selected(index);
                    onOriginalColorClicked(item->key);
                });

            original->add_controller(original_click);
            recolored->add_controller(recolored_click);
        }
    });

    _list_view.set_model(_selection_model);
    _list_view.set_factory(_color_factory);

    auto lm = _list_view.get_layout_manager();
    if (auto grid_layout = std::dynamic_pointer_cast<Gtk::GridLayout>(lm)) {
        grid_layout->set_row_spacing(0);
    }
    _list_view.set_hexpand(false);
    _list_view.set_vexpand(false);

    _selection_model->signal_selection_changed().connect([this](guint pos, guint n_items) {
        int index = _selection_model->get_selected();
        if (index < 0) {
            return;
        }

        auto item = _color_model->get_item(index);
        auto color_item = std::dynamic_pointer_cast<ColorItem>(item);
        if (!color_item) {
            return;
        }

        onOriginalColorClicked(color_item->key);
    });

    _color_wheel_page.append(*_color_wheel);
}

void RecolorArt::setDesktop(SPDesktop *desktop)
{
    if (_desktop == desktop) {
        return;
    }

    if (_desktop) {
        _sel_changed_conn.disconnect();
        _desktop_destroyed_conn.disconnect();

        _desktop->setHideSelectionBoxes(false);

        if (!_is_preview) {
            _manager.convertToRecoloredColors();
            DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Change item color"), INKSCAPE_ICON("object-recolor-art"));
        }
    }

    _desktop = desktop;

    if (_desktop) {
        if (get_mapped()) {
            _desktop->setHideSelectionBoxes(true);
        }

        _desktop_destroyed_conn = _desktop->connectDestroy([this] (auto) {
            setDesktop(nullptr);
        });
    }

    set_sensitive(_desktop);
}

/*
 * prepare color model by creating color items and populate the color model
 * then push the _list_view to color list page to show it in the ui
 */
void RecolorArt::generateVisualList()
{
    _color_model->remove_all();
    std::vector<Glib::RefPtr<ColorItem>> items;
    for (auto const &[key, value] : _manager.getSelectedColorsMap()) {
        auto old_color = value.second.value().old_color;
        auto new_color = value.second.value().new_color;
        items.push_back(ColorItem::create(key, old_color, new_color));
    }
    _color_model->splice(0,0,items);
    if (_color_model->get_n_items() > 0) {
        _selection_model->set_selected(0);
    }
}

/*
 * setup the layout of the colornotebook ui in the colorlist page
 * connect _solid_colors to color changed signal and call the signal handler
 */
void RecolorArt::layoutColorPicker(std::shared_ptr<Colors::ColorSet> updated_color)
{
    _color_picker_wdgt = Gtk::make_managed<ColorNotebook>(_solid_colors);
    _color_picker_wdgt->set_label(_("<b>Selected Color</b>"));

    _solid_colors->signal_changed.connect([this]() { onColorPickerChanged(); });

    for (auto child : _color_picker_container.get_children()) {
        _color_picker_container.remove(*child);
    }
    _color_picker_container.append(*_color_picker_wdgt);
}

/*
 * set colorpreview colors used in color factory signal bind
 */
void RecolorArt::colorButtons(Gtk::Box *button, Color color, bool is_original)
{
    if (button) {
        auto child =
            is_original
                ? dynamic_cast<ColorPreview *>(dynamic_cast<Gtk::Overlay *>(button->get_first_child())->get_child())
                : dynamic_cast<ColorPreview *>(button->get_first_child());
        if (child) {
            auto rgba = color.toRGBA();
            child->setRgba32(rgba);
        }
    }
}

/*
 * set up the type box which shows the color usage in what kind (fill,strok,pattern...etc)
 */
void RecolorArt::setUpTypeBox(Gtk::Box *box, Color const &color)
{
    if (!box->get_children().empty()) {
        return;
    }

    auto items = _manager.getSelectedItems(color.toRGBA());
    if (!items.empty()) {
        std::string size = "<b>" + std::to_string(items.size()) + "</b>";
        std::map<std::string,std::pair<int,std::string>> kinds;
        for (auto item : items) {
            if (item.type == ObjectStyleType::Fill) {
                kinds[INKSCAPE_ICON("object-fill")].first++;
                kinds[INKSCAPE_ICON("object-fill")].second = "fill";
            } else if (item.type == ObjectStyleType::Stroke) {
                kinds[INKSCAPE_ICON("object-stroke")].first++;
                kinds[INKSCAPE_ICON("object-stroke")].second = "stroke";
            } else if (item.type == ObjectStyleType::Mesh) {
                kinds[INKSCAPE_ICON("paint-gradient-mesh")].first++;
                kinds[INKSCAPE_ICON("paint-gradient-mesh")].second = "mesh gradient";
            } else if (item.type == ObjectStyleType::Linear) {
                kinds[INKSCAPE_ICON("paint-gradient-linear")].first++;
                kinds[INKSCAPE_ICON("paint-gradient-linear")].second = "linear gradient";
            } else if (item.type == ObjectStyleType::Radial) {
                kinds[INKSCAPE_ICON("paint-gradient-radial")].first++;
                kinds[INKSCAPE_ICON("paint-gradient-radial")].second = "radial gradient";
            } else if (item.type == ObjectStyleType::Pattern) {
                kinds[INKSCAPE_ICON("paint-pattern")].first++;
                kinds[INKSCAPE_ICON("paint-pattern")].second = "pattern";
            } else if (item.type == ObjectStyleType::Marker) {
                kinds[INKSCAPE_ICON("markers")].first++;
                kinds[INKSCAPE_ICON("markers")].second = "marker";
            } else if (item.type == ObjectStyleType::Mask) {
                kinds[INKSCAPE_ICON("overlay-mask")].first++;
                kinds[INKSCAPE_ICON("overlay-mask")].second = "mask";
            } else if (item.type == ObjectStyleType::Swatch) {
                size = "<b>" + std::to_string(items.size() / 2) + "</b>";
                kinds[INKSCAPE_ICON("paint-swatch")].first++;
                kinds[INKSCAPE_ICON("paint-swatch")].second = "swatch";
            }
        }
        auto label = Gtk::make_managed<Gtk::Label>();
        label->set_use_markup(true);
        label->set_markup(size);
        box->append(*label);
        std::string tooltip;
        int sz = kinds.size(), s = 0;
        for (auto [icon, pair] : kinds) {
            Gtk::Image *img = nullptr;
            img = Gtk::make_managed<Gtk::Image>();
            img->set_from_icon_name(icon);

            if (img) {
                if (icon == INKSCAPE_ICON("overlay-mask")) {
                    img->set_pixel_size(16);
                    img->set_halign(Gtk::Align::CENTER);
                    img->set_valign(Gtk::Align::CENTER);
                } else {
                    img->set_pixel_size(8);
                }
                box->append(*img);
                if(icon == INKSCAPE_ICON("paint-swatch")) pair.first /= 2;
                tooltip += std::to_string(pair.first)+" x " + pair.second;
                if (s != sz - 1) {
                    tooltip += "\n";
                }
            }
            s++;
        }
        box->set_tooltip_text(tooltip);
    }
}

/*
 * signal handler to set solid colors(color notebook at color list page),_color_picker_wdgt and the active index in
 * colorwheel page to the color of the colorpreview clicked
 */
void RecolorArt::onOriginalColorClicked(uint32_t color_id)
{
    if (!_manager.isColorsEmpty()) {
        int index = _manager.getColorIndex(color_id);
        if (index > -1) {
            _color_wheel->setActiveIndex(_manager.getColorIndex(color_id));
        }
    }
    _current_color_id = color_id;
    if (auto color = _manager.getSelectedNewColor(color_id)) {
        _solid_colors->set(*color);                          // update sliders under the colorwheel in the colorlist page
        _color_picker_wdgt->setCurrentColor(_solid_colors); /* solves the issue of needing to create new
         colornotebook every time the _solid_colors changes because it only changes the sliders not the
         color wheel it self in colornotebook */
    }
}

/*
* if LP is checked it searches for the items that has a key matching to the parameter color
* and loop on them to change their color lively
* put the recolor action into the undo stack as well
*/
void RecolorArt::lpChecked(Color color, bool wheel)
{
    std::optional<Color> new_color = wheel ? color : _solid_colors->get();
    if (!new_color.has_value()) {
        return;
    }
    if (!_manager.applyNewColorToSelection(_current_color_id, new_color.value())) {
        return;
    }

    DocumentUndo::maybeDone(_desktop->getDocument(), "changed-item-color", RC_("Undo", "Recolor items"),
                            INKSCAPE_ICON("object-recolor-art"));
}

/*
 * this is a signal handler for when solid color changes either in the sliders or the color wheels in
 * both notebook pages
 * searches for the selected color items then change them through lpchecked() function and update the pair
 * in _selected_colors map
 * sync the change through notebook pages what happens in one get updated in the other
 * update color model item to refresh the listview ui
 */
void RecolorArt::onColorPickerChanged(Color color, bool wheel)
{
    auto guard = _blocker.block();
    std::optional<Color> new_color = wheel ? color : _solid_colors->get();
    if (!new_color.has_value()) {
        return;
    }
    // to prevent unnecessary changes if the "new_color" is still equal to the current color on the wheel
    if (_manager.getSelectedNewColor(_current_color_id) == new_color.value()) {
        return;
    }
    std::string _color_string = new_color.value().toString();
    _manager.setSelectedNewColor(_current_color_id, new_color.value());

    // apply changes to selected items
    if (_live_preview.property_active()) {
        if (wheel)
            lpChecked(color, wheel);
        else
            lpChecked();
    }
    guint index = _selection_model->get_selected();
    Glib::RefPtr<ColorItem> item;
    // if change is coming from colorlist page sync that to colorwheel page
    if (!wheel){
        if (index < 0) {
            return;
        }
        item = _color_model->get_item(index);
        int i = _manager.getColorIndex(_current_color_id);
        if (i > -1) {
            _color_wheel->changeColor(i, new_color.value());
        }
    }
    else {  // if change is coming from colorwheel page sync that to colorlist page
        auto item_index = findColorItemByKey(_current_color_id);
        item = item_index.first;
        index = item_index.second;
    }
    if (!item) {
        return;
    }
    // update colormodel item to refresh listview ui
    auto color_item = std::dynamic_pointer_cast<ColorItem>(item);
    auto new_item = ColorItem::create(color_item->key, color_item->old_color, new_color.value());
    _color_model->splice(index, 1, {new_item});
}

/*
 * update color model to refresh the listview ui with the new chossen colors
 */
void RecolorArt::updateColorModel(std::vector<Colors::Color> const &new_colors)
{
    if (!new_colors.empty() && new_colors.size() != _color_model->get_n_items()) {
        return;
    }
    std::vector<Glib::RefPtr<ColorItem>> new_colors_buttons;
    for (auto i = 0; i < _color_model->get_n_items(); i++) {
        auto item = _color_model->get_item(i);
        if (!item) {
            continue;
        }
        auto color_item = std::dynamic_pointer_cast<ColorItem>(item);
        if(!color_item) {
            continue;
        }
        int index = _manager.getColorIndex(color_item->key);
        auto new_item = ColorItem::create(color_item->key, color_item->old_color,
                                          new_colors.empty() ? color_item->old_color : new_colors[index]);
        new_colors_buttons.push_back(new_item);
    }
    _color_model->splice(0, _color_model->get_n_items(), new_colors_buttons);
}

/*
 * finding a color model item by key
 */
std::pair<Glib::RefPtr<ColorItem>, guint> RecolorArt::findColorItemByKey(uint32_t key)
{
    for (auto i = 0; i < _color_model->get_n_items(); i++) {
        auto item = _color_model->get_item(i);
        auto color_item = std::dynamic_pointer_cast<ColorItem>(item);
        if (key == color_item->key) {
            return {item, i};
        }
    }
    return {nullptr, -1};
}

/*
 * signal function handler for reset button clicked
 * that reset every thing to its original states
 */
void RecolorArt::onResetClicked()
{
    _color_wheel->toggleHueLock(false);
    _color_wheel->setLightness(100.0);
    _color_wheel->setSaturation(100.0);
    _color_wheel->setColors(_manager.getColors());
    updateColorModel();
    _manager.revertToOriginalColors(true);
    guint index = _selection_model->get_selected();
    if (index == GTK_INVALID_LIST_POSITION) { 
        return;
    }
    auto item = _color_model->get_item(index);
    auto color_item = std::dynamic_pointer_cast<ColorItem>(item);

    onOriginalColorClicked(color_item->key);
}

/*
 * apply recoloring when the LP check box is checked
 * and get back to original colors when it is unchecked
 */
void RecolorArt::onLivePreviewToggled()
{
    _is_preview = _live_preview.property_active();
    if (_is_preview) {
        _manager.convertToRecoloredColors();
    } else {
        _manager.revertToOriginalColors();
    }
}

void RecolorArt::showForSelection(SPDesktop *desktop)
{
    assert(desktop);

    setDesktop(desktop);
    _sel_changed_conn = _desktop->getSelection()->connectChanged([this] (auto) {
        updateFromSelection();
    });

    updateFromSelection();
}

/*
 * main function that :
 * 1- clears old data
 * 2- get selection items from desktop
 * 3- unlink selection items if there are clones
 * 4- call collect colors func
 * 5- put the generated list in the UI
 */
void RecolorArt::updateFromSelection()
{
    if (_selection_blocker.pending()) {
        return;
    }

    _manager.clearData();

    _color_wheel->toggleHueLock(false);
    _color_wheel->setLightness(100.0);
    _color_wheel->setSaturation(100.0);

    auto selection = _desktop->getSelection();

    auto items = selection->items();
    auto vec = std::vector<SPObject *>(items.begin(), items.end());
    _manager = collect_colours(vec);
    if (!_manager.isColorsEmpty()) {
        generateVisualList();
        auto first_button_id = _manager.getFirstKey();
        onOriginalColorClicked(first_button_id);
        _color_wheel->setColors(_manager.getColors());
    }
}

void RecolorArt::showForObject(SPDesktop *desktop, SPObject *object)
{
    assert(desktop);
    assert(object);

    setDesktop(desktop);

    _manager.clearData();

    _color_wheel->toggleHueLock(false);
    _color_wheel->setLightness(100.0);
    _color_wheel->setSaturation(100.0);

    _manager = collect_colours({object});
    if (!_manager.isColorsEmpty()) {
        generateVisualList();
        auto first_button_id = _manager.getFirstKey();
        onOriginalColorClicked(first_button_id);
        _color_wheel->setColors(_manager.getColors());
    }
}

} // namespace Inkscape::UI::Widget
