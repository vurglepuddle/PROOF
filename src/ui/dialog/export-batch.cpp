// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Anshudhar Kumar Singh <anshudhar2001@gmail.com>
 *
 * Copyright (C) 1999-2007, 2021 Authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/dialog/export-batch.h"

#include <algorithm>
#include <regex>
#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/error.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/progressbar.h>

#include "desktop.h"
#include "document-undo.h"
#include "extension/output.h"
#include "inkscape-window.h"
#include "io/fix-broken-links.h"
#include "io/sandbox.h"
#include "io/sys.h"
#include "layer-manager.h"
#include "object/sp-page.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/dialog-run.h"
#include "ui/dialog/export.h"
#include "ui/dialog/link-missing.h"
#include "ui/icon-names.h"
#include "ui/widget/color-picker.h"
#include "ui/widget/export-lists.h"
#include "util/units.h"
#include "util/scope_exit.h"

namespace Inkscape::UI::Dialog {

BatchItem::BatchItem(SPItem *item, bool isolate_item, std::shared_ptr<PreviewDrawing> drawing)
    : _item{item}
    , _isolate_item{isolate_item}
{
    init(std::move(drawing));
    _object_modified_conn = _item->connectModified([=, this](SPObject *obj, unsigned int flags) {
        update_label();
    });
    update_label();
}

BatchItem::BatchItem(SPPage *page, std::shared_ptr<PreviewDrawing> drawing)
    : _page{page}
{
    init(std::move(drawing));
    _object_modified_conn = _page->connectModified([=, this](SPObject *obj, unsigned int flags) {
        update_label();
    });
    update_label();
}

BatchItem::~BatchItem() = default;

void BatchItem::update_label()
{
    Glib::ustring label = "no-name";
    if (_page) {
        label = _page->getDefaultLabel();
        if (auto id = _page->label()) {
            label = id;
        }
    } else if (_item) {
        label = _item->defaultLabel();
        if (label.empty()) {
            if (auto _id = _item->getId()) {
                label = _id;
            } else {
                label = "no-id";
            }
        }
    }
    _label_str = label;
    _label.set_text(label);
    set_tooltip_text(label);
}

/// @brief Set "Export selected only"
/// @param isolate true if only selected items should be shown in export
void BatchItem::setIsolateItem(bool isolate)
{
    if (_isolate_item != isolate) {
        _isolate_item = isolate;
        _preview.setItem(_item, _isolate_item);
    }
}

void BatchItem::init(std::shared_ptr<PreviewDrawing> drawing) {
    _grid.set_row_spacing(5);
    _grid.set_column_spacing(5);
    _grid.set_valign(Gtk::Align::CENTER);

    _selector.set_active(true);
    _selector.set_focusable(false);
    _selector.set_margin_start(2);
    _selector.set_margin_bottom(2);
    _selector.set_valign(Gtk::Align::END);

    _option.set_active(false);
    _option.set_focusable(false);
    _option.set_margin_start(2);
    _option.set_margin_bottom(2);
    _option.set_valign(Gtk::Align::END);

    _preview.set_name("export_preview_batch");
    _preview.setItem(_item, _isolate_item);
    _preview.setDrawing(std::move(drawing));
    _preview.setSize(64);
    _preview.set_halign(Gtk::Align::CENTER);
    _preview.set_valign(Gtk::Align::CENTER);

    _label.set_width_chars(10);
    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.set_halign(Gtk::Align::CENTER);

    set_valign(Gtk::Align::START);
    set_halign(Gtk::Align::START);
    set_child(_grid);
    set_focusable(false);

    _selector.signal_toggled().connect([this]() {
        set_selected(_selector.get_active());
    });
    _option.signal_toggled().connect([this]() {
        set_selected(_option.get_active());
    });

    // This initially packs the widgets with a hidden preview.
    refresh(!is_hide, 0);

    property_parent().signal_changed().connect([this] { on_parent_changed(); });
}

/**
 * Syncronise the FlowBox selection to the active widget activity.
 */
void BatchItem::set_selected(bool selected)
{
    auto box = dynamic_cast<Gtk::FlowBox *>(get_parent());
    if (box && selected != is_selected()) {
        if (selected) {
            box->select_child(*this);
        } else {
            box->unselect_child(*this);
        }
    }
}

/**
 * Syncronise the FlowBox selection to the existing active widget state.
 */
void BatchItem::update_selected()
{
    if (auto parent = dynamic_cast<Gtk::FlowBox *>(get_parent()))
        on_mode_changed(parent->get_selection_mode());
    if (_selector.get_visible()) {
        set_selected(_selector.get_active());
    } else if (_option.get_visible()) {
        set_selected(_option.get_active());
    }
}

/**
 * A change in the selection mode for the flow box.
 */
void BatchItem::on_mode_changed(Gtk::SelectionMode mode)
{
    _selector.set_visible(mode == Gtk::SelectionMode::MULTIPLE);
    _option.set_visible(mode == Gtk::SelectionMode::SINGLE);
}

/**
 * Update the connection to the parent FlowBox
 */
void BatchItem::on_parent_changed()
{
    auto parent = dynamic_cast<Gtk::FlowBox *>(get_parent());
    if (!parent) {
        return;
    }

    _selection_widget_changed_conn = parent->signal_selected_children_changed().connect([this]() {
        // Synchronise the active widget state to the Flowbox selection.
        if (_selector.get_visible()) {
            _selector.set_active(is_selected());
        } else if (_option.get_visible()) {
            _option.set_active(is_selected());
        }
    });
    update_selected();

    for (auto child = parent->get_first_child(); child; child = child->get_next_sibling()) {
        if (child != this) {
            if (auto item = dynamic_cast<BatchItem *>(child)) {
                auto group = item->get_radio_group();
                _option.set_group(*group);
                break;
            }
        }
    }
}

void BatchItem::refresh(bool hide, uint32_t bg_color)
{
    if (_page) {
        _preview.setBox(_page->getDocumentRect());
    }

    _preview.setBackgroundColor(bg_color);

    // When hiding the preview, we show the items as a checklist
    // So all items must be packed differently on refresh.
    if (hide != is_hide) {
        is_hide = hide;

        auto remove_grid_child = [&] (Gtk::Widget &widget) {
            if (widget.get_parent() == &_grid) {
                _grid.remove(widget);
            }
        };
        remove_grid_child(_selector);
        remove_grid_child(_option);
        remove_grid_child(_label);
        remove_grid_child(_preview);

        if (hide) {
            _selector.set_valign(Gtk::Align::BASELINE);
            _label.set_xalign(0.0);
            _label.set_max_width_chars(-1);
            _grid.attach(_selector, 0, 1, 1, 1);
            _grid.attach(_option, 0, 1, 1, 1);
            _grid.attach(_label, 1, 1, 1, 1);
        } else {
            _selector.set_valign(Gtk::Align::END);
            _label.set_xalign(0.5);
            _label.set_max_width_chars(18);
            _grid.attach(_preview, 0, 0, 2, 2);
            _grid.attach(_selector, 0, 1, 1, 1);
            _grid.attach(_option, 0, 1, 1, 1);
            _grid.attach(_label, 0, 2, 2, 1);
        }
        update_selected();
    }

    if (!hide) {
        _preview.queueRefresh();
    }
}

void BatchItem::setDrawing(std::shared_ptr<PreviewDrawing> drawing)
{
    _preview.setDrawing(std::move(drawing));
}

/**
 * Add and remove batch items and their previews carefully and insert new ones into the container FlowBox
 *
 * @param items List of batch-items (e.g. layers) in the batch dialog. Is updated by this function.
 * @param objects New list of objects (e.g. layers). Taken as input.
 * @param container GUI widget containing the selection of batch items. Is updated by this function.
 * @param isolate_items true if only the given (selected) items should be shown in export
 */
void BatchItem::syncItems(BatchItems &items, std::map<std::string, SPObject*> const &objects, Gtk::FlowBox &container, std::shared_ptr<PreviewDrawing> preview, bool isolate_items)
{
    // Pre- and post-condition of this function is that
    // `items` and `container.children` have the same content.
    // (They have different types and contain slightly different information,
    // but there is still a 1:1 correspondence.)
    //
    // We update `items` so that it matches `objects`.
    // Any necessary change to `items` is also applied to `container.children`.

    // a) Remove any items not in objects
    for (auto it = items.begin(); it != items.end();) {
        if (!objects.contains(it->first)) {
            container.remove(*it->second);
            it = items.erase(it);
        } else {
            it->second->setIsolateItem(isolate_items);
            ++it;
        }
    }

    // b) Add any objects not in items

    // A special container for pages allows them to be sorted correctly
    std::set<SPPage *, SPPage::PageIndexOrder> pages;

    for (auto &[id, obj] : objects) {
        if (auto page = cast<SPPage>(obj)) {
            if (!items[id] || items[id]->getPage() != page)
                pages.insert(page);
            continue;
        }

        auto item = cast<SPItem>(obj);

        // If an Item or Page with same Id is already present, Skip
        if (items[id] && items[id]->getItem() == item)
            continue;

        if (items[id]) {
            // Remove existing item with same id
            // (can occur when switching between document tabs)
            container.remove(*items[id]);
        }
        // Add new item to the end of list
        items[id] = std::make_unique<BatchItem>(item, isolate_items, preview);
        container.insert(*items[id], -1);
        items[id]->set_selected(true);
    }

    for (auto &page : pages) {
        if (auto id = page->getId()) {
            if (items[id]) {
                container.remove(*items[id]);
            }
            items[id] = std::make_unique<BatchItem>(page, preview);
            container.insert(*items[id], -1);
            items[id]->set_selected(true);
        }
    }

    // Check postconditions
    g_assert(items.size() == objects.size());
    g_assert(container.get_children().size() == items.size());
}

BatchExport::BatchExport(BaseObjectType * const cobject, Glib::RefPtr<Gtk::Builder> const &builder)
    : Gtk::Box{cobject}
    , preview_container(get_widget<Gtk::FlowBox>      (builder, "b_preview_box"))
    , show_preview     (get_widget<Gtk::CheckButton>  (builder, "b_show_preview"))
    , num_elements     (get_widget<Gtk::Label>        (builder, "b_num_elements"))
    , hide_all         (get_widget<Gtk::CheckButton>  (builder, "b_hide_all"))
    , overwrite        (get_widget<Gtk::CheckButton>  (builder, "b_overwrite"))
    , name_text        (get_widget<Gtk::Entry>        (builder, "b_name"))
    , path_chooser     (get_widget<Gtk::Button>       (builder, "b_path"))
    , export_btn       (get_widget<Gtk::Button>       (builder, "b_export"))
    , cancel_btn       (get_widget<Gtk::Button>       (builder, "b_cancel"))
    , progress_box     (get_widget<Gtk::Box>          (builder, "b_inprogress"))

