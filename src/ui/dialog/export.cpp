// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Peter Bostrom
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *   Anshudhar Kumar Singh <anshudhar2001@gmail.com>
 *
 * Copyright (C) 1999-2007, 2012, 2021 Authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "export.h"

#include <glibmm/convert.h>
#include <glibmm/fileutils.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <glibmm/markup.h>


#include "desktop.h"
#include "extension/output.h"
#include "helper/png-write.h"
#include "inkscape.h"
#include "io/resource.h"
#include "io/sys.h"            // for sanitizeString, file_test
#include "object/object-set.h" // for ObjectSet
#include "object/sp-page.h"    // for SPPage
#include "object/sp-root.h"    // for SPRoot
#include "object/weakptr.h"    // for SPWeakPtr
#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/dialog/export-panel.h"
#include "ui/dialog/link-missing.h"
#include "ui/interface.h"

namespace Inkscape::UI::Dialog {

Export::Export()
    : DialogBase("/dialogs/export/", "Export")
    , builder(create_builder("dialog-export.glade"))
    , container       (get_widget<Gtk::Box>            (builder, "export-box"))
    , panel(std::make_unique<ExportPanel>())
{
    append(container);
    container.append(*panel);
}

Export::~Export() = default;
void Export::documentReplaced() { panel->setContext(getDesktop(), getDocument()); }
void Export::desktopReplaced() { documentReplaced(); }
void Export::selectionChanged(Inkscape::Selection *) { panel->refresh(true); }
void Export::selectionModified(Inkscape::Selection *, unsigned) { panel->refresh(); }

/**
 * Convert path to absolute path.
 *
 * @arg doc Document. Relative paths are interpreted relative to the document path.
 * @arg filename File path which may be relative.
 * Value is in platform-native encoding (see Glib::filename_to_utf8).
 *
 * @returns File path changed to absolute path.
 * Value is in platform-native encoding (see Glib::filename_to_utf8).
 */
std::string Export::absolutizePath(SPDocument *doc, const std::string &filename)
{
    std::string path;
    // Make relative paths go from the document location, if possible:
    if (!Glib::path_is_absolute(filename) && doc->getDocumentFilename()) {
        auto dirname = Glib::path_get_dirname(doc->getDocumentFilename());
        if (!dirname.empty()) {
            path = Glib::build_filename(dirname, filename);
        }
    }
    if (path.empty()) {
        path = filename;
    }
    return path;
}

bool Export::unConflictFilename(SPDocument *doc, std::string &filename, std::string const extension)
{
    std::string path = absolutizePath(doc, filename);
    std::string test_filename = path + extension;
    if (!Inkscape::IO::file_test(test_filename.c_str(), G_FILE_TEST_EXISTS)) {
        filename = test_filename;
        return true;
    }
    for (int i = 1; i <= 100; i++) {
        test_filename = path + "_copy_" + std::to_string(i) + extension;
        if (!Inkscape::IO::file_test(test_filename.c_str(), G_FILE_TEST_EXISTS)) {
            filename = test_filename;
            return true;
        }
    }
    return false;
}

/**
 * Checks if the directory exists and if not, tries to create the directory and if failed, displays an error message.
 *
 * @arg filename - File path (may be absolute or relative to current document).
 * Value is in platform-native encoding (see Glib::filename_to_utf8).
 */
bool Export::checkOrCreateDirectory(std::string const &filename)
{
    SPDesktop *desktop = SP_ACTIVE_DESKTOP;
    if (!desktop) {
        return false;
    }

    std::string path = absolutizePath(desktop->getDocument(), filename);
    std::string dirname = Glib::path_get_dirname(path);

    if (dirname.empty() || !Inkscape::IO::file_test(dirname.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
        if (g_mkdir_with_parents(dirname.c_str(), S_IRWXU) != 0) {
            Glib::ustring safeDir = Inkscape::IO::sanitizeString(dirname.c_str());
            Glib::ustring error = g_strdup_printf(_("Directory <b>%s</b> does not exist and can't be created.\n"), safeDir.c_str());
            desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, error.c_str());
            sp_ui_error_dialog(error.c_str());
            return false;
        }
    }
    return true;
}

/**
 * Export to raster graphics
 *
 * @arg filename Filename. Path is absolute or relative to the current document.
 * Value is in UTF8 encoding
 */
bool Export::exportRaster(
        Geom::Rect const &area, unsigned long int const &width, unsigned long int const &height,
        float const &dpi, Inkscape::Colors::Color const &bgcolor, Glib::ustring const &filename, bool overwrite,
        unsigned (*callback)(float, void *), void *data,
        Inkscape::Extension::Output *extension, std::vector<SPItem const *> *items)
{
    SPDesktop *desktop = SP_ACTIVE_DESKTOP;
    if (!desktop)
        return false;
    SPDocument *doc = desktop->getDocument();

    if (area.hasZeroArea() || width == 0 || height == 0) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("The chosen area to be exported is invalid."));
        sp_ui_error_dialog(_("The chosen area to be exported is invalid"));
        return false;
    }
    if (filename.empty()) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("You have to enter a filename."));
        sp_ui_error_dialog(_("You have to enter a filename"));
        return false;
    }

    if (!extension || !extension->is_raster()) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Raster Export Error"));
        sp_ui_error_dialog(_("Raster export Method is used for NON RASTER EXTENSION"));
        return false;
    }

    float pHYs = extension->get_param_float("png_phys", dpi);
    if (pHYs < 0.01) pHYs = dpi;

    bool use_interlacing = extension->get_param_bool("png_interlacing", false);
    int antialiasing = extension->get_param_int("png_antialias", 2); // Cairo anti aliasing
    int zlib = extension->get_param_int("png_compression", 1); // Default is 6 for png, but 1 for non-png
    auto val = extension->get_param_int("png_bitdepth", 99); // corresponds to RGBA 8

    int bit_depth = pow(2, (val & 0x0F));
    int color_type = (val & 0xF0) >> 4;

    std::string path = absolutizePath(doc, Glib::filename_from_utf8(filename));

    // Do the over-write protection now, since the png is just a temp file.
    if (!overwrite && !sp_ui_overwrite_file(path)) {
        return false;
    }

    auto fn = Glib::path_get_basename(path);
    /// Path to temporary PNG file.
    /// Value is in platform-native encoding (see Glib::filename_to_utf8).
    auto png_filename = path;
    {
        // Select the extension and set the filename to a temporary file
        int tempfd_out = Glib::file_open_tmp(png_filename, "ink_ext_");
        close(tempfd_out);
    }

    // Export Start Here
    std::vector<SPItem const *> selected;
    if (items && items->size() > 0) {
        selected = *items;
    }

    ExportResult result = sp_export_png_file(
        desktop->getDocument(), Glib::filename_to_utf8(png_filename).c_str(), area, width, height, pHYs,
        pHYs, // previously xdpi, ydpi.
        bgcolor, callback, data, true, selected, use_interlacing, color_type, bit_depth, zlib, antialiasing);

    bool failed = result == EXPORT_ERROR; // || prog_dialog->get_stopped();

    if (failed) {
        Glib::ustring safeFile = Inkscape::IO::sanitizeString(Glib::filename_to_utf8(path).c_str());
        Glib::ustring error = g_strdup_printf(_("Could not export to filename <b>%s</b>.\n"), safeFile.c_str());

        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, error.c_str());
        sp_ui_error_dialog(error.c_str());
        return false;
    } else if (result == EXPORT_OK) {
        // Don't ask for preferences on every run.
        try {
            extension->export_raster(doc, png_filename, path.c_str(), false);
        } catch (Inkscape::Extension::Output::save_failed &e) {
            return false;
        }
    } else {
        // Extensions have their own error popup, so this only tracks failures in the png step
        desktop->messageStack()->flash(Inkscape::INFORMATION_MESSAGE, _("Export aborted."));
        return false;
    }

    Glib::ustring safeFile = Inkscape::IO::sanitizeString(path.c_str());
    desktop->messageStack()->flashF(Inkscape::INFORMATION_MESSAGE, _("Drawing exported to <b>%s</b>."),
                                    safeFile.c_str());

    unlink(png_filename.c_str());
    return true;
}

