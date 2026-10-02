// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * File export from the command line. This code, greatly modified, use to be in main.cpp. It should
 * be replaced by code shared with the file dialog (Gio::Actions?).
 *
 * Copyright (C) 2018 Tavmjong Bah
 *
 * Git blame shows that bulia byak is the main author of the original export code from
 * main.cpp. Other authors of note include Nicolas Dufour, Vinicius dos Santos Oliveira, and Bob
 * Jamison; none of whom bothered to add their names to the copyright of main.cc.
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 */

#include "file-export-cmd.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <boost/algorithm/string.hpp>
#include <giomm/file.h>
#include <glibmm/convert.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <glibmm/regex.h>
#include <png.h> // PNG export
#include <ranges>

#include "colors/color.h"
#include "colors/manager.h"
#include "document.h"
#include "extension/db.h"
#include "extension/extension.h"
#include "extension/output.h"
#include "extension/system.h"
#include "helper/png-write.h" // PNG Export
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "path-chemistry.h"      // sp_item_list_to_curves
#include "preferences.h"
#include "selection-chemistry.h" // fit_canvas_to_drawing
#include "util/units.h"
#include "util/parse-int-range.h"
#include "io/sys.h"

namespace filesystem = std::filesystem;

InkFileExportCmd::InkFileExportCmd()
    : export_overwrite(false)
    , export_margin(0)
    , export_area_snap(false)
    , export_use_hints(false)
    , export_width(0)
    , export_height(0)
    , export_dpi(0)
    , export_ignore_filters(false)
    , export_text_to_path(false)
    , export_ps_level(3)
    , export_pdf_level("2.0")
    , export_latex(false)
    , export_id_only(false)
    , export_background_opacity(-1) // default is unset != actively set to 0
    , export_plain_svg(false)
    , export_png_compression(6)
    , export_png_antialias(2)
    , make_paths(false)
{
}

