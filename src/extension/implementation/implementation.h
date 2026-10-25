// SPDX-License-Identifier: GPL-2.0-or-later
/*
    Author:  Ted Gould <ted@gould.cx>
    Copyright (c) 2003-2005,2007

    Released under GNU GPL v2+, read the file 'COPYING' for more information.

    This file is the backend to the extensions system.  These are
    the parts of the system that most users will never see, but are
    important for implementing the extensions themselves.  This file
    contains the base class for all of that.
*/
#ifndef SEEN_INKSCAPE_EXTENSION_IMPLEMENTATION_H
#define SEEN_INKSCAPE_EXTENSION_IMPLEMENTATION_H

#include <vector>
#include <memory>
#include <optional>
#include <sigc++/signal.h>
#include <glibmm/value.h>
#include <2geom/forward.h>

#include "extension/output.h"

namespace Gtk {
    class Widget;
}

class SPDesktop;
class SPDocument;
class SPPage;
class SPStyle;
class SPItem;

namespace Inkscape {

namespace XML {
    class Node;
}

namespace Extension {

class Effect;
class Extension;
class Template;
class TemplatePreset;
class Input;
class Output;
class Print;
class ExecutionEnv;

typedef std::vector<std::shared_ptr<TemplatePreset>> TemplatePresets;

namespace Implementation {

/**
 * A cache for the document and this implementation.
 */
class ImplementationDocumentCache {

    /**
         * The document that this instance is working on.
         */
    SPDesktop * _desktop;
public:
    explicit ImplementationDocumentCache (SPDesktop * desktop) { _desktop = desktop;};

    virtual ~ImplementationDocumentCache ( ) { return; };
    SPDesktop const * desktop() { return _desktop; }
};

/**
 * Base class for all implementations of modules.  This is whether they are done systematically by
 * having something like the scripting system, or they are implemented internally they all derive
 * from this class.
 */
class Implementation {
public:
    // ----- Constructor / destructor -----
    Implementation() = default;

    virtual ~Implementation() = default;

    // ----- Basic functions for all Extension -----
    virtual bool load(Inkscape::Extension::Extension * /*module*/) { return true; }

    virtual void unload(Inkscape::Extension::Extension * /*module*/) {}

    /**
     * Create a new document cache object.
     * This function just returns \c NULL.  Subclasses are likely
     * to reimplement it to do something useful.
     * @param  ext  The extension that is referencing us
     * @param  doc  The document to create the cache of
     * @return A new document cache that is valid as long as the document
     *         is not changed.
     */
    virtual ImplementationDocumentCache * newDocCache (Inkscape::Extension::Extension * /*ext*/, SPDesktop * /*desktop*/) { return nullptr; }

    /** Verify any dependencies. */
    virtual bool check(Inkscape::Extension::Extension * /*module*/) { return true; }

    virtual bool cancelProcessing () { return true; }
    virtual void commitDocument () {}

    // Indicate if the implementation has it's own GUI replacing prefs
    virtual bool custom_gui() const { return false; }

    // ---- Template and Page functions -----
    virtual std::unique_ptr<SPDocument> new_from_template(Inkscape::Extension::Template *);
    virtual void get_template_presets(const Template *tmod, TemplatePresets &presets) const {};
    virtual void resize_to_template(Inkscape::Extension::Template *tmod, SPDocument *doc, SPPage *page){};
    virtual bool match_template_size(Inkscape::Extension::Template *tmod, double width, double height){ return false; }

    // ----- Input functions -----
    /**
     * Open a file.
     * @param filename File path. Value is in platform-native encoding (see Glib::filename_to_utf8).
     */
    virtual std::unique_ptr<SPDocument> open(Inkscape::Extension::Input *module, char const *filename, bool is_importing);

    // ----- Output functions -----
    // nullopt keeps the generated options; false cancels. Settings mode uses an OK button.
    virtual std::optional<bool> output_prefs(Output * /*module*/, bool /*settings*/) { return std::nullopt; }

    /** Find out information about the file. */
    virtual void save(Inkscape::Extension::Output * /*module*/, SPDocument * /*doc*/, gchar const * /*filename*/) {
        throw Inkscape::Extension::Output::save_failed(); // flag the lack of implementation to the user
    }
    /**
     * Convert from PNG to raster format.
     * 
     * The function takes a PNG file and converts it into the specific format.
     * 
     * @param png_file Path to input file in PNG format.
     *             Value is in platform-native encoding (see Glib::filename_to_utf8).
     * @param filename Path to output file.
     *             Value is in platform-native encoding (see Glib::filename_to_utf8).
     */
    virtual void export_raster(
            Inkscape::Extension::Output * module,
            const SPDocument * doc,
            std::string const &png_file,
            gchar const * filename) {}