    , _prog            (get_widget<Gtk::ProgressBar>  (builder, "b_progress"))
    , _prog_batch      (get_widget<Gtk::ProgressBar>  (builder, "b_progress_batch"))
    , export_list      (get_derived_widget<ExportList>(builder, "b_export_list"))
    , _background_color(get_derived_widget<UI::Widget::ColorPicker>(builder, "b_backgnd", _("Background color"), true))
{
    prefs = Inkscape::Preferences::get();

    selection_names[SELECTION_SELECTION] = "selection";
    selection_names[SELECTION_LAYER] = "layer";
    selection_names[SELECTION_PAGE] = "page";

    selection_buttons[SELECTION_SELECTION] = &get_widget<Gtk::ToggleButton>(builder, "b_s_selection");
    selection_buttons[SELECTION_LAYER]     = &get_widget<Gtk::ToggleButton>(builder, "b_s_layers");
    selection_buttons[SELECTION_PAGE]      = &get_widget<Gtk::ToggleButton>(builder, "b_s_pages");

    path_chooser.signal_clicked().connect([this] { pickBatchPath(); });

    setup();
}

BatchExport::~BatchExport() = default;

void BatchExport::selectionModified(Inkscape::Selection *selection, guint flags)
{
    if (!_desktop || _desktop->getSelection() != selection) {
        return;
    }
    if (!(flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_PARENT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
        return;
    }
    queueRefreshItems();
}

void BatchExport::selectionChanged(Inkscape::Selection *selection)
{
    if (!_desktop || _desktop->getSelection() != selection) {
        return;
    }
    selection_buttons[SELECTION_SELECTION]->set_sensitive(!selection->isEmpty());
    if (selection->isEmpty()) {
        if (current_key == SELECTION_SELECTION) {
            selection_buttons[SELECTION_LAYER]->set_active(true); // This causes refresh area
            // return otherwise refreshArea will be called again
            // even though we are at default key, selection is the one which was original key.
            prefs->setString("/dialogs/export/batchexportarea/value", selection_names[SELECTION_SELECTION]);
            return;
        }
    } else {
        Glib::ustring pref_key_name = prefs->getString("/dialogs/export/batchexportarea/value");
        if (selection_names[SELECTION_SELECTION] == pref_key_name && current_key != SELECTION_SELECTION) {
            selection_buttons[SELECTION_SELECTION]->set_active();
            return;
        }
    }
    queueRefresh();
}

void BatchExport::pagesChanged()
{
    if (!_desktop || !_document) return;

    bool has_pages = _document->getPageManager().hasPages();
    selection_buttons[SELECTION_PAGE]->set_sensitive(has_pages);

    if (current_key == SELECTION_PAGE && !has_pages) {
        current_key = SELECTION_LAYER;
        selection_buttons[SELECTION_LAYER]->set_active();
    }

    queueRefresh();
}

// Setup Single Export.Called by export on realize
void BatchExport::setup()
{
    if (setupDone) {
        return;
    }
    setupDone = true;

    export_list.setup();

    // set them before connecting to signals
    setDefaultSelectionMode();
    setExporting(false);
    queueRefresh(true);

    // Connect Signals
    for (auto [key, button] : selection_buttons) {
        button->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &BatchExport::onAreaTypeToggle), key));
    }
    show_preview.signal_toggled().connect(sigc::mem_fun(*this, &BatchExport::refreshPreview));
    export_conn = export_btn.signal_clicked().connect(sigc::mem_fun(*this, &BatchExport::onExport));
    cancel_conn = cancel_btn.signal_clicked().connect(sigc::mem_fun(*this, &BatchExport::onCancel));
    hide_all.signal_toggled().connect(sigc::mem_fun(*this, &BatchExport::refreshItems));
    _background_color.connectChanged([=, this](Colors::Color const &color){
        if (_desktop) {
            Inkscape::UI::Dialog::set_export_bg_color(_desktop->getNamedView(), color);
        }
        refreshPreview();
    });
}