void
InkFileExportCmd::do_export(SPDocument* doc, std::string filename_in)
{
    std::string export_type_filename;
    std::vector<Glib::ustring> export_type_list;

    // Get export type from filename supplied with --export-filename
    if (!export_filename.empty() && export_filename != "-") {

        // Attempt to result variable and home path use in export filenames.
        Glib::RefPtr<Gio::File> gfile = Gio::File::create_for_parse_name(export_filename);
        export_filename = gfile->get_parse_name();

#ifdef G_OS_WIN32
        auto fn = filesystem::u8path(export_filename);
#else
        auto fn = filesystem::path(export_filename);
#endif
        if (!fn.has_extension()) {
            if (export_type.empty() && export_extension.empty()) {
                std::cerr << "InkFileExportCmd::do_export: No export type specified. "
                          << "Append a supported file extension to filename provided with --export-filename or "
                          << "provide one or more extensions separately using --export-type" << std::endl;
                return;
            } else {
                // no extension is fine if --export-type is given
                // explicitly stated extensions are handled later
            }
        } else {
            export_type_filename = fn.extension().string().substr(1);
            boost::algorithm::to_lower(export_type_filename);
            export_filename = (fn.parent_path() / fn.stem()).string();
        }
    }

    // Get export type(s) from string supplied with --export-type
    if (!export_type.empty()) {
        export_type_list = Glib::Regex::split_simple("[,;]", export_type);
    }

    // Determine actual type(s) for export.
    if (export_use_hints) {
        // Override type if --export-use-hints is used (hints presume PNG export for now)
        // TODO: There's actually no reason to presume. We could allow to export to any format using hints!
        if (export_id.empty() && export_area_type != ExportAreaType::Drawing) {
            std::cerr << "InkFileExportCmd::do_export: "
                      << "--export-use-hints can only be used with --export-id or --export-area-drawing." << std::endl;
            return;
        }
        if (export_type_list.size() > 1 || (export_type_list.size() == 1 && export_type_list[0] != "png")) {
            std::cerr << "InkFileExportCmd::do_export: --export-use-hints can only be used with PNG export! "
                      << "Ignoring --export-type=" << export_type.raw() << "." << std::endl;
        }
        if (!export_filename.empty()) {
            std::cerr << "InkFileExportCmd::do_export: --export-filename is ignored when using --export-use-hints!" << std::endl;
        }
        export_type_list.clear();
        export_type_list.emplace_back("png");
    } else if (export_type_list.empty()) {
        if (!export_type_filename.empty()) {
            export_type_list.emplace_back(export_type_filename); // use extension from filename
        } else if (!export_extension.empty()) {
            // guess export type from extension
            auto ext =
                dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(export_extension.data()));
            if (ext) {
                export_type_list.emplace_back(std::string(ext->get_extension()).substr(1));
            } else {
                std::cerr << "InkFileExportCmd::do_export: "
                          << "The supplied --export-extension was not found. Specify a file extension "
                          << "to get a list of available extensions for this file type.";
                return;
            }
        } else {
            export_type_list.emplace_back("svg"); // fall-back to SVG by default
        }
    }
    // check if multiple export files are requested, but --export_extension was supplied
    if (!export_extension.empty() && export_type_list.size() != 1) {
        std::cerr
            << "InkFileExportCmd::do_export: You may only specify one export type if --export-extension is supplied";
        return;
    }
    Inkscape::Extension::DB::OutputList extension_list;
    Inkscape::Extension::db.get_output_list(extension_list);

    // Export filename should be used when specified as the output file
    auto const filename_out = !export_filename.empty() ? export_filename : filename_in;

    auto path_out = Glib::path_get_dirname(filename_out);
    if (make_paths && !Inkscape::IO::file_test(path_out.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
        g_mkdir_with_parents(path_out.c_str(), 0755);
    }

    if (!Inkscape::IO::file_test(path_out.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
        std::cerr << "InkFileExportCmd::do_export: "
                  << "file directory doesn't exist for export: "
                  << path_out.c_str() << std::endl;
        return;
    }

    for (auto const &Type : export_type_list) {
        // use lowercase type for following comparisons
        auto type = Type.lowercase();
        g_info("exporting '%s' to type '%s'", filename_in.c_str(), type.c_str());

        // Remove initial "." if present in --export-type
        if (!type.empty() && type[0] == '.') {
            type = type.substr(1);
        }
        export_type_current = type;

        // Check for consistency between extension of --export-filename and --export-type if both are given
        if (!export_type_filename.empty() && type.raw() != export_type_filename) {
            std::cerr << "InkFileExportCmd::do_export: "
                      << "Ignoring extension of export filename (" << export_type_filename << ") "
                      << "as it does not match the current export type (" << type.raw() << ")." << std::endl;
        }
        bool export_extension_forced = !export_extension.empty();
        // For PNG export, there is no extension, so the method below can not be used.
        if (type == "png") {
            if (!export_extension_forced) {
                do_export_png(doc, filename_out);
            } else {
                std::cerr << "InkFileExportCmd::do_export: "
                          << "The parameter --export-extension is invalid for PNG export" << std::endl;
            }
            continue;
        }
        // for SVG export, we let the do_export_svg function handle the selection of the extension, unless
        // an extension ID was explicitly given. This makes handling of --export-plain-svg easier (which
        // should also work when multiple file types are given, unlike --export-extension)
        if (type == "svg" && !export_extension_forced) {
            do_export_svg(doc, filename_out);
            continue;
        }

        bool extension_for_fn_exists = false;
        bool exported = false;
        // if no extension is found, the entire list of extensions is walked through,
        // so we can use the same loop to construct the list of available formats for the error message
        std::list<std::string> filetypes({".svg", ".png", ".ps", ".eps", ".pdf"});
        std::list<std::string> exts_for_fn;
        for (auto const &oext : extension_list) {
            if (oext->deactivated()) {
                continue;
            }
            auto name = Glib::ustring(oext->get_extension()).lowercase();
            filetypes.emplace_back(name);
            if (name == "." + type) {
                extension_for_fn_exists = true;
                exts_for_fn.emplace_back(oext->get_id());
                if (!export_extension_forced ||
                    (export_extension == Glib::ustring(oext->get_id()).lowercase())) {
                    if (type == "svg") {
                        do_export_vector(doc, filename_out, *oext);
                    } else if (type == "ps") {
                        do_export_ps_pdf(doc, filename_out, "image/x-postscript", *oext);
                    } else if (type == "eps") {
                        do_export_ps_pdf(doc, filename_out, "image/x-e-postscript", *oext);
                    } else if (type == "pdf" || type == "ai") {
                        do_export_ps_pdf(doc, filename_out, "application/pdf", *oext);
                    } else {
                        do_export_extension(doc, filename_out, oext);
                    }
                    exported = true;
                    break;
                }
            }
        }
        if (!exported) {
            if (export_extension_forced && extension_for_fn_exists) {
                // the located extension for this file type did not match the provided --export-extension parameter
                std::cerr << "InkFileExportCmd::do_export: "
                          << "The supplied extension ID (" << export_extension
                          << ") does not match any of the extensions "
                          << "available for this file type." << std::endl
                          << "Supported IDs for this file type: [";
                copy(exts_for_fn.begin(), exts_for_fn.end(), std::ostream_iterator<std::string>(std::cerr, ", "));
                std::cerr << "\b\b]" << std::endl;
            } else {
                std::cerr << "InkFileExportCmd::do_export: Unknown export type: " << type.raw() << ". Allowed values: [";
                filetypes.sort();
                filetypes.unique();
                std::ranges::copy(filetypes |
                    std::views::transform([](auto& ext){ return ext.starts_with(".") ? ext.substr(1) : ext; }),
                    std::ostream_iterator<std::string>(std::cerr, ", "));
                std::cerr << "\b\b]" << std::endl;
            }
        }
    }
}

// Resizes the doc to just be the target export area (including snapping, margins, etc)
// If `object` is not null, the doc will be scoped down to just the one object.
void InkFileExportCmd::fit_to_export_area(SPDocument *doc, SPObject *object)
{
    Geom::Rect area;
    doc->ensureUpToDate();

    auto obj_export_type = export_area_type;
    auto drawing_bounds = static_cast<SPItem *>(doc->getRoot());

    if (obj_export_type == ExportAreaType::Area) {
        /* Try to parse area (given in SVG pixels) */
        gdouble x0, y0, x1, y1;
        if (sscanf(export_area.c_str(), "%lg:%lg:%lg:%lg", &x0, &y0, &x1, &y1) != 4) {
            g_warning("Cannot parse export area '%s'; use 'x0:y0:x1:y1'. Ignoring argument.",
                        export_area.c_str());
            obj_export_type = ExportAreaType::Unset;
        } else {
            area = Geom::Rect(Geom::Interval(x0, x1), Geom::Interval(y0, y1));
        }
    }

    if (obj_export_type == ExportAreaType::Unset) {
        // Default to just the object bbox if provided, otherwise export page
        if (object) {
            obj_export_type = ExportAreaType::Drawing;
            drawing_bounds = static_cast<SPItem *>(object); // chop drawing to our object bbox
        } else {
            obj_export_type = ExportAreaType::Page;
        }
    }

    // Three choices: 1. Command-line export_area  2. Page area  3. Drawing area
    switch (obj_export_type) {
        case ExportAreaType::Unset:
            std::cerr << "ExportAreaType::not_set should be handled before" << std::endl;
            break;
        case ExportAreaType::Page: {
            // Export area page (explicit or if no object is given).
            Geom::Point origin(doc->getRoot()->x.computed, doc->getRoot()->y.computed);
            area = Geom::Rect(origin, origin + doc->getDimensions());
            break;
        }
        case ExportAreaType::Area:
            // already parsed and set above
            break;
        case ExportAreaType::Drawing: {
            // Export area drawing (explicit or if object is given).
            Geom::OptRect areaMaybe = drawing_bounds->documentVisualBounds();
            if (areaMaybe) {
                area = *areaMaybe;
            } else {
                std::cerr << "InkFileExport::fit_to_export_area: "
                            << "Unable to determine a valid bounding box. Skipping." << std::endl;
            }
            break;
        }
    }

    if (export_area_snap) {
        area = area.roundOutwards();
    }

    // Scope document down to the provided object, if any
    if (object && export_id_only) {
        // Remove all other objects to complete the "crop"
        std::cerr << "Exporting only object with id=\""
                << object->getId() << "\"; all other objects hidden." << std::endl;
        doc->getRoot()->cropToObject(object);
    }

    if (export_margin != 0) {
        auto unit = doc->getDisplayUnit();
        auto margin_px = Inkscape::Util::Quantity::convert(export_margin, unit, "px");
        area.expandBy(margin_px);
    }

    // Only adjust the doc if we have to
    if (*doc->preferredBounds() != area) {
        doc->fitToRect(area);
    }
}

// File names use std::string. HTML5 and presumably SVG 2 allows UTF-8 characters. Do we need to convert "object_id" here?
std::string
InkFileExportCmd::get_filename_out(std::string filename_in, std::string object_id)
{
    // Pipe out
    if (export_filename == "-") {
        return "-";
    }

    auto const export_type_current_native = Glib::filename_from_utf8(export_type_current);

    // Use filename provided with --export-filename if given
    if (!export_filename.empty()) {
        auto ext = Inkscape::IO::get_file_extension(export_filename);
        auto cmp = "." + export_type_current_native;
        return export_filename + (ext == cmp ? "" : cmp);
    }

    // Check for pipe
    if (filename_in == "-") {
        return "-";
    }

    // Construct output filename from input filename and export_type.
    auto extension_pos = filename_in.find_last_of('.');
    if (extension_pos == std::string::npos) {
        std::cerr << "InkFileExportCmd::get_filename_out: cannot determine input file type from filename extension: " << filename_in << std::endl;
        return (std::string());
    }

    std::string extension = filename_in.substr(extension_pos+1);
    if (export_overwrite && export_type_current_native == extension) {
        return filename_in;
    } else {
        std::string tag;
        if (export_type_current_native == extension) {
            tag = "_out";
        }
        if (!object_id.empty()) {
            tag = "_" + object_id;
        }
        return filename_in.substr(0, extension_pos) + tag + "." + export_type_current_native;
    }
}

/**
 *  Perform an SVG export
 *
 *  \param doc Document to export.
 *  \param export_filename Filename for export
 */
int InkFileExportCmd::do_export_svg(SPDocument *doc, std::string const &export_filename)
{
    Inkscape::Extension::Output *oext;
    if (export_plain_svg) {
        oext =
            dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get("org.inkscape.output.svg.plain"));
    } else {
        oext = dynamic_cast<Inkscape::Extension::Output *>(
            Inkscape::Extension::db.get("org.inkscape.output.svg.inkscape"));
    }
    return do_export_vector(doc, export_filename, *oext);
}
/**
 *  Perform a vector file export (SVG, PDF, or PS)
 *
 *  \param doc Document to export.
 *  \param export_filename Filename for export
 *  \param extension Output extension used for exporting
 */
