// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * File/Print operations.
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Chema Celorio <chema@celorio.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Bruno Dilly <bruno.dilly@gmail.com>
 *   Stephen Silver <sasilver@users.sourceforge.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Tavmjong Bah
 *
 * Copyright (C) 2006 Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 1999-2016 Authors
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

/** @file
 * @note This file needs to be cleaned up extensively.
 * What it probably needs is to have one .h file for
 * the API, and two or more .cpp files for the implementations.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include <libintl.h>

#include <gtkmm.h>

#include "desktop.h"
#include "document-undo.h"
#include "document-update.h"
#include "event-log.h"
#include "extension/db.h"
#include "extension/effect.h"
#include "extension/input.h"
#include "extension/output.h"
#include "file.h"
#include "id-clash.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "io/file.h"
#include "io/fix-broken-links.h"
#include "io/resource.h"
#include "io/sys.h"
#include "layer-manager.h"
#include "libnrtype/font-lister.h"
#include "message-stack.h"
#include "object/sp-defs.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "path-prefix.h"
#include "rdf.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h" // for sp_svg_transform_write, used in sp_import_document
#include "ui/dialog/choose-file.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/icon-names.h"
#include "ui/interface.h"
#include "ui/tools/tool-base.h"
#include "util/recently-used-fonts.h"
#include "xml/rebase-hrefs.h"
#include "xml/sp-css-attr.h"

using Inkscape::DocumentUndo;
using Inkscape::IO::Resource::TEMPLATES;
using Inkscape::IO::Resource::USER;


/*######################
## N E W
######################*/

/**
 * Create a blank document and add it to the desktop
 * Input: empty string or template file name.
 */
SPDesktop *sp_file_new(const std::string &templ)
{
    auto *app = InkscapeApplication::instance();

    auto doc = app->document_new(templ);
    if (!doc) {
        std::cerr << "sp_file_new: failed to open document: " << templ << std::endl;
    }

    return app->desktopOpen(doc);
}

std::string sp_file_default_template_uri()
{
    return Inkscape::IO::Resource::get_filename(TEMPLATES, "default.svg", true);
}

SPDesktop* sp_file_new_default()
{
    SPDesktop* desk = sp_file_new(sp_file_default_template_uri());
    //rdf_add_from_preferences( SP_ACTIVE_DOCUMENT );

    return desk;
}

/**
 *  Handle prompting user for "do you want to revert"?  Revert on "OK"
 */
void sp_file_revert_dialog()
{
    SPDesktop  *desktop = SP_ACTIVE_DESKTOP;
    g_assert(desktop != nullptr);

    SPDocument *doc = desktop->getDocument();
    g_assert(doc != nullptr);

    Inkscape::XML::Node *repr = doc->getReprRoot();
    g_assert(repr != nullptr);

    gchar const *filename = doc->getDocumentFilename();
    if (!filename) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved yet.  Cannot revert."));
        return;
    }

    bool do_revert = true;
    if (doc->isModifiedSinceSave()) {
        Glib::ustring tmpString = Glib::ustring::compose(_("Changes will be lost! Are you sure you want to reload document %1?"), filename);
        bool response = desktop->warnDialog (tmpString);
        if (!response) {
            do_revert = false;
        }
    }

    bool reverted = false;
    if (do_revert) {
        auto *app = InkscapeApplication::instance();
        reverted = app->document_revert (doc);
    }

    if (reverted) {
        desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Document reverted."));
    } else {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not reverted."));
    }
}


/*######################
## S A V E
######################*/

/**
 * This 'save' function called by the others below
 *
 * \param    official  whether to set :output_module and :modified in the
 *                     document; is true for normal save, false for temporary saves
 */