bool Export::exportVector(
        Inkscape::Extension::Output *extension, SPDocument *doc,
        Glib::ustring const &filename,
        bool overwrite, Geom::Rect const &area, std::optional<Extension::MissingLinkAction> links)
{
    doc->ensureUpToDate();
    doc->getPageManager().disablePages();
    doc->fitToRect(area, false);
    doc->getPageManager().enablePages();
    auto page = doc->getPageManager().getFirstPage();
    return exportVector(extension, doc, filename, overwrite, {}, page, links);
}

bool Export::exportVector(
        Inkscape::Extension::Output *extension, SPDocument *doc,
        Glib::ustring const &filename,
        bool overwrite, const std::vector<SPItem const *> &items, SPPage const *page,
        std::optional<Extension::MissingLinkAction> links)
{
    std::vector<SPPage const *> pages;
    if (page)
        pages.push_back(page);
    return exportVector(extension, doc, filename, overwrite, items, pages, links);
}

/**
 * Export to vector graphics
 *
 * @arg filename Filename. Path is absolute or relative to the current document.
 * Value is in UTF8 encoding.
 */
bool Export::exportVector(
        Inkscape::Extension::Output *extension, SPDocument *copy_doc,
        Glib::ustring const &filename,
        bool overwrite, const std::vector<SPItem const *> &items, const std::vector<SPPage const *> &pages,
        std::optional<Extension::MissingLinkAction> preset_links)
{
    SPDesktop *desktop = SP_ACTIVE_DESKTOP;
    if (!desktop)
        return false;

    if (filename.empty()) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("You have to enter a filename."));
        sp_ui_error_dialog(_("You have to enter a filename"));
        return false;
    }

    if (!extension || extension->is_raster()) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Vector Export Error"));
        sp_ui_error_dialog(_("Vector export Method is used for RASTER EXTENSION"));
        return false;
    }

    std::string path = absolutizePath(copy_doc, Glib::filename_from_utf8(filename));
    Glib::ustring safeFile = Inkscape::IO::sanitizeString(path.c_str());

    // Do the over-write protection now
    if (!overwrite && !sp_ui_overwrite_file(path)) {
        return false;
    }
    // A confirmed discard can delete items in both the copy and the working document.
    // Snapshot IDs before the dialog; never dereference the original item pointers after it.
    std::vector<std::string> selected_ids;
    for (auto *item : items) if (auto const *id = item->getId()) selected_ids.emplace_back(id);
    auto links = Inkscape::Extension::MissingLinkAction::RequirePresent;
    if (extension->needs_picture_bytes()) {
        links = preset_links ? *preset_links : resolveMissingLinksForSave(desktop, copy_doc);
        if (links == Inkscape::Extension::MissingLinkAction::Cancel) return false;
    }
    copy_doc->ensureUpToDate();

    std::vector<SPItem const *> objects;
    for (auto const &id : selected_ids) {
        if (auto *item = cast<SPItem>(copy_doc->getObjectById(id))) objects.push_back(item);
    }
    std::set<std::string> obj_ids;
    std::set<std::string> page_ids;
    for (auto page : pages) {
        if (auto _id = page->getId()) {
            page_ids.insert(std::string(_id));
        }
        // If page then our item set is limited to the overlapping items
        auto page_items = page->getOverlappingItems(true, true);

        if (items.empty()) {
            // Items is page_items, remove all items not in this page.
            objects.insert(objects.end(), page_items.begin(), page_items.end());
        } else {
            for (auto &item : page_items) {
                item->getIds(obj_ids);
            }
        }
    }

    // Delete any pages not specified, delete all pages if none specified
    auto &pm = copy_doc->getPageManager();

    // Make weak pointers to pages, since deletePage() can delete more than just the requested page.
    std::vector<SPWeakPtr<SPPage>> copy_pages;
    copy_pages.reserve(pm.getPageCount());
    for (auto *page : pm.getPages()) {
        copy_pages.emplace_back(page);
    }

    // We refuse to delete anything if everything would be deleted.
    for (auto &page : copy_pages) {
        if (page) {
            auto _id = page->getId();
            if (_id && page_ids.find(_id) == page_ids.end()) {
                pm.deletePage(page.get(), false);
            }
        }
    }

    // Page export ALWAYS restricts, even if nothing would be on the page.
    if (!items.empty() || !objects.empty() || !pages.empty()) {
        std::vector<SPObject *> objects_to_export;
        Inkscape::ObjectSet object_set(copy_doc);
        for (auto &object : objects) {
            auto _id = object->getId();
            if (!_id || (!obj_ids.empty() && obj_ids.find(_id) == obj_ids.end())) {
                // This item is off the page so can be ignored for export
                continue;
            }

            SPObject *obj = copy_doc->getObjectById(_id);
            if (!obj) {
                Glib::ustring error = g_strdup_printf(_("Could not export to filename <b>%s</b>. (missing object)\n"), safeFile.c_str());

                desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, error.c_str());
                sp_ui_error_dialog(error.c_str());

                return false;
            }
            copy_doc->ensureUpToDate();

            object_set.add(obj, true);
            objects_to_export.push_back(obj);
        }

        copy_doc->getRoot()->cropToObjects(objects_to_export);

        if (pages.empty()) {
            object_set.fitCanvas(false, true);
        }
    }

    // Remove all unused definitions
    copy_doc->vacuumDocument();

    try {
        extension->save(copy_doc, path.c_str(), false, links);
    } catch (Inkscape::Extension::Output::save_failed &e) {
        Glib::ustring safeFile = Inkscape::IO::sanitizeString(path.c_str());
        Glib::ustring error = g_strdup_printf(_("Could not export to filename <b>%s</b>.\n"), safeFile.c_str());

        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, error.c_str());
        sp_ui_error_dialog(error.c_str());

        return false;
    } catch (std::exception const &e) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, Glib::Markup::escape_text(e.what()));
        sp_ui_error_dialog(Glib::Markup::escape_text(e.what()).c_str());
        return false;
    }

    if (!extension->export_report.findings.empty()) {
        auto const text = extension->export_report.summary();
        desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE, Glib::Markup::escape_text(text));
        sp_ui_error_dialog(Glib::Markup::escape_text(text).c_str());
    } else {
        desktop->messageStack()->flashF(Inkscape::INFORMATION_MESSAGE, _("Drawing exported to <b>%s</b>."), safeFile.c_str());
    }
    return true;
}

