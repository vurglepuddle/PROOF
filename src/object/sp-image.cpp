// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SVG <image> implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Edward Flick (EAF)
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 1999-2005 Authors
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-image.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <giomm/error.h>
#include <glib/gstdio.h>
#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

// Added for preserveAspectRatio support -- EAF
#include "attributes.h"
#include "colors/document-cms.h"
#include "colors/document-colors.h"
#include "colors/spaces/cms.h"
#include "display/cairo-utils.h"
#include "display/drawing-image.h"
#include "document.h"
#include "io/link-policy.h"
#include "object/sp-root.h"
#include "object/uri.h"
#include "path/path-curve.h"
#include "preferences.h"
#include "print.h"
#include "snap-candidate.h"
#include "snap-preferences.h"
#include "xml/href-attribute-helper.h"
#include "xml/quote.h"

//#define DEBUG_LCMS
#ifdef DEBUG_LCMS
#define DEBUG_MESSAGE(key, ...)\
{\
    g_message( __VA_ARGS__ );\
}
#include <gtk/gtk.h>
#else
#define DEBUG_MESSAGE(key, ...)
#endif // DEBUG_LCMS
/*
 * SPImage
 */

// TODO: give these constants better names:
#define MAGIC_EPSILON 1e-9
#define MAGIC_EPSILON_TOO 1e-18
// TODO: also check if it is correct to be using two different epsilon values

static void sp_image_set_curve(SPImage *image);
static void sp_image_update_arenaitem (SPImage *img, Inkscape::DrawingImage *ai);
static void sp_image_update_canvas_image (SPImage *image);

#ifdef DEBUG_LCMS
extern guint update_in_progress;
#define DEBUG_MESSAGE_SCISLAC(key, ...) \
{\
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();\
    bool dump = prefs->getBool("/options/scislac/" #key);\
    bool dumpD = prefs->getBool("/options/scislac/" #key "D");\
    bool dumpD2 = prefs->getBool("/options/scislac/" #key "D2");\
    dumpD &&= ( (update_in_progress == 0) || dumpD2 );\
    if ( dump )\
    {\
        g_message( __VA_ARGS__ );\
\
    }\
    if ( dumpD )\
    {\
        GtkWidget *dialog = gtk_message_dialog_new(NULL,\
                                                   GTK_DIALOG_DESTROY_WITH_PARENT, \
                                                   GTK_MESSAGE_INFO,    \
                                                   GTK_BUTTONS_OK,      \
                                                   __VA_ARGS__          \
                                                   );\
        g_signal_connect_swapped(dialog, "response",\
                                 G_CALLBACK(gtk_widget_destroy),        \
                                 dialog);                               \
        gtk_widget_set_visible(dialog, true);\
    }\
}
#else // DEBUG_LCMS
#define DEBUG_MESSAGE_SCISLAC(key, ...)
#endif // DEBUG_LCMS

SPImage::SPImage() : SPItem(), SPViewBox() {

    this->x.unset();
    this->y.unset();
    this->width.unset();
    this->height.unset();
    this->clipbox = Geom::Rect();
    this->sx = this->sy = 1.0;
    this->ox = this->oy = 0.0;
    this->dpi = 96.00;
    this->prev_width = 0.0;
    this->prev_height = 0.0;

    this->href = nullptr;
    this->color_profile = nullptr;
}

SPImage::~SPImage() = default;

void SPImage::build(SPDocument *document, Inkscape::XML::Node *repr) {
    SPItem::build(document, repr);

    this->readAttr(SPAttr::XLINK_HREF);
    this->readAttr(SPAttr::X);
    this->readAttr(SPAttr::Y);
    this->readAttr(SPAttr::WIDTH);
    this->readAttr(SPAttr::HEIGHT);
    this->readAttr(SPAttr::SVG_DPI);
    this->readAttr(SPAttr::PRESERVEASPECTRATIO);
    this->readAttr(SPAttr::COLOR_PROFILE);

    /* Register */
    document->addResource("image", this);
}

void SPImage::release() {
    if (this->document) {
        // Unregister ourselves
        this->document->removeResource("image", this);
    }

    if (this->href) {
        g_free (this->href);
        this->href = nullptr;
    }

    pixbuf.reset();

    if (this->color_profile) {
        g_free (this->color_profile);
        this->color_profile = nullptr;
    }

    curve.reset();

    SPItem::release();
}

void SPImage::set(SPAttr key, const gchar* value) {
    switch (key) {
        case SPAttr::XLINK_HREF:
            g_free (this->href);
            this->href = (value) ? g_strdup (value) : nullptr;
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_IMAGE_HREF_MODIFIED_FLAG);
            break;

        case SPAttr::X:
            /* ex, em not handled correctly. */
            if (!this->x.read(value)) {
                this->x.unset();
            }

            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::Y:
            /* ex, em not handled correctly. */
            if (!this->y.read(value)) {
                this->y.unset();
            }

            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::WIDTH:
            /* ex, em not handled correctly. */
            if (!this->width.read(value)) {
                this->width.unset();
            }

            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::HEIGHT:
            /* ex, em not handled correctly. */
            if (!this->height.read(value)) {
                this->height.unset();
            }

            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::SVG_DPI:
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_IMAGE_HREF_MODIFIED_FLAG);
            break;

        case SPAttr::PRESERVEASPECTRATIO:
            set_preserveAspectRatio( value );
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG);
            break;

        case SPAttr::COLOR_PROFILE:
            if ( this->color_profile ) {
                g_free (this->color_profile);
            }

            this->color_profile = (value) ? g_strdup (value) : nullptr;

            if ( value ) {
                DEBUG_MESSAGE( lcmsFour, "<this> color-profile set to '%s'", value );
            } else {
                DEBUG_MESSAGE( lcmsFour, "<this> color-profile cleared" );
            }

            // TODO check on this HREF_MODIFIED flag
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_IMAGE_HREF_MODIFIED_FLAG);
            break;


        default:
            SPItem::set(key, value);
            break;
    }

    sp_image_set_curve(this); //creates a curve at the image's boundary for snapping
}