int InkFileExportCmd::do_export_vector(SPDocument *doc, std::string const &export_filename,
                                       Inkscape::Extension::Output &extension)
{
    // Start with options that are once per document.
    if (export_text_to_path) {
        Inkscape::convert_text_to_curves(doc);
    }

    // Export pages instead of objects
    if (!export_page.empty()) {
        auto &pm = doc->getPageManager();
        std::string base = export_filename;
        std::string ext = "svg";
        // Strip any possible extension
        std::string tmp_out = get_filename_out(export_filename, "");
        auto extension_pos = tmp_out.find_last_of('.');
        if (extension_pos != std::string::npos) {
            base = tmp_out.substr(0, extension_pos);
            ext = tmp_out.substr(extension_pos+1);
        }

        auto pages = Inkscape::parseIntRange(export_page, 1, pm.getPageCount());
        for (auto page_num : pages) {
            // And if only one page is selected then we assume the user knows the filename they intended.
            std::string filename_out = base + (pages.size() > 1 ? "_p" + std::to_string(page_num) : "") + "." + ext;

            auto copy_doc = doc->copy();
            copy_doc->prunePages(std::to_string(page_num), true);
            copy_doc->ensureUpToDate();
            copy_doc->vacuumDocument();

            try {
                extension.set_gui(false);
                Inkscape::Extension::save(&extension, copy_doc.get(),
                                          filename_out.c_str(), false, false, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);
            } catch (Inkscape::Extension::Output::save_failed const &) {
                std::cerr << "InkFileExportCmd::do_export_vector: Failed to save " << (export_plain_svg ? "" : "Inkscape")
                          << " file to: " << filename_out << std::endl;
                return 1;
            }
        }
        return 0;
    }

    // Export each object in list (or root if empty).  Use ';' so in future it could be possible to selected multiple objects to export together.
    std::vector<Glib::ustring> objects = Glib::Regex::split_simple("\\s*;\\s*", export_id);
    if (objects.empty()) {
        objects.emplace_back(); // So we do loop at least once for root.
    }

    for (auto const &object : objects) {
        auto copy_doc = doc->copy();

        std::string filename_out = get_filename_out(export_filename, Glib::filename_from_utf8(object));
        if (filename_out.empty()) {
            return 1;
        }

        SPObject *obj = nullptr;
        if(!object.empty()) {
            copy_doc->ensureUpToDate();

            // "crop" the document to the specified object, cleaning as we go.
            obj = copy_doc->getObjectById(object);
            if (obj == nullptr) {
                std::cerr << "InkFileExportCmd::do_export_vector: Object " << object.raw() << " not found in document, nothing to export." << std::endl;
                return 1;
            }
        }

        fit_to_export_area(copy_doc.get(), obj);

        try {
            extension.set_gui(false);
            Inkscape::Extension::save(&extension, copy_doc.get(),
                                      filename_out.c_str(), false, false,
                                      export_plain_svg ? Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY
                                                       : Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG);
        } catch (Inkscape::Extension::Output::save_failed &e) {
            std::cerr << "InkFileExportCmd::do_export_vector: Failed to save " << (export_plain_svg ? "" : "Inkscape")
                      << " file to: " << filename_out << std::endl;
            return 1;
        }
    }
    return 0;
}