static bool
file_save(Gtk::Window &parentWindow,
          SPDocument *doc,
          const Glib::RefPtr<Gio::File> file,
          Inkscape::Extension::Extension *key,
          bool checkoverwrite,
          bool official,
          Inkscape::Extension::FileSaveMethod save_method)
{
    if (!doc) { //Safety check
        return false;
    }

    auto path = file->get_path();
    auto display_name = file->get_parse_name();

    try {
        Inkscape::Extension::save(key, doc, file->get_path().c_str(),
                                  checkoverwrite, official,
                                  save_method);
    } catch (Inkscape::Extension::Output::file_read_only &e) {
        auto const text = Glib::ustring::sprintf(_("File %s is write protected. Please remove write protection and try again."), display_name);
        SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        sp_ui_error_dialog(text.c_str());
        return false;
    } catch (Inkscape::Extension::Output::save_failed &e) {
        auto const text = Glib::ustring::sprintf(_("File %s could not be saved."), display_name);
        SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        sp_ui_error_dialog(text.c_str());
        return false;
    } catch (Inkscape::Extension::Output::save_cancelled &e) {
        SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        return false;
    } catch (Inkscape::Extension::Output::export_id_not_found &e) {
        auto const text = Glib::ustring::sprintf(_("File could not be saved:\nNo object with ID '%s' found."), e.id);
        SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        sp_ui_error_dialog(text.c_str());
        return false;
    } catch (Inkscape::Extension::Output::no_overwrite &e) {
        return sp_file_save_dialog(parentWindow, doc, save_method);
    } catch (std::exception &e) {
        auto const text = Glib::ustring::sprintf(_("File %s could not be saved.\n\n"
                                        "The following additional information was returned by the output extension:\n"
                                        "'%s'"), display_name, e.what());
        SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        sp_ui_error_dialog(text.c_str());
        return false;
    } catch (...) {
        g_critical("Extension '%s' threw an unspecified exception.", key ? key->get_id() : nullptr);
        auto const text = Glib::ustring::sprintf(_("File %s could not be saved."), display_name);
        SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        sp_ui_error_dialog(text.c_str());
        return false;
    }

    if (SP_ACTIVE_DESKTOP) {
        if (! SP_ACTIVE_DESKTOP->messageStack()) {
            g_message("file_save: ->messageStack() == NULL. please report to bug #967416");
        }
    } else {
        g_message("file_save: SP_ACTIVE_DESKTOP == NULL. please report to bug #967416");
    }

    auto font_lister = Inkscape::FontLister::get_instance();
    auto recently_used = Inkscape::RecentlyUsedFonts::get();
    recently_used->prepend_to_list(font_lister->get_font_family());
    recently_used->set_continuous_streak(false);

    doc->get_event_log()->rememberFileSave();
    Glib::ustring msg;
    if (doc->getDocumentFilename() == nullptr) {
        msg = Glib::ustring::format(_("Document saved."));
    } else {
        msg = Glib::ustring::format(_("Document saved."), " ", doc->getDocumentFilename());
    }
    SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::NORMAL_MESSAGE, msg.c_str());
    return true;
}

/**
 * Returns an output extension suitable for saving (i.e. not a raster extension).
 */
Inkscape::Extension::Output *get_output_extension_for_save(std::string filename, bool allow_save_copy = false)
{
    Inkscape::Extension::DB::OutputList extension_list;
    Inkscape::Extension::db.get_output_list(extension_list);

    for (auto omod : extension_list) {
        if ((allow_save_copy || !omod->savecopy_only()) && omod->can_save_filename(filename.c_str())) {
            return omod;
        }
    }

    return nullptr;
}

/**
 *  Display a SaveAs dialog.  Save the document if OK pressed.
 */