// BLIP
void SPImage::update(SPCtx *ctx, unsigned int flags) {
    SPItem::update(ctx, flags);

    if (flags & SP_IMAGE_HREF_MODIFIED_FLAG) {
        pixbuf.reset();
        if (href) {
            Inkscape::Pixbuf *pb = nullptr;
            double svgdpi = 96;
            if (getRepr()->attribute("inkscape:svg-dpi")) {
                svgdpi = g_ascii_strtod(getRepr()->attribute("inkscape:svg-dpi"), nullptr);
            }
            dpi = svgdpi;
            // PROOF: a CMYK picture's numbers are the document's own, and show through its profile
            // (Illustrator's "Preserve Numbers").
            std::shared_ptr<Inkscape::Colors::CMS::Profile> cmyk;
            if (auto space = Inkscape::Colors::DocumentColors::assignedSpace(document);
                space && space->getComponentType() == Inkscape::Colors::Space::Type::CMYK) {
                cmyk = space->getProfile();
            }
            pb = readImage(Inkscape::getHrefAttribute(*getRepr()).second,
                           getRepr()->attribute("sodipodi:absref"),
                           document->getDocumentBase(), svgdpi, cmyk, pdfPage());
            if (!pb) {
                missing = true;
                // PROOF: what the document keeps of the picture stands in for it.
                if (auto const *preview = getRepr()->attribute(LINK_PREVIEW_ATTRIBUTE)) {
                    pb = readImage(preview, nullptr, nullptr, svgdpi);
                }
                if (!pb) {
                    // Passing in our previous size allows us to preserve the image's expected size.
                    auto broken_width = width._set ? width.computed : 640;
                    auto broken_height = height._set ? height.computed : 640;
                    pb = getBrokenImage(broken_width, broken_height);
                }
            }
            else {
                missing = false;
            }

            if (pb) {
                if (color_profile) {
                    if (auto cp = document->getDocumentCMS().getSpace(color_profile)) {
                        pb->ensurePixelFormat(Inkscape::Pixbuf::PF_GDK);
                        // XXX TODO cp->transformToRGB(pb);
                    }
                }
                pb->ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO); // Expected by rendering code, so convert now before making immutable.
                pixbuf = std::shared_ptr<Inkscape::Pixbuf>(pb);
            }
        }
    }

    SPItemCtx *ictx = (SPItemCtx *) ctx;

    // Why continue without a pixbuf? So we can display "Missing Image" png.
    // Eventually, we should properly support SVG image type (i.e. render it ourselves).
    if (this->pixbuf) {
        if (!this->x._set) {
            this->x.unit = SVGLength::PX;
            this->x.computed = 0;
        }

        if (!this->y._set) {
            this->y.unit = SVGLength::PX;
            this->y.computed = 0;
        }

        if (!this->width._set) {
            this->width.unit = SVGLength::PX;
            this->width.computed = this->pixbuf->width();
        }

        if (!this->height._set) {
            this->height.unit = SVGLength::PX;
            this->height.computed = this->pixbuf->height();
        }
    }

    // Calculate x, y, width, height from parent/initial viewport, see sp-root.cpp
    this->calcDimsFromParentViewport(ictx);

    // Image creates a new viewport
    ictx->viewport = Geom::Rect::from_xywh(this->x.computed, this->y.computed,
                                           this->width.computed, this->height.computed);

    this->clipbox = ictx->viewport;

    this->ox = this->x.computed;
    this->oy = this->y.computed;

    if (this->pixbuf) {

        // Viewbox is either from SVG (not supported) or dimensions of pixbuf (PNG, JPG)
        this->viewBox = Geom::Rect::from_xywh(0, 0, this->pixbuf->width(), this->pixbuf->height());
        this->viewBox_set = true;

        // SPItemCtx rctx =
        get_rctx( ictx );

        this->ox = c2p[4];
        this->oy = c2p[5];
        this->sx = c2p[0];
        this->sy = c2p[3];
    }

    // TODO: eliminate ox, oy, sx, sy

    sp_image_update_canvas_image ((SPImage *) this);

    // don't crash with missing xlink:href attribute
    if (!this->pixbuf) {
        return;
    }

    // PROOF: what stands in for a file that can't be found isn't the picture, and has no say
    // in the picture's box. (Below, a picture of another shape than the one before reshapes
    // the box. A stand-in did too: undoing a relink put the kept picture back and then
    // changed the width, as a change no Undo knew of.)
    if (missing) {
        return;
    }

    double proportion_pixbuf = this->pixbuf->height() / (double)this->pixbuf->width();
    double proportion_image = this->height.computed / (double)this->width.computed;
    if (this->prev_width &&
        (this->prev_width != this->pixbuf->width() || this->prev_height != this->pixbuf->height())) {
        if (std::abs(this->prev_width - this->pixbuf->width()) > std::abs(this->prev_height - this->pixbuf->height())) {
            proportion_pixbuf = this->pixbuf->width() / (double)this->pixbuf->height();
            proportion_image = this->width.computed / (double)this->height.computed;
            if (proportion_pixbuf != proportion_image) {
                double new_height = this->height.computed * proportion_pixbuf;
                this->getRepr()->setAttributeSvgDouble("width", new_height);
            }
        }
        else {
            if (proportion_pixbuf != proportion_image) {
                double new_width = this->width.computed * proportion_pixbuf;
                this->getRepr()->setAttributeSvgDouble("height", new_width);
            }
        }
    }
    this->prev_width = this->pixbuf->width();
    this->prev_height = this->pixbuf->height();
}