std::string Export::filePathFromObject(SPDocument *doc, SPObject *obj, const std::string &file_entry_text)
{
    Glib::ustring id = _("bitmap");
    if (obj && obj->getId()) {
        id = obj->getId();
    }
    return prependDirectory(Glib::filename_from_utf8(id), file_entry_text, doc);
}

/**
 * Adds the full directory path to the final part of a file name.
 *
 * @arg name - The final name part of a path, if absolute path, this is returned unchanged.
 *             Value is in platform-native encoding (see Glib::filename_to_utf8).
 * @arg orig - The original filename, an optional path used previously which this name
 *             will be appanded in order to produce the full path.
 *             Value is in platform-native encoding (see Glib::filename_to_utf8).
 * @arg doc - The document to take a fallback directory from if orig is empty.
 *
 * @returns
 *  A path ending with `name` and starting with the directory part of
 *    1. `name` (if it is an absolute path or empty)
 *    2. `orig`
 *    3. `doc`
 *    or
 *    4. the user home directory
 *  Value is in platform-native encoding (see Glib::filename_to_utf8).
 */

// FIXME change type of name...?
std::string Export::prependDirectory(const std::string &name, const std::string &orig, SPDocument *doc)
{
    if (Glib::path_is_absolute(name) || name.empty())
        return name;

    if (Glib::path_get_basename(name) != name) {
        return name;
    }

    std::string directory;

    if (!orig.empty()) {
        directory = Glib::path_get_dirname(orig);
        // Note: Glib::path_get_dirname() returns "." if there is no directory component
    }
    if (directory.empty() || directory == ".") {
        /* Grab document directory */
        const gchar *docFilename = doc->getDocumentFilename();
        if (docFilename) {
            directory = Glib::path_get_dirname(docFilename);
        }
    }
    if (directory.empty() || directory == ".") {
        directory = Inkscape::IO::Resource::homedir_path();
    }

    return Glib::build_filename(directory, name);
}

std::string Export::defaultFilename(SPDocument *doc, const std::string &filename_entry_text,
                                    const std::string &extension)
{
    std::string filename;
    if (doc && doc->getDocumentFilename()) {
        filename = doc->getDocumentFilename();
        //appendExtensionToFilename(filename, extension);
    } else if (doc) {
        filename = prependDirectory(Glib::filename_from_utf8(_("bitmap")), filename_entry_text, doc);
        filename = filename + extension;
    }
    return filename;
}

void set_export_bg_color(SPObject* object, Inkscape::Colors::Color const &color) {
    if (object) {
        object->setAttribute("inkscape:export-bgcolor", color.toString());
    }
}

Inkscape::Colors::Color get_export_bg_color(SPObject* object, Inkscape::Colors::Color const &default_color) {
    if (object) {
        if (auto c = Inkscape::Colors::Color::parse(object->getAttribute("inkscape:export-bgcolor"))) {
            return *c;
        }
    }
    return default_color;
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
