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

#include "export-single.h"

#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/messagedialog.h>
#include <glibmm/fileutils.h>
#include <cmath>
#include <gtkmm/flowbox.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/recentmanager.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <png.h>

#include "desktop.h"
#include "document-undo.h"
#include "extension/output.h"
#include "inkscape-window.h"
#include "io/sandbox.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog/export.h"
#include "ui/dialog/export-page-plan.h"
#include "ui/dialog-run.h"
#include "ui/icon-names.h"
#include "ui/util.h"
#include "ui/widget/color-picker.h"
#include "ui/widget/export-lists.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-menu.h"

using Inkscape::Util::UnitTable;
using Inkscape::UI::Widget::UnitMenu;

namespace Inkscape::UI::Dialog {

SingleExport::SingleExport(BaseObjectType *cobject, const Glib::RefPtr<Gtk::Builder> &builder)
    : Gtk::Box(cobject)
    , pages_list        (get_widget<Gtk::FlowBox>         (builder, "si_pages"))
    , pages_list_box    (get_widget<Gtk::ScrolledWindow>  (builder, "si_pages_box"))
    , size_box          (get_widget<Gtk::Grid>            (builder, "si_sizes"))
    , units             (get_derived_widget<UnitMenu>     (builder, "si_units"))
    , si_units_row      (get_widget<Gtk::Box>             (builder, "si_units_row"))
    , si_hide_all       (get_widget<Gtk::CheckButton>     (builder, "si_hide_all"))
    , si_show_preview   (get_widget<Gtk::CheckButton>     (builder, "si_show_preview"))
    , preview           (get_derived_widget<ExportPreview>(builder, "si_preview"))
    , preview_box       (get_widget<Gtk::Box>             (builder, "si_preview_box"))

    , si_extension_cb   (get_derived_widget<ExtensionList>(builder, "si_extention"))
    , si_filename_entry (get_widget<Gtk::Entry>           (builder, "si_filename"))
    , si_filename_button(get_widget<Gtk::Button>          (builder, "si_filename_button"))
    , si_export         (get_widget<Gtk::Button>          (builder, "si_export"))

    , progress_bar      (get_widget<Gtk::ProgressBar>     (builder, "si_progress"))
    , cancel_button     (get_widget<Gtk::Button>          (builder, "si_cancel"))
    , progress_box      (get_widget<Gtk::Box>             (builder, "si_inprogress"))
    , _background_color (get_derived_widget<UI::Widget::ColorPicker>(builder, "si_backgnd", _("Background color"), true))
{
    prefs = Inkscape::Preferences::get();

    selection_names[SELECTION_DRAWING] = "drawing";
    selection_names[SELECTION_PAGE] = "page";
    selection_names[SELECTION_SELECTION] = "selection";
    selection_names[SELECTION_CUSTOM] = "custom";

    selection_buttons[SELECTION_DRAWING]   = &get_widget<Gtk::ToggleButton>(builder, "si_s_document");
    selection_buttons[SELECTION_PAGE]      = &get_widget<Gtk::ToggleButton>(builder, "si_s_page");
    selection_buttons[SELECTION_SELECTION] = &get_widget<Gtk::ToggleButton>(builder, "si_s_selection");
    selection_buttons[SELECTION_CUSTOM]    = &get_widget<Gtk::ToggleButton>(builder, "si_s_custom");

    spin_buttons[SPIN_X0]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_left_sb");
    spin_buttons[SPIN_X1]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_right_sb");
    spin_buttons[SPIN_Y0]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_top_sb");
    spin_buttons[SPIN_Y1]       = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_bottom_sb");
    spin_buttons[SPIN_HEIGHT]   = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_height_sb");
    spin_buttons[SPIN_WIDTH]    = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_width_sb");
    spin_buttons[SPIN_BMHEIGHT] = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_img_height_sb");
    spin_buttons[SPIN_BMWIDTH]  = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_img_width_sb");
    spin_buttons[SPIN_DPI]      = &get_derived_widget<UI::Widget::SpinButton>(builder, "si_dpi_sb");

    spin_labels[SPIN_X0]        = &get_widget<Gtk::Label>(builder, "si_label_left");
    spin_labels[SPIN_X1]        = &get_widget<Gtk::Label>(builder, "si_label_right");
    spin_labels[SPIN_Y0]        = &get_widget<Gtk::Label>(builder, "si_label_top");
    spin_labels[SPIN_Y1]        = &get_widget<Gtk::Label>(builder, "si_label_bottom");
    spin_labels[SPIN_HEIGHT]    = &get_widget<Gtk::Label>(builder, "si_label_height");
    spin_labels[SPIN_WIDTH]     = &get_widget<Gtk::Label>(builder, "si_label_width");

    auto &pref_button_box = get_widget<Gtk::Box>(builder, "si_prefs");
    auto &pref_button = *si_extension_cb.getPrefButton();
    pref_button_box.append(pref_button);
    pref_button.set_expand(false);
    pref_button_box.set_expand(false);
    pref_button.set_valign(Gtk::Align::BASELINE_CENTER);
    pref_button_box.set_valign(Gtk::Align::BASELINE_CENTER);

    setup();
}

// Inkscape Selection Modified CallBack
void SingleExport::selectionModified(Inkscape::Selection *selection, guint flags)
{
    if (!_desktop || _desktop->getSelection() != selection) {
        return;
    }
    if (!(flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_PARENT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
        return;
    }
    refreshArea();
    // Do not load export hits for modifications
}

void SingleExport::selectionChanged(Inkscape::Selection *selection)
{
    if (!_desktop || _desktop->getSelection() != selection) {
        return;
    }

    Glib::ustring pref_key_name = prefs->getString("/dialogs/export/exportarea/value");
    for (auto [key, name] : selection_names) {
        if (name == pref_key_name && current_key != key && key != SELECTION_SELECTION) {
            selection_buttons[key]->set_active(true);
            current_key = key;
            break;
        }
    }
    if (selection->isEmpty()) {
        selection_buttons[SELECTION_SELECTION]->set_sensitive(false);
        if (current_key == SELECTION_SELECTION) {
            selection_buttons[(selection_mode)0]->set_active(true); // This causes refresh area
            // even though we are at default key, selection is the one which was original key.
            prefs->setString("/dialogs/export/exportarea/value", selection_names[SELECTION_SELECTION]);
            // return otherwise refreshArea will be called again
            return;
        }
    } else {
        selection_buttons[SELECTION_SELECTION]->set_sensitive(true);
        if (selection_names[SELECTION_SELECTION] == pref_key_name && current_key != SELECTION_SELECTION) {
            selection_buttons[SELECTION_SELECTION]->set_active();
            return;
        }
    }

    refreshArea();
    loadExportHints();
}

// Setup Single Export.Called by export on realize
void SingleExport::setup()
{
    if (setupDone) {
        // We need to setup only once
        return;
    }
    setupDone = true;

    si_extension_cb.setup();

    setupUnits();
    setupSpinButtons();

    // set them before connecting to signals
    setDefaultSelectionMode();
    setPagesMode(true);
    setExporting(false);

    // Make the filename entry box read-only when the filesystem is sandboxed.
    // "Sandboxed" means that the user can't access arbitrary paths but only those
    // that come from the file chooser.
    if (Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        si_filename_entry.set_editable(false);
        si_filename_entry.set_can_focus(false);
        si_filename_entry.set_has_frame(false);
    }

    // Refresh the filename when the user selects a different page
    _pages_list_changed = pages_list.signal_selected_children_changed().connect([this]() {
        loadExportHints();
        refreshArea();
        refreshPage();
    });

    // Connect Signals Here
    for (auto [key, button] : selection_buttons) {
        button->signal_toggled().connect(sigc::bind(sigc::mem_fun(*this, &SingleExport::onAreaTypeToggle), key));
    }
    units.signal_changed().connect(sigc::mem_fun(*this, &SingleExport::onUnitChanged));
    extensionConn = si_extension_cb.signal_changed().connect(sigc::mem_fun(*this, &SingleExport::onExtensionChanged));
    exportConn = si_export.signal_clicked().connect(sigc::mem_fun(*this, &SingleExport::onExport));
    filenameConn = si_filename_entry.signal_changed().connect(sigc::mem_fun(*this, &SingleExport::onFilenameModified));
    cancelConn = cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &SingleExport::onCancel));
    si_filename_entry.signal_activate().connect(sigc::mem_fun(*this, &SingleExport::onExport));
    browseConn = si_filename_button.signal_clicked().connect(sigc::mem_fun(*this, &SingleExport::onBrowse));
    si_show_preview.signal_toggled().connect(sigc::mem_fun(*this, &SingleExport::refreshPreview));
    si_hide_all.signal_toggled().connect(sigc::mem_fun(*this, &SingleExport::refreshPreview));
    _background_color.connectChanged([=, this](Colors::Color const &color){
        if (_desktop) {
            Inkscape::UI::Dialog::set_export_bg_color(_desktop->getNamedView(), color);
        }
        refreshPreview();
    });
}