void SPImage::modified(unsigned int flags) {
//  SPItem::onModified(flags);

    if (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) {
        for (auto &v : views) {
            auto img = cast<Inkscape::DrawingImage>(v.drawingitem.get());
            img->setStyle(style);
        }
    }
}

Inkscape::XML::Node *SPImage::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags ) {
    if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = xml_doc->createElement("svg:image");
    }

    Inkscape::setHrefAttribute(*repr, this->href);

    /* fixme: Reset attribute if needed (Lauris) */
    if (this->x._set) {
        repr->setAttributeSvgDouble("x", this->x.computed);
    }

    if (this->y._set) {
        repr->setAttributeSvgDouble("y", this->y.computed);
    }

    if (this->width._set) {
        repr->setAttributeSvgDouble("width", this->width.computed);
    }

    if (this->height._set) {
        repr->setAttributeSvgDouble("height", this->height.computed);
    }
    repr->setAttribute("inkscape:svg-dpi", this->getRepr()->attribute("inkscape:svg-dpi"));
    repr->setAttribute(PDF_PAGE_ATTRIBUTE, getRepr()->attribute(PDF_PAGE_ATTRIBUTE));

    this->write_preserveAspectRatio(repr);

    if (this->color_profile) {
        repr->setAttribute("color-profile", this->color_profile);
    }

    SPItem::write(xml_doc, repr, flags);

    return repr;
}

Geom::OptRect SPImage::bbox(Geom::Affine const &transform, SPItem::BBoxType /*type*/) const {
    Geom::OptRect bbox;

    if ((this->width.computed > 0.0) && (this->height.computed > 0.0)) {
        bbox = Geom::Rect::from_xywh(this->x.computed, this->y.computed, this->width.computed, this->height.computed);
        *bbox *= transform;
    }

    return bbox;
}