void BatchExport::refreshItems()
{
    if (!_desktop || !_document) return;

    // Create New List of Items
    std::map<std::string, SPObject*> objects;

    bool isolate = false;
    char *num_str = nullptr;
    switch (current_key) {
        case SELECTION_SELECTION: {
            isolate = hide_all.get_active();
            for (auto item : _desktop->getSelection()->items()) {
                // Ignore empty items (empty groups, other bad items)
                if (item && item->visualBounds() && item->getId()) {
                    objects[item->getId()] = item;
                }
            }
            num_str = g_strdup_printf(ngettext("%d Item", "%d Items", objects.size()), (int)objects.size());
            break;
        }
        case SELECTION_LAYER: {
            isolate = true;
            for (auto layer : _desktop->layerManager().getAllLayers()) {
                // Ignore empty layers, they have no size.
                if (layer->geometricBounds() && layer->getId()) {
                    objects[layer->getId()] = layer;
                }
            }
            num_str = g_strdup_printf(ngettext("%d Layer", "%d Layers", objects.size()), (int)objects.size());
            break;
        }
        case SELECTION_PAGE: {
            for (auto page : _desktop->getDocument()->getPageManager().getPages()) {
                if (auto id = page->getId()) {
                    objects[id] = page;
                }
            }
            num_str = g_strdup_printf(ngettext("%d Page", "%d Pages", objects.size()), (int)objects.size());
            break;
        }
        default:
            break;
    }
    if (num_str) {
        num_elements.set_text(num_str);
        g_free(num_str);
    }

    BatchItem::syncItems(current_items, objects, preview_container, _preview_drawing, isolate);

    refreshPreview();
}