// Setup units combobox
void SingleExport::setupUnits()
{
    units.setUnitType(Inkscape::Util::UNIT_TYPE_LINEAR);
    if (_desktop) {
        units.setUnit(_desktop->getNamedView()->display_units->abbr);
    }
}

// Create all spin buttons
void SingleExport::setupSpinButtons()
{
    setupSpinButton<sb_type>(spin_buttons[SPIN_X0], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaXChange, SPIN_X0);
    setupSpinButton<sb_type>(spin_buttons[SPIN_X1], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaXChange, SPIN_X1);
    setupSpinButton<sb_type>(spin_buttons[SPIN_Y0], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaYChange, SPIN_Y0);
    setupSpinButton<sb_type>(spin_buttons[SPIN_Y1], 0.0, -1000000.0, 1000000.0, 0.1, 1.0, EXPORT_COORD_PRECISION, true,
                             &SingleExport::onAreaYChange, SPIN_Y1);

    setupSpinButton<sb_type>(spin_buttons[SPIN_HEIGHT], 0.0, 0.0, PNG_UINT_31_MAX, 0.1, 1.0, EXPORT_COORD_PRECISION,
                             true, &SingleExport::onAreaYChange, SPIN_HEIGHT);
    setupSpinButton<sb_type>(spin_buttons[SPIN_WIDTH], 0.0, 0.0, PNG_UINT_31_MAX, 0.1, 1.0, EXPORT_COORD_PRECISION,
                             true, &SingleExport::onAreaXChange, SPIN_WIDTH);

    setupSpinButton<sb_type>(spin_buttons[SPIN_BMHEIGHT], 1.0, 1.0, 1000000.0, 1.0, 10.0, 0, true,
                             &SingleExport::onDpiChange, SPIN_BMHEIGHT);
    setupSpinButton<sb_type>(spin_buttons[SPIN_BMWIDTH], 1.0, 1.0, 1000000.0, 1.0, 10.0, 0, true,
                             &SingleExport::onDpiChange, SPIN_BMWIDTH);
    setupSpinButton<sb_type>(spin_buttons[SPIN_DPI], prefs->getDouble("/dialogs/export/defaultxdpi/value", DPI_BASE),
                             1.0, 100000.0, 0.1, 1.0, 2, true, &SingleExport::onDpiChange, SPIN_DPI);
}