Inkscape::Colors::Color InkFileExportCmd::get_bgcolor(SPDocument *doc) {
    Inkscape::Colors::Color bgcolor(0xffffffff);
    if (!export_background.empty()) {
        // override the page color
        if (auto c = Inkscape::Color::parse(export_background)) {
            bgcolor = *c;
        }
        // default is opaque if a color is given on commandline
        if (export_background_opacity < -.5 ) {
            export_background_opacity = 255;
        }
    } else {
        // read from namedview
        Inkscape::XML::Node *nv = doc->getReprNamedView();
        if (nv && nv->attribute("pagecolor")){
            if (auto c = Inkscape::Color::parse(nv->attribute("pagecolor"))) {
                bgcolor = *c;
            }
        }
    }

    if (export_background_opacity > -.5) { // if the value is manually set
        if (export_background_opacity > 1.0) {
            float value = CLAMP (export_background_opacity, 1.0f, 255.0f);
            bgcolor.addOpacity(floor(value) / 255.0);
        } else {
            float value = CLAMP (export_background_opacity, 0.0f, 1.0f);
            bgcolor.addOpacity(value);
        }
    } else {
        Inkscape::XML::Node *nv = doc->getReprNamedView();
        if (nv && nv->attribute("inkscape:pageopacity")){
            bgcolor.addOpacity(nv->getAttributeDouble("inkscape:pageopacity", 1.0));
        } // else it's transparent
    }
    return bgcolor;
}