void BatchExport::refreshPreview()
{
    if (!_desktop) return;

    // For Batch Export we are now hiding all object except current object
    bool hide = hide_all.get_active();
    bool preview = show_preview.get_active();

    if (preview) {
        std::vector<SPItem const *> selected;
        if (hide) {
            auto sels = _desktop->getSelection()->items();
            selected = {sels.begin(), sels.end()};
        }
        _preview_drawing->set_shown_items(std::move(selected));
    }
    for (auto &[key, val] : current_items) {
        val->refresh(!preview, _background_color.get_current_color().toRGBA());
    }
}
/**
 * Get the currently selected batch path.
 * If it is not set, fall back to the last used one (see getPreviousBatchPath()).
 *
 * @returns Path, UTF8 encoded.
 */
std::optional<Glib::RefPtr<Gio::File const>> BatchExport::getBatchPath() const
{
    if (export_path.has_value()) {
        return export_path;
    }
    return getPreviousBatchPath();
}

/**
 * Get the last used batch path for the document:
 *
 * @returns One of:
 *   1. An absolute path in the document's export-batch-path attribute
 *   2. An absolute path in the preference /dialogs/export/batch/path
 *   3. A relative attribute path from the document's location
 *   4. A relative preferences path from the document's location
 *   5. The document's location
 *   6. Empty string.
 */