template <typename T>
void SingleExport::setupSpinButton(UI::Widget::SpinButton *sb, double val, double min, double max, double step, double page,
                                   int digits, bool sensitive, void (SingleExport::*cb)(T), T param)
{
    if (sb) {
        sb->set_digits(digits);
        sb->set_increments(step, page);
        sb->set_range(min, max);
        sb->set_value(val);
        sb->set_sensitive(sensitive);
        if (cb) {
            auto signal = sb->signal_value_changed().connect(sigc::bind(sigc::mem_fun(*this, cb), param));
            // add signals to list to block all easily
            spinButtonConns.push_back(signal);
        }
    }
}

void SingleExport::refreshArea()
{
    if (_document) {
        Geom::OptRect bbox;
        auto sel = getSelectedPages();

        switch (current_key) {
            case SELECTION_SELECTION:
                if ((_desktop->getSelection())->isEmpty() == false) {
                    bbox = _desktop->getSelection()->visualBounds();
                    break;
                }
            case SELECTION_DRAWING:
                bbox = _document->getRoot()->desktopVisualBounds();
                if (bbox) {
                    break;
                }
            case SELECTION_PAGE:
                // If the page is set in the multi-selection use that.
                if (sel.size() == 1 || (current_key == SELECTION_PAGE && !sel.empty() &&
                    si_extension_cb.getExtension() && si_extension_cb.getExtension()->is_raster())) {
                    bbox = sel[0]->getDesktopRect();
                } else {
                    bbox = _document->getPageManager().getSelectedPageRect();
                }
                break;
            case SELECTION_CUSTOM:
                break;
            default:
                break;
        }
        if (current_key != SELECTION_CUSTOM && bbox) {
            setArea(bbox->min()[Geom::X], bbox->min()[Geom::Y], bbox->max()[Geom::X], bbox->max()[Geom::Y]);
        }
    }
    refreshPreview();
}

void SingleExport::refreshPage()
{
    if (!_document)
        return;

    bool multi = pages_list.get_selection_mode() == Gtk::SelectionMode::MULTIPLE;
    auto &pm = _document->getPageManager();
    bool has_pages = current_key == SELECTION_PAGE && pm.getPageCount() > 1;
    pages_list_box.set_visible(has_pages);
    preview_box.set_visible(!has_pages);
    auto const ext = si_extension_cb.getExtension();
    bool const raster = ext && ext->is_raster();
    size_box.set_visible(!has_pages || !multi || raster);
    size_box.set_tooltip_text(has_pages && raster && getSelectedPages().size() > 1 ?
        _("Width and height show the first ticked page at this DPI. Each page is exported at its own size.") : "");
}

void SingleExport::setPagesMode(bool multi)
{
    // Set set the internal mode to NONE to preserve selections while changing
    for (auto &widget : children(pages_list)) {
        if (auto item = dynamic_cast<BatchItem *>(&widget)) {
            item->on_mode_changed(Gtk::SelectionMode::NONE);
        }
    }
    pages_list.set_selection_mode(multi ? Gtk::SelectionMode::MULTIPLE : Gtk::SelectionMode::SINGLE);
    // A second call is needed in its own loop because of how updates happen in the FlowBox
    for (auto &widget : children(pages_list)) {
        if (auto item = dynamic_cast<BatchItem *>(&widget)) {
            item->update_selected();
        }
    }
    refreshPage();
}

void SingleExport::selectPage(SPPage *page)
{
    for (auto &widget : children(pages_list)) {
        if (auto item = dynamic_cast<BatchItem *>(&widget)) {
            if (item->getPage() == page) {
                item->set_selected(true);
            }
        }
    }
}

std::vector<SPPage const *> SingleExport::getSelectedPages() const
{
    std::vector<SPPage const *> pages;
    pages_list.selected_foreach([&pages](Gtk::FlowBox *box, Gtk::FlowBoxChild *child) {
        if (auto item = dynamic_cast<BatchItem *>(child))
            pages.push_back(item->getPage());
    });
    return pages;
}

void SingleExport::onPagesChanged(SPPage *new_page)
{
    std::map<std::string, SPObject*> itemsList;

    if (_document) {
        auto &pm = _document->getPageManager();
        if (pm.getPageCount() > 1) {
            for (auto page : pm.getPages()) {
                if (auto id = page->getId()) {
                    itemsList[id] = page;
                }
            }
        }
    }

    _pages_list_changed.block();
    BatchItem::syncItems(current_items, itemsList, pages_list, _preview_drawing, false);
    refreshPage();
    if (auto ext = si_extension_cb.getExtension()) {
        setPagesMode(true);
    }
    _pages_list_changed.unblock();
    refreshArea();
}

void SingleExport::onPagesModified(SPPage *page)
{
    refreshArea();
}

void SingleExport::onPagesSelected(SPPage *page) {
    if (pages_list.get_selection_mode() != Gtk::SelectionMode::MULTIPLE) {
        selectPage(page);
    }
    refreshArea();
}

/**
 * Update suggested DPI and filename when the selection has changed.
 */
