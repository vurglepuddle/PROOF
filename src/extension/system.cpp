// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This is file is kind of the junk file.  Basically everything that
 * didn't fit in one of the other well defined areas, well, it's now
 * here.  Which is good in someways, but this file really needs some
 * definition.  Hopefully that will come ASAP.
 *
 * Authors:
 *   Ted Gould <ted@gould.cx>
 *   Johan Engelen <johan@shouraizou.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2006-2007 Johan Engelen
 * Copyright (C) 2002-2004 Ted Gould
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "system.h"

#include <glibmm/miscutils.h>
#include <glibmm/fileutils.h>

#include "db.h"
#include "document.h"
#include "document-undo.h"
#include "effect.h"
#include "extension.h"
#include "implementation/script.h"
#include "implementation/xslt.h"
#include "inkscape.h"
#include "input.h"
#include "io/sys.h"
#include "loader.h"
#include "output.h"
#include "patheffect.h"
#include "preferences.h"
#include "print.h"
#include "template.h"
#include "ui/interface.h"
#include "xml/rebase-hrefs.h"

namespace Inkscape::Extension {

/**
 * \return   A new document created from the filename passed in
 * \brief    This is a generic function to use the open function of
 *           a module (including Autodetect)
 * \param    key       Identifier of which module to use
 * \param    filename  The file that should be opened
 * \param    is_importing Is the request an import request, for example drag & drop
 *
 * First things first, are we looking at an autodetection?  Well if that's the case then the module
 * needs to be found, and that is done with a database lookup through the module DB.  The foreach
 * function is called, with the parameter being a gpointer array.  It contains both the filename
 * (to find its extension) and where to write the module when it is found.
 *
 * If there is no autodetection, then the module database is queried with the key given.
 *
 * If everything is cool at this point, the module is loaded, and there is possibility for
 * preferences.  If there is a function, then it is executed to get the dialog to be displayed.
 * After it is finished the function continues.
 *
 * Lastly, the open function is called in the module itself.
 */
std::unique_ptr<SPDocument> open(Extension *key, char const *filename, bool is_importing)
{
    Input *imod = dynamic_cast<Input *>(key ? key : Input::find_by_filename(filename));

    bool last_chance_svg = false;
    if (!key && !imod) {
        last_chance_svg = true;
        imod = dynamic_cast<Input *>(db.get(SP_MODULE_KEY_INPUT_SVG));
    }

    if (!imod) {
        throw Input::no_extension_found();
    }

    // Hide pixbuf extensions depending on user preferences.
    //g_warning("Extension: %s", imod->get_id());

    bool show = true;
    if (std::strlen(imod->get_id()) > 21) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        bool ask = prefs->getBool("/dialogs/import/ask");
        bool ask_svg = prefs->getBool("/dialogs/import/ask_svg");
        Glib::ustring id = Glib::ustring(imod->get_id(), 22);
        if (id.compare("org.inkscape.input.svg") == 0) {
            if (ask_svg && is_importing) {
                show = true;
                imod->set_gui(true);
            } else {
                show = false;
                imod->set_gui(false);
            }
        } else if(strlen(imod->get_id()) > 27) {
            id = Glib::ustring(imod->get_id(), 28);
            if (!ask && id.compare( "org.inkscape.input.gdkpixbuf") == 0) {
                show = false;
                imod->set_gui(false);
            }
        }
    }
    imod->set_state(Extension::STATE_LOADED);

    if (!imod->loaded()) {
        throw Input::open_failed();
    }

    if (!imod->prefs()) {
        throw Input::open_cancelled();
    }

    auto doc = imod->open(filename, is_importing);

    if (!doc) {
        if (last_chance_svg) {
            if ( INKSCAPE.use_gui() ) {
                sp_ui_error_dialog(_("Could not detect file format. Tried to open it as an SVG anyway but this also failed."));
            } else {
                g_warning("%s", _("Could not detect file format. Tried to open it as an SVG anyway but this also failed."));
            }
        }
        throw Input::open_failed();
    }
    // If last_chance_svg is true here, it means we successfully opened a file as an svg
    // and there's no need to warn the user about it, just do it.

    doc->setDocumentFilename(filename);
    if (!show) {
        imod->set_gui(true);
    }

    return doc;
}

/**
 * \return   None
 * \brief    This is a generic function to use the save function of
 *           a module (including Autodetect)
 * \param    key       Identifier of which module to use
 * \param    doc       The document to be saved
 * \param    filename  The file that the document should be saved to
 * \param    official  (optional) whether to set :output_module and :modified in the
 *                     document; is true for normal save, false for temporary saves
 *
 * The module database is queried with the key given.
 *
 * If everything is cool at this point, the module is loaded, and there is possibility for
 * preferences.  If there is a function, then it is executed to get the dialog to be displayed.
 * After it is finished the function continues.
 *
 * Lastly, the save function is called in the module itself.
 */