    // ----- Effect functions -----
    /** Find out information about the file. */
    virtual Gtk::Widget * prefs_effect(Inkscape::Extension::Effect *module,
                                       SPDesktop *desktop,
                                       sigc::signal<void ()> *changeSignal,
                                       ImplementationDocumentCache *docCache);
    virtual void effect(Inkscape::Extension::Effect * /*module*/, ExecutionEnv * /*executionEnv*/,
                        SPDesktop * /*desktop*/, ImplementationDocumentCache * /*docCache*/);
    virtual void effect(Inkscape::Extension::Effect * /*module*/, ExecutionEnv * /*executionEnv*/, SPDocument *document)
    {}

    virtual bool apply_filter(Inkscape::Extension::Effect* module, SPItem* item) { return false; }

    // ----- Print functions -----
    virtual unsigned setup(Inkscape::Extension::Print * /*module*/) { return 0; }
    virtual unsigned set_preview(Inkscape::Extension::Print * /*module*/) { return 0; }

    virtual unsigned begin(Inkscape::Extension::Print * /*module*/,
                           SPDocument * /*doc*/) { return 0; }
    virtual unsigned finish(Inkscape::Extension::Print * /*module*/) { return 0; }

    /**
     * Tell the printing engine whether text should be text or path.
     * Default value is false because most printing engines will support
     * paths more than they'll support text.  (at least they do today)
     * \retval true  Render the text as a path
     * \retval false Render text using the text function (above)
     */
    virtual bool     textToPath(Inkscape::Extension::Print * /*ext*/) { return false; }

    /**
     * Get "fontEmbedded" param, i.e. tell the printing engine whether fonts should be embedded.
     * Only available for Adobe Type 1 fonts in EPS output as of now
     * \retval true Fonts have to be embedded in the output so that the user might not need
     *              to install fonts to have the interpreter read the document correctly
     * \retval false Do not embed fonts
     */
    virtual bool     fontEmbedded(Inkscape::Extension::Print * /*ext*/) { return false; }

    // ----- Rendering methods -----
    virtual unsigned bind(Inkscape::Extension::Print * /*module*/,
                          Geom::Affine const & /*transform*/,
                          float /*opacity*/) { return 0; }
    virtual unsigned release(Inkscape::Extension::Print * /*module*/) { return 0; }
    virtual unsigned fill(Inkscape::Extension::Print * /*module*/,
                          Geom::PathVector const & /*pathv*/,
                          Geom::Affine const & /*ctm*/,
                          SPStyle const * /*style*/,
                          Geom::OptRect const & /*pbox*/,
                          Geom::OptRect const & /*dbox*/,
                          Geom::OptRect const & /*bbox*/) { return 0; }
    virtual unsigned stroke(Inkscape::Extension::Print * /*module*/,
                            Geom::PathVector const & /*pathv*/,
                            Geom::Affine const & /*transform*/,
                            SPStyle const * /*style*/,
                            Geom::OptRect const & /*pbox*/,
                            Geom::OptRect const & /*dbox*/,
                            Geom::OptRect const & /*bbox*/) { return 0; }
    virtual unsigned image(Inkscape::Extension::Print * /*module*/,
                           unsigned char * /*px*/,
                           unsigned int /*w*/,
                           unsigned int /*h*/,
                           unsigned int /*rs*/,
                           Geom::Affine const & /*transform*/,
                           SPStyle const * /*style*/) { return 0; }
    virtual unsigned text(Inkscape::Extension::Print * /*module*/,
                          char const * /*text*/,
                          Geom::Point const & /*p*/,
                          SPStyle const * /*style*/) { return 0; }
    virtual void     processPath(Inkscape::XML::Node * /*node*/) {}

    /**
     * If detach = true, when saving to a file, don't store URIs relative to the filename
     */
    virtual void setDetachBase(bool detach) {}
};


}  // namespace Implementation
}  // namespace Extension
}  // namespace Inkscape

#endif // SEEN_INKSCAPE_EXTENSION_IMPLEMENTATION_H

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