void SingleExport::loadExportHints()
{
    if (!_document || !_desktop)
        return;

    /// Current file path (before the new suggested filename is determined). Value is in platform-native encoding.
    std::string old_raw_filepath = filepath_native;
    /// Suggested new file path. Value is in platform-native encoding.
    std::string filename;
    Geom::Point dpi;
    switch (current_key) {
        case SELECTION_PAGE:
        {
            auto pages = getSelectedPages();
            if (pages.size() == 1) {
                dpi = pages[0]->getExportDpi();

                auto page_filename = pages[0]->getExportFilename();
                if (page_filename.empty()) {
                    page_filename = pages[0]->getLabel();
                }

                filename =
                    Export::prependDirectory(Glib::filename_from_utf8(page_filename), old_raw_filepath, _document);

                break;
            }
            // No or many pages means output is drawing, continue.
        }
        case SELECTION_CUSTOM:
        case SELECTION_DRAWING:
        {
            dpi = _document->getRoot()->getExportDpi();
            filename = Export::prependDirectory(Glib::filename_from_utf8(_document->getRoot()->getExportFilename()),
                                                old_raw_filepath, _document);
            break;
        }
        case SELECTION_SELECTION:
        {
            auto selection = _desktop->getSelection();
            if (selection->isEmpty()) break;

            // Get filename and dpi from selected items
            for (auto item : selection->items()) {
                if (!dpi.x()) {
                    dpi = item->getExportDpi();
                }
                if (filename.empty()) {
                    filename = Export::prependDirectory(Glib::filename_from_utf8(item->getExportFilename()),
                                                        old_raw_filepath, _document);
                }
            }

            if (filename.empty()) {
                filename = Export::filePathFromObject(_document, selection->firstItem(), old_raw_filepath);
            }
            break;
        }
        default:
            break;
    }
    if (filename.empty()) {
        filename = old_raw_filepath;
        si_extension_cb.removeExtension(filename);
        filename = Export::defaultFilename(_document, filename, ".png");
    }
    if (auto ext = si_extension_cb.getExtension()) {
        si_extension_cb.removeExtension(filename);
        ext->add_extension(filename);
    }

    if (!filename_modified_by_user) {
        // If the filename was NOT manually modified by the user
        // since the last export, then update the filename edit box
        // and internal filename value to the new suggested filename.
        setFilename(filename, false);
    }

    if (dpi.x() != 0.0) { // XXX Should this deal with dpi.y() ?
        spin_buttons[SPIN_DPI]->set_value(dpi.x());
    }
}

/**
 * Set filename and update filename entry box.
 *
 * The value of `filename_modified_by_user` will be updated.
 *
 * @param filename
 *     Raw file path.
 *     Value is in platform-native encoding (see Glib::filename_to_utf8).
 * @param is_user_input
 *     True if the new filename comes from user input (by file chooser, editing the text box, or by pushing the Export
 * button). False if the new value is auto-generated.
 */
void SingleExport::setFilename(std::string filename, bool is_user_input)
{
    // Update modification status:
    if (filepath_native != filename) {
        // Filename was changed.
        // If it was changed based on user input, then set the "modified by user" flag.
        // Else, reset the flag.
        filename_modified_by_user = is_user_input;
    }

    // Update filename:
    if (!is_user_input && Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        // With a sandboxed filesystem, autogenerated filenames don't work.
        // We can only use files coming from the file chooser dialog.
        // Therefore, if the filename changed and it is not from user input,
        // make the filename invalid to force that the user opens the file dialog again.
        if (filepath_native != filename) {
            filename = "";
        }
    }
    filepath_native = filename;

    // Determine filename label for filename entry box:
    Glib::ustring filename_label = Glib::filename_to_utf8(filename);
    if (Inkscape::IO::Sandbox::filesystem_is_sandboxed()) {
        // In a sandboxed filesystem, the file entry box is not editable.
        // We show a nice label instead of the raw path.
        filename_label = Inkscape::IO::Sandbox::filesystem_get_display_path(Gio::File::create_for_path(filename));
    }

    // Set value of filename entry box:
    if (si_filename_entry.get_text().raw() != filename_label.raw()) {
        auto old_block_state = filenameConn.blocked();
        filenameConn.block();
        si_filename_entry.set_text(filename_label);
        si_filename_entry.set_position(filename_label.length());
        filenameConn.block(old_block_state);
    }

    // Unconditionally update the original value to prevent it from becoming stale.
    filename_entry_original_value = filename_label;
}

void SingleExport::saveExportHints(SPObject *target)
{
    if (target) {
        target->setExportFilename(Glib::filename_to_utf8(filepath_native));
        target->setExportDpi(Geom::Point(
            spin_buttons[SPIN_DPI]->get_value(),
            spin_buttons[SPIN_DPI]->get_value()
        ));
    }
}

void SingleExport::setArea(double x0, double y0, double x1, double y1)
{
    blockSpinConns(true);

    Unit const *unit = units.getUnit();
    auto px = UnitTable::get().getUnit("px");
    spin_buttons[SPIN_X0]->get_adjustment()->set_value(px->convert(x0, unit));
    spin_buttons[SPIN_X1]->get_adjustment()->set_value(px->convert(x1, unit));
    spin_buttons[SPIN_Y0]->get_adjustment()->set_value(px->convert(y0, unit));
    spin_buttons[SPIN_Y1]->get_adjustment()->set_value(px->convert(y1, unit));

    areaXChange(SPIN_X1);
    areaYChange(SPIN_Y1);

    blockSpinConns(false);
}

// Signals CallBack

void SingleExport::onUnitChanged()
{
    refreshArea();
}

void SingleExport::onAreaTypeToggle(selection_mode key)
{
    // Prevent executing function twice
    if (!selection_buttons[key]->get_active()) {
        return;
    }
    // If you have reached here means the current key is active one ( not sure if multiple transitions happen but
    // last call will change values)
    current_key = key;
    prefs->setString("/dialogs/export/exportarea/value", selection_names[current_key]);

    refreshArea();
    loadExportHints();
    toggleSpinButtonVisibility();
    refreshPage();
}