std::optional<Glib::RefPtr<Gio::File const>> BatchExport::getPreviousBatchPath() const
{
    auto path = prefs->getString("/dialogs/export/batch/path");
    if (auto attr = _document->getRoot()->getAttribute("inkscape:export-batch-path")) {
        path = attr;
    }
    if (!path.empty() && Glib::path_is_absolute(Glib::filename_from_utf8(path))) {
        return Gio::File::create_for_parse_name(path);
    }

    if (Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        // With a sandboxed filesystem, automatically determined paths typically won't work.
        // We give up instead of guessing some relative paths.
        return std::nullopt;
    }

    // Relative to the document's position
    // TODO: it is unclear which encoding `getDocumentFilename()` has. We assume it is platform-native.
    if (const char *doc_filename = _document->getDocumentFilename()) {
        auto doc_path = Glib::path_get_dirname(doc_filename);

        if (!path.empty()) {
            return Gio::File::create_for_path(Glib::canonicalize_filename(path.raw(), doc_path));
        }
        return Gio::File::create_for_path(doc_path);
    }
    return std::nullopt;
}

/**
 * Set batch export folder.
 * @param path Folder path.
 */
void BatchExport::setBatchPath(std::optional<Glib::RefPtr<Gio::File const>> path)
{
    export_path = path;
    Glib::ustring path_utf8 = "";
    if (path.has_value()) {
        path_utf8 = path.value()->get_parse_name();
    }

    Glib::ustring path_label = Inkscape::IO::Sandbox::filesystem_get_display_path(export_path, _("Choose folder..."));

    if (!Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        // We have direct access to the filesystem.
        // Clean up the path (convert to relative directory).
        // Show this path to the user.
        if (const char *doc_filename = _document->getDocumentFilename()) {
            auto doc_path = Glib::path_get_dirname(doc_filename);
            path_utf8 = Inkscape::optimizePath(path_utf8, doc_path, 2);
            path_label = path_utf8;
        }
    }
    prefs->setString("/dialogs/export/batch/path", path_utf8);

    path_chooser.set_label(path_label);
}

/**
 * Get the last used batch base name for the document:
 *
 * @returns either
 *   1. The name stored in the document's export-batch-name attribute
 *   2. The document's basename stripped of its extension
 */
Glib::ustring BatchExport::getBatchName(bool fallback) const
{
    if (auto attr = _document->getRoot()->getAttribute("inkscape:export-batch-name")) {
        return attr;
    } else if (!fallback)
        return "";
    if (const char *doc_filename = _document->getDocumentFilename()) {
        std::string name = Glib::path_get_basename(doc_filename);
        Inkscape::IO::remove_file_extension(name);
        return name;
    }
    return "batch";
}