bool
sp_file_save_dialog(Gtk::Window &parentWindow, SPDocument *doc, Inkscape::Extension::FileSaveMethod save_method)
{
    bool is_copy = (save_method == Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);

    // Note: default_extension has the format "org.inkscape.output.svg.inkscape",
    //       whereas filename_extension only uses ".svg"
    auto default_extension = Inkscape::Extension::get_file_save_extension(save_method);
    auto extension = dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(default_extension.c_str()));

    std::string filename_extension = ".svg";
    if (extension) {
        filename_extension = extension->get_extension(); // Glib::ustring -> std::string FIXME
    }

    std::string save_path = Inkscape::Extension::get_file_save_path(doc, save_method); // Glib::ustring -> std::string FIXME

    if (!Inkscape::IO::file_test(save_path.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
        save_path.clear();
    }

    if (save_path.empty()) {
        save_path = Glib::get_home_dir();
    }

    std::string save_loc = save_path;
    save_loc.append(G_DIR_SEPARATOR_S);

    int i = 1;
    if ( !doc->getDocumentFilename() ) {
        // We are saving for the first time; create a unique default filename
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        Glib::ustring default_filename = prefs->getString("/options/defaultfilename/value", _("drawing"));
        save_loc = save_loc + default_filename + filename_extension;

        while (Inkscape::IO::file_test(save_loc.c_str(), G_FILE_TEST_EXISTS)) {
            save_loc = save_path;
            save_loc.append(G_DIR_SEPARATOR_S);
            save_loc = save_loc + default_filename + Glib::ustring::compose("-%1", i++) + filename_extension;
        }
    } else {
        save_loc.append(Glib::path_get_basename(doc->getDocumentFilename()));

        // The current document might be named not after an svg - e.g. if we opened a png, the
        // document is named after the png. But we want to save it as an svg instead.
        // Or even if the filename is something weird like "my-drawing.blarg" - we don't want to
        // try to save as a ".blarg" file.
        // So we'll swap out whatever the current file-ending is and prompt the user to save with
        // the correct target filename extension.
        Inkscape::IO::swap_file_extension(save_loc, filename_extension);
    }

    // Show the SaveAs dialog.
    const Glib::ustring dialog_title = is_copy ?
        _("Select file to save a copy to") :
        _("Select file to save to");

    // Note, there are currently multiple modules per filename extension (.svg, .dxf, .zip).
    // We cannot distinguish between them.
    std::string basename = Glib::path_get_basename(save_loc);
    std::string dirname = Glib::path_get_dirname(save_loc);
    auto file = Inkscape::choose_file_save( dialog_title, &parentWindow,
                                            Inkscape::UI::Dialog::create_export_filters(true, is_copy),
                                            basename,
                                            dirname);

    if (!file) {
        return false; // Cancelled
    }

    // Set title here (call RDF to ensure metadata and title element are updated).
    // Is this necessary? In 1.4.x, the Windows native dialog shows the title in
    // an entry which can be changed but 1.5.x doesn't allow that.
    gchar* doc_title = doc->getRoot()->title();
    if (doc_title) {
        rdf_set_work_entity(doc, rdf_find_entity("title"), doc_title);
        g_free(doc_title);
    }

    // Find output module from file extension.
    extension = get_output_extension_for_save(file->get_path(), is_copy);

    if (!extension) {
        auto display_name = file->get_parse_name();
        auto const text = Glib::ustring::sprintf(_("No Inkscape extension found to save document (%s).  This may have been caused by an unknown or missing filename extension."), display_name);
        SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        sp_ui_error_dialog(text.c_str());
        return false;
    }

    if (file_save(parentWindow, doc, file, extension, true, !is_copy, save_method)) {
        if (doc->getDocumentFilename()) {
            Glib::RefPtr<Gtk::RecentManager> recent = Gtk::RecentManager::get_default();
            recent->add_item(file->get_uri()); // Gtk4 add_item(file)
        }

        save_path = Glib::path_get_dirname(file->get_path());
        Inkscape::Extension::store_save_path_in_prefs(save_path, save_method);

        return true;
    }

    return false;
}

/**
 * Save a document, displaying a SaveAs dialog if necessary.
 */
bool
sp_file_save_document(Gtk::Window &parentWindow, SPDocument *doc)
{
    if (auto path = doc->getDocumentFilename()) {
        // Determine the extension from the filename, which may not lead to a valid extension.
        // In which case, we'll fall back to the save-as dialog below.
        if (auto ext = get_output_extension_for_save(path)) {
            auto file = Gio::File::create_for_path(path);
            return file_save(parentWindow, doc, file, ext, false, true, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS);
        }
    }

    // In this case `path == nullptr`, therefore, an argument should be given
    // that indicates that the document is the firsttime saved,
    // so that .svg is selected as the default
    // and not the last one "Save as ..." extension used
    return sp_file_save_dialog(parentWindow, doc, Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG);
}

/**
 * Save a document.
 */
bool
sp_file_save(Gtk::Window &parentWindow, gpointer /*object*/, gpointer /*data*/)
{
    if (!SP_ACTIVE_DOCUMENT) {
        return false;
    }

    SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::IMMEDIATE_MESSAGE, _("Saving document..."));

    sp_namedview_document_from_window(SP_ACTIVE_DESKTOP);
    return sp_file_save_document(parentWindow, SP_ACTIVE_DOCUMENT);
}

/**
 *  Save a document, always displaying the SaveAs dialog.
 */
bool
sp_file_save_as(Gtk::Window &parentWindow, gpointer /*object*/, gpointer /*data*/)
{
    if (!SP_ACTIVE_DOCUMENT) {
        return false;
    }

    sp_namedview_document_from_window(SP_ACTIVE_DESKTOP);
    return sp_file_save_dialog(parentWindow, SP_ACTIVE_DOCUMENT, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS);
}