void SPImage::print(SPPrintContext *ctx) {
    if (pixbuf && width.computed > 0.0 && height.computed > 0.0) {
        auto pb = *pixbuf;
        pb.ensurePixelFormat(Inkscape::Pixbuf::PF_GDK);

        guchar *px = pb.pixels();
        int w = pb.width();
        int h = pb.height();
        int rs = pb.rowstride();

        double vx = this->ox;
        double vy = this->oy;

        Geom::Affine t;
        Geom::Translate tp(vx, vy);
        Geom::Scale s(this->sx, this->sy);
        t = s * tp;
        ctx->image_R8G8B8A8_N(px, w, h, rs, t, this->style);
    }
}

const char* SPImage::typeName() const {
    return "image";
}

const char* SPImage::displayName() const {
    return _("Image");
}

/**
 * Return this image's href as a URI object.
 */
Inkscape::URI SPImage::getURI() const
{
    return Inkscape::URI::from_href_and_basedir(href, document->getDocumentBase());
}

gchar* SPImage::description() const {
    char *href_desc;

    if (this->href) {
        href_desc = (strncmp(this->href, "data:", 5) == 0)
            ? g_strdup(_("embedded"))
            : xml_quote_strdup(this->href);
    } else {
        g_warning("Attempting to call strncmp() with a null pointer.");
        href_desc = g_strdup("(null_pointer)"); // we call g_free() on href_desc
    }

    char *ret = ( !pixbuf
                  ? g_strdup_printf(_("[bad reference]: %s"), href_desc)
                  : linkMissing()
                  ? g_strdup_printf(_("[file not found]: %s"), href_desc)
                  : g_strdup_printf(_("%d &#215; %d: %s"),
                                    pixbuf->width(),
                                    pixbuf->height(),
                                    href_desc) );

    if (!pixbuf && document)
    {
        Inkscape::Pixbuf * pb = nullptr;
        double svgdpi = 96;
        if (this->getRepr()->attribute("inkscape:svg-dpi")) {
            svgdpi = g_ascii_strtod(this->getRepr()->attribute("inkscape:svg-dpi"), nullptr);
        }
        pb = readImage(Inkscape::getHrefAttribute(*this->getRepr()).second,
                       this->getRepr()->attribute("sodipodi:absref"),
                       this->document->getDocumentBase(), svgdpi, {}, pdfPage());

        if (pb) {
            ret = g_strdup_printf(_("%d &#215; %d: %s"),
                                        pb->width(),
                                        pb->height(),
                                        href_desc);
            delete pb;
        } else {
            ret = g_strdup(_("{Broken Image}"));
        }
    }

    g_free(href_desc);
    return ret;
}

bool SPImage::linkMissing() const
{
    return missing && href && *href && g_ascii_strncasecmp(href, "data:", 5) != 0;
}

int SPImage::pdfPage() const
{
    auto const *value = getRepr()->attribute(PDF_PAGE_ATTRIBUTE);
    if (!value) return 1;
    int page = 0;
    auto const *end = value + std::strlen(value);
    auto const result = std::from_chars(value, end, page);
    return result.ec == std::errc{} && result.ptr == end && page > 0 ? page : 0;
}

std::string SPImage::linkedFile() const
{
    if (!href || !*href || g_ascii_strncasecmp(href, "data:", 5) == 0) {
        return {};
    }
    try {
        auto const uri = getURI();
        if (uri.hasScheme("file")) {
            return uri.toNativeFilename();
        }
    } catch (...) {
        // Not an address a file name can be made of: the link as it is written.
    }
    if (auto const *absref = getRepr()->attribute("sodipodi:absref")) {
        return absref;
    }
    return href;
}

void SPImage::relink(std::string const &file)
{
    auto *repr = getRepr();
    int w = 0, h = 0;
    bool const picture = gdk_pixbuf_get_file_info(file.c_str(), &w, &h) && w > 0 && h > 0;
    gchar *uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);
    Inkscape::setHrefAttribute(*repr, uri ? uri : file.c_str());
    g_free(uri);
    repr->setAttribute("sodipodi:absref", file);
    repr->removeAttribute(LINK_PREVIEW_ATTRIBUTE);
    // The picture that was there filled its box. Another of the same shape fills it too; one
    // of another shape would be pulled out of its own, and is fitted inside the box.
    if (picture && width._set && height._set && width.computed > 0 && height.computed > 0 && aspect_set &&
        aspect_align == SP_ASPECT_NONE) {
        double const box = width.computed / height.computed, own = double(w) / h;
        if (std::abs(own - box) > 0.01 * box) {
            repr->setAttribute("preserveAspectRatio", "xMidYMid");
        }
    }
}

