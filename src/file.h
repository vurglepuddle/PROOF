// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_FILE_H
#define SEEN_SP_FILE_H

/*
 * File/Print operations
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Chema Celorio <chema@celorio.com>
 *
 * Copyright (C) 2006 Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 2001-2002 Ximian, Inc.
 * Copyright (C) 1999-2002 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/ustring.h>
#include <optional>
#include <string>
#include <2geom/point.h>
#include "extension/system.h"

class SPDesktop;
class SPDocument;
class SPObject;
class SPRoot;

// Dropping an Illustrator document onto a canvas without artwork opens it.
// Explicit File > Import continues to import into the current document.
bool file_drop_opens_document(SPDocument *document, std::string const &path);

namespace Inkscape {
    namespace Extension {
        class Extension;
    }
}

namespace Gtk {
class Window;
}

// Get the name of the default template uri
std::string sp_file_default_template_uri();

/*######################
## N E W
######################*/

/**
 * Creates a new Inkscape document and window.
 * Return value is a pointer to the newly created desktop.
 */
SPDesktop* sp_file_new (const std::string &templ);
SPDesktop* sp_file_new_default ();

/*######################
## D E L E T E
######################*/

/**
 * Close the document/view
 */
void sp_file_exit ();

/*######################
## O P E N
######################*/

// See src/actions/actions-file-window.h

/**
 * Reverts file to disk-copy on "YES"
 */
void sp_file_revert_dialog ();

/*######################
## S A V E
######################*/

/**
 *
 */
bool sp_file_save (Gtk::Window &parentWindow, void* object, void* data);

/**
 *  Saves the given document.  Displays a file select dialog
 *  to choose the new name.
 */
bool sp_file_save_as (Gtk::Window &parentWindow, void* object, void* data);

/**
 *  Saves a copy of the given document.  Displays a file select dialog
 *  to choose a name for the copy.
 */
bool sp_file_save_a_copy (Gtk::Window &parentWindow, void* object, void* data);

/**
 *  Save a copy of a document as template.
 */
bool
sp_file_save_template(Gtk::Window &parentWindow, Glib::ustring name,
    Glib::ustring author, Glib::ustring description, Glib::ustring keywords,
    bool isDefault);

/**
 *  Saves the given document.  Displays a file select dialog
 *  if needed.
 */
bool sp_file_save_document (Gtk::Window &parentWindow, SPDocument *document);

/* Do the saveas dialog with a document as the parameter */
bool sp_file_save_dialog (Gtk::Window &parentWindow, SPDocument *doc, Inkscape::Extension::FileSaveMethod save_method);


/*######################
## I M P O R T
######################*/

void sp_import_document(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page = false);

// See src/actions/actions-file-window.h

/**
 * Imports pages into the document.
 */
void file_import_pages(SPDocument *this_doc, SPDocument *that_doc);

/**
 * Imports a resource
 */
SPObject* file_import(SPDocument *in_doc, const std::string &path,
                 Inkscape::Extension::Extension *key,
                 std::optional<Geom::Point> drop_pos = std::nullopt);

#endif // SEEN_SP_FILE_H


/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vi: set autoindent shiftwidth=4 tabstop=8 filetype=cpp expandtab softtabstop=4 fileencoding=utf-8 textwidth=99 :