/**
 *  Save a copy of a document, always displaying a sort of SaveAs dialog.
 */
bool
sp_file_save_a_copy(Gtk::Window &parentWindow, gpointer /*object*/, gpointer /*data*/)
{
    if (!SP_ACTIVE_DOCUMENT) {
        return false;
    }

    sp_namedview_document_from_window(SP_ACTIVE_DESKTOP);
    return sp_file_save_dialog(parentWindow, SP_ACTIVE_DOCUMENT, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);
}

/**
 *  Save a copy of a document as template.
 */
bool
sp_file_save_template(Gtk::Window &parentWindow, Glib::ustring name,
    Glib::ustring author, Glib::ustring description, Glib::ustring keywords,
    bool isDefault)
{
    if (!SP_ACTIVE_DOCUMENT || name.length() == 0)
        return true;

    auto document = SP_ACTIVE_DOCUMENT;

    DocumentUndo::ScopedInsensitive _no_undo(document);

    auto root = document->getReprRoot();
    auto xml_doc = document->getReprDoc();

    auto templateinfo_node = xml_doc->createElement("inkscape:templateinfo");
    Inkscape::GC::release(templateinfo_node);

    auto element_node = xml_doc->createElement("inkscape:name");
    Inkscape::GC::release(element_node);

    element_node->appendChild(xml_doc->createTextNode(name.c_str()));
    templateinfo_node->appendChild(element_node);

    if (author.length() != 0) {

        element_node = xml_doc->createElement("inkscape:author");
        Inkscape::GC::release(element_node);

        element_node->appendChild(xml_doc->createTextNode(author.c_str()));
        templateinfo_node->appendChild(element_node);
    }

    if (description.length() != 0) {

        element_node = xml_doc->createElement("inkscape:shortdesc");
        Inkscape::GC::release(element_node);

        element_node->appendChild(xml_doc->createTextNode(description.c_str()));
        templateinfo_node->appendChild(element_node);

    }

    element_node = xml_doc->createElement("inkscape:date");
    Inkscape::GC::release(element_node);

    element_node->appendChild(xml_doc->createTextNode(
        Glib::DateTime::create_now_local().format("%F").c_str()));
    templateinfo_node->appendChild(element_node);

    if (keywords.length() != 0) {

        element_node = xml_doc->createElement("inkscape:keywords");
        Inkscape::GC::release(element_node);

        element_node->appendChild(xml_doc->createTextNode(keywords.c_str()));
        templateinfo_node->appendChild(element_node);

    }

    root->appendChild(templateinfo_node);

    // Escape filenames for windows users, but filenames are not URIs so
    // Allow UTF-8 and don't escape spaces which are popular chars.
    auto encodedName = Glib::uri_escape_string(name, " ", true);
    encodedName.append(".svg");

    auto path = Inkscape::IO::Resource::get_path_string(USER, TEMPLATES, encodedName.c_str());

    auto operation_confirmed = sp_ui_overwrite_file(path);

    auto file = Gio::File::create_for_path(path);

    if (operation_confirmed) {
        file_save(parentWindow, document, file,
            Inkscape::Extension::db.get(".svg"), false, false,
            Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG);

        if (isDefault) {
            // save as "default.svg" by default (so it works independently of UI language), unless
            // a localized template like "default.de.svg" is already present (which overrides "default.svg")
            std::string default_svg_localized = std::string("default.") + _("en") + ".svg";
            path = Inkscape::IO::Resource::get_path_string(USER, TEMPLATES, default_svg_localized.c_str());

            if (!Inkscape::IO::file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
                path = Inkscape::IO::Resource::get_path_string(USER, TEMPLATES, "default.svg");
            }

            file = Gio::File::create_for_path(path);
            file_save(parentWindow, document, file,
                Inkscape::Extension::db.get(".svg"), false, false,
                Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG);
        }
    }

    // remove this node from current document after saving it as template
    root->removeChild(templateinfo_node);

    return operation_confirmed;
}

/*######################
## I M P O R T
######################*/

/**
 * Paste the contents of a document into the active desktop.
 * @param clipdoc The document to paste
 * @param in_place Whether to paste the selection where it was when copied
 * @pre @c clipdoc is not empty and items can be added to the current layer
 */