std::vector<SPImage *> sp_images_missing(SPDocument *document)
{
    std::vector<SPImage *> out;
    if (!document || !document->getRoot()) {
        return out;
    }
    document->ensureUpToDate();
    auto visit = [&](auto const &self, SPObject *object) -> void {
        if (auto *image = cast<SPImage>(object)) {
            if (image->linkMissing()) {
                out.push_back(image);
            }
            return;
        }
        for (auto &child : object->children) {
            self(self, &child);
        }
    };
    visit(visit, document->getRoot());
    return out;
}

unsigned sp_images_relink(SPImage *image, std::string const &file)
{
    if (!image || !image->document || !gdk_pixbuf_get_file_info(file.c_str(), nullptr, nullptr)) {
        return 0;
    }
    auto *document = image->document;
    // The user chose this file: its share is followed now, if it is on one (io/link-policy.h).
    Inkscape::IO::trust_link_share(file);
    // Listed before anything changes: what is linked below is no longer missing.
    auto const others = sp_images_missing(document);
    image->relink(file);
    unsigned count = 1;
    gchar *folder = g_path_get_dirname(file.c_str());
    for (auto *other : others) {
        if (other == image) {
            continue;
        }
        // The name alone, whichever kind of machine wrote the folders before it.
        auto name = other->linkedFile();
        if (auto const cut = name.find_last_of("/\\"); cut != std::string::npos) {
            name.erase(0, cut + 1);
        }
        if (name.empty()) {
            continue;
        }
        gchar *beside = g_build_filename(folder, name.c_str(), nullptr);
        if (g_file_test(beside, G_FILE_TEST_IS_REGULAR) && gdk_pixbuf_get_file_info(beside, nullptr, nullptr)) {
            other->relink(beside);
            ++count;
        }
        g_free(beside);
    }
    g_free(folder);
    document->ensureUpToDate();
    return count;
}

Inkscape::DrawingItem* SPImage::show(Inkscape::Drawing &drawing, unsigned int /*key*/, unsigned int /*flags*/) {
    Inkscape::DrawingImage *ai = new Inkscape::DrawingImage(drawing);

    sp_image_update_arenaitem(this, ai);

    return ai;
}


Inkscape::Pixbuf *SPImage::readImage(gchar const *href, gchar const *absref, gchar const *base, double svgdpi,
                                     std::shared_ptr<Inkscape::Colors::CMS::Profile> const &cmyk, int pdf_page)
{
    if (pdf_page < 1) return nullptr;
    Inkscape::Pixbuf *inkpb = nullptr;

    char const *filename = href;

    if (filename) {
        if (g_ascii_strncasecmp(filename, "data:", 5) == 0) {
            /* data URI - embedded image */
            filename += 5;
            inkpb = Inkscape::Pixbuf::create_from_data_uri(filename, svgdpi, cmyk, pdf_page);
        } else {
            auto url = Inkscape::URI::from_href_and_basedir(href, base);

            if (url.hasScheme("file")) {
                try {
                    auto native = url.toNativeFilename();
                    // PROOF: a picture on another computer isn't looked up (io/link-policy.h).
                    if (Inkscape::IO::may_follow_link(native, base)) {
                        inkpb = Inkscape::Pixbuf::create_from_file(native.c_str(), svgdpi, cmyk, pdf_page);
                    }
                } catch (Glib::ConvertError const &e) {
                    g_warning("readImage: %s", e.what());
                    inkpb = nullptr;
                }
            } else {
                // PROOF: on Windows, nor one at a web or network address (http:, smb:…), which
                // is on another computer too. Elsewhere these are fetched as before.
#ifndef _WIN32
                try {
                    auto contents = url.getContents();
                    inkpb = Inkscape::Pixbuf::create_from_buffer(contents, svgdpi, "", cmyk, pdf_page);
                } catch (const Gio::Error &e) {
                    g_warning("URI::getContents failed for '%.100s'", href);
                }
#endif
            }
        }

        if (inkpb) {
            return inkpb;
        }
    }

    /* at last try to load from sp absolute path name */
    filename = absref;
    if (filename != nullptr) {
        // using absref is outside of SVG rules, so we must at least warn the user
        if ( base != nullptr && href != nullptr ) {
            g_warning ("<image xlink:href=\"%s\"> did not resolve to a valid image file (base dir is %s), now trying sodipodi:absref=\"%s\"", href, base, absref);
        } else {
            g_warning ("xlink:href did not resolve to a valid image file, now trying sodipodi:absref=\"%s\"", absref);
        }

        if (Inkscape::IO::may_follow_link(filename, base)) {
            inkpb = Inkscape::Pixbuf::create_from_file(filename, svgdpi, cmyk, pdf_page);
        }
        if (inkpb != nullptr) {
            return inkpb;
        }
    }
    return inkpb;
}