void SingleExport::toggleSpinButtonVisibility()
{
    bool show = current_key == SELECTION_CUSTOM;
    spin_buttons[SPIN_X0]->set_visible(show);
    spin_buttons[SPIN_X1]->set_visible(show);
    spin_buttons[SPIN_Y0]->set_visible(show);
    spin_buttons[SPIN_Y1]->set_visible(show);
    spin_buttons[SPIN_WIDTH]->set_visible(show);
    spin_buttons[SPIN_HEIGHT]->set_visible(show);

    spin_labels[SPIN_X0]->set_visible(show);
    spin_labels[SPIN_X1]->set_visible(show);
    spin_labels[SPIN_Y0]->set_visible(show);
    spin_labels[SPIN_Y1]->set_visible(show);
    spin_labels[SPIN_WIDTH]->set_visible(show);
    spin_labels[SPIN_HEIGHT]->set_visible(show);

    si_units_row.set_visible(show);
}

void SingleExport::onAreaXChange(sb_type type)
{
    blockSpinConns(true);
    areaXChange(type);
    selection_buttons[SELECTION_CUSTOM]->set_active(true);
    refreshPreview();
    blockSpinConns(false);
}
void SingleExport::onAreaYChange(sb_type type)
{
    blockSpinConns(true);
    areaYChange(type);
    selection_buttons[SELECTION_CUSTOM]->set_active(true);
    refreshPreview();
    blockSpinConns(false);
}
void SingleExport::onDpiChange(sb_type type)
{
    blockSpinConns(true);
    dpiChange(type);
    blockSpinConns(false);
}

/**
 * Filename in filename entry field was changed.
 */
void SingleExport::onFilenameModified()
{
    extensionConn.block();

    Glib::ustring filename = si_filename_entry.get_text();
    if (filename_entry_original_value.raw() != filename.raw()) {
        // Textbox entry was changed
        setFilename(filename, true);
        si_extension_cb.setExtensionFromFilename(filename);
    }

    // This will not change the output extension if filename extension is same as previously
    // selected extension's filename extension.  In otherwords, selecting "SVG" in file dialog
    // won't override "Plain SVG" in export dialog.
    si_extension_cb.setExtensionFromFilename(filename);

    extensionConn.unblock();
}

void SingleExport::onExtensionChanged()
{
    if (auto ext = si_extension_cb.getExtension()) {
        setPagesMode(true);
        loadExportHints();
        refreshArea();
    }
}

void SingleExport::onCancel()
{
    interrupted = true;
    setExporting(false);
}

