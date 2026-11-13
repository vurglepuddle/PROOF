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

#ifndef SP_EXPORT_SINGLE_H
#define SP_EXPORT_SINGLE_H

#include "ui/dialog/export-batch.h"

namespace Gtk {
class Builder;
class Button;
class CheckButton;
class FlowBox;
class Grid;
class Label;
class ProgressBar;
class ToggleButton;
class ScrolledWindow;
} // namespace Gtk

class InkscapeApplication;
class SPDesktop;
class SPDocument;
class SPObject;
class SPPage;

namespace Inkscape {

class Selection;
class Preferences;

namespace UI {

namespace Widget {
class SpinButton;
class UnitMenu;
class ColorPicker;
} // namespace Widget

namespace Dialog {

class PreviewDrawing;
class ExportPreview;
class ExtensionList;

class SingleExport : public Gtk::Box
{
public:
    SingleExport(BaseObjectType *cobject, const Glib::RefPtr<Gtk::Builder> &refGlade);
    ~SingleExport() override;

    void setApp(InkscapeApplication *app) { _app = app; }
    void setDocument(SPDocument *document);
    void setDesktop(SPDesktop *desktop);
    void selectionChanged(Inkscape::Selection *selection);
    void selectionModified(Inkscape::Selection *selection, guint flags);
    void refresh()
    {
        refreshArea();
        refreshPage();
        loadExportHints();
    };

private:
    enum sb_type
    {
        SPIN_X0 = 0,
        SPIN_X1,
        SPIN_Y0,
        SPIN_Y1,
        SPIN_WIDTH,
        SPIN_HEIGHT,
        SPIN_BMWIDTH,
        SPIN_BMHEIGHT,
        SPIN_DPI
    };

    enum selection_mode
    {
        SELECTION_PAGE = 0, // Default is alaways placed first
        SELECTION_SELECTION,
        SELECTION_DRAWING,
        SELECTION_CUSTOM,
    };

    InkscapeApplication *_app = nullptr;
    SPDesktop *_desktop = nullptr;
    SPDocument *_document = nullptr;
    std::shared_ptr<PreviewDrawing> _preview_drawing;

    bool setupDone = false; // To prevent setup() call add connections again.

    std::map<sb_type, UI::Widget::SpinButton *> spin_buttons;
    std::map<sb_type, Gtk::Label *> spin_labels;
    std::map<selection_mode, Gtk::ToggleButton *> selection_buttons;

    Gtk::CheckButton *show_export_area = nullptr;

    BatchItems current_items;

    // In order of intialization
    Gtk::FlowBox &pages_list;
    Gtk::ScrolledWindow &pages_list_box;
    Gtk::Grid &size_box;
    Inkscape::UI::Widget::UnitMenu &units;
    Gtk::Box &si_units_row;
    Gtk::CheckButton &si_hide_all;
    Gtk::CheckButton &si_show_preview;

    ExportPreview &preview;
    Gtk::Box &preview_box;

    ExtensionList &si_extension_cb;

    Gtk::Entry &si_filename_entry;
    Gtk::Button &si_filename_button;
    Gtk::Button &si_export;
    Gtk::ProgressBar &progress_bar;
    Gtk::Widget &progress_box;
    Gtk::Button &cancel_button;
    UI::Widget::ColorPicker &_background_color;

    /// True if the value of the selected filename was changed by the user since the last export.
    /// False when the filename is e.g. an auto-generated suggestion or remembered in the document attributes.
    bool filename_modified_by_user = false;
    /// Last value of filename entry field that was set programmatically. Used to detect modification by the user.
    Glib::ustring filename_entry_original_value;

    Glib::ustring doc_export_name;
    /// File path as returned by the file chooser.
    /// Value is in platform-native encoding (see Glib::filename_to_utf8).
    std::string filepath_native;

    Inkscape::Preferences *prefs = nullptr;
    std::map<selection_mode, Glib::ustring> selection_names;
    selection_mode current_key = (selection_mode)0;

    void setup();
    void setupUnits();
    void setupSpinButtons();
    void toggleSpinButtonVisibility();
    void refreshPreview();

    // change range and callbacks to spinbuttons
    template <typename T>
    void setupSpinButton(UI::Widget::SpinButton *sb, double val, double min, double max, double step, double page, int digits,
                         bool sensitive, void (SingleExport::*cb)(T), T param);

    void setDefaultSelectionMode();
    void onAreaXChange(sb_type type);
    void onAreaYChange(sb_type type);
    void onDpiChange(sb_type type);
    void onAreaTypeToggle(selection_mode key);
    void onUnitChanged();
    void onFilenameModified();
    void onExtensionChanged();
    void onExport();
    void onCancel();
    void onBrowse();

    void refreshArea();
    void refreshPage();
    void loadExportHints();
    void setFilename(std::string filename, bool modified_by_user);
    void saveExportHints(SPObject *target);
    void areaXChange(sb_type type);
    void areaYChange(sb_type type);
    void dpiChange(sb_type type);
    void setArea(double x0, double y0, double x1, double y1);
    void blockSpinConns(bool status);

    void setExporting(bool exporting, Glib::ustring const &text = "");
    /**
     * Callback to be used in for loop to update the progress bar.
     *
     * @param value number between 0 and 1 indicating the fraction of progress (0.17 = 17 % progress)
     */
    static unsigned int onProgressCallback(float value, void *data);

    /**
     * Page functions
     */
    void onPagesChanged(SPPage *new_page);
    void onPagesModified(SPPage *page);
    void onPagesSelected(SPPage *page);
    void setPagesMode(bool multi);
    void selectPage(SPPage *page);
    std::vector<SPPage const *> getSelectedPages() const;

    bool interrupted;
    std::size_t _raster_page_index = 0, _raster_page_count = 0;

    // Gtk Signals
    std::vector<sigc::scoped_connection> spinButtonConns;
    sigc::scoped_connection filenameConn;
    sigc::scoped_connection extensionConn;
    sigc::scoped_connection exportConn;
    sigc::scoped_connection cancelConn;
    sigc::scoped_connection browseConn;
    sigc::scoped_connection _pages_list_changed;
    // Document Signals
    sigc::scoped_connection _page_selected_connection;
    sigc::scoped_connection _page_modified_connection;
    sigc::scoped_connection _page_changed_connection;
};

} // namespace Dialog
} // namespace UI
} // namespace Inkscape

#endif // SP_EXPORT_SINGLE_H

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