/**
 *  Perform a PNG export
 *
 *  \param doc Document to export.
 *  \param export_filename filename for export
 */
int
InkFileExportCmd::do_export_png(SPDocument *doc, std::string const &export_filename)
{
    bool filename_from_hint = false;
    gdouble dpi = 0.0;

    auto prefs = Inkscape::Preferences::get();
    bool old_dither = prefs->getBool("/options/dithering/value", true);
    prefs->setBool("/options/dithering/value", export_png_use_dithering);

    // Export each object in list (or root if empty).  Use ';' so in future it could be possible to selected multiple objects to export together.
    std::vector<Glib::ustring> objects = Glib::Regex::split_simple("\\s*;\\s*", export_id);

    std::vector<SPItem const *> items;
    std::vector<Glib::ustring> objects_found;
    for (auto const &object_id : objects) {
        // Find export object. (Either root or object with specified id.)
        auto object = doc->getObjectById(object_id);

        if (!object) {
            std::cerr << "InkFileExport::do_export_png: "
                      << "Object with id=\"" << object_id.raw()
                      << "\" was not found in the document. Skipping." << std::endl;
            continue;
        }

        if (!is<SPItem>(object)) {
            std::cerr << "InkFileExportCmd::do_export_png: "
                      << "Object with id=\"" << object_id.raw()
                      << "\" is not a visible item. Skipping." << std::endl;
            continue;
        }

        items.push_back(cast<SPItem>(object));
        objects_found.push_back(object_id);
    }

    // Export pages instead of objects
    if (!export_page.empty()) {
        auto &pm = doc->getPageManager();
        std::string base = export_filename;
        // Strip any possible extension
        auto extension_pos = export_filename.find_last_of('.');
        if (extension_pos != std::string::npos)
            base = export_filename.substr(0, extension_pos);

        auto pages = Inkscape::parseIntRange(export_page, 1, pm.getPageCount());
        for (auto page_num : pages) {
            // We always use the png extension and ignore the extension given by the user
            // And if only one page is selected then we assume the user knows the filename they intended.
            std::string filename_out = base + (pages.size() > 1 ? "_p" + std::to_string(page_num) : "") + ".png";
            if (auto page = pm.getPage(page_num - 1)) {
                do_export_png_now(doc, filename_out, page->getDesktopRect(), dpi, items);
            }
        }
        return 0;
    }

    if (objects.empty()) {
        objects_found.emplace_back(); // So we do loop at least once for root.
    }

    for (auto const &object_id : objects_found) {
        auto copy_doc = doc->copy();

        SPObject *object = copy_doc->getRoot();
        if (!object_id.empty()) {
            object = copy_doc->getObjectById(object_id);
        }

        std::string filename_out = get_filename_out(export_filename, Glib::filename_from_utf8(object_id));

        // Find file name and dpi from hints.
        if (export_use_hints) {

            // Retrieve export filename hint.
            const gchar *fn_hint = object->getRepr()->attribute("inkscape:export-filename");
            if (fn_hint) {
                filename_out = fn_hint;
                filename_from_hint = true;
            } else {
                std::cerr << "InkFileExport::do_export_png: "
                          << "Export filename hint not found for object " << object_id.raw() << ". Skipping." << std::endl;
                continue;
            }

            // Retrieve export dpi hint. Only xdpi as ydpi is always the same now.
            const gchar *dpi_hint = object->getRepr()->attribute("inkscape:export-xdpi");
            if (dpi_hint) {
                if (export_dpi || export_width || export_height) {
                    std::cerr << "InkFileExport::do_export_png: "
                              << "Using bitmap dimensions from the command line "
                              << "(--export-dpi, --export-width, or --export-height). "
                              << "DPI hint " << dpi_hint << " is ignored." << std::endl;
                } else {
                    dpi = g_ascii_strtod(dpi_hint, nullptr);
                }
            } else {
                std::cerr << "InkFileExport::do_export_png: "
                          << "Export DPI hint not found for the object." << std::endl;
            }
        }

        // ------------------------- File name -------------------------

        // Check we have a filename.
        if (filename_out.empty()) {
            std::cerr << "InkFileExport::do_export_png: "
                      << "No valid export filename given and no filename hint. Skipping." << std::endl;
            continue;
        }

        //Make relative paths go from the document location, if possible:
        if (filename_from_hint && !Glib::path_is_absolute(filename_out) && copy_doc->getDocumentFilename()) {
            std::string dirname = Glib::path_get_dirname(copy_doc->getDocumentFilename());
            if (!dirname.empty()) {
                filename_out = Glib::build_filename(dirname, filename_out);
            }
        }

        // Check if directory exists
        std::string directory = Glib::path_get_dirname(filename_out);
        if (!Glib::file_test(directory, Glib::FileTest::IS_DIR)) {
            std::cerr << "File path " << filename_out << " includes directory that doesn't exist. Skipping." << std::endl;
            continue;
        }

        fit_to_export_area(copy_doc.get(), object_id.empty() ? nullptr : object);
        auto const area = *copy_doc->preferredBounds(); // whole document after fitting

        do_export_png_now(copy_doc.get(), filename_out, area, dpi, items);

    } // End loop over objects.
    prefs->setBool("/options/dithering/value", old_dither);
    return 0;
}