void SingleExport::onExport()
{
    interrupted = false;
    if (!_desktop || !_document)
        return;

    if (filepath_native.empty()) {
        // No file selected yet - call file chooser first
        onBrowse();
    }

    auto &page_manager = _document->getPageManager();
    auto selection = _desktop->getSelection();
    bool exportSuccessful = false;
    auto omod = si_extension_cb.getExtension();
    if (!omod) {
        std::cerr << "SingleExport::onExport(): Cannot find export extension!" << std::endl;
        return;
    }

    bool selected_only = si_hide_all.get_active();
    Unit const *unit = units.getUnit();

    if (!Export::checkOrCreateDirectory(filepath_native)) {
        return;
    }

    /// Label for displaying to user
    Glib::ustring filename_label =
        Inkscape::IO::Sandbox::filesystem_get_display_path(Gio::File::create_for_path(filepath_native));

    /// File path converted to UTF8
    Glib::ustring filename_utf8 = Glib::filename_to_utf8(filepath_native);

    setExporting(true, _("Exporting"));

    float x0 = unit->convert(spin_buttons[SPIN_X0]->get_value(), "px");
    float x1 = unit->convert(spin_buttons[SPIN_X1]->get_value(), "px");
    float y0 = unit->convert(spin_buttons[SPIN_Y0]->get_value(), "px");
    float y1 = unit->convert(spin_buttons[SPIN_Y1]->get_value(), "px");
    auto area = Geom::Rect(Geom::Point(x0, y0), Geom::Point(x1, y1)) * _desktop->dt2doc();

    auto const raster_pages = current_key == SELECTION_PAGE ? getSelectedPages() : std::vector<SPPage const *>{};
    if (omod->is_raster() && current_key == SELECTION_PAGE && page_manager.getPageCount() > 1 && raster_pages.empty()) {
        Gtk::MessageDialog dialog(*_desktop->getInkscapeWindow(), _("Select at least one page to export."),
                                  false, Gtk::MessageType::INFO, Gtk::ButtonsType::CLOSE);
        UI::dialog_run(dialog);
    } else if (omod->is_raster() && raster_pages.size() > 1) {
        std::vector<PageExportLabel> labels;
        for (auto *page : raster_pages) labels.push_back({page->getLabel(), page->getDefaultLabel()});
        auto plan = plan_page_exports(Export::absolutizePath(_document, filepath_native), omod->get_extension(), labels,
            [](auto const &file) { return Glib::file_test(file, Glib::FileTest::EXISTS); });
        bool replace = true;
        if (!plan.conflicts.empty()) {
            Gtk::MessageDialog dialog(*_desktop->getInkscapeWindow(), _("Replace existing files?"),
                                      false, Gtk::MessageType::QUESTION, Gtk::ButtonsType::NONE);
            std::string text;
            if (plan.conflicts.size() <= 10) {
                for (auto const &file : plan.conflicts) text += Glib::path_get_basename(file) + "\n";
            } else {
                text = Glib::ustring::compose(_("%1 files already exist.\n"), plan.conflicts.size());
            }
            dialog.set_secondary_text(text + _("They will be replaced. Cancel exports nothing."));
            dialog.add_button(_("_Cancel"), Gtk::ResponseType::CANCEL);
            dialog.add_button(_("_Replace"), Gtk::ResponseType::ACCEPT);
            dialog.set_default_response(Gtk::ResponseType::CANCEL);
            replace = UI::dialog_run(dialog) == Gtk::ResponseType::ACCEPT;
        }
        if (replace) {
            auto const dpi = float(spin_buttons[SPIN_DPI]->get_value());
            auto items = selection->items();
            std::vector<SPItem const *> selected(items.begin(), items.end());
            std::string failures;
            std::size_t written = 0;
            _raster_page_count = raster_pages.size();
            for (_raster_page_index = 0; _raster_page_index < _raster_page_count && !interrupted; ++_raster_page_index) {
                auto const i = _raster_page_index;
                auto *page = raster_pages[i];
                progress_bar.set_text(Glib::ustring::compose(_("Page %1 of %2: %3"), i + 1, raster_pages.size(), page->getLabel()));
                progress_bar.set_fraction(double(i) / _raster_page_count);
                auto const page_area = page->getDocumentRect();
                auto const w = std::round(page_area.width() * dpi / DPI_BASE), h = std::round(page_area.height() * dpi / DPI_BASE);
                bool ok = false;
                std::string reason;
                try {
                    if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > PNG_UINT_31_MAX || h > PNG_UINT_31_MAX) {
                        reason = _("The pixel size is out of range.");
                    } else {
                        ok = Export::exportRaster(page_area, static_cast<unsigned long>(w), static_cast<unsigned long>(h), dpi,
                            _background_color.get_current_color(),
                            Glib::filename_to_utf8(plan.files[i]), true, onProgressCallback, this, omod,
                            selected_only ? &selected : nullptr);
                    }
                } catch (std::exception const &e) { reason = e.what(); }
                if (ok) ++written;
                else if (!interrupted) failures += page->getLabel() + ": " + (reason.empty() ? _("Export failed.") : reason) + "\n";
            }
            _raster_page_count = 0;
            exportSuccessful = written == raster_pages.size() && !interrupted;
            if (!failures.empty()) {
                Gtk::MessageDialog dialog(*_desktop->getInkscapeWindow(), _("Some pages could not be exported."),
                                          false, Gtk::MessageType::ERROR, Gtk::ButtonsType::CLOSE);
                dialog.set_secondary_text(failures);
                UI::dialog_run(dialog);
            }
        }
    } else if (omod->is_raster()) {
        unsigned long int width = int(spin_buttons[SPIN_BMWIDTH]->get_value() + 0.5);
        unsigned long int height = int(spin_buttons[SPIN_BMHEIGHT]->get_value() + 0.5);

        float dpi = spin_buttons[SPIN_DPI]->get_value();

        setExporting(true, Glib::ustring::compose(_("Exporting %1 (%2 x %3)"), filename_label, width, height));

        auto items = selection->items();
        auto selected = std::vector<SPItem const *>(items.begin(), items.end());

        exportSuccessful = Export::exportRaster(area, width, height, dpi,
                                                _background_color.get_current_color(), filename_utf8, false,
                                                onProgressCallback, this, omod, selected_only ? &selected : nullptr);

    } else {
        setExporting(true, Glib::ustring::compose(_("Exporting %1"), filename_label));

        auto copy_doc = _document->copy();

        std::vector<SPItem const *> items;
        if (selected_only) {
            auto item_range = selection->items();
            items = std::vector<SPItem const *>(item_range.begin(), item_range.end());
        }

        if (current_key == SELECTION_PAGE && page_manager.hasPages()) {
            auto pages = getSelectedPages();
            // A single page won't have a selection UI, so emplace it
            if (page_manager.getPageCount() == 1) {
                pages.emplace_back(page_manager.getPage(0));
            }
            exportSuccessful = Export::exportVector(omod, copy_doc.get(), filename_utf8, false, items, pages);
        } else {
            // To get the right kind of export, we're going to make a page
            // This allows all the same raster options to work for vectors
            auto const page = copy_doc->getPageManager().newDocumentPage(area);
            exportSuccessful = Export::exportVector(omod, copy_doc.get(), filename_utf8, false, items, page);
        }
    }
    // Save the export hints back to the svg document
    if (exportSuccessful) {
        std::string path = Export::absolutizePath(_document, filepath_native);
        auto recentmanager = Gtk::RecentManager::get_default();
        if (recentmanager && Glib::path_is_absolute(path)) {
            Glib::ustring uri = Glib::filename_to_uri(path);
            recentmanager->add_item(uri);
        }

        SPObject *target;
        switch (current_key) {
            case SELECTION_CUSTOM:
            case SELECTION_DRAWING:
                target = _document->getRoot();
                break;
            case SELECTION_PAGE:
            {
                auto pages = getSelectedPages();
                if (pages.size() == 1) {
                    if ((target = page_manager.getSelected()))
                        break;
                }
                target = _document->getRoot();
                break;
            }
            case SELECTION_SELECTION:
                target = _desktop->getSelection()->firstItem();
                break;
            default:
                break;
        }
        if (target) {
            saveExportHints(target);
            DocumentUndo::done(_document, RC_("Undo", "Set Export Options"), INKSCAPE_ICON("export"));
        }
    }
    setExporting(false);
    filename_modified_by_user = false;
    interrupted = false;
}