void BatchExport::setBatchName(Glib::ustring const &name)
{
    _document->getRoot()->setAttribute("inkscape:export-batch-name", name);
}

void BatchExport::loadExportHints(bool rename_file)
{
    if (!_desktop) return;

    // update labels
    setBatchPath(getBatchPath());

    if (name_text.get_text().empty()) {
        auto const name = getBatchName(rename_file);
        name_text.set_text(name);
        name_text.set_position(name.length());
    }
}

void BatchExport::pickBatchPath()
{
    auto dialog = Gtk::FileDialog::create();
    dialog->select_folder(dynamic_cast<Gtk::Window &>(*get_root()), sigc::track_object([&dialog = *dialog, this] (auto &result) {
        try {
            if (auto old_file = dialog.select_folder_finish(result)) {
                setBatchPath(old_file);
                return;
            }
        } catch (Gtk::DialogError const &) {
        }
    }, *this), {});
}

// Signals CallBack

void BatchExport::onAreaTypeToggle(selection_mode key)
{
    // Prevent executing function twice
    if (!selection_buttons[key]->get_active()) {
        return;
    }
    // If you have reached here means the current key is active one ( not sure if multiple transitions happen but
    // last call will change values)
    current_key = key;
    prefs->setString("/dialogs/export/batchexportarea/value", selection_names[current_key]);

    queueRefresh();
}

void BatchExport::onCancel()
{
    interrupted = true;
    setExporting(false);
}

