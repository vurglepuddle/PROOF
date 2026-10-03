// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This is the code that moves all of the SVG loading and saving into
 * the module format.  Really Inkscape is built to handle these formats
 * internally, so this is just calling those internal functions.
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Ted Gould <ted@gould.cx>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2002-2003 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <giomm/file.h>

#include "document.h"
#include "inkscape.h"
#include "preferences.h"
#include "extension/output.h"
#include "extension/input.h"
#include "extension/system.h"
#include "svg.h"
#include "display/cairo-utils.h"
#include "extension/system.h"
#include "extension/output.h"

#include "object/sp-image.h"
#include "object/sp-root.h"

#include "util/units.h"
#include "util-string/ustring-format.h"

#include "selection-chemistry.h"

// TODO due to internal breakage in glibmm headers, this must be last:
#include <glibmm/i18n.h>

namespace Inkscape::Extension::Internal {

#include "clear-n_.h"

/**
    \return   None
    \brief    What would an SVG editor be without loading/saving SVG
              files.  This function sets that up.

    For each module there is a call to Inkscape::Extension::build_from_mem
    with a rather large XML file passed in.  This is a constant string
    that describes the module.  At the end of this call a module is
    returned that is basically filled out.  The one thing that it doesn't
    have is the key function for the operation.  And that is linked at
    the end of each call.
*/
void
Svg::init()
{
    // clang-format off
    /* SVG in */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("SVG Input") "</name>\n"
            "<id>" SP_MODULE_KEY_INPUT_SVG "</id>\n"
            SVG_COMMON_INPUT_PARAMS
            "<input priority='1'>\n"
                "<extension>.svg</extension>\n"
                "<mimetype>image/svg+xml</mimetype>\n"
                "<filetypename>" N_("Scalable Vector Graphic (*.svg)") "</filetypename>\n"
                "<filetypetooltip>" N_("Inkscape native file format and W3C standard") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<Svg>());

    /* SVG out Inkscape */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("SVG Output Inkscape") "</name>\n"
            "<id>" SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE "</id>\n"
            "<output is_exported='true' priority='1'>\n"
                "<extension>.svg</extension>\n"
                "<mimetype>image/x-inkscape-svg</mimetype>\n"
                "<filetypename>" N_("Inkscape SVG (*.svg)") "</filetypename>\n"
                "<filetypetooltip>" N_("SVG format with Inkscape extensions") "</filetypetooltip>\n"
                "<dataloss>false</dataloss>\n"
            "</output>\n"
            "<action>prune-proprietary-namespaces</action>\n"
            "<action>set-svg-version-2</action>\n"
            "<action>set-inkscape-version</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>reverse-auto-start-markers</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>remove-marker-context-paint</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>set-svg-version-1</action>\n"
            "<action pref='/options/svgexport/text_insertfallback'>insert-text-fallback</action>\n"
            "<action pref='/options/svgexport/mesh_insertpolyfill'>insert-mesh-polyfill</action>\n"
            "<action pref='/options/svgexport/hatch_insertpolyfill'>insert-hatch-polyfill</action>\n"
            "<action>insert-color-fallback</action>\n"
        "</inkscape-extension>", std::make_unique<Svg>());

    /* SVG out */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("SVG Output") "</name>\n"
            "<id>" SP_MODULE_KEY_OUTPUT_SVG "</id>\n"
            "<output is_exported='true' priority='2'>\n"
                "<extension>.svg</extension>\n"
                "<mimetype>image/svg+xml</mimetype>\n"
                "<filetypename>" N_("Plain SVG (*.svg)") "</filetypename>\n"
                "<filetypetooltip>" N_("Scalable Vector Graphics format as defined by the W3C") "</filetypetooltip>\n"
            "</output>\n"
            "<action>set-svg-version-2</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>reverse-auto-start-markers</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>remove-marker-context-paint</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>set-svg-version-1</action>\n"
            "<action pref='/options/svgexport/text_insertfallback'>insert-text-fallback</action>\n"
            "<action>insert-color-fallback</action>\n"
            "<action>prune-proprietary-namespaces</action>\n"
            "<action>prune-inkscape-namespaces</action>\n"
        "</inkscape-extension>", std::make_unique<Svg>());
    // clang-format on

    return;
}