void SingleExport::onBrowse()
{
    if (!_app || !_app->get_active_window() || !_document) {
        return;
    }

    Gtk::Window *window = _app->get_active_window();

    browseConn.block();
    auto omod = si_extension_cb.getExtension();
    assert(omod);

    std::string filename = Glib::filename_from_utf8(si_filename_entry.get_text());

    if (filename.empty()) {
        filename = Export::defaultFilename(_document, filename, omod->get_extension());
    }

    // Note, there are currently multiple modules per filename extension (.svg, .dxf, .zip).
    // We cannot distinguish between them.
    std::string basename = Glib::path_get_basename(filename);
    std::string dirname = Glib::path_get_dirname(filename);
    auto file = choose_file_save( _("Select a filename for exporting"), window,
                                  create_export_filters(), // {}, // mimetype
                                  basename,
                                  dirname);

    if (file) {
        Glib::ustring filename_utf8 = file->get_parse_name();
        si_filename_entry.set_text(filename_utf8);
        si_filename_entry.set_position(filename_utf8.length());
        onExport();
    }

    browseConn.unblock();
}

// Utils Functions

void SingleExport::blockSpinConns(bool status = true)
{
    for (auto &signal : spinButtonConns) {
        signal.block(status);
    }
}

void SingleExport::areaXChange(sb_type type)
{
    auto x0_adj = spin_buttons[SPIN_X0]->get_adjustment();
    auto x1_adj = spin_buttons[SPIN_X1]->get_adjustment();
    auto width_adj = spin_buttons[SPIN_WIDTH]->get_adjustment();

    float x0, x1, dpi, width, bmwidth;

    // Get all values in px
    Unit const *unit = units.getUnit();
    x0 = unit->convert(x0_adj->get_value(), "px");
    x1 = unit->convert(x1_adj->get_value(), "px");
    width = unit->convert(width_adj->get_value(), "px");
    bmwidth = spin_buttons[SPIN_BMWIDTH]->get_value();
    dpi = spin_buttons[SPIN_DPI]->get_value();

    switch (type) {
        case SPIN_X0:
            bmwidth = (x1 - x0) * dpi / DPI_BASE;
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                x0 = x1 - (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_X1:
            bmwidth = (x1 - x0) * dpi / DPI_BASE;
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                x1 = x0 + (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_WIDTH:
            bmwidth = width * dpi / DPI_BASE;
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                width = (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            x1 = x0 + width;
            break;
        default:
            break;
    }

    width = x1 - x0;
    bmwidth = floor(width * dpi / DPI_BASE + 0.5);

    auto px = UnitTable::get().getUnit("px");
    x0_adj->set_value(px->convert(x0, unit));
    x1_adj->set_value(px->convert(x1, unit));
    width_adj->set_value(px->convert(width, unit));
    spin_buttons[SPIN_BMWIDTH]->set_value(bmwidth);
}

void SingleExport::areaYChange(sb_type type)
{
    auto y0_adj = spin_buttons[SPIN_Y0]->get_adjustment();
    auto y1_adj = spin_buttons[SPIN_Y1]->get_adjustment();
    auto height_adj = spin_buttons[SPIN_HEIGHT]->get_adjustment();

    float y0, y1, dpi, height, bmheight;

    // Get all values in px
    Unit const *unit = units.getUnit();
    y0 = unit->convert(y0_adj->get_value(), "px");
    y1 = unit->convert(y1_adj->get_value(), "px");
    height = unit->convert(height_adj->get_value(), "px");
    bmheight = spin_buttons[SPIN_BMHEIGHT]->get_value();
    dpi = spin_buttons[SPIN_DPI]->get_value();

    switch (type) {
        case SPIN_Y0:
            bmheight = (y1 - y0) * dpi / DPI_BASE;
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                y0 = y1 - (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_Y1:
            bmheight = (y1 - y0) * dpi / DPI_BASE;
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                y1 = y0 + (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            break;
        case SPIN_HEIGHT:
            bmheight = height * dpi / DPI_BASE;
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                height = (SP_EXPORT_MIN_SIZE * DPI_BASE) / dpi;
            }
            y1 = y0 + height;
            break;
        default:
            break;
    }

    height = y1 - y0;
    bmheight = floor(height * dpi / DPI_BASE + 0.5);

    auto px = UnitTable::get().getUnit("px");
    y0_adj->set_value(px->convert(y0, unit));
    y1_adj->set_value(px->convert(y1, unit));
    height_adj->set_value(px->convert(height, unit));
    spin_buttons[SPIN_BMHEIGHT]->set_value(bmheight);
}

void SingleExport::dpiChange(sb_type type)
{
    float dpi, height, width, bmheight, bmwidth;

    // Get all values in px
    Unit const *unit = units.getUnit();
    height = unit->convert(spin_buttons[SPIN_HEIGHT]->get_value(), "px");
    width = unit->convert(spin_buttons[SPIN_WIDTH]->get_value(), "px");
    bmheight = spin_buttons[SPIN_BMHEIGHT]->get_value();
    bmwidth = spin_buttons[SPIN_BMWIDTH]->get_value();
    dpi = spin_buttons[SPIN_DPI]->get_value();

    switch (type) {
        case SPIN_BMHEIGHT:
            if (bmheight < SP_EXPORT_MIN_SIZE) {
                bmheight = SP_EXPORT_MIN_SIZE;
            }
            dpi = bmheight * DPI_BASE / height;
            break;
        case SPIN_BMWIDTH:
            if (bmwidth < SP_EXPORT_MIN_SIZE) {
                bmwidth = SP_EXPORT_MIN_SIZE;
            }
            dpi = bmwidth * DPI_BASE / width;
            break;
        case SPIN_DPI:
            prefs->setDouble("/dialogs/export/defaultdpi/value", dpi);
            break;
        default:
            break;
    }

    bmwidth = floor(width * dpi / DPI_BASE + 0.5);
    bmheight = floor(height * dpi / DPI_BASE + 0.5);

    spin_buttons[SPIN_BMHEIGHT]->set_value(bmheight);
    spin_buttons[SPIN_BMWIDTH]->set_value(bmwidth);
    spin_buttons[SPIN_DPI]->set_value(dpi);
}

void SingleExport::setDefaultSelectionMode()
{
    current_key = (selection_mode)0; // default key
    bool found = false;
    Glib::ustring pref_key_name = prefs->getString("/dialogs/export/exportarea/value");
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
        if (current_key == SELECTION_SELECTION && (_desktop->getSelection())->isEmpty()) {
            current_key = (selection_mode)0;
        }
        if ((_desktop->getSelection())->isEmpty()) {
            selection_buttons[SELECTION_SELECTION]->set_sensitive(false);
        }
        if (current_key == SELECTION_CUSTOM &&
            (spin_buttons[SPIN_HEIGHT]->get_value() == 0 || spin_buttons[SPIN_WIDTH]->get_value() == 0)) {
            Geom::OptRect bbox = _document->preferredBounds();
            setArea(bbox->min()[Geom::X], bbox->min()[Geom::Y], bbox->max()[Geom::X], bbox->max()[Geom::Y]);
        }
    } else {
        current_key = (selection_mode)0;
    }
    selection_buttons[current_key]->set_active(true);
    prefs->setString("/dialogs/export/exportarea/value", pref_key_name);

    toggleSpinButtonVisibility();
    refreshPage();
}

void SingleExport::setExporting(bool exporting, Glib::ustring const &text)
{
    if (exporting) {
        set_sensitive(false);
        set_opacity(0.2);
        progress_box.set_visible(true);
        progress_bar.set_text(text);
        progress_bar.set_fraction(0.0);
    } else {
        set_sensitive(true);
        set_opacity(1.0);
        progress_box.set_visible(false);
        progress_bar.set_text("");
        progress_bar.set_fraction(0.0);
    }
    auto main_context = Glib::MainContext::get_default();
    main_context->iteration(false);
}

// Called for every progress iteration
unsigned int SingleExport::onProgressCallback(float value, void *data)
{
    if (auto si = static_cast<SingleExport *>(data)) {
        si->progress_bar.set_fraction(si->_raster_page_count ?
            (si->_raster_page_index + value) / si->_raster_page_count : value);
        auto main_context = Glib::MainContext::get_default();
        main_context->iteration(false);
        return !si->interrupted;
    }
    return false;
}

void SingleExport::refreshPreview()
{
    if (!_desktop) {
        preview.resetPixels();
        return;
    }

    std::vector<SPItem const *> selected;
    if (si_hide_all.get_active()) {
        // This is because selection items is not a std::vector yet. FIXME.
        auto sel_range = _desktop->getSelection()->items();
        selected = {sel_range.begin(), sel_range.end()};
    }
    _preview_drawing->set_shown_items(std::move(selected));

    bool show = si_show_preview.get_active();
    if (!show || current_key == SELECTION_PAGE) {
        bool have_pages = false;
        for (auto &child : UI::children(pages_list)) {
            if (auto bi = dynamic_cast<BatchItem *>(&child)) {
                bi->refresh(!show, _background_color.get_current_color().toRGBA());
                have_pages = true;
            }
        }
        if (have_pages) {
            // We don't want to update the main preview for pages, it's hidden
            preview.resetPixels();
            return;
        }
    }

    Unit const *unit = units.getUnit();
    float x0 = unit->convert(spin_buttons[SPIN_X0]->get_value(), "px");
    float x1 = unit->convert(spin_buttons[SPIN_X1]->get_value(), "px");
    float y0 = unit->convert(spin_buttons[SPIN_Y0]->get_value(), "px");
    float y1 = unit->convert(spin_buttons[SPIN_Y1]->get_value(), "px");
    preview.setBox(Geom::Rect(x0, y0, x1, y1) * _document->dt2doc());
    preview.setBackgroundColor(_background_color.get_current_color().toRGBA());
    preview.queueRefresh();
}

void SingleExport::setDesktop(SPDesktop *desktop)
{
    if (desktop != _desktop) {
        _page_selected_connection.disconnect();
        _desktop = desktop;
    }
}

void SingleExport::setDocument(SPDocument *document)
{
    if (_document == document)
        return;

    _document = document;

    _page_selected_connection.disconnect();
    _page_modified_connection.disconnect();
    _page_changed_connection.disconnect();

    if (document) {
        auto &pm = document->getPageManager();
        _page_selected_connection = pm.connectPageSelected(sigc::mem_fun(*this, &SingleExport::onPagesSelected));
        _page_modified_connection = pm.connectPageModified(sigc::mem_fun(*this, &SingleExport::onPagesModified));
        _page_changed_connection = pm.connectPagesChanged(sigc::mem_fun(*this, &SingleExport::onPagesChanged));
        _background_color.setColor(get_export_bg_color(document->getNamedView(), Colors::Color(0xffffff00)));
        _preview_drawing = std::make_shared<PreviewDrawing>(document);
        preview.setDrawing(_preview_drawing);

        // Refresh values to sync them with defaults.
        onPagesChanged(nullptr);
        refreshArea();
        filename_modified_by_user = false;
        loadExportHints();
    } else {
        preview.setDrawing({});
        _preview_drawing.reset();
        onPagesChanged(nullptr);
    }
}

SingleExport::~SingleExport() = default;

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