static std::string broken_image_svg = R"A(
<svg xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">
  <defs>
    <symbol id="nope" style="fill:none;stroke:#ffffff;stroke-width:3" viewBox="0 0 10 10" preserveAspectRatio="{aspect}">
      <circle cx="0" cy="0" r="10" style="fill:#a40000;stroke:#cc0000" />
      <line x1="0" x2="0" y1="-5" y2="5" transform="rotate(45)" />
      <line x1="0" x2="0" y1="-5" y2="5" transform="rotate(-45)" />
    </symbol>
  </defs>
  <rect width="100%" height="100%" style="fill:white;stroke:#cc0000;stroke-width:6%" />
  <use xlink:href="#nope" width="30%" height="30%" x="50%" y="50%" />
</svg>

)A";

/**
 * Load a standard broken image svg, used if we fail to load pixbufs from the href.
 */
Inkscape::Pixbuf *SPImage::getBrokenImage(double width, double height)
{
    // Limit the size of the broken image raster. smaller than the size in cairo-utils.
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double dpi = prefs->getDouble("/dialogs/import/defaultxdpi/value", 96.0);
    width = std::max(std::min(width, dpi * 20), 1.0);
    height = std::max(std::min(height, dpi * 20), 1.0);

    // Cheap templating for size allows for dynamic sized svg
    std::string copy = broken_image_svg;
    copy.replace(copy.find("{width}"), std::string("{width}").size(), std::to_string(width));
    copy.replace(copy.find("{height}"), std::string("{height}").size(), std::to_string(height));

    // Aspect attempts to make the image better for different ratios of images we might be dropped into
    copy.replace(copy.find("{aspect}"), std::string("{aspect}").size(), width > height ? "xMinYMid" : "xMidYMin");

    auto inkpb = Inkscape::Pixbuf::create_from_buffer(copy, 0, "brokenimage.svg");

    /* It's included here so if it still does not does load, our libraries are broken! */
    g_assert (inkpb != nullptr);

    return inkpb;
}

/* We assert that realpixbuf is either NULL or identical size to pixbuf */
static void
sp_image_update_arenaitem (SPImage *image, Inkscape::DrawingImage *ai)
{
    ai->setStyle(image->style);
    ai->setPixbuf(image->pixbuf);
    ai->setOrigin(Geom::Point(image->ox, image->oy));
    ai->setScale(image->sx, image->sy);
    ai->setClipbox(image->clipbox);
}

static void sp_image_update_canvas_image(SPImage *image)
{
    for (auto &v : image->views) {
        sp_image_update_arenaitem(image, cast<Inkscape::DrawingImage>(v.drawingitem.get()));
    }
}

void SPImage::snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const {
    /* An image doesn't have any nodes to snap, but still we want to be able snap one image
    to another. Therefore we will create some snappoints at the corner, similar to a rect. If
    the image is rotated, then the snappoints will rotate with it. Again, just like a rect.
    */

    if (this->getClipObject()) {
        //We are looking at a clipped image: do not return any snappoints, as these might be
        //far far away from the visible part from the clipped image
        //TODO Do return snappoints, but only when within visual bounding box
    } else {
        if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_IMG_CORNER)) {
            // The image has not been clipped: return its corners, which might be rotated for example
            double const x0 = this->x.computed;
            double const y0 = this->y.computed;
            double const x1 = x0 + this->width.computed;
            double const y1 = y0 + this->height.computed;

            Geom::Affine const i2d (this->i2dt_affine ());

            p.emplace_back(Geom::Point(x0, y0) * i2d, Inkscape::SNAPSOURCE_IMG_CORNER, Inkscape::SNAPTARGET_IMG_CORNER);
            p.emplace_back(Geom::Point(x0, y1) * i2d, Inkscape::SNAPSOURCE_IMG_CORNER, Inkscape::SNAPTARGET_IMG_CORNER);
            p.emplace_back(Geom::Point(x1, y1) * i2d, Inkscape::SNAPSOURCE_IMG_CORNER, Inkscape::SNAPTARGET_IMG_CORNER);
            p.emplace_back(Geom::Point(x1, y0) * i2d, Inkscape::SNAPSOURCE_IMG_CORNER, Inkscape::SNAPTARGET_IMG_CORNER);
        }
    }
}