/**
    \return    A new document just for you!
    \brief     This function takes in a filename of a SVG document and
               turns it into a SPDocument.
    \param     mod   Module to use
    \param     uri   The path or URI to the file. 
                     FIXME: Path in UTF8 or in platform-native encoding?
                     It seems like the variable is used as both.
                     Please change to Gio::File here and in the called functions to avoid the confusion.

    This function is really simple, it just calls sp_document_new...
    That's BS, it does all kinds of things for importing documents
    that probably should be in a separate function.

    Most of the import code was copied from gdkpixpuf-input.cpp.
*/
std::unique_ptr<SPDocument> Svg::open(Inkscape::Extension::Input *mod, char const *uri, bool is_importing)
{
    g_assert(mod);

    // This is only used at the end... but it should go here once uri stuff is fixed.
    auto file = Gio::File::create_for_commandline_arg(uri);
    const auto path = file->get_path();

    // Fixing this means fixing a whole string of things.
    // if (path.empty()) {
    //     // We lied, the uri wasn't a uri, try as path.
    //     file = Gio::File::create_for_path(uri);
    // }

    // std::cout << "Svg::open: uri in: " << uri << std::endl;
    // std::cout << "         : uri:    " << file->get_uri() << std::endl;
    // std::cout << "         : scheme: " << file->get_uri_scheme() << std::endl;
    // std::cout << "         : path:   " << file->get_path() << std::endl;
    // std::cout << "         : parse:  " << file->get_parse_name() << std::endl;
    // std::cout << "         : base:   " << file->get_basename() << std::endl;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // Get import preferences.
    bool ask_svg                   = prefs->getBool(  "/dialogs/import/ask_svg");
    Glib::ustring import_mode_svg  = prefs->getString("/dialogs/import/import_mode_svg");
    Glib::ustring scale            = prefs->getString("/dialogs/import/scale");

    // Selecting some of the pages (via command line) in some future update
    // we could add an option which would allow user page selection.
    auto page_nums = INKSCAPE.get_pages();

    // If we popped up a window asking about import preferences, get values from
    // there and update preferences.
    if(mod->get_gui() && ask_svg) {
        ask_svg         = !mod->get_param_bool("do_not_ask");
        import_mode_svg =  mod->get_param_optiongroup("import_mode_svg");
        scale           =  mod->get_param_optiongroup("scale");

        prefs->setBool(  "/dialogs/import/ask_svg",         ask_svg);
        prefs->setString("/dialogs/import/import_mode_svg", import_mode_svg );
        prefs->setString("/dialogs/import/scale",           scale );
    }

    bool const import_pages = import_mode_svg == "pages";
    bool const import_new = import_mode_svg == "new";

    // Do we "import" as <image>?
    if (is_importing && import_mode_svg != "include" && !import_pages && !import_new) {
        // We import!

        // New wrapper document.
        auto doc = SPDocument::createNewDoc(nullptr, true);

        // Imported document
        auto ret = SPDocument::createNewDoc(uri);

        if (!ret) {
            return nullptr;
        }

        // What is display unit doing here?
        Glib::ustring display_unit = doc->getDisplayUnit()->abbr;
        double width = ret->getWidth().value(display_unit);
        double height = ret->getHeight().value(display_unit);
        if (width < 0 || height < 0) {
            return nullptr;
        }

        // Create image node
        Inkscape::XML::Document *xml_doc = doc->getReprDoc();
        Inkscape::XML::Node *image_node = xml_doc->createElement("svg:image");

        // Set default value as we honor "preserveAspectRatio".
        image_node->setAttribute("preserveAspectRatio", "none");

        double svgdpi = mod->get_param_float("svgdpi");
        image_node->setAttribute("inkscape:svg-dpi", Inkscape::ustring::format_classic(svgdpi));

        image_node->setAttribute("width", Inkscape::ustring::format_classic(width));
        image_node->setAttribute("height", Inkscape::ustring::format_classic(height));

        // This is actually "image-rendering"
        Glib::ustring scale = prefs->getString("/dialogs/import/scale");
        if( scale != "auto") {
            SPCSSAttr *css = sp_repr_css_attr_new();
            sp_repr_css_set_property(css, "image-rendering", scale.c_str());
            sp_repr_css_set(image_node, css, "style");
            sp_repr_css_attr_unref( css );
        }

        // Do we embed or link?
        if (import_mode_svg == "embed") {
            std::unique_ptr<Inkscape::Pixbuf> pb(Inkscape::Pixbuf::create_from_file(uri, svgdpi));
            if(pb) {
                sp_embed_svg(image_node, uri);
            }
        } else {
            // Convert filename to uri (why do we need to do this, we claimed it was already a uri).
            gchar* _uri = g_filename_to_uri(uri, nullptr, nullptr);
            if(_uri) {
                // if (strcmp(_uri, uri) != 0) {
                //     std::cout << "Svg::open: _uri != uri! " << _uri << ":" << uri << std::endl;
                // }
                image_node->setAttribute("xlink:href", _uri);
                g_free(_uri);
            } else {
                image_node->setAttribute("xlink:href", uri);
            }
        }

        // Add the image to a layer.
        Inkscape::XML::Node *layer_node = xml_doc->createElement("svg:g");
        layer_node->setAttribute("inkscape:groupmode", "layer");
        layer_node->setAttribute("inkscape:label", "Image");
        doc->getRoot()->appendChildRepr(layer_node);
        layer_node->appendChild(image_node);
        Inkscape::GC::release(image_node);
        Inkscape::GC::release(layer_node);
        fit_canvas_to_drawing(doc.get());

        // Set viewBox if it doesn't exist. What is display unit doing here?
        if (!doc->getRoot()->viewBox_set) {
            doc->setViewBox(Geom::Rect::from_xywh(0, 0, doc->getWidth().value(doc->getDisplayUnit()), doc->getHeight().value(doc->getDisplayUnit())));
        }
        return doc;
    }

    // We are not importing as <image>. Open as new document.

    // Try to open non-local file (when does this occur?).
    if (!file->get_uri_scheme().empty()) {
        if (path.empty()) {
            try {
                char *contents;
                gsize length;
                file->load_contents(contents, length);
                return SPDocument::createNewDocFromMem({contents, length});
            } catch (Gio::Error &e) {
                g_warning("Could not load contents of non-local URI %s\n", uri);
                return nullptr;
            }
        } else {
            // Do we ever get here and does this actually work?
            uri = path.c_str();
        }
    }

    auto doc = SPDocument::createNewDoc(uri);

    // Page selection is achieved by removing any page not in the found list, the exports
    // Can later figure out how they'd like to process the remaining pages.
    if (doc && !page_nums.empty()) {
        doc->prunePages(page_nums, true);
    }

    // Convert single page docs into multi page mode, and visa-versa if
    // we are importing. We never change the mode for opening.
    if (doc && is_importing && !import_new) {
        doc->setPages(import_pages);
    }

    return doc;
}

/**
    \return    None
    \brief     This is the function that does all of the SVG saves in
               Inkscape.  It detects whether it should do a Inkscape
               namespace save internally.
    \param     mod   Extension to use.
    \param     doc   Document to save.
    \param     uri   The filename to save the file to.
*/
void
Svg::save(Inkscape::Extension::Output *mod, SPDocument *doc, gchar const *filename)
{
    g_return_if_fail(doc != nullptr);
    g_return_if_fail(filename != nullptr);

    if (!sp_repr_save_rebased_file(doc->getReprDoc(), filename, SP_SVG_NS_URI,
                                   doc->getDocumentBase(),
                                   m_detachbase ? nullptr : filename)) {
        throw Inkscape::Extension::Output::save_failed();
    }
}

} // namespace Inkscape::Extension::Internal

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