void sp_import_document(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page)
{
    SPDocument *target_document = desktop->getDocument();
    Inkscape::XML::Node *root = clipdoc->getReprRoot();
    auto layer = desktop->layerManager().currentLayer();
    Inkscape::XML::Node *target_parent = layer->getRepr();

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // Get page manager for on_page pasting, this must be done before selection changes
    Inkscape::PageManager &pm = target_document->getPageManager();
    SPPage *to_page = pm.getSelected();
    Geom::OptRect from_page;
    Inkscape::XML::Node *clipboard = sp_repr_lookup_name(root, "inkscape:clipboard", 1);
    if (clipboard && clipboard->attribute("page-min")) {
        from_page = Geom::OptRect(clipboard->getAttributePoint("page-min"), clipboard->getAttributePoint("page-max"));
    }

    auto *node_after = desktop->getSelection()->topRepr();
    if (node_after && prefs->getBool("/options/paste/aboveselected", true) && node_after != target_parent) {
        target_parent = node_after->parent();

        // find parent group
        for (auto p = target_document->getObjectByRepr(node_after->parent()); p; p = p->parent) {
            if (auto parent_group = cast<SPGroup>(p)) {
                layer = parent_group;
                break;
            }
        }
    } else {
        node_after = target_parent->lastChild();
    }

    Geom::Point offset(0, 0);
    Geom::Rect bbox;
    if (clipboard) {
        Geom::Point min, max;
        min = clipboard->getAttributePoint("min", min);
        max = clipboard->getAttributePoint("max", max);
        bbox = Geom::Rect(min, max) * target_document->dt2doc();
        offset = bbox.min();
    }
    if (!in_place) {
        auto &m = desktop->getNamedView()->snap_manager;
        m.setup(desktop);
        desktop->getTool()->discard_delayed_snap_event();

        // Get offset from mouse pointer to bbox center, snap to grid if enabled
        auto cursor_position = desktop->point() * target_document->dt2doc();
        auto snap_shift = m.multipleOfGridPitch(cursor_position - bbox.midpoint(), bbox.midpoint());
        offset += snap_shift;
        m.unSetup();
    }
    if (on_page && from_page && to_page) {
        auto page_offset = to_page->getDocumentRect().min() - (from_page.value() * target_document->dt2doc()).min();
        offset += page_offset;
    }
    Geom::Affine transform = Geom::Translate(offset);

    // copy objects
    std::vector<Inkscape::XML::Node *> pasted_objects;
    target_document->import(*clipdoc, layer->getRepr(), node_after, transform, &pasted_objects);

    target_document->ensureUpToDate();
    Inkscape::Selection *selection = desktop->getSelection();
    // Change the selection to the freshly pasted objects
    selection->setReprList(pasted_objects);
    target_document->emitReconstructionFinish();
}

/**
 *  Import a resource.  Called by document_import() and Drag and Drop.
 *  The only place 'key' is used non-null is in drag-and-drop of a GDK_TYPE_TEXTURE.
 */
bool file_drop_opens_document(SPDocument *document, std::string const &path)
{
    if (!document || path.size() < 3 || g_ascii_strcasecmp(path.c_str() + path.size() - 3, ".ai")) {
        return false;
    }
    // Empty layers do not turn a blank canvas into an existing drawing. Hidden
    // or locked artwork does: never discard it when deciding how to drop a file.
    auto has_art = [&](auto &&self, SPObject const *object) -> bool {
        for (auto const &child : object->children) {
            if (is<SPGroup>(&child)) {
                if (self(self, &child)) return true;
            } else if (is<SPItem>(&child)) {
                return true;
            }
        }
        return false;
    };
    return !has_art(has_art, document->getRoot());
}