void BatchExport::onExport()
{
    interrupted = false;
    if (!_desktop)
        return;

    // If there are no selected button, simply flash message in status bar
    int num = current_items.size();
    if (current_items.size() == 0) {
        _desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("No items selected."));
        return;
    }

    setExporting(true);
    auto finish = scope_exit([&] { setExporting(false); });

    // create vector of exports
    int num_rows = export_list.get_rows();
    std::vector<Glib::ustring> suffixs;
    std::vector<Inkscape::Extension::Output *> extensions;
    std::vector<double> dpis;
    for (int i = 0; i < num_rows; i++) {
        suffixs.emplace_back(export_list.get_suffix(i));
        extensions.push_back(export_list.getExtension(i));
        dpis.push_back(export_list.get_dpi(i));
    }

    // Resolve before any row (including raster rows) writes or changes saved batch options.
    auto links = Extension::MissingLinkAction::RequirePresent;
    bool const needs_pictures = std::any_of(extensions.begin(), extensions.end(), [](auto *ext) {
        return ext && !ext->deactivated() && !ext->is_raster() && ext->needs_picture_bytes();
    });
    if (needs_pictures && !sp_images_missing(_document).empty()) {
        links = resolveMissingLinksForSave(_desktop, _document);
        if (links == Extension::MissingLinkAction::Cancel) {
            interrupted = true;
            return;
        }
        // Accepted discards may have removed selected batch items or emptied their groups.
        refreshItems();
        num = current_items.size();
        if (!num) return;
    }


    auto path = getBatchPath();
    if (!path.has_value()) {
        return;
    }
    std::string name = name_text.get_text();

    if (path.value()->query_file_type() != Gio::FileType::DIRECTORY) {
        auto const window = _desktop->getInkscapeWindow();
        if (path.value()->query_exists()) {
            auto dialog = Gtk::MessageDialog(*window, _("Can not save to a directory that is actually a file."), true, Gtk::MessageType::ERROR, Gtk::ButtonsType::OK);
            UI::dialog_run(dialog);
            return;
        }
        Glib::ustring message = g_markup_printf_escaped(
            _("<span weight=\"bold\" size=\"larger\">Directory \"%s\" doesn't exist. Create it now?</span>"),
            path.value()->get_parse_name().c_str());

        auto dialog = Gtk::MessageDialog(*window, message, true, Gtk::MessageType::WARNING, Gtk::ButtonsType::YES_NO);
        if (UI::dialog_run(dialog) != Gtk::ResponseType::YES) {
            return;
        }
        path.value()->dup()->make_directory_with_parents();
    }

    setBatchPath(path);
    setBatchName(name);

    bool ow = overwrite.get_active();
    bool hide = hide_all.get_active();
    auto sels = _desktop->getSelection()->items();
    std::vector<SPItem const *> selected_items(sels.begin(), sels.end());

    // Start Exporting Each Item
    for (int j = 0; j < num_rows && !interrupted; j++) {

        auto suffix = export_list.get_suffix(j);
        auto ext = export_list.getExtension(j);
        float dpi = export_list.get_dpi(j);

        if (!ext || ext->deactivated()) {
            continue;
        }

        int count = 0;
        for (auto i = current_items.begin(); i != current_items.end() && !interrupted; ++i) {
            count++;

            auto &batchItem = i->second;
            if (!batchItem->is_selected()) {
                continue;
            }

            SPItem *item = batchItem->getItem();
            SPPage *page = batchItem->getPage();
            bool isolate_item = batchItem->isolateItem();

            std::vector<SPItem const *> show_only;
            Geom::Rect area;
            if (item) {
                if (auto bounds = item->documentVisualBounds()) {
                    area = *bounds;
                } else {
                    continue;
                }
                if (hide) {
                    for (auto &sel_item : selected_items) {
                        // Layers want their descendants, selections want themselves
                        if (item->isAncestorOf(sel_item) || sel_item == item) {
                            show_only.emplace_back(sel_item);
                        }
                    }
                    if (show_only.empty())
                        continue; // Nothing to export
                } else if (isolate_item) {
                    // Layers are isolated even when they aren't hiding other items
                    show_only.emplace_back(item);
                }
            } else if (page) {
                area = page->getDocumentRect();
                if (hide)
                    show_only = selected_items;
            } else {
                continue;
            }

            std::string id = Glib::filename_from_utf8(batchItem->getLabel());
            if (id.empty()) {
                continue;
            }

            std::string item_name = name;
            if (!name.empty()) {
                std::string::value_type last_char = name.at(name.length() - 1);
                if (last_char != '/' && last_char != '\\') {
                    item_name += "_";
                }
            }
            if (id.at(0) == '#' && batchItem->getItem() && !batchItem->getItem()->label()) {
                item_name += id.substr(1);
            } else {
                item_name += id;
            }

            if (!suffix.empty()) {
                if (ext->is_raster()) {
                    // Put the dpi in at the user's requested location.
                    suffix = std::regex_replace(suffix.c_str(), std::regex("\\{dpi\\}"), std::to_string((int)dpi));
                }
                item_name += "_" + suffix;
            }

            if (item_name.empty()) {
                g_error("Empty item name in batch export, refusing to export.");
                continue;
            }

            // Add the path last so item_name has a chance to be filled without path confusion
            /// Filename for export item, in platform-native encoding.
            std::string item_filename = Glib::build_filename(path.value()->get_path(), item_name);
            if (!ow) {
                if (!Export::unConflictFilename(_document, item_filename, ext->get_extension())) {
                    continue;
                }
            } else {
                item_filename += ext->get_extension();
            }
            auto item_file = Gio::File::create_for_path(item_filename);
            auto item_filename_utf8 = Glib::filename_to_utf8(item_filename);
            auto item_filename_label = Inkscape::IO::Sandbox::filesystem_get_display_path(item_file);

            // Set the progress bar with our updated information
            double progress = (((double)count / num) + j) / num_rows;
            _prog_batch.set_fraction(progress);

            setExporting(true, Glib::ustring::compose(_("Exporting %1"), item_filename_label),
                         Glib::ustring::compose(_("Format %1, Selection %2"), j + 1, count));

            if (ext->is_raster()) {
                unsigned long int width = (int)(area.width() * dpi / DPI_BASE + 0.5);
                unsigned long int height = (int)(area.height() * dpi / DPI_BASE + 0.5);

                Export::exportRaster(area, width, height, dpi, _background_color.get_current_color(),
                                     item_filename_utf8, true, onProgressCallback, this, ext, &show_only);
            } else if (page || !show_only.empty()) {
                auto copy_doc = _document->copy();
                Export::exportVector(ext, copy_doc.get(), item_filename_utf8, true, show_only, page, links);
            } else {
                auto copy_doc = _document->copy();
                Export::exportVector(ext, copy_doc.get(), item_filename_utf8, true, area, links);
            }
        }
    }
    // Save the export batch path only on successful export
    _document->getRoot()->setAttribute("inkscape:export-batch-path", path.value()->get_parse_name());
    DocumentUndo::done(_document, RC_("Undo", "Set Batch Export Options"), INKSCAPE_ICON("export"));
    // The scope guard also restores the controls and progress display on early exits.
}