/**
 * @param filename_out Filename and path. Value is UTF8 encoded.
 */
void
InkFileExportCmd::do_export_png_now(SPDocument *doc, std::string const &filename_out, Geom::Rect area, double dpi_in, const std::vector<SPItem const *> &items)
{
    // -------------------------- DPI -------------------------------

    double dpi = dpi_in;

    if (export_dpi != 0.0 && dpi == 0.0) {
        dpi = export_dpi;
        if ((dpi < 0.1) || (dpi > 10000.0)) {
            std::cerr << "InkFileExport::do_export_png: "
                      << "DPI value " << export_dpi
                      << " out of range [0.1 - 10000.0]. Skipping.";
            return;
        }
    }

    // default dpi
    if (dpi == 0.0) {
        dpi = Inkscape::Util::Quantity::convert(1, "in", "px");
    }

        // -------------------------- Width and Height ---------------------------------

        unsigned long int width = 0;
        unsigned long int height = 0;
        double xdpi = dpi;
        double ydpi = dpi;

        if (export_height != 0) {
            height = export_height;
            if ((height < 1) || (height > PNG_UINT_31_MAX)) {
                std::cerr << "InkFileExport::do_export_png: "
                          << "Export height " << height << " out of range (1 to " << PNG_UINT_31_MAX << ")" << std::endl;
                return;
            }
            ydpi = Inkscape::Util::Quantity::convert(height, "in", "px") / area.height();
            xdpi = ydpi;
            dpi = ydpi;
        }

        if (export_width != 0) {
            width = export_width;
            if ((width < 1) || (width > PNG_UINT_31_MAX)) {
                std::cerr << "InkFileExport::do_export_png: "
                          << "Export width " << width << " out of range (1 to " << PNG_UINT_31_MAX << ")." << std::endl;
                return;
            }
            xdpi = Inkscape::Util::Quantity::convert(width, "in", "px") / area.width();
            ydpi = export_height ? ydpi : xdpi;
            dpi = xdpi;
        }

        if (width == 0) {
            width = (unsigned long int) (Inkscape::Util::Quantity::convert(area.width(), "px", "in") * dpi + 0.5);
        }

        if (height == 0) {
            height = (unsigned long int) (Inkscape::Util::Quantity::convert(area.height(), "px", "in") * dpi + 0.5);
        }

        if ((width < 1) || (height < 1) || (width > PNG_UINT_31_MAX) || (height > PNG_UINT_31_MAX)) {
            std::cerr << "InkFileExport::do_export_png: Dimensions " << width << "x" << height << " are out of range (1 to " << PNG_UINT_31_MAX << ")." << std::endl;
            return;
        }

        // -------------------------- Bit Depth and Color Type --------------------

        int bit_depth = 8; // default of sp_export_png_file function
        int color_type = PNG_COLOR_TYPE_RGB_ALPHA; // default of sp_export_png_file function

        if (!export_png_color_mode.empty()) {
            // data as in ui/dialog/export.cpp:
            const std::map<std::string, std::pair<int, int>> color_modes = {
                {"Gray_1", {PNG_COLOR_TYPE_GRAY, 1}},
                {"Gray_2", {PNG_COLOR_TYPE_GRAY, 2}},
                {"Gray_4", {PNG_COLOR_TYPE_GRAY, 4}},
                {"Gray_8", {PNG_COLOR_TYPE_GRAY, 8}},
                {"Gray_16", {PNG_COLOR_TYPE_GRAY, 16}},
                {"RGB_8", {PNG_COLOR_TYPE_RGB, 8}},
                {"RGB_16", {PNG_COLOR_TYPE_RGB, 16}},
                {"GrayAlpha_8", {PNG_COLOR_TYPE_GRAY_ALPHA, 8}},
                {"GrayAlpha_16", {PNG_COLOR_TYPE_GRAY_ALPHA, 16}},
                {"RGBA_8", {PNG_COLOR_TYPE_RGB_ALPHA, 8}},
                {"RGBA_16", {PNG_COLOR_TYPE_RGB_ALPHA, 16}},
            };
            auto it = color_modes.find(export_png_color_mode);
            if (it == color_modes.end()) {
                std::cerr << "InkFileExport::do_export_png: "
                          << "Color mode " << export_png_color_mode.raw() << " is invalid. It must be one of Gray_1/Gray_2/Gray_4/Gray_8/Gray_16/RGB_8/RGB_16/GrayAlpha_8/GrayAlpha_16/RGBA_8/RGBA_16." << std::endl;
                return;
            } else {
                std::tie(color_type, bit_depth) = it->second;
            }
        }

        auto bgcolor = get_bgcolor(doc);
        // ----------------------  Generate the PNG -------------------------------
#ifdef DEBUG
        std::cerr << "Background: " << bgcolor.toString() << std::endl;
        std::cerr << "Area "
                  << area[Geom::X][0] << ":" << area[Geom::Y][0] << ":"
                  << area[Geom::X][1] << ":" << area[Geom::Y][1] << " exported to "
                  << width << " x " << height << " pixels (" << dpi << " dpi)" << std::endl;
#endif

        // -------------------------- Compression level ---------------------------

        if (export_png_compression < 0 || export_png_compression > 9) {
            std::cerr << "InkFileExport::do_export_png: "
                      << "Compression level " << export_png_compression
                      << " out of range [0 - 9]. Skipping.";
            return;
        }

        // ---------------------------- Antialias level ---------------------------

        if (export_png_antialias < 0 || export_png_antialias > 3) {
            std::cerr << "InkFileExport::do_export_png: "
                      << "Antialias level " << export_png_antialias
                      << " out of range [0 - 3]. Skipping.";
            return;
        }

        // Pass an empty items vector, because we've already cropped to just the item we care about.
        if( sp_export_png_file(doc, filename_out.c_str(), area, width, height, xdpi, ydpi,
                               bgcolor, nullptr, nullptr, true, std::vector<SPItem const *>(),
                               false, color_type, bit_depth, export_png_compression, export_png_antialias) == 1 ) {
        } else {
            std::cerr << "InkFileExport::do_export_png: Failed to export to " << filename_out << std::endl;
        }
}