void
save(Extension *key, SPDocument *doc, gchar const *filename, bool check_overwrite, bool official,
    Inkscape::Extension::FileSaveMethod save_method)
{
    assert(key);

    Output *omod = dynamic_cast<Output *>(key);
    if (!omod) {
        g_warning("No valid output module provided to handle file: %s\n", filename);
        throw Output::no_extension_found();
    }

    omod->set_state(Extension::STATE_LOADED);
    if (!omod->loaded()) {
        throw Output::save_failed();
    }

    if (!omod->prefs()) {
        throw Output::save_cancelled();
    }

    if (check_overwrite && !sp_ui_overwrite_file(filename)) {
        throw Output::no_overwrite();
    }

    // test if the file exists and is writable
    // the test only checks the file attributes and might pass where ACL does not allow writes
    if (Inkscape::IO::file_test(filename, G_FILE_TEST_EXISTS) && !Inkscape::IO::file_is_writable(filename)) {
        throw Output::file_read_only();
    }

    Inkscape::XML::Node *repr = doc->getReprRoot();


    // remember attributes in case this is an unofficial save and/or overwrite fails
    gchar *saved_filename = g_strdup(doc->getDocumentFilename());
    gchar *saved_output_extension = nullptr;
    gchar *saved_dataloss = nullptr;
    bool saved_modified = doc->isModifiedSinceSave();
    saved_output_extension = g_strdup(get_file_save_extension(save_method).c_str());
    saved_dataloss = g_strdup(repr->attribute("inkscape:dataloss"));
    if (official) {
        // The document is changing name/uri.
        doc->changeFilenameAndHrefs(filename);
    }

    // Update attributes:
    {
        {
            DocumentUndo::ScopedInsensitive _no_undo(doc);
            // also save the extension for next use
            store_file_extension_in_prefs (omod->get_id(), save_method);
            // set the "dataloss" attribute if the chosen extension is lossy
            repr->removeAttribute("inkscape:dataloss");
            if (omod->causes_dataloss()) {
                repr->setAttribute("inkscape:dataloss", "true");
            }
        }
        doc->setModifiedSinceSave(false);
    }

    try {
        omod->save(doc, filename);
    }
    catch(...) {
        // revert attributes in case of official and overwrite
        if(check_overwrite && official) {
            {
                DocumentUndo::ScopedInsensitive _no_undo(doc);
                store_file_extension_in_prefs (saved_output_extension, save_method);
                repr->setAttribute("inkscape:dataloss", saved_dataloss);
            }
            doc->changeFilenameAndHrefs(saved_filename);
        }
        doc->setModifiedSinceSave(saved_modified);
        // free used resources
        g_free(saved_output_extension);
        g_free(saved_dataloss);
        g_free(saved_filename);

        throw;
    }

    // If it is an unofficial save, set the modified attributes back to what they were.
    if ( !official) {
        {
            DocumentUndo::ScopedInsensitive _no_undo(doc);
            store_file_extension_in_prefs (saved_output_extension, save_method);
            repr->setAttribute("inkscape:dataloss", saved_dataloss);
        }
        doc->setModifiedSinceSave(saved_modified);

        g_free(saved_output_extension);
        g_free(saved_dataloss);
    }

    return;
}

Print *
get_print(gchar const *key)
{
    return dynamic_cast<Print *>(db.get(key));
}

/**
 * \return   true if extension successfully parsed, false otherwise
 *           A true return value does not guarantee an extension was actually registered,
 *           but indicates no errors occurred while parsing the extension.
 * \brief    Creates a module from a Inkscape::XML::Document describing the module
 * \param    doc  The XML description of the module
 *
 * This function basically has two segments.  The first is that it goes through the Repr tree
 * provided, and determines what kind of module this is, and what kind of implementation to use.
 * All of these are then stored in two enums that are defined in this function.  This makes it
 * easier to add additional types (which will happen in the future, I'm sure).
 *
 * Second, there is case statements for these enums.  The first one is the type of module.  This is
 * the one where the module is actually created.  After that, then the implementation is applied to
 * get the load and unload functions.  If there is no implementation then these are not set.  This
 * case could apply to modules that are built in (like the SVG load/save functions).
 */