SPObject *file_import(SPDocument *in_doc, std::string const &path, Inkscape::Extension::Extension *key,
                      std::optional<Geom::Point> drop_pos)
{
    SPDesktop *desktop = SP_ACTIVE_DESKTOP;
    bool cancelled = false;
    auto prefs = Inkscape::Preferences::get();

    // Store mouse pointer location before opening any dialogs, so we can drop the item where initially intended.
    auto pointer_location = drop_pos.value_or(desktop->point());

    // We need access to the module locally for our import logic
    if (!key) {
        key = Inkscape::Extension::Input::find_by_filename(path.c_str());
    }

    if (drop_pos && file_drop_opens_document(in_doc, path)) {
        // Use the same open operation as File > Open, including colour mode,
        // artboard coordinates, file identity and replacement of a virgin tab.
        InkscapeApplication::instance()->create_window(Gio::File::create_for_path(path));
        return nullptr;
    }

    // DEBUG_MESSAGE( fileImport, "file_import( in_doc:%p uri:[%s], key:%p", in_doc, uri, key );
    std::unique_ptr<SPDocument> doc;
    try {
        doc = Inkscape::Extension::open(key, path.c_str(), true);
    } catch (Inkscape::Extension::Input::no_extension_found const &) {
    } catch (Inkscape::Extension::Input::open_failed const &) {
    } catch (Inkscape::Extension::Input::open_cancelled const &) {
        cancelled = true;
    }

    bool is_svg = !key || !strcmp(key->get_id(), SP_MODULE_KEY_INPUT_SVG);

    if (!doc) {
        // Open failed or canceled
        if (!cancelled) {
            auto text = Glib::ustring::sprintf(_("Failed to load the requested file %s"), path);
            sp_ui_error_dialog(text.c_str());
        }
        return nullptr;
    }

    if (is_svg && prefs->getString("/dialogs/import/import_mode_svg") == "new") {
        // Special case: "SVG Import mode" is set to "New"
        // (open imported/drag-and-dropped SVGs as new file, do not import them into the current document)
        // --> open and return nothing
        auto *app = InkscapeApplication::instance();
        auto doc_ptr = app->document_add(std::move(doc));
        app->desktopOpen(doc_ptr);
        return nullptr;
    }
    // The extension should set it's pages enabled or disabled when opening
    // in order to indicate if pages are being imported or if objects are.
    if (doc->getPageManager().hasPages()) {
        file_import_pages(in_doc, doc.get());
        DocumentUndo::done(in_doc, RC_("Undo", "Import Pages"), INKSCAPE_ICON("document-import"));
        // This return is only used by dbus in document-interface.cpp (now removed).
        return nullptr;
    }

    // Standard case: Import

    // Determine the place to insert the new object.
    // This will be the current layer, if possible.
    // FIXME: If there's no desktop (command line run?) we need
    //        a document:: method to return the current layer.
    //        For now, we just use the root in this case.
    SPObject *place_to_insert;
    if (desktop) {
        place_to_insert = desktop->layerManager().currentLayer();
    } else {
        place_to_insert = in_doc->getRoot();
    }

    std::vector<Inkscape::XML::Node *> result;

    doc->ensureUpToDate();
    auto const bbox = doc->getRoot()->desktopPreferredBounds().value_or(Geom::Rect());
    auto const bbox_doc = bbox * doc->dt2doc();
    Geom::Affine transform = Geom::Translate(pointer_location * in_doc->dt2doc() - bbox_doc.midpoint());

    in_doc->import(*doc, place_to_insert->getRepr(), nullptr, transform, &result,
                   is_svg ? SPDocument::ImportRoot::Single
                          : SPDocument::ImportRoot::UngroupSingle, // remove groups for imported bitmap images
                   SPDocument::ImportLayersMode::ToGroup);

    SPObject *import_root = nullptr;
    if (!result.empty()) {
        g_assert(result.size() == 1);
        import_root = in_doc->getObjectByRepr(result[0]);
    }
    Inkscape::Selection *selection = desktop->getSelection();
    selection->setReprList(result);
    in_doc->emitReconstructionFinish();
    DocumentUndo::done(in_doc, RC_("Undo", "Import"), INKSCAPE_ICON("document-import"));
    return import_root;
}

/**
 * Import the given document as a set of multiple pages and append to this one.
 *
 * @param this_doc - Our current document, to be changed
 * @param that_doc - The documennt that contains our importable pages
 */
void file_import_pages(SPDocument *this_doc, SPDocument *that_doc)
{
    auto &this_pm = this_doc->getPageManager();
    auto &that_pm = that_doc->getPageManager();

    // Resolve both coordinate systems before creating pages or placing artwork.
    this_doc->ensureUpToDate();
    that_doc->ensureUpToDate();
    this_pm.enablePages();

    Geom::Affine tr = Geom::Translate(this_pm.nextPageLocation() * this_doc->getDocumentScale());
    for (auto &that_page : that_pm.getPages()) {
        auto this_page = this_pm.newDocumentPage(that_page->getDocumentRect() * tr);
        // Set the margin, bleed, etc
        this_page->copyFrom(that_page);
        this_page->getRepr()->setAttribute("inkscape:label", that_page->label());
    }

    this_doc->import(*that_doc, nullptr, nullptr, tr, nullptr);
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