/**
 *  Perform a PDF/PS/EPS export
 *
 *  \param doc Document to export.
 *  \param filename File to write to.
 *  \param mime MIME type to export as.
 */
int
InkFileExportCmd::do_export_ps_pdf(SPDocument* doc, std::string const &filename_in, std::string const & mime_type)
{
    // Check if we support mime type.
    Inkscape::Extension::DB::OutputList o;
    Inkscape::Extension::db.get_output_list(o);
    Inkscape::Extension::DB::OutputList::const_iterator i = o.begin();
    while (i != o.end() && strcmp( (*i)->get_mimetype(), mime_type.c_str() ) != 0) {
        ++i;
    }

    if (i == o.end()) {
        std::cerr << "InkFileExportCmd::do_export_ps_pdf: Could not find an extension to export to MIME type: " << mime_type << std::endl;
        return 1;
    }
    return do_export_ps_pdf(doc, filename_in, mime_type, *dynamic_cast<Inkscape::Extension::Output *>(*i));
}

/**
 *  Perform a PDF/PS/EPS export
 *
 *  \param doc Document to export.
 *  \param filename File to write to.
 *  \param mime MIME type to export as.
 *  \param Extension used for exporting
 */
int InkFileExportCmd::do_export_ps_pdf(SPDocument *doc, std::string const &filename_in, std::string const & mime_type,
                                       Inkscape::Extension::Output &extension)
{
    // check if the passed extension conforms to the mime type.
    assert(std::string(extension.get_mimetype()) == mime_type);
    bool const ai_interchange = std::string(extension.get_id()) == "org.inkscape.output.ai.pdf";
    // Start with options that are once per document.

    // Set export options.
    if (export_text_to_path) {
        extension.set_param_optiongroup("textToPath", "paths");
    } else if (ai_interchange) {
        // Do not inherit "paths" from an earlier CLI export or GUI choice.
        extension.set_param_optiongroup("textToPath", "embed");
    } else if (export_latex && extension.get_param_optiongroup_contains("textToPath", "LaTeX")) {
        extension.set_param_optiongroup("textToPath", "LaTeX");
    }

    if (export_ignore_filters) {
        extension.set_param_bool("blurToBitmap", false);
    } else {
        extension.set_param_bool("blurToBitmap", true);

        // The Illustrator interchange preset uses print-sized effect rasters.
        gdouble dpi = ai_interchange ? 300.0 : 96.0;
        if (export_dpi) {
            dpi = export_dpi;
            if ((dpi < 1) || (dpi > 10000.0)) {
                g_warning("DPI value %lf out of range [1 - 10000]. Using 96 dpi instead.", export_dpi);
                dpi = 96;
            }
        }

        extension.set_param_int("resolution", (int)dpi);
    }

    // handle --export-pdf-version
    if (ai_interchange) {
        // The generic CLI defaults to PDF 2.0. This format deliberately uses
        // Cairo's supported older versions, regardless of that generic default.
        extension.set_param_optiongroup("PDFversion", export_pdf_level == "1.4" ? "PDF-1.4" : "PDF-1.5");
    } else if (mime_type == "application/pdf") {
        bool set_export_pdf_version_fail = true;
        const gchar *pdfver_param_name = "PDFversion";
        if (!export_pdf_level.empty()) {
            // combine "PDF " and the given command line
            std::string version_gui_string = std::string("PDF-") + export_pdf_level.raw();
            try {
                // first, check if the given pdf version is selectable in the ComboBox
                if (extension.get_param_optiongroup_contains("PDFversion", version_gui_string.c_str())) {
                    extension.set_param_optiongroup(pdfver_param_name, version_gui_string.c_str());
                    set_export_pdf_version_fail = false;
                } else {
                    g_warning("Desired PDF export version \"%s\" not supported! Hint: input one of the versions found in the pdf export dialog e.g. \"2.0\".",
                              export_pdf_level.c_str());
                }
            } catch (...) {
                // can be thrown along the way:
                // throw Extension::param_not_exist();
                // throw Extension::param_not_enum_param();
                g_warning("Parameter or Enum \"%s\" might not exist", pdfver_param_name);
            }
        }

        // set default pdf export version to 1.4, also if something went wrong
        if(set_export_pdf_version_fail) {
            extension.set_param_optiongroup(pdfver_param_name, "PDF-1.4");
        }
    }

    if (mime_type == "image/x-postscript" || mime_type == "image/x-e-postscript") {
        if ( export_ps_level < 2 || export_ps_level > 3 ) {
            g_warning("Only supported PostScript levels are 2 and 3."
                      " Defaulting to 2.");
            export_ps_level = 2;
        }

        extension.set_param_optiongroup("PSlevel", (export_ps_level == 3) ? "PS3" : "PS2");
    }

    return do_export_vector(doc, filename_in, extension);
}