bool
build_from_reprdoc(Inkscape::XML::Document *doc, std::unique_ptr<Implementation::Implementation> in_imp, std::string* baseDir, std::string* file_name)
{
    ModuleImpType module_implementation_type = MODULE_UNKNOWN_IMP;
    ModuleFuncType module_functional_type = MODULE_UNKNOWN_FUNC;

    g_return_val_if_fail(doc != nullptr, false);

    Inkscape::XML::Node *repr = doc->root();

    if (strcmp(repr->name(), INKSCAPE_EXTENSION_NS "inkscape-extension")) {
        g_warning("Extension definition started with <%s> instead of <" INKSCAPE_EXTENSION_NS "inkscape-extension>.  Extension will not be created. See http://wiki.inkscape.org/wiki/index.php/Extensions for reference.\n", repr->name());
        return false;
    }

    Inkscape::XML::Node *child_repr = repr->firstChild();
    while (child_repr != nullptr) {
        char const *element_name = child_repr->name();
        /* printf("Child: %s\n", child_repr->name()); */
        if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "input")) {
            module_functional_type = MODULE_INPUT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "template")) {
            module_functional_type = MODULE_TEMPLATE;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "output")) {
            module_functional_type = MODULE_OUTPUT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "effect")) {
            module_functional_type = MODULE_FILTER;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "print")) {
            module_functional_type = MODULE_PRINT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "path-effect")) {
            module_functional_type = MODULE_PATH_EFFECT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "script")) {
            module_implementation_type = MODULE_EXTENSION;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "xslt")) {
            module_implementation_type = MODULE_XSLT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "plugin")) {
            module_implementation_type = MODULE_PLUGIN;
        }

        //Inkscape::XML::Node *old_repr = child_repr;
        child_repr = child_repr->next();
        //Inkscape::GC::release(old_repr);
    }

    using ImplementationHolder = Util::HybridPointer<Implementation::Implementation>;
    ImplementationHolder imp;
    if (in_imp) {
        imp = std::move(in_imp);
    } else {
        switch (module_implementation_type) {
            case MODULE_EXTENSION: {
                imp = ImplementationHolder::make_owning<Implementation::Script>();
                break;
            }
            case MODULE_XSLT: {
                imp = ImplementationHolder::make_owning<Implementation::XSLT>();
                break;
            }
            case MODULE_PLUGIN: {
                auto loader = Inkscape::Extension::Loader();
                if (baseDir) {
                    loader.set_base_directory(*baseDir);
                }
                imp = ImplementationHolder::make_nonowning(loader.load_implementation(doc));
                break;
            }
        }
    }

    std::unique_ptr<Extension> module;
    try {
        switch (module_functional_type) {
            case MODULE_INPUT: {
                module = std::make_unique<Input>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_TEMPLATE: {
                module = std::make_unique<Template>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_OUTPUT: {
                module = std::make_unique<Output>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_FILTER: {
                module = std::make_unique<Effect>(repr, std::move(imp), baseDir, file_name);
                break;
            }
            case MODULE_PRINT: {
                module = std::make_unique<Print>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_PATH_EFFECT: {
                module = std::make_unique<PathEffect>(repr, std::move(imp), baseDir);
                break;
            }
            default: {
                g_warning("Extension of unknown type!"); // TODO: Should not happen! Is this even useful?
                module = std::make_unique<Extension>(repr, std::move(imp), baseDir);
                break;
            }
        }
    } catch (const Extension::extension_no_id &) {
        g_warning("Building extension failed. Extension does not have a valid ID");
        return false;
    } catch (const Extension::extension_no_name &) {
        g_warning("Building extension failed. Extension does not have a valid name");
        return false;
    } catch (const Extension::extension_not_compatible &) {
        return true; // This is not an actual error; just silently ignore the extension
    } catch (Extension::no_implementation_for_extension &) {
        g_warning("Building extension failed: no implementation was found");
        return false;
    }

    assert(module);
    db.take_ownership(std::move(module));
    return true;
}

/**
 * \brief    This function creates a module from a filename of an
 *           XML description.
 * \param    filename  The file holding the XML description of the module.
 *
 * This function calls build_from_reprdoc with using sp_repr_read_file to create the reprdoc.
 */
void
build_from_file(gchar const *filename)
{
    std::string dir = Glib::path_get_dirname(filename);
    auto file_name = Glib::path_get_basename(filename);

    Inkscape::XML::Document *doc = sp_repr_read_file(filename, INKSCAPE_EXTENSION_URI);
    if (!doc) {
        g_critical("Inkscape::Extension::build_from_file() - XML description loaded from '%s' not valid.", filename);
        return;
    }

    if (!build_from_reprdoc(doc, {}, &dir, &file_name)) {
        g_warning("Inkscape::Extension::build_from_file() - Could not parse extension from '%s'.", filename);
    }

    Inkscape::GC::release(doc);
}

/**
 * \brief Create a module from a buffer holding an XML description.
 * \param buffer The buffer holding the XML description of the module.
 * \param in_imp An owning pointer to a freshly created implementation.
 *
 * This function calls build_from_reprdoc with using sp_repr_read_mem to create the reprdoc.  It
 * finds the length of the buffer using strlen.
 */
void
build_from_mem(gchar const *buffer, std::unique_ptr<Implementation::Implementation> in_imp)
{
    Inkscape::XML::Document *doc = sp_repr_read_mem(buffer, strlen(buffer), INKSCAPE_EXTENSION_URI);
    if (!doc) {
        g_critical("Inkscape::Extension::build_from_mem() - XML description loaded from memory buffer not valid.");
        return;
    }

    if (!build_from_reprdoc(doc, std::move(in_imp), nullptr, nullptr)) {
        g_critical("Inkscape::Extension::build_from_mem() - Could not parse extension from memory buffer.");
    }

    Inkscape::GC::release(doc);
}

/*
 * TODO: Is it guaranteed that the returned extension is valid? If so, we can remove the check for
 * filename_extension in sp_file_save_dialog().
 */
Glib::ustring
get_file_save_extension (Inkscape::Extension::FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Glib::ustring extension;
    switch (method) {
        case FILE_SAVE_METHOD_SAVE_AS:
        case FILE_SAVE_METHOD_TEMPORARY:
            extension = prefs->getString("/dialogs/save_as/default");
            break;
        case FILE_SAVE_METHOD_SAVE_COPY:
            extension = prefs->getString("/dialogs/save_copy/default");
            break;
        case FILE_SAVE_METHOD_INKSCAPE_SVG:
            extension = SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE;
            break;
        case FILE_SAVE_METHOD_EXPORT:
            /// \todo no default extension set for Export? defaults to SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE is ok?
            break;
    }

    if(extension.empty()) {
        extension = SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE;
    }

    return extension;
}

Glib::ustring
get_file_save_path (SPDocument *doc, FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    auto directory = [](Glib::ustring const &path) {
        return !path.empty() && Glib::file_test(path, Glib::FileTest::IS_DIR);
    };
    if (method == FILE_SAVE_METHOD_SAVE_AS || method == FILE_SAVE_METHOD_SAVE_COPY ||
        method == FILE_SAVE_METHOD_INKSCAPE_SVG) {
        Glib::ustring document_path;
        if (auto filename = doc->getDocumentFilename(); filename && Glib::path_is_absolute(filename)) {
            auto path = Glib::path_get_dirname(filename);
            if (directory(path)) document_path = path;
        }
        bool const use_current_dir = prefs->getBool("/dialogs/save_as/use_current_dir", true);
        if (use_current_dir && !document_path.empty()) return document_path;
        auto last_path = prefs->getString("/dialogs/file/last_path");
        if (directory(last_path)) return last_path;
        if (!document_path.empty()) return document_path;
        // Open and save dialogs used separate histories (and two spellings of "dialog").
        for (auto key : {"/dialog/open/path", "/dialogs/open/path",
                         method == FILE_SAVE_METHOD_SAVE_COPY ? "/dialogs/save_copy/path" : "/dialogs/save_as/path",
                         "/dialogs/save_as/path"}) {
            auto path = prefs->getString(key);
            if (directory(path)) return path;
        }
    } else if (method == FILE_SAVE_METHOD_TEMPORARY) {
        auto path = prefs->getString("/dialogs/save_as/path");
        if (directory(path)) return path;
    }
    return Glib::get_home_dir();
}

void
store_file_extension_in_prefs (Glib::ustring extension, FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    switch (method) {
        case FILE_SAVE_METHOD_SAVE_AS:
        case FILE_SAVE_METHOD_TEMPORARY:
            prefs->setString("/dialogs/save_as/default", extension);
            break;
        case FILE_SAVE_METHOD_SAVE_COPY:
            prefs->setString("/dialogs/save_copy/default", extension);
            break;
        case FILE_SAVE_METHOD_INKSCAPE_SVG:
        case FILE_SAVE_METHOD_EXPORT:
            // do nothing
            break;
    }
}

void
store_save_path_in_prefs (Glib::ustring path, FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (method != FILE_SAVE_METHOD_TEMPORARY && method != FILE_SAVE_METHOD_EXPORT) {
        prefs->setString("/dialogs/file/last_path", path);
    }
    switch (method) {
        case FILE_SAVE_METHOD_SAVE_AS:
        case FILE_SAVE_METHOD_TEMPORARY:
            prefs->setString("/dialogs/save_as/path", path);
            break;
        case FILE_SAVE_METHOD_SAVE_COPY:
            prefs->setString("/dialogs/save_copy/path", path);
            break;
        case FILE_SAVE_METHOD_INKSCAPE_SVG:
        case FILE_SAVE_METHOD_EXPORT:
            // do nothing
            break;
    }
}

} // namespace Inkscape::Extension

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