/*
 * Initially we'll do:
 * Transform x, y, set x, y, clear translation
 */

Geom::Affine SPImage::set_transform(Geom::Affine const &xform) {
    /* Calculate position in parent coords. */
    Geom::Point pos( Geom::Point(this->x.computed, this->y.computed) * xform );

    /* This function takes care of translation and scaling, we return whatever parts we can't
       handle. */
    Geom::Affine ret(Geom::Affine(xform).withoutTranslation());
    Geom::Point const scale(hypot(ret[0], ret[1]),
                            hypot(ret[2], ret[3]));

    if ( scale[Geom::X] > MAGIC_EPSILON ) {
        ret[0] /= scale[Geom::X];
        ret[1] /= scale[Geom::X];
    } else {
        ret[0] = 1.0;
        ret[1] = 0.0;
    }

    if ( scale[Geom::Y] > MAGIC_EPSILON ) {
        ret[2] /= scale[Geom::Y];
        ret[3] /= scale[Geom::Y];
    } else {
        ret[2] = 0.0;
        ret[3] = 1.0;
    }

    this->width = this->width.computed * scale[Geom::X];
    this->height = this->height.computed * scale[Geom::Y];

    /* Find position in item coords */
    pos = pos * ret.inverse();
    this->x = pos[Geom::X];
    this->y = pos[Geom::Y];

    return ret;
}

static void sp_image_set_curve( SPImage *image )
{
    //create a curve at the image's boundary for snapping
    if ((image->height.computed < MAGIC_EPSILON_TOO) || (image->width.computed < MAGIC_EPSILON_TOO) || (image->getClipObject())) {
    } else {
        Geom::OptRect rect = image->bbox(Geom::identity(), SPItem::VISUAL_BBOX);

        if (rect->isFinite()) {
            image->curve = rect_to_open_path(*rect);
        }
    }
}

/**
 * Return a borrowed pointer to curve (if any exists) or NULL if there is no curve
 */
Geom::PathVector const *SPImage::get_curve() const
{
    return curve ? &*curve : nullptr;
}

void sp_embed_image(Inkscape::XML::Node *image_node, Inkscape::Pixbuf *pb)
{
    bool free_data = false;

    // check whether the pixbuf has MIME data
    guchar *data = nullptr;
    gsize len = 0;
    std::string data_mimetype;

    data = const_cast<guchar *>(pb->getMimeData(len, data_mimetype));

    if (data == nullptr) {
        // if there is no supported MIME data, embed as PNG
        data_mimetype = "image/png";
        gdk_pixbuf_save_to_buffer(pb->getPixbufRaw(), reinterpret_cast<gchar**>(&data), &len, "png", nullptr, nullptr);
        free_data = true;
    }

    // Save base64 encoded data in image node
    // this formula taken from Glib docs
    gsize needed_size = len * 4 / 3 + len * 4 / (3 * 72) + 7;
    needed_size += 5 + 8 + data_mimetype.size(); // 5 bytes for data: + 8 for ;base64,

    gchar *buffer = (gchar *) g_malloc(needed_size);
    gchar *buf_work = buffer;
    buf_work += g_sprintf(buffer, "data:%s;base64,", data_mimetype.c_str());

    gint state = 0;
    gint save = 0;
    gsize written = 0;
    written += g_base64_encode_step(data, len, TRUE, buf_work, &state, &save);
    written += g_base64_encode_close(TRUE, buf_work + written, &state, &save);
    buf_work[written] = 0; // null terminate

    // TODO: this is very wasteful memory-wise.
    // It would be better to only keep the binary data around,
    // and base64 encode on the fly when saving the XML.
    Inkscape::setHrefAttribute(*image_node, buffer);

    g_free(buffer);
    if (free_data) g_free(data);
}