/**
 *  Export a document using an export extension
 *
 *  \param doc Document to export.
 *  \param filename to export to.
 *  \param output extension used for export
 */
int InkFileExportCmd::do_export_extension(SPDocument *doc, std::string const &filename_in,
                                          Inkscape::Extension::Output *extension)
{
    std::string filename_out = get_filename_out(filename_in);
    if (extension) {
        fit_to_export_area(doc, nullptr);
        extension->set_state(Inkscape::Extension::Extension::STATE_LOADED);
        try {
            extension->set_gui(false);
            extension->save(doc, filename_out.c_str());
        } catch (Inkscape::Extension::Output::save_failed const &) {

            std::cerr << __PRETTY_FUNCTION__ << ": Failed to save " << extension->get_id()
                      << " to: " << filename_out << std::endl;
            return 1;
        }
    }
    return 0;
}

std::string export_area_type_string(ExportAreaType type)
{
    switch (type) {
        case ExportAreaType::Area:
            return "--export-area";
        case ExportAreaType::Page:
            return "--export-area-page";
        case ExportAreaType::Drawing:
            return "--export-area-drawing";
        default:
            return "default";
    }
}

void InkFileExportCmd::set_export_area_type(ExportAreaType type)
{
    if ((export_area_type != ExportAreaType::Unset) && (export_area_type != type)) {
        std::cerr << "Warning: multiple export area types have been set, overriding "
                  << export_area_type_string(export_area_type) << " with " << export_area_type_string(type)
                  << std::endl;
    }
    export_area_type = type;
}

void InkFileExportCmd::set_export_area(const Glib::ustring &area)
{
    export_area = area;
    set_export_area_type(ExportAreaType::Area);
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