void BatchExport::setDefaultSelectionMode()
{
    current_key = (selection_mode)0; // default key
    bool found = false;
    Glib::ustring pref_key_name = prefs->getString("/dialogs/export/batchexportarea/value");
    for (auto [key, name] : selection_names) {
        if (pref_key_name == name) {
            current_key = key;
            found = true;
            break;
        }
    }
    if (!found) {
        pref_key_name = selection_names[current_key];
    }
    if (_desktop) {
        if (auto _sel = _desktop->getSelection()) {
            selection_buttons[SELECTION_SELECTION]->set_sensitive(!_sel->isEmpty());
        }
        bool has_pages = _document->getPageManager().hasPages();
        selection_buttons[SELECTION_PAGE]->set_sensitive(has_pages);
    }
    if (!selection_buttons[current_key]->get_sensitive()) {
        current_key = SELECTION_LAYER;
    }
    selection_buttons[current_key]->set_active(true);

    // we need to set pref key because signals above will set set pref == current key but we sometimes change
    // current key like selection key
    prefs->setString("/dialogs/export/batchexportarea/value", pref_key_name);
}

void BatchExport::setExporting(bool exporting, Glib::ustring const &text, Glib::ustring const &text_batch)
{
    if (exporting) {
        set_sensitive(false);
        set_opacity(0.2);
        progress_box.set_visible(true);
        _prog.set_text(text);
        _prog.set_fraction(0.0);
        _prog_batch.set_text(text_batch);
    } else {
        set_sensitive(true);
        set_opacity(1.0);
        progress_box.set_visible(false);
        _prog.set_text("");
        _prog.set_fraction(0.0);
        _prog_batch.set_text("");
    }
}

unsigned int BatchExport::onProgressCallback(float value, void *data)
{
    if (auto bi = static_cast<BatchExport *>(data)) {
        bi->_prog.set_fraction(value);
        auto main_context = Glib::MainContext::get_default();
        main_context->iteration(false);
        return !bi->interrupted;
    }
    return false;
}

void BatchExport::setDesktop(SPDesktop *desktop)
{
    if (desktop != _desktop) {
        _pages_changed_connection.disconnect();
        _desktop = desktop;
    }
}

void BatchExport::setDocument(SPDocument *document)
{
    if (!_desktop) {
        document = nullptr;
    }
    if (_document == document)
        return;

    _document = document;
    _pages_changed_connection.disconnect();
    if (document) {
        // when the page selected is changed, update the export area
        _pages_changed_connection = document->getPageManager().connectPagesChanged([this](SPPage *) { pagesChanged(); });
        _background_color.setColor(get_export_bg_color(document->getNamedView(), Colors::Color(0xffffff00)));
        pagesChanged();

        _preview_drawing = std::make_shared<PreviewDrawing>(document);
    } else {
        _preview_drawing.reset();
    }

    name_text.set_text("");
    path_chooser.set_label("");
    refreshItems();
}

void BatchExport::queueRefreshItems()
{
    if (refresh_items_conn) {
        return;
    }
    // Asynchronously refresh the preview
    refresh_items_conn = Glib::signal_idle().connect([this] {
        refreshItems();
        return false;
    }, Glib::PRIORITY_HIGH);
}

void BatchExport::queueRefresh(bool rename_file)
{
    if (refresh_conn) {
        return;
    }
    refresh_conn = Glib::signal_idle().connect([this, rename_file] {
        refreshItems();
        loadExportHints(rename_file);
        return false;
    }, Glib::PRIORITY_HIGH);
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