void sp_embed_svg(Inkscape::XML::Node *image_node, std::string const &fn)
{
    if (!g_file_test(fn.c_str(), G_FILE_TEST_EXISTS)) {
        return;
    }
    GStatBuf stdir;
    int val = g_stat(fn.c_str(), &stdir);
    if (val == 0 && stdir.st_mode & S_IFDIR){
        return;
    }

    // we need to load the entire file into memory,
    // since we'll store it as MIME data
    gchar *data = nullptr;
    gsize len = 0;
    GError *error = nullptr;

    if (g_file_get_contents(fn.c_str(), &data, &len, &error)) {

        if (error != nullptr) {
            std::cerr << "Pixbuf::create_from_file: " << error->message << std::endl;
            std::cerr << "   (" << fn << ")" << std::endl;
            return;
        }

        std::string data_mimetype = "image/svg+xml";


        // Save base64 encoded data in image node
        // this formula taken from Glib docs
        gsize needed_size = len * 4 / 3 + len * 4 / (3 * 72) + 7;
        needed_size += 5 + 8 + data_mimetype.size(); // 5 bytes for data: + 8 for ;base64,

        gchar *buffer = (gchar *) g_malloc(needed_size);
        gchar *buf_work = buffer;
        buf_work += g_sprintf(buffer, "data:%s;base64,", data_mimetype.c_str());

        gint state = 0;
        gint save = 0;
        gsize written = 0;
        written += g_base64_encode_step(reinterpret_cast<guchar *>(data), len, TRUE, buf_work, &state, &save);
        written += g_base64_encode_close(TRUE, buf_work + written, &state, &save);
        buf_work[written] = 0; // null terminate

        // TODO: this is very wasteful memory-wise.
        // It would be better to only keep the binary data around,
        // and base64 encode on the fly when saving the XML.
        Inkscape::setHrefAttribute(*image_node, buffer);

        g_free(buffer);
        g_free(data);
    }
}

void SPImage::refresh_if_outdated()
{
    if ( href && pixbuf && pixbuf->modificationTime()) {
        // It *might* change

        GStatBuf st;
        memset(&st, 0, sizeof(st));
        int val = 0;
        if (g_file_test(pixbuf->originalPath().c_str(), G_FILE_TEST_EXISTS)) {
            val = g_stat(pixbuf->originalPath().c_str(), &st);
        }
        if ( !val ) {
            // stat call worked. Check time now
            if ( st.st_mtime != pixbuf->modificationTime() ) {
                requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_IMAGE_HREF_MODIFIED_FLAG);
            }
        }
    }
}

/**
 * Crop the image (remove pixels) based on the area rectangle
 * and translate image to componsate for movement.
 *
 * @param area - Rectangle in document units
 *
 * @returns true if any pixels were removed.
 */
bool SPImage::cropToArea(Geom::Rect area)
{
    area *= i2doc_affine().inverse();

    // Apply the image's viewbox and scal to get us image pixels
    area *= Geom::Translate(-x.computed, -y.computed);
    area *= Geom::Scale(pixbuf->width() / width.computed, pixbuf->height() / height.computed);

    // Any precision problems and we choose to retain more pixels (roundOut)
    return cropToArea(area.roundOutwards());
}

/**
 * Crop to the actual pixel area of the image, and adjusting the
 * image's coordinates to compensate for the changes.
 *
 * @param area - Rectangle in image pixel units
 *
 * @returns true if any pixels were removed.
 */
bool SPImage::cropToArea(const Geom::IntRect &area)
{
    // Contrain requested area to the available pixels.
    auto px = Geom::IntRect::from_xywh(0.0, 0.0, pixbuf->width(), pixbuf->height());
    auto px_area = area & px;
    if (!px_area)
        return false;

    if (auto pb = pixbuf->cropTo(*px_area)) {
        // Crop ended up with bad pixels, this should rarely happen.
        if (pb->width() <= 0 || pb->height() <= 0)
            return false;

        // Cropping is done, now embed this image back into image tag.
        sp_embed_image(getRepr(), pb);

        // Our new image has new sizes, so adjust image tag's internal viewbox
        auto repr = getRepr();
        auto scale_x = px.width() / width.computed;
        auto scale_y = px.height() / height.computed;
        repr->setAttributeSvgDouble("x", this->x.computed + (px_area->left() / scale_x));
        repr->setAttributeSvgDouble("y", this->y.computed + (px_area->top() / scale_y));
        repr->setAttributeSvgDouble("width", px_area->width() / scale_x);
        repr->setAttributeSvgDouble("height", px_area->height() / scale_y);

        return true;
    }
    return false;
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
