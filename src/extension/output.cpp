// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   Ted Gould <ted@gould.cx>
 *
 * Copyright (C) 2006 Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 2002-2004 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "output.h"

#include "document.h"
#include "inkscape.h"

#include "io/sys.h"
#include "implementation/implementation.h"

#include "xml/attribute-record.h"
#include "xml/node.h"

/* Inkscape::Extension::Output */

namespace Inkscape {
namespace Extension {

bool Output::prefs()
{
    return prefs(false);
}

bool Output::prefs(bool settings)
{
    if (!INKSCAPE.use_gui() || !get_gui()) return true;
    if (!loaded()) set_state(STATE_LOADED);
    if (!loaded()) return false;
    if (auto result = imp->output_prefs(this, settings)) return *result;
    return Extension::prefs();
}

/**
    \brief    Builds an Output object from a XML description
    \param    in_repr        The XML description in a Inkscape::XML::Node tree
    \param    implementation The implementation of the extension

    Okay, so you want to build an Output object.

    This function looks for the <output> section of the
    XML description.  Under there should be several fields which
    describe the output module to excruciating detail.  Those are parsed,
    copied, and put into the structure that is passed in as module.
    Overall, there are many levels of indentation, just to handle the
    levels of indentation in the XML file.
*/
Output::Output(Inkscape::XML::Node *in_repr, ImplementationHolder implementation, std::string *base_directory)
    : Extension(in_repr, std::move(implementation), base_directory)
{
    mimetype = nullptr;
    extension = nullptr;
    filetypename = nullptr;
    filetypetooltip = nullptr;
    dataloss = true;
    savecopyonly = false;

    if (repr != nullptr) {
        Inkscape::XML::Node * child_repr;

        child_repr = repr->firstChild();

        while (child_repr != nullptr) {
            if (!strcmp(child_repr->name(), INKSCAPE_EXTENSION_NS "output")) {

                for (const auto &iter : child_repr->attributeList()) {
                    std::string name = g_quark_to_string(iter.key);
                    std::string value = std::string(iter.value);
                    if (name == "raster")
                        raster = value == "true";
                    else if (name == "is_exported")
                        exported = value == "true";
                    else if (name == "priority")
                        set_sort_priority(strtol(value.c_str(), nullptr, 0));
                }

                child_repr = child_repr->firstChild();
                while (child_repr != nullptr) {
                    char const * chname = child_repr->name();
					if (!strncmp(chname, INKSCAPE_EXTENSION_NS_NC, strlen(INKSCAPE_EXTENSION_NS_NC))) {
						chname += strlen(INKSCAPE_EXTENSION_NS);
					}
                    if (chname[0] == '_') /* Allow _ for translation of tags */
                        chname++;
                    if (!strcmp(chname, "extension")) {
                        g_free (extension);
                        extension = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "mimetype")) {
                        g_free (mimetype);
                        mimetype = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "filetypename")) {
                        g_free (filetypename);
                        filetypename = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "filetypetooltip")) {
                        g_free (filetypetooltip);
                        filetypetooltip = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "dataloss")) {
                        dataloss = strcmp(child_repr->firstChild()->content(), "false");
                    }
                    if (!strcmp(chname, "savecopyonly")) {
                        savecopyonly = !strcmp(child_repr->firstChild()->content(), "true");
                    }

                    child_repr = child_repr->next();
                }

                break;
            }

            child_repr = child_repr->next();
        }

    }
}

/**
    \brief  Destroy an output extension
*/
Output::~Output ()
{
    g_free(mimetype);
    g_free(extension);
    g_free(filetypename);
    g_free(filetypetooltip);
    return;
}

/**
    \return  Whether this extension checks out
	\brief   Validate this extension

	This function checks to make sure that the output extension has
	a filename extension and a MIME type.  Then it calls the parent
	class' check function which also checks out the implementation.
*/
bool
Output::check ()
{
	if (extension == nullptr)
		return FALSE;
	if (mimetype == nullptr)
		return FALSE;

	return Extension::check();
}

/**
    \return  IETF mime-type for the extension
	\brief   Get the mime-type that describes this extension
*/
gchar const *
Output::get_mimetype() const
{
    return mimetype;
}

/**
    \return  Filename extension for the extension
	\brief   Get the filename extension for this extension
*/
gchar const *
Output::get_extension() const
{
    return extension;
}

/**
    \return  The name of the filetype supported
	\brief   Get the name of the filetype supported
*/
const char *
Output::get_filetypename(bool translated) const
{
    const char *name;

    if (filetypename)
        name = filetypename;
    else
        name = get_name();

    if (name && translated && filetypename) {
        return get_translation(name);
    } else {
        return name;
    }
}

/**
    \return  Tooltip giving more information on the filetype
	\brief   Get the tooltip for more information on the filetype
*/
const char *
Output::get_filetypetooltip(bool translated) const
{
    if (filetypetooltip && translated) {
        return get_translation(filetypetooltip);
    } else {
        return filetypetooltip;
    }
}

/**
    \return  None
	\brief   Save a document as a file
	\param   doc  Document to save
	\param   filename  File to save the document as

	This function does a little of the dirty work involved in saving
	a document so that the implementation only has to worry about getting
	bits on the disk.

	The big thing that it does is remove and read the fields that are
	only used at runtime and shouldn't be saved.  One that may surprise
	people is the output extension.  This is not saved so that the IDs
	could be changed, and old files will still work properly.
*/
void
Output::save(SPDocument *doc, gchar const *filename, bool detachbase)
{
    if (!loaded())
        set_state(Extension::STATE_LOADED);

    if (loaded()) {
        imp->setDetachBase(detachbase);
        auto new_doc = doc->copy();
        new_doc->ensureUpToDate();
        run_processing_actions(new_doc.get());
        imp->save(this, new_doc.get(), filename);
    }
}

/**
    \return  None
    \brief   Save a rendered png as a raster output
    \param   png_filename source png file.
    \param   filename  File to save the raster as

*/
void
Output::export_raster(const SPDocument *doc, std::string png_filename, gchar const *filename, bool detachbase)
{
    if (!loaded())
        set_state(Extension::STATE_LOADED);

    if (loaded()) {
        imp->setDetachBase(detachbase);
        imp->export_raster(this, doc, png_filename, filename);
    }
}

/**
 * Adds a valid extension to the filename if it's missing. Only used by export-single.cpp.
 */
void
Output::add_extension(std::string &filename)
{
    auto current_ext = Inkscape::IO::get_file_extension(filename);
    if (extension && current_ext != extension) {
        filename = filename + extension;
    }
}

/**
    \return  True if the filename matches
    \brief   Match filename to extension that can open it.
*/
bool
Output::can_save_filename(gchar const *filename) const
{
    // Ignore raster Outputs, because those are only used as part of the "export-as" pipeline,
    // not for the "save/save-as" pipeline. Such output extensions don't override save().
    if (is_raster()) {
        return false;
    }

    gchar *filenamelower = g_utf8_strdown(filename, -1);
    gchar *extensionlower = g_utf8_strdown(extension, -1);
    bool result = g_str_has_suffix(filenamelower, extensionlower);
    g_free(filenamelower);
    g_free(extensionlower);
    return result;
}

} }  /* namespace Inkscape, Extension */

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
