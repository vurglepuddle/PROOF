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



#ifndef INKSCAPE_EXTENSION_OUTPUT_H__
#define INKSCAPE_EXTENSION_OUTPUT_H__

#include "extension.h"
class SPDocument;

namespace Inkscape {
namespace Extension {

class Output : public Extension {
    gchar *mimetype;             /**< What is the mime type this inputs? */
    gchar *extension;            /**< The extension of the input files */
    gchar *filetypename;         /**< A userfriendly name for the file type */
    gchar *filetypetooltip;      /**< A more detailed description of the filetype */
    bool   dataloss;             /**< The extension causes data loss on save */
    bool   savecopyonly;         /**< Limit output option to Save a Copy */
    bool   raster = false;       /**< Is the extension expecting a png file */
    bool   exported = false;     /**< Is the extension available in the export dialog */

public:
    class save_failed {};        /**< Generic failure for an undescribed reason */
    class save_cancelled {};     /**< Saving was cancelled */
    class no_extension_found {}; /**< Failed because we couldn't find an extension to match the filename */
    class file_read_only {};     /**< The existing file can not be opened for writing */
    class export_id_not_found {  /**< The object ID requested for export could not be found in the document */
        public:
            const gchar * const id;
            export_id_not_found(const gchar * const id = nullptr) : id{id} {};
    };
    struct lost_document {}; ///< Document was closed during execution of async extension.

    Output(Inkscape::XML::Node *in_repr, ImplementationHolder implementation, std::string *base_directory);
    ~Output () override;

    bool check() override;
    bool prefs() override;
    bool prefs(bool settings);

    void         save (SPDocument *doc,
                       gchar const *filename,
                       bool detachbase = false);
    void         export_raster (const SPDocument *doc,
                                std::string png_filename,
                                gchar const *filename,
                                bool detachbase);
    gchar const *get_mimetype() const;
    gchar const *get_extension() const;
    const char * get_filetypename(bool translated=false) const;
    const char * get_filetypetooltip(bool translated=false) const;
    bool         causes_dataloss() const { return dataloss; };
    bool         savecopy_only() const { return savecopyonly; };
    bool         is_raster() const { return raster; };
    bool         is_exported() const { return exported; };
    void         add_extension(std::string &filename);
    bool         can_save_filename(gchar const *filename) const;
};

} }  /* namespace Inkscape, Extension */
#endif /* INKSCAPE_EXTENSION_OUTPUT_H__ */

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
