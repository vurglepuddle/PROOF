// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Helper functions to use cairo with inkscape
 *
 * Copyright (C) 2007 bulia byak
 * Copyright (C) 2008 Johan Engelen
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 */

#include "display/cairo-utils.h"
#include "display/psd-pixels.h"
#include "display/tiff-pixels.h"
#ifdef WITH_POPPLER
#include "extension/internal/pdfinput/placed-page.h"
#endif

#include <2geom/affine.h>
#include <2geom/curves.h>
#include <2geom/path-sink.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/sbasis-to-bezier.h>
#include <2geom/transforms.h>
#include <boost/algorithm/string.hpp>
#include <boost/operators.hpp>
#include <boost/optional/optional.hpp>
#include <cairomm/context.h>
#include <cairomm/matrix.h>
#include <cairomm/pattern.h>
#include <cairomm/refptr.h>
#include <cairomm/surface.h>
#include <cmath>
#include <cstdint>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>

#include "cairo-templates.h"
#include "colors/manager.h"
#include "colors/spaces/rgb.h"
#include "colors/utils.h"
#include "document.h"
#include "helper/pixbuf-ops.h"
#include "preferences.h"
#include "ui/util.h"
#include "util/scope_exit.h"
#include "util/units.h"
#include "util/uri.h"

#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 17, 6)
#define CAIRO_HAS_HAIRLINE
#endif

/**
 * Key for cairo_surface_t to keep track of current color interpolation value
 * Only the address of the structure is used, it is never initialized. See:
 * http://www.cairographics.org/manual/cairo-Types.html#cairo-user-data-key-t
 */
static cairo_user_data_key_t ink_color_interpolation_key;

namespace Inkscape {

/* The class below implement the following hack:
 * 
 * The pixels formats of Cairo and GdkPixbuf are different.
 * GdkPixbuf accesses pixels as bytes, alpha is not premultiplied,
 * and successive bytes of a single pixel contain R, G, B and A components.
 * Cairo accesses pixels as 32-bit ints, alpha is premultiplied,
 * and each int contains as 0xAARRGGBB, accessed with bitwise operations.
 *
 * In other words, on a little endian system, a GdkPixbuf will contain:
 *   char *data = "rgbargbargba...."
 *   int *data = { 0xAABBGGRR, 0xAABBGGRR, 0xAABBGGRR, ... }
 * while a Cairo image surface will contain:
 *   char *data = "bgrabgrabgra...."
 *   int *data = { 0xAARRGGBB, 0xAARRGGBB, 0xAARRGGBB, ... }
 *
 * It is possible to convert between these two formats (almost) losslessly.
 * Some color information from partially transparent regions of the image
 * is lost, but the result when displaying this image will remain the same.
 *
 * The class allows interoperation between GdkPixbuf
 * and Cairo surfaces without creating a copy of the image.
 * This is implemented by creating a GdkPixbuf and a Cairo image surface
 * which share their data. Depending on what is needed at a given time,
 * the pixels are converted in place to the Cairo or the GdkPixbuf format.
 */

/** Create a pixbuf from a Cairo surface.
 * The constructor takes ownership of the passed surface,
 * so it should not be destroyed. */
Pixbuf::Pixbuf(cairo_surface_t *s)
    : _pixbuf(gdk_pixbuf_new_from_data(
        cairo_image_surface_get_data(s), GDK_COLORSPACE_RGB, TRUE, 8,
        cairo_image_surface_get_width(s), cairo_image_surface_get_height(s),
        cairo_image_surface_get_stride(s),
        ink_cairo_pixbuf_cleanup, s))
    , _surface(s)
    , _mod_time(0)
    , _pixel_format(PF_CAIRO)
    , _cairo_store(true)
{}

/** Create a pixbuf from a GdkPixbuf.
 * The constructor takes ownership of the passed GdkPixbuf reference,
 * so it should not be unrefed. */
Pixbuf::Pixbuf(GdkPixbuf *pb)
    : _pixbuf(pb)
    , _surface(nullptr)
    , _mod_time(0)
    , _pixel_format(PF_GDK)
    , _cairo_store(false)
{
    _forceAlpha();
    _surface = cairo_image_surface_create_for_data(
        gdk_pixbuf_get_pixels(_pixbuf), CAIRO_FORMAT_ARGB32,
        gdk_pixbuf_get_width(_pixbuf), gdk_pixbuf_get_height(_pixbuf), gdk_pixbuf_get_rowstride(_pixbuf));
}

Pixbuf::Pixbuf(Inkscape::Pixbuf const &other)
    : _pixbuf(gdk_pixbuf_copy(other._pixbuf))
    , _surface(cairo_image_surface_create_for_data(
        gdk_pixbuf_get_pixels(_pixbuf), CAIRO_FORMAT_ARGB32,
        gdk_pixbuf_get_width(_pixbuf), gdk_pixbuf_get_height(_pixbuf), gdk_pixbuf_get_rowstride(_pixbuf)))
    , _mod_time(other._mod_time)
    , _path(other._path)
    , _pixel_format(other._pixel_format)
    , _cairo_store(false)
{}

Pixbuf::~Pixbuf()
{
    if (!_cairo_store) {
        cairo_surface_destroy(_surface);
    }
    g_object_unref(_pixbuf);
}

#if !GDK_PIXBUF_CHECK_VERSION(2, 41, 0)
/**
 * Incremental file read introduced to workaround
 * https://gitlab.gnome.org/GNOME/gdk-pixbuf/issues/70
 */
static bool _workaround_issue_70__gdk_pixbuf_loader_write( //
    GdkPixbufLoader *loader, guchar *decoded, gsize decoded_len, GError **error)
{
    bool success = true;
    gsize bytes_left = decoded_len;
    gsize secret_limit = 0xffff;
    guchar *decoded_head = decoded;
    while (bytes_left && success) {
        gsize bytes = (bytes_left > secret_limit) ? secret_limit : bytes_left;
        success = gdk_pixbuf_loader_write(loader, decoded_head, bytes, error);
        decoded_head += bytes;
        bytes_left -= bytes;
    }

    return success;
}
#define gdk_pixbuf_loader_write _workaround_issue_70__gdk_pixbuf_loader_write
#endif

/**
 * Create a new Pixbuf with the image cropped to the given area.
 */
Pixbuf *Pixbuf::cropTo(const Geom::IntRect &area) const
{
    GdkPixbuf *copy = nullptr;
    auto source = _pixbuf;
    if (_pixel_format == PF_CAIRO) {
        // This copies twice, but can be run on const, which is useful.
        copy = gdk_pixbuf_copy(_pixbuf);
        ensure_pixbuf(copy);
        source = copy;
    }
    auto cropped = gdk_pixbuf_new_subpixbuf(source,
        area.left(), area.top(), area.width(), area.height());
    if (copy) {
        // Clean up our pixbuf copy
        g_object_unref(copy);
    }
    return new Pixbuf(cropped);
}

Pixbuf *Pixbuf::create_from_own_pixels(guchar *data, gsize len, std::shared_ptr<Colors::CMS::Profile> const &cmyk)
{
    std::string_view const bytes(reinterpret_cast<char const *>(data), len);
    std::optional<RasterPixels> pixels;
    cairo_surface_t *surface = nullptr;
    if (looks_like_psd(bytes)) {
        pixels = read_psd(bytes);
    } else if (is_cmyk_tiff(bytes)) {
        pixels = read_tiff(bytes);
#ifdef WITH_POPPLER
    } else if (Extension::Internal::looks_like_pdf(bytes)) {
        // A placed page (a linked .pdf or .ai), drawn by Poppler.
        surface = Extension::Internal::placed_page_surface(bytes);
#endif
    }
    if (pixels) surface = raster_surface(*pixels, cmyk);
    if (!surface) return nullptr;
    auto *pixbuf = new Pixbuf(surface);
    pixbuf->_setMimeData(data, len, "own");
    return pixbuf;
}

Pixbuf *Pixbuf::create_from_data_uri(gchar const *uri_data, double svgdpi,
                                     std::shared_ptr<Colors::CMS::Profile> const &cmyk)
{
    Pixbuf *pixbuf = nullptr;
    auto [data, type] = extract_uri_data(uri_data);

    if ((*data) && type == Base64Data::RASTER) {
        gsize decoded_len = 0;
        guchar *decoded = g_base64_decode(data, &decoded_len);
        // PROOF: a CMYK TIFF in the document's own CMYK; a Photoshop file.
        if (auto *own = create_from_own_pixels(decoded, decoded_len, cmyk)) {
            return own;
        }

        GdkPixbufLoader *loader = gdk_pixbuf_loader_new();

        if (!loader) {
            g_free(decoded);
            return nullptr;
        }

        if (gdk_pixbuf_loader_write(loader, decoded, decoded_len, nullptr)) {
            gdk_pixbuf_loader_close(loader, nullptr);
            GdkPixbuf *buf = gdk_pixbuf_loader_get_pixbuf(loader);
            if (buf) {
                g_object_ref(buf);
                bool has_ori = Pixbuf::get_embedded_orientation(buf) != Geom::identity();
                buf = Pixbuf::apply_embedded_orientation(buf);
                pixbuf = new Pixbuf(buf);

                if (!has_ori) {
                    // We DO NOT want to store the original data if it contains orientation
                    // data since many exports that will use the surface do not handle it.
                    // TODO: Preserve the original meta data from the file by stripping out
                    // orientation but keeping all other aspects of the raster.
                    GdkPixbufFormat *fmt = gdk_pixbuf_loader_get_format(loader);
                    gchar *fmt_name = gdk_pixbuf_format_get_name(fmt);
                    pixbuf->_setMimeData(decoded, decoded_len, fmt_name);
                    g_free(fmt_name);
                }
            } else {
                g_free(decoded);
            }
        } else {
            g_free(decoded);
        }
        g_object_unref(loader);
    }
    
    if ((*data) && type == Base64Data::SVG) {
        gsize decoded_len = 0;
        guchar *decoded = g_base64_decode(data, &decoded_len);
        auto svgDoc = SPDocument::createNewDocFromMem({reinterpret_cast<char const *>(decoded), decoded_len});
        // Check the document loaded properly
        if (!svgDoc || !svgDoc->getRoot()) {
            g_free(decoded);
            return nullptr;
        }
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        double dpi = prefs->getDouble("/dialogs/import/defaultxdpi/value", 96.0);
        if (svgdpi && svgdpi > 0) {
            dpi = svgdpi;
        }

        // Get the size of the document
        Inkscape::Util::Quantity svgWidth = svgDoc->getWidth();
        Inkscape::Util::Quantity svgHeight = svgDoc->getHeight();
        // Limit the size of the document to 100 inches square, mirroring create_from_buffer
        double const svgWidth_px = std::min(svgWidth.value("px"), dpi * 100);
        double const svgHeight_px = std::min(svgHeight.value("px"), dpi * 100);
        if (svgWidth_px < 0 || svgHeight_px < 0) {
            g_warning("create_from_data_uri: malformed document: svgWidth_px=%f, svgHeight_px=%f", svgWidth_px,
                      svgHeight_px);
            g_free(decoded);
            return nullptr;
        }

        assert(!pixbuf);
        Geom::Rect area(0, 0, svgWidth_px, svgHeight_px);
        pixbuf = sp_generate_internal_bitmap(svgDoc.get(), area, dpi);
        if (!pixbuf) {
            std::cerr << "Pixbuf::create_from_data_uri: failed to rasterize embedded SVG" << std::endl;
            g_free(decoded);
            return nullptr;
        }
        GdkPixbuf const *buf = pixbuf->getPixbufRaw();

        // Tidy up
        if (buf == nullptr) {
            std::cerr << "Pixbuf::create_from_data: failed to load contents: " << std::endl;
            delete pixbuf;
            g_free(decoded);
            return nullptr;
        } else {
            pixbuf->_setMimeData(decoded, decoded_len, "svg+xml");
        }
    }

    return pixbuf;
}

Pixbuf *Pixbuf::create_from_file(std::string const &fn, double svgdpi, std::shared_ptr<Colors::CMS::Profile> const &cmyk)
{
    Pixbuf *pb = nullptr;
    // test correctness of filename
    if (!g_file_test(fn.c_str(), G_FILE_TEST_EXISTS)) { 
        return nullptr;
    }
    GStatBuf stdir;
    int val = g_stat(fn.c_str(), &stdir);
    if (val == 0 && stdir.st_mode & S_IFDIR){
        return nullptr;
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
            return nullptr;
        }

        pb = Pixbuf::create_from_buffer(std::move(data), len, svgdpi, fn, cmyk);

        if (pb) {
            pb->_mod_time = stdir.st_mtime;
        }
    } else {
        std::cerr << "Pixbuf::create_from_file: failed to get contents: " << fn << std::endl;
        return nullptr;
    }

    return pb;
}

GdkPixbuf *Pixbuf::apply_embedded_orientation(GdkPixbuf *buf)
{
    GdkPixbuf *old = buf;
    buf = gdk_pixbuf_apply_embedded_orientation(buf);
    g_object_unref(old);
    return buf;
}

/**
 * Gets any available orientation data and returns it as an affine.
 */
Geom::Affine Pixbuf::get_embedded_orientation(GdkPixbuf *buf)
{
    // See gdk_pixbuf_apply_embedded_orientation in gdk-pixbuf
    if (auto opt_str = gdk_pixbuf_get_option(buf, "orientation")) {
        switch ((int)g_ascii_strtoll(opt_str, NULL, 10)) {
            case 2: // Flip Horz
                return Geom::Scale(-1, 1);
            case 3: // +180 Rotate
                return Geom::Scale(-1, -1);
            case 4: // Flip Vert
                return Geom::Scale(1, -1);
            case 5: // +90 Rotate & Flip Horz
                return Geom::Rotate(90) * Geom::Scale(-1, 1);
            case 6: // +90 Rotate
                return Geom::Rotate(90);
            case 7: // +90 Rotate * Flip Vert
                return Geom::Rotate(90) * Geom::Scale(1, -1);
            case 8: // -90 Rotate
                return Geom::Rotate(-90);
            default:
                break;

        }
    }
    return Geom::identity();
}

Pixbuf *Pixbuf::create_from_buffer(std::string const &buffer, double svgdpi, std::string const &fn,
                                   std::shared_ptr<Colors::CMS::Profile> const &cmyk)
{
#if GLIB_CHECK_VERSION(2,67,3)
    auto datacopy = (gchar *)g_memdup2(buffer.data(), buffer.size());
#else
    auto datacopy = (gchar *)g_memdup(buffer.data(), buffer.size());
#endif
    return Pixbuf::create_from_buffer(std::move(datacopy), buffer.size(), svgdpi, fn, cmyk);
}

Pixbuf *Pixbuf::create_from_buffer(gchar *&&data, gsize len, double svgdpi, std::string const &fn,
                                   std::shared_ptr<Colors::CMS::Profile> const &cmyk)
{
    // PROOF: a CMYK TIFF in the document's own CMYK, with its transparency; a Photoshop file's
    // merged image.
    if (auto *own = create_from_own_pixels(reinterpret_cast<guchar *>(data), len, cmyk)) {
        own->_path = fn;
        return own;
    }
    bool has_ori = false;
    Pixbuf *pb = nullptr;
    GError *error = nullptr;
    {
        GdkPixbuf *buf = nullptr;
        GdkPixbufLoader *loader = nullptr;
        std::string::size_type idx;
        idx = fn.rfind('.');
        bool is_svg = false;    
        if(idx != std::string::npos)
        {
            if (boost::iequals(fn.substr(idx+1).c_str(), "svg")) {
                auto svgDoc = SPDocument::createNewDocFromMem({data, len}, fn.c_str());

                // Check the document loaded properly
                if (!svgDoc || !svgDoc->getRoot()) {
                    return nullptr;
                }

                Inkscape::Preferences *prefs = Inkscape::Preferences::get();
                double dpi = prefs->getDouble("/dialogs/import/defaultxdpi/value", 96.0);
                if (svgdpi && svgdpi > 0) {
                    dpi = svgdpi;
                }

                // Get the size of the document
                Inkscape::Util::Quantity svgWidth = svgDoc->getWidth();
                Inkscape::Util::Quantity svgHeight = svgDoc->getHeight();
                // Limit the size of the document to 100 inches square
                const double svgWidth_px = std::min(svgWidth.value("px"), dpi * 100);
                const double svgHeight_px = std::min(svgHeight.value("px"), dpi * 100);
                if (svgWidth_px < 0 || svgHeight_px < 0) {
                    g_warning("create_from_buffer: malformed document: svgWidth_px=%f, svgHeight_px=%f", svgWidth_px,
                              svgHeight_px);
                    return nullptr;
                }

                Geom::Rect area(0, 0, svgWidth_px, svgHeight_px);
                pb = sp_generate_internal_bitmap(svgDoc.get(), area, dpi);
                if (!pb)
                    return nullptr;

                buf = pb->getPixbufRaw();

                // Tidy up
                if (buf == nullptr) {
                    delete pb;
                    return nullptr;
                }
                buf = Pixbuf::apply_embedded_orientation(buf);
                is_svg = true;
            }
        }
        if (!is_svg) {
            loader = gdk_pixbuf_loader_new();
            gdk_pixbuf_loader_write(loader, (guchar *) data, len, &error);
            if (error != nullptr) {
                std::cerr << "Pixbuf::create_from_file: " << error->message << std::endl;
                std::cerr << "   (" << fn << ")" << std::endl;
                g_free(data);
                g_object_unref(loader);
                return nullptr;
            }

            gdk_pixbuf_loader_close(loader, &error);
            if (error != nullptr) {
                std::cerr << "Pixbuf::create_from_file: " << error->message << std::endl;
                std::cerr << "   (" << fn << ")" << std::endl;
                g_free(data);
                g_object_unref(loader);
                return nullptr;
            }
            
            buf = gdk_pixbuf_loader_get_pixbuf(loader);
            if (buf) {
                // gdk_pixbuf_loader_get_pixbuf returns a borrowed reference
                g_object_ref(buf);
                has_ori = Pixbuf::get_embedded_orientation(buf) != Geom::identity();
                buf = Pixbuf::apply_embedded_orientation(buf);
                pb = new Pixbuf(buf);
            }
        }

        if (pb) {
            pb->_path = fn;
            if (is_svg) {
                pb->_setMimeData((guchar *) data, len, "svg");
            } else if(!has_ori) {
                // We DO NOT want to store the original data if it contains orientation
                // data since many exports that will use the surface do not handle it.
                GdkPixbufFormat *fmt = gdk_pixbuf_loader_get_format(loader);
                gchar *fmt_name = gdk_pixbuf_format_get_name(fmt);
                pb->_setMimeData((guchar *) data, len, fmt_name);
                g_free(fmt_name);
                g_object_unref(loader);
            }
        } else {
            std::cerr << "Pixbuf::create_from_file: failed to load contents: " << fn << std::endl;
            g_free(data);
        }

        // TODO: we could also read DPI, ICC profile, gamma correction, and other information
        // from the file. This can be done by using format-specific libraries e.g. libpng.
    }

    return pb;
}

/**
 * Converts the pixbuf to GdkPixbuf pixel format.
 * The returned pixbuf can be used e.g. in calls to gdk_pixbuf_save().
 */
GdkPixbuf *Pixbuf::getPixbufRaw(bool convert_format)
{
    if (convert_format) {
        ensurePixelFormat(PF_GDK);
    }
    return _pixbuf;
}

GdkPixbuf *Pixbuf::getPixbufRaw() const
{
    assert(_pixel_format == PF_GDK);
    return _pixbuf;
}

/**
 * Converts the pixbuf to Cairo pixel format and returns an image surface
 * which can be used as a source.
 *
 * The returned surface is owned by the GdkPixbuf and should not be freed.
 * Calling this function causes the pixbuf to be unsuitable for use
 * with GTK drawing functions until ensurePixelFormat(Pixbuf::PIXEL_FORMAT_PIXBUF) is called.
 */
cairo_surface_t *Pixbuf::getSurfaceRaw()
{
    ensurePixelFormat(PF_CAIRO);
    return _surface;
}

cairo_surface_t *Pixbuf::getSurfaceRaw() const
{
    assert(_pixel_format == PF_CAIRO);
    return _surface;
}

/* Declaring this function in the header requires including <gdkmm/pixbuf.h>,
 * which stupidly includes <glibmm.h> which in turn pulls in <glibmm/threads.h>.
 * However, since glib 2.32, <glibmm/threads.h> has to be included before <glib.h>
 * when compiling with G_DISABLE_DEPRECATED, as we do in non-release builds.
 * This necessitates spamming a lot of files with #include <glibmm/threads.h>
 * at the top.
 *
 * Since we don't really use gdkmm, do not define this function for now. */

/*
Glib::RefPtr<Gdk::Pixbuf> Pixbuf::getPixbuf(bool convert_format = true)
{
    g_object_ref(_pixbuf);
    Glib::RefPtr<Gdk::Pixbuf> p(getPixbuf(convert_format));
    return p;
}
*/

Cairo::RefPtr<Cairo::Surface> Pixbuf::getSurface()
{
    return Cairo::RefPtr<Cairo::Surface>(new Cairo::Surface(getSurfaceRaw(), false));
}

/** Retrieves the original compressed data for the surface, if any.
 * The returned data belongs to the object and should not be freed. */
guchar const *Pixbuf::getMimeData(gsize &len, std::string &mimetype) const
{
    static gchar const *mimetypes[] = {
        CAIRO_MIME_TYPE_JPEG, CAIRO_MIME_TYPE_JP2, CAIRO_MIME_TYPE_PNG, nullptr };
    static guint mimetypes_len = g_strv_length(const_cast<gchar**>(mimetypes));

    guchar const *data = nullptr;

    for (guint i = 0; i < mimetypes_len; ++i) {
        unsigned long len_long = 0;
        cairo_surface_get_mime_data(const_cast<cairo_surface_t*>(_surface), mimetypes[i], &data, &len_long);
        if (data != nullptr) {
			len = len_long;
            mimetype = mimetypes[i];
            break;
        }
    }

    return data;
}

int Pixbuf::width() const {
    return gdk_pixbuf_get_width(const_cast<GdkPixbuf*>(_pixbuf));
}
int Pixbuf::height() const {
    return gdk_pixbuf_get_height(const_cast<GdkPixbuf*>(_pixbuf));
}
int Pixbuf::rowstride() const {
    return gdk_pixbuf_get_rowstride(const_cast<GdkPixbuf*>(_pixbuf));
}
guchar const *Pixbuf::pixels() const {
    return gdk_pixbuf_get_pixels(const_cast<GdkPixbuf*>(_pixbuf));
}
guchar *Pixbuf::pixels() {
    return gdk_pixbuf_get_pixels(_pixbuf);
}
void Pixbuf::markDirty() {
    cairo_surface_mark_dirty(_surface);
}

void Pixbuf::_forceAlpha()
{
    if (gdk_pixbuf_get_has_alpha(_pixbuf)) return;

    GdkPixbuf *old = _pixbuf;
    _pixbuf = gdk_pixbuf_add_alpha(old, FALSE, 0, 0, 0);
    g_object_unref(old);
}

void Pixbuf::_setMimeData(guchar *data, gsize len, Glib::ustring const &format)
{
    gchar const *mimetype = nullptr;

    if (format == "jpeg") {
        mimetype = CAIRO_MIME_TYPE_JPEG;
    } else if (format == "jpeg2000") {
        mimetype = CAIRO_MIME_TYPE_JP2;
    } else if (format == "png") {
        mimetype = CAIRO_MIME_TYPE_PNG;
    }

    if (mimetype != nullptr) {
        cairo_surface_set_mime_data(_surface, mimetype, data, len, g_free, data);
        //g_message("Setting Cairo MIME data: %s", mimetype);
    } else {
        g_free(data);
        //g_message("Not setting Cairo MIME data: unknown format %s", name.c_str());
    }
}

/**
 * Convert the internal pixel format between CAIRO and GDK formats.
 */
void Pixbuf::ensurePixelFormat(PixelFormat fmt)
{
    if (fmt == PF_CAIRO && _pixel_format == PF_GDK) {
        ensure_argb32(_pixbuf);
        _pixel_format = fmt;
    } else if (fmt == PF_GDK && _pixel_format == PF_CAIRO) {
        ensure_pixbuf(_pixbuf);
        _pixel_format = fmt;
    } else if (fmt != _pixel_format) {
        g_assert_not_reached();
    }
}

/**
 * Converts GdkPixbuf's data to premultiplied ARGB.
 * This function will convert a GdkPixbuf in place into Cairo's native pixel format.
 * Note that this is a hack intended to save memory. When the pixbuf is in Cairo's format,
 * using it with GTK will result in corrupted drawings.
 */
void Pixbuf::ensure_argb32(GdkPixbuf *pb)
{
    convert_pixels_pixbuf_to_argb32(
        gdk_pixbuf_get_pixels(pb),
        gdk_pixbuf_get_width(pb),
        gdk_pixbuf_get_height(pb),
        gdk_pixbuf_get_rowstride(pb));
}

/**
 * Converts GdkPixbuf's data back to its native format.
 * Once this is done, the pixbuf can be used with GTK again.
 */
void Pixbuf::ensure_pixbuf(GdkPixbuf *pb)
{
    convert_pixels_argb32_to_pixbuf(
        gdk_pixbuf_get_pixels(pb),
        gdk_pixbuf_get_width(pb),
        gdk_pixbuf_get_height(pb),
        gdk_pixbuf_get_rowstride(pb));
}

} // namespace Inkscape

/*
 * Can be called recursively.
 * If optimize_stroke == false, the view Rect is not used.
 */
static void
feed_curve_to_cairo(cairo_t *cr, Geom::Curve const &c, Geom::Affine const &trans, Geom::Rect const &view, bool optimize_stroke)
{
    using Geom::X;
    using Geom::Y;

    unsigned order = 0;
    if (auto b = dynamic_cast<Geom::BezierCurve const*>(&c)) {
        order = b->order();
    }

    // handle the three typical curve cases
    switch (order) {
    case 1:
    {
        Geom::Point end_tr = c.finalPoint() * trans;
        if (!optimize_stroke) {
            cairo_line_to(cr, end_tr[0], end_tr[1]);
        } else {
            Geom::Rect swept(c.initialPoint()*trans, end_tr);
            if (swept.intersects(view)) {
                cairo_line_to(cr, end_tr[0], end_tr[1]);
            } else {
                cairo_move_to(cr, end_tr[0], end_tr[1]);
            }
        }
    }
    break;
    case 2:
    {
        auto quadratic_bezier = static_cast<Geom::QuadraticBezier const*>(&c);
        std::array<Geom::Point, 3> points;
        for (int i = 0; i < 3; i++) {
            points[i] = quadratic_bezier->controlPoint(i) * trans;
        }
        // degree-elevate to cubic Bezier, since Cairo doesn't do quadratic Beziers
        Geom::Point b1 = points[0] + (2./3) * (points[1] - points[0]);
        Geom::Point b2 = b1 + (1./3) * (points[2] - points[0]);
        if (!optimize_stroke) {
            cairo_curve_to(cr, b1[X], b1[Y], b2[X], b2[Y], points[2][X], points[2][Y]);
        } else {
            Geom::Rect swept(points[0], points[2]);
            swept.expandTo(points[1]);
            if (swept.intersects(view)) {
                cairo_curve_to(cr, b1[X], b1[Y], b2[X], b2[Y], points[2][X], points[2][Y]);
            } else {
                cairo_move_to(cr, points[2][X], points[2][Y]);
            }
        }
    }
    break;
    case 3:
    {
        auto cubic_bezier = static_cast<Geom::CubicBezier const*>(&c);
        std::array<Geom::Point, 4> points;
        for (int i = 0; i < 4; i++) {
            points[i] = cubic_bezier->controlPoint(i);
        }
        //points[0] *= trans; // don't do this one here for fun: it is only needed for optimized strokes
        points[1] *= trans;
        points[2] *= trans;
        points[3] *= trans;
        if (!optimize_stroke) {
            cairo_curve_to(cr, points[1][X], points[1][Y], points[2][X], points[2][Y], points[3][X], points[3][Y]);
        } else {
            points[0] *= trans;  // didn't transform this point yet
            Geom::Rect swept(points[0], points[3]);
            swept.expandTo(points[1]);
            swept.expandTo(points[2]);
            if (swept.intersects(view)) {
                cairo_curve_to(cr, points[1][X], points[1][Y], points[2][X], points[2][Y], points[3][X], points[3][Y]);
            } else {
                cairo_move_to(cr, points[3][X], points[3][Y]);
            }
        }
    }
    break;
    default:
    {
        if (Geom::EllipticalArc const *arc = dynamic_cast<Geom::EllipticalArc const*>(&c)) {
            if (arc->isChord()) {
                Geom::Point endPoint(arc->finalPoint());
                cairo_line_to(cr, endPoint[0], endPoint[1]);
            } else {
                Geom::Affine xform = arc->unitCircleTransform() * trans;
                // Don't draw anything if the angle is borked
                if(std::isnan(arc->initialAngle()) || std::isnan(arc->finalAngle())) {
                    g_warning("Bad angle while drawing EllipticalArc");
                    break;
                }

                // Apply the transformation to the current context
                auto cm = geom_to_cairo(xform);

                cairo_save(cr);
                cairo_transform(cr, &cm);

                // Draw the circle
                if (arc->sweep()) {
                    cairo_arc(cr, 0, 0, 1, arc->initialAngle(), arc->finalAngle());
                } else {
                    cairo_arc_negative(cr, 0, 0, 1, arc->initialAngle(), arc->finalAngle());
                }
                // Revert the current context
                cairo_restore(cr);
            }
        } else {
            // handles sbasis as well as all other curve types
            // this is very slow
            Geom::Path sbasis_path = Geom::cubicbezierpath_from_sbasis(c.toSBasis(), 0.1);

            // recurse to convert the new path resulting from the sbasis to svgd
            for (const auto & iter : sbasis_path) {
                feed_curve_to_cairo(cr, iter, trans, view, optimize_stroke);
            }
        }
    }
    break;
    }
}


/** Feeds path-creating calls to the cairo context translating them from the Path */
static void
feed_path_to_cairo (cairo_t *ct, Geom::Path const &path)
{
    if (path.empty())
        return;

    cairo_move_to(ct, path.initialPoint()[0], path.initialPoint()[1] );

    for (Geom::Path::const_iterator cit = path.begin(); cit != path.end_open(); ++cit) {
        feed_curve_to_cairo(ct, *cit, Geom::identity(), Geom::Rect(), false); // optimize_stroke is false, so the view rect is not used
    }

    if (path.closed()) {
        cairo_close_path(ct);
    }
}

/** Feeds path-creating calls to the cairo context translating them from the Path, with the given transform and shift */
static void
feed_path_to_cairo (cairo_t *ct, Geom::Path const &path, Geom::Affine trans, Geom::OptRect area, bool optimize_stroke, double stroke_width)
{
    if (!area)
        return;
    if (path.empty())
        return;

    // Transform all coordinates to coords within "area"
    Geom::Point shift = area->min();
    Geom::Rect view = *area;
    view.expandBy (stroke_width);
    view = view * (Geom::Affine)Geom::Translate(-shift);
    //  Pass transformation to feed_curve, so that we don't need to create a whole new path.
    Geom::Affine transshift(trans * Geom::Translate(-shift));

    Geom::Point initial = path.initialPoint() * transshift;
    cairo_move_to(ct, initial[0], initial[1] );

    for(Geom::Path::const_iterator cit = path.begin(); cit != path.end_open(); ++cit) {
        feed_curve_to_cairo(ct, *cit, transshift, view, optimize_stroke);
    }

    if (path.closed()) {
        if (!optimize_stroke) {
            cairo_close_path(ct);
        } else {
            cairo_line_to(ct, initial[0], initial[1]);
            /* We cannot use cairo_close_path(ct) here because some parts of the path may have been
               clipped and not drawn (maybe the before last segment was outside view area), which 
               would result in closing the "subpath" after the last interruption, not the entire path.

               However, according to cairo documentation:
               The behavior of cairo_close_path() is distinct from simply calling cairo_line_to() with the equivalent coordinate
               in the case of stroking. When a closed sub-path is stroked, there are no caps on the ends of the sub-path. Instead,
               there is a line join connecting the final and initial segments of the sub-path. 

               The correct fix will be possible when cairo introduces methods for moving without
               ending/starting subpaths, which we will use for skipping invisible segments; then we
               will be able to use cairo_close_path here. This issue also affects ps/eps/pdf export,
               see bug 168129
            */
        }
    }
}

/** Feeds path-creating calls to the cairo context translating them from the PathVector, with the given transform and shift
 *  One must have done cairo_new_path(ct); before calling this function. */
void
feed_pathvector_to_cairo (cairo_t *ct, Geom::PathVector const &pathv, Geom::Affine trans, Geom::OptRect area, bool optimize_stroke, double stroke_width)
{
    if (!area)
        return;
    if (pathv.empty())
        return;

    for(const auto & it : pathv) {
        feed_path_to_cairo(ct, it, trans, area, optimize_stroke, stroke_width);
    }
}

/** Feeds path-creating calls to the cairo context translating them from the PathVector
 *  One must have done cairo_new_path(ct); before calling this function. */
void
feed_pathvector_to_cairo (cairo_t *ct, Geom::PathVector const &pathv)
{
    if (pathv.empty())
        return;

    for(const auto & it : pathv) {
        feed_path_to_cairo(ct, it);
    }
}

/*
 * Pulls out the last cairo path context and reconstitutes it
 * into a local geom path vector for inkscape use.
 *
 * @param ct - The cairo context
 *
 * @returns an optioal Geom::PathVector object
 */
std::optional<Geom::PathVector> extract_pathvector_from_cairo(cairo_t *ct)
{
    cairo_path_t *path = cairo_copy_path(ct);
    if (!path)
        return std::nullopt;

    auto path_freer = scope_exit([&] { cairo_path_destroy(path); });

    Geom::PathBuilder res;
    auto end = &path->data[path->num_data];
    for (auto p = &path->data[0]; p < end; p += p->header.length) {
        switch (p->header.type) {
            case CAIRO_PATH_MOVE_TO:
                if (p->header.length != 2)
                    return std::nullopt;
                res.moveTo(Geom::Point(p[1].point.x, p[1].point.y));
                break;

            case CAIRO_PATH_LINE_TO:
                if (p->header.length != 2)
                    return std::nullopt;
                res.lineTo(Geom::Point(p[1].point.x, p[1].point.y));
                break;

            case CAIRO_PATH_CURVE_TO:
                if (p->header.length != 4)
                    return std::nullopt;
                res.curveTo(Geom::Point(p[1].point.x, p[1].point.y), Geom::Point(p[2].point.x, p[2].point.y),
                            Geom::Point(p[3].point.x, p[3].point.y));
                break;

            case CAIRO_PATH_CLOSE_PATH:
                if (p->header.length != 1)
                    return std::nullopt;
                res.closePath();
                break;
            default:
                return std::nullopt;
        }
    }

    res.flush();
    return res.peek();
}

SPColorInterpolation
get_cairo_surface_ci(cairo_surface_t *surface) {
    void* data = cairo_surface_get_user_data( surface, &ink_color_interpolation_key );
    if( data != nullptr ) {
        return (SPColorInterpolation)GPOINTER_TO_INT( data );
    } else {
        return SP_CSS_COLOR_INTERPOLATION_AUTO;
    }
}

/** Set the color_interpolation_value for a Cairo surface.
 *  Transform the surface between sRGB and linearRGB if necessary. */
void
set_cairo_surface_ci(cairo_surface_t *surface, SPColorInterpolation ci) {

    if( cairo_surface_get_content( surface ) != CAIRO_CONTENT_ALPHA ) {

        SPColorInterpolation ci_in = get_cairo_surface_ci( surface );

        if( ci_in == SP_CSS_COLOR_INTERPOLATION_SRGB &&
            ci    == SP_CSS_COLOR_INTERPOLATION_LINEARRGB ) {
            ink_cairo_surface_srgb_to_linear( surface );
        }
        if( ci_in == SP_CSS_COLOR_INTERPOLATION_LINEARRGB &&
            ci    == SP_CSS_COLOR_INTERPOLATION_SRGB ) {
            ink_cairo_surface_linear_to_srgb( surface );
        }

        cairo_surface_set_user_data(surface, &ink_color_interpolation_key, GINT_TO_POINTER (ci), nullptr);
    }
}

void
copy_cairo_surface_ci(cairo_surface_t *in, cairo_surface_t *out) {
    cairo_surface_set_user_data(out, &ink_color_interpolation_key, cairo_surface_get_user_data(in, &ink_color_interpolation_key), nullptr);
}

namespace Colors = Inkscape::Colors;

/**
 * Set the source color of the Cairo context.
 *
 * @arg ctx - The cairo context C pointer, must exist.
 * @arg color - The Color object containing floating point channel values.
 * @arg to_srgb - Default true, does a conversion to sRGB if needed, force no conversion
 *                to hack the data of the Cairo surface to non sRGB spaces.
 */
void ink_cairo_set_source_color(Cairo::RefPtr<Cairo::Context> &ctx, Colors::Color const &color, bool to_srgb)
{
    ink_cairo_set_source_color(ctx->cobj(), color, to_srgb);
}
void ink_cairo_set_source_color(cairo_t *ctx, Colors::Color const &color, bool to_srgb)
{
    if (!to_srgb || *color.getSpace() == Colors::Space::Type::RGB) {
        // Do not copy the color if we don't need to.
        // This sets floats so if the Surface is RGB96F or RGBA128F your surface will have more color data.
        cairo_set_source_rgba(ctx, color[0], color[1], color[2], color.getOpacity());
    } else if (auto copy = color.converted(Colors::Space::Type::RGB)) {
        ink_cairo_set_source_color(ctx, *copy, false); // Callback
    }
}
void ink_cairo_pattern_add_color_stop(cairo_pattern_t *ptn, double offset, Colors::Color const &color, bool to_srgb)
{
    if (!to_srgb || *color.getSpace() == Colors::Space::Type::RGB) {
        cairo_pattern_add_color_stop_rgba(ptn, offset, color[0], color[1], color[2], color.getOpacity());
    } else if (auto copy = color.converted(Colors::Space::Type::RGB)) {
        ink_cairo_pattern_add_color_stop(ptn, offset, *copy, false); // Callback
    }
}
cairo_pattern_t *ink_cairo_pattern_create(Colors::Color const &color, bool to_srgb)
{
    if (!to_srgb || *color.getSpace() == Colors::Space::Type::RGB) {
        return cairo_pattern_create_rgba(color[0], color[1], color[2], color.getOpacity());
    } else if (auto copy = color.converted(Colors::Space::Type::RGB)) {
        return ink_cairo_pattern_create(*copy, false);
    }
    return nullptr;
}

void ink_cairo_mesh_pattern_set_corner_color(cairo_pattern_t *pattern, unsigned corner_num, Colors::Color color)
{
    color.convert(Colors::Space::Type::RGB);
    cairo_mesh_pattern_set_corner_color_rgba(pattern, corner_num, color[0], color[1], color[2], color.getOpacity());
}


void ink_matrix_to_2geom(Geom::Affine &m, cairo_matrix_t const &cm)
{
    m[0] = cm.xx;
    m[2] = cm.xy;
    m[4] = cm.x0;
    m[1] = cm.yx;
    m[3] = cm.yy;
    m[5] = cm.y0;
}

void ink_matrix_to_cairo(cairo_matrix_t &cm, Geom::Affine const &m)
{
    cm.xx = m[0];
    cm.xy = m[2];
    cm.x0 = m[4];
    cm.yx = m[1];
    cm.yy = m[3];
    cm.y0 = m[5];
}

Geom::Affine ink_matrix_to_2geom(cairo_matrix_t const &cairo_matrix)
{
    Geom::Affine result;
    ink_matrix_to_2geom(result, cairo_matrix);
    return result;
}

void
ink_cairo_transform(cairo_t *ct, Geom::Affine const &m)
{
    cairo_matrix_t cm;
    ink_matrix_to_cairo(cm, m);
    cairo_transform(ct, &cm);
}

void
ink_cairo_pattern_set_matrix(cairo_pattern_t *cp, Geom::Affine const &m)
{
    cairo_matrix_t cm;
    ink_matrix_to_cairo(cm, m);
    cairo_pattern_set_matrix(cp, &cm);
}

void
ink_cairo_set_hairline(cairo_t *ct)
{
#ifdef CAIRO_HAS_HAIRLINE
    cairo_set_hairline(ct, true);
#else
    // As a backup, use a device unit of 1
    double x = 1.0, y = 0.0;
    cairo_device_to_user_distance(ct, &x, &y);
    cairo_set_line_width(ct, std::hypot(x, y));
#endif
}

void ink_cairo_pattern_set_dither(cairo_pattern_t *pattern, bool enabled)
{
#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 18, 0)
    cairo_pattern_set_dither(pattern, enabled ? CAIRO_DITHER_BEST : CAIRO_DITHER_NONE);
#endif
}

/**
 * Create an exact copy of a surface.
 * Creates a surface that has the same type, content type, dimensions and contents
 * as the specified surface.
 */
cairo_surface_t *
ink_cairo_surface_copy(cairo_surface_t *s)
{
    cairo_surface_t *ns = ink_cairo_surface_create_identical(s);

    if (cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE) {
        // use memory copy instead of using a Cairo context
        cairo_surface_flush(s);
        int stride = cairo_image_surface_get_stride(s);
        int h = cairo_image_surface_get_height(s);
        memcpy(cairo_image_surface_get_data(ns), cairo_image_surface_get_data(s), stride * h);
        cairo_surface_mark_dirty(ns);
    } else {
        // generic implementation
        cairo_t *ct = cairo_create(ns);
        cairo_set_source_surface(ct, s, 0, 0);
        cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
        cairo_paint(ct);
        cairo_destroy(ct);
    }

    return ns;
}

/**
 * Create an exact copy of an image surface.
 */
Cairo::RefPtr<Cairo::ImageSurface>
ink_cairo_surface_copy(Cairo::RefPtr<Cairo::ImageSurface> surface )
{
    int width  = surface->get_width();
    int height = surface->get_height();
    int stride = surface->get_stride();
    auto new_surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, width, height); // device scale?

    surface->flush();
    memcpy(new_surface->get_data(), surface->get_data(), stride * height);
    new_surface->mark_dirty(); // Clear caches. Mandatory after messing directly with contents.

    return new_surface;
}

/**
 * Create a surface that differs only in pixel content.
 * Creates a surface that has the same type, content type and dimensions
 * as the specified surface. Pixel contents are not copied.
 */
cairo_surface_t *
ink_cairo_surface_create_identical(cairo_surface_t *s)
{
    cairo_surface_t *ns = ink_cairo_surface_create_same_size(s, cairo_surface_get_content(s));
    cairo_surface_set_user_data(ns, &ink_color_interpolation_key, cairo_surface_get_user_data(s, &ink_color_interpolation_key), nullptr);
    return ns;
}

cairo_surface_t *
ink_cairo_surface_create_same_size(cairo_surface_t *s, cairo_content_t c)
{
    // ink_cairo_surface_get_width()/height() returns value in pixels
    // cairo_surface_create_similar() uses device units
    double x_scale = 0;
    double y_scale = 0;
    cairo_surface_get_device_scale( s, &x_scale, &y_scale );

    assert (x_scale > 0);
    assert (y_scale > 0);

    cairo_surface_t *ns =
        cairo_surface_create_similar(s, c,
                                     ink_cairo_surface_get_width(s)/x_scale,
                                     ink_cairo_surface_get_height(s)/y_scale);
    return ns;
}

/**
 * Extract the alpha channel into a new surface.
 * Creates a surface with a content type of CAIRO_CONTENT_ALPHA that contains
 * the alpha values of pixels from @a s.
 */
cairo_surface_t *
ink_cairo_extract_alpha(cairo_surface_t *s)
{
    cairo_surface_t *alpha = ink_cairo_surface_create_same_size(s, CAIRO_CONTENT_ALPHA);

    cairo_t *ct = cairo_create(alpha);
    cairo_set_source_surface(ct, s, 0, 0);
    cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
    cairo_paint(ct);
    cairo_destroy(ct);

    return alpha;
}

cairo_surface_t *
ink_cairo_surface_create_output(cairo_surface_t *image, cairo_surface_t *bg)
{
    cairo_content_t imgt = cairo_surface_get_content(image);
    cairo_content_t bgt = cairo_surface_get_content(bg);
    cairo_surface_t *out = nullptr;

    if (bgt == CAIRO_CONTENT_ALPHA && imgt == CAIRO_CONTENT_ALPHA) {
        out = ink_cairo_surface_create_identical(bg);
    } else {
        out = ink_cairo_surface_create_same_size(bg, CAIRO_CONTENT_COLOR_ALPHA);
    }

    return out;
}

void
ink_cairo_surface_blit(cairo_surface_t *src, cairo_surface_t *dest)
{
    if (cairo_surface_get_type(src) == CAIRO_SURFACE_TYPE_IMAGE &&
        cairo_surface_get_type(dest) == CAIRO_SURFACE_TYPE_IMAGE &&
        cairo_image_surface_get_format(src) == cairo_image_surface_get_format(dest) &&
        cairo_image_surface_get_height(src) == cairo_image_surface_get_height(dest) &&
        cairo_image_surface_get_width(src) == cairo_image_surface_get_width(dest) &&
        cairo_image_surface_get_stride(src) == cairo_image_surface_get_stride(dest))
    {
        // use memory copy instead of using a Cairo context
        cairo_surface_flush(src);
        int stride = cairo_image_surface_get_stride(src);
        int h = cairo_image_surface_get_height(src);
        memcpy(cairo_image_surface_get_data(dest), cairo_image_surface_get_data(src), stride * h);
        cairo_surface_mark_dirty(dest);
    } else {
        // generic implementation
        cairo_t *ct = cairo_create(dest);
        cairo_set_source_surface(ct, src, 0, 0);
        cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
        cairo_paint(ct);
        cairo_destroy(ct);
    }
}

/**
 * Return width in pixels.
 */
int
ink_cairo_surface_get_width(cairo_surface_t *surface)
{
    // For now only image surface is handled.
    // Later add others, e.g. cairo-gl
    assert(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE);
    return cairo_image_surface_get_width(surface);
}

/**
 * Return height in pixels.
 */
int
ink_cairo_surface_get_height(cairo_surface_t *surface)
{
    assert(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE);
    return cairo_image_surface_get_height(surface);
}

static double ink_cairo_surface_average_color_internal(cairo_surface_t *surface, cairo_surface_t *mask, double &rf, double &gf, double &bf, double &af)
{
    rf = gf = bf = af = 0.0;

    cairo_surface_flush(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    double count = 0;

    if (width * 4 != cairo_image_surface_get_stride(surface)) {
        g_warning("Stride and width don't match, expect zero buffer for average color.");
    }

    unsigned char *mask_data = nullptr;
    if (mask) {
        cairo_surface_flush(mask);
        if (width != cairo_image_surface_get_width(mask)
         || height != cairo_image_surface_get_height(mask)) {
            g_warning("Mask size must be exactly the same size as the image surface.");
            mask = nullptr;
        } else {
            mask_data = cairo_image_surface_get_data(mask);
        }
    }

    for (int p = 0; p < (width * height); ++p, data += 4) {
        EXTRACT_ARGB32(*reinterpret_cast<guint32*>(data), a, r, g, b)

        double amount = mask_data ? mask_data[p] / 255.0 : 1.0;
        rf += r / 255.0 * amount;
        gf += g / 255.0 * amount;
        bf += b / 255.0 * amount;
        af += a / 255.0 * amount;
        count += amount;
    }
    return count;
}

// We extract colors from pattern background, if we need to extract sometimes from a gradient we can add
// a extra parameter with the spot number and use cairo_pattern_get_color_stop_rgba
// also if the pattern is a image we can pass a boolean like solid = false to get the color by image average ink_cairo_surface_average_color
guint32 ink_cairo_pattern_get_argb32(cairo_pattern_t *pattern)
{
    double red = 0;
    double green = 0;
    double blue = 0;
    double alpha = 0;
    auto status = cairo_pattern_get_rgba(pattern, &red, &green, &blue, &alpha);
    if (status != CAIRO_STATUS_PATTERN_TYPE_MISMATCH) {
        // in ARGB32 format
        return SP_RGBA32_F_COMPOSE(alpha, red, green, blue);
    }
        
    cairo_surface_t *surface;
    status = cairo_pattern_get_surface (pattern, &surface);
    if (status != CAIRO_STATUS_PATTERN_TYPE_MISMATCH) {
        // first pixel only
        auto *pxbsurface =  cairo_image_surface_get_data(surface);
        return *reinterpret_cast<guint32 const *>(pxbsurface);
    }
    return 0;
}

/**
 * Get the average color from the given surface.
 *
 * @arg surface - The cairo surface to get data from.
 * @arg masked - If true, ignores any truely invisible pixels.
 */
Colors::Color ink_cairo_surface_average_color(cairo_surface_t *surface, cairo_surface_t * mask)
{
    double r, g, b, a = 0.0;
    double count = ink_cairo_surface_average_color_internal(surface, mask, r, g, b, a);
    if (a == 0.0) {
        return Colors::Color(Colors::Space::Type::RGB, {0, 0, 0, 0});
    }
    auto color = Colors::Color(Colors::Space::Type::RGB, {r / a, g / a, b / a, a / count});
    color.normalize();
    return color;
}

static guint32 srgb_to_linear( const guint32 c, const guint32 a ) {

    const guint32 c1 = unpremul_alpha( c, a );

    double cc = c1/255.0;

    if( cc < 0.04045 ) {
        cc /= 12.92;
    } else {
        cc = pow( (cc+0.055)/1.055, 2.4 );
    }
    cc *= 255.0;

    const guint32 c2 = (int)cc;

    return premul_alpha( c2, a );
}

static guint32 linear_to_srgb( const guint32 c, const guint32 a ) {

    const guint32 c1 = unpremul_alpha( c, a );

    double cc = c1/255.0;

    if( cc < 0.0031308 ) {
        cc *= 12.92;
    } else {
        cc = pow( cc, 1.0/2.4 )*1.055-0.055;
    }
    cc *= 255.0;

    const guint32 c2 = (int)cc;

    return premul_alpha( c2, a );
}

static uint32_t srgb_to_linear_argb32(uint32_t in)
{
    EXTRACT_ARGB32(in, a, r, g, b);
    if (a != 0) {
        r = srgb_to_linear(r, a);
        g = srgb_to_linear(g, a);
        b = srgb_to_linear(b, a);
    }
    ASSEMBLE_ARGB32(out, a, r, g, b);
    return out;
}

int ink_cairo_surface_srgb_to_linear(cairo_surface_t *surface)
{
    cairo_surface_flush(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);

    ink_cairo_surface_filter(surface, surface, srgb_to_linear_argb32);

    return width * height;
}

static uint32_t linear_to_srgb_argb32(uint32_t in)
{
    EXTRACT_ARGB32(in, a, r, g, b);
    if (a != 0) {
        r = linear_to_srgb(r, a);
        g = linear_to_srgb(g, a);
        b = linear_to_srgb(b, a);
    }
    ASSEMBLE_ARGB32(out, a, r, g, b);
    return out;
}

SPBlendMode ink_cairo_operator_to_css_blend(cairo_operator_t cairo_operator)
{
    // All of the blend modes are implemented in Cairo as of 1.10.
    // For a detailed description, see:
    // http://cairographics.org/operators/

    switch (cairo_operator) {
        case CAIRO_OPERATOR_MULTIPLY:
            return SP_CSS_BLEND_MULTIPLY;
        case CAIRO_OPERATOR_SCREEN:
            return SP_CSS_BLEND_SCREEN;
        case CAIRO_OPERATOR_DARKEN:
            return SP_CSS_BLEND_DARKEN;
        case CAIRO_OPERATOR_LIGHTEN:
            return SP_CSS_BLEND_LIGHTEN;
        case CAIRO_OPERATOR_OVERLAY:
            return SP_CSS_BLEND_OVERLAY;
        case CAIRO_OPERATOR_COLOR_DODGE:
            return SP_CSS_BLEND_COLORDODGE;
        case CAIRO_OPERATOR_COLOR_BURN:
            return SP_CSS_BLEND_COLORBURN;
        case CAIRO_OPERATOR_HARD_LIGHT:
            return SP_CSS_BLEND_HARDLIGHT;
        case CAIRO_OPERATOR_SOFT_LIGHT:
            return SP_CSS_BLEND_SOFTLIGHT;
        case CAIRO_OPERATOR_DIFFERENCE:
            return SP_CSS_BLEND_DIFFERENCE;
        case CAIRO_OPERATOR_EXCLUSION:
            return SP_CSS_BLEND_EXCLUSION;
        case CAIRO_OPERATOR_HSL_HUE:
            return SP_CSS_BLEND_HUE;
        case CAIRO_OPERATOR_HSL_SATURATION:
            return SP_CSS_BLEND_SATURATION;
        case CAIRO_OPERATOR_HSL_COLOR:
            return SP_CSS_BLEND_COLOR;
        case CAIRO_OPERATOR_HSL_LUMINOSITY:
            return SP_CSS_BLEND_LUMINOSITY;
        case CAIRO_OPERATOR_OVER:
            return SP_CSS_BLEND_NORMAL;
        default:
            return SP_CSS_BLEND_NORMAL;
    }
}

cairo_operator_t ink_css_blend_to_cairo_operator(SPBlendMode css_blend)
{
    // All of the blend modes are implemented in Cairo as of 1.10.
    // For a detailed description, see:
    // http://cairographics.org/operators/

    switch (css_blend) {
        case SP_CSS_BLEND_MULTIPLY:
            return CAIRO_OPERATOR_MULTIPLY;
        case SP_CSS_BLEND_SCREEN:
            return CAIRO_OPERATOR_SCREEN;
        case SP_CSS_BLEND_DARKEN:
            return CAIRO_OPERATOR_DARKEN;
        case SP_CSS_BLEND_LIGHTEN:
            return CAIRO_OPERATOR_LIGHTEN;
        case SP_CSS_BLEND_OVERLAY:
            return CAIRO_OPERATOR_OVERLAY;
        case SP_CSS_BLEND_COLORDODGE:
            return CAIRO_OPERATOR_COLOR_DODGE;
        case SP_CSS_BLEND_COLORBURN:
            return CAIRO_OPERATOR_COLOR_BURN;
        case SP_CSS_BLEND_HARDLIGHT:
            return CAIRO_OPERATOR_HARD_LIGHT;
        case SP_CSS_BLEND_SOFTLIGHT:
            return CAIRO_OPERATOR_SOFT_LIGHT;
        case SP_CSS_BLEND_DIFFERENCE:
            return CAIRO_OPERATOR_DIFFERENCE;
        case SP_CSS_BLEND_EXCLUSION:
            return CAIRO_OPERATOR_EXCLUSION;
        case SP_CSS_BLEND_HUE:
            return CAIRO_OPERATOR_HSL_HUE;
        case SP_CSS_BLEND_SATURATION:
            return CAIRO_OPERATOR_HSL_SATURATION;
        case SP_CSS_BLEND_COLOR:
            return CAIRO_OPERATOR_HSL_COLOR;
        case SP_CSS_BLEND_LUMINOSITY:
            return CAIRO_OPERATOR_HSL_LUMINOSITY;
        case SP_CSS_BLEND_NORMAL:
            return CAIRO_OPERATOR_OVER;
        default:
            g_error("Invalid SPBlendMode %d", css_blend);
            return CAIRO_OPERATOR_OVER;
    }
}

int ink_cairo_surface_linear_to_srgb(cairo_surface_t *surface)
{
    cairo_surface_flush(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);

    ink_cairo_surface_filter(surface, surface, linear_to_srgb_argb32);

    return width * height;
}

Cairo::RefPtr<Cairo::Pattern> ink_cairo_pattern_create_slanting_stripes(uint32_t color)
{
    constexpr int width = 10;
    constexpr int line_width = 10;

    auto surface = Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, width, 1);
    auto context = Cairo::Context::create(surface);
    context->rectangle(0, 0, line_width / 2.0, 1);
    ink_cairo_set_source_color(context, Colors::Color(color));
    context->fill();
    context->paint();

    auto pattern = Cairo::SurfacePattern::create(surface);
    pattern->set_extend(Cairo::SurfacePattern::Extend::REPEAT);
    pattern->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
    pattern->set_matrix(Cairo::rotation_matrix(3 * M_PI / 4));
    return pattern;
}

Cairo::RefPtr<Cairo::Pattern> create_checkerboard_pattern(uint32_t dark, uint32_t light, int size) {
    auto img = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 2 * size, 2 * size);
    auto ctx = Cairo::Context::create(img);
    ctx->set_source_rgb(SP_RGBA32_R_F(dark), SP_RGBA32_G_F(dark), SP_RGBA32_B_F(dark));
    ctx->paint();
    ctx->set_source_rgb(SP_RGBA32_R_F(light), SP_RGBA32_G_F(light), SP_RGBA32_B_F(light));
    ctx->rectangle(0, 0, size, size);
    ctx->fill();
    ctx->rectangle(size, size, size, size);
    ctx->fill();
    auto pattern = Cairo::SurfacePattern::create(img);
    pattern->set_extend(Cairo::Pattern::Extend::REPEAT);
    pattern->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
    return pattern;
}

Cairo::RefPtr<Cairo::Pattern> ink_cairo_pattern_create_checkerboard(guint32 rgba, bool use_alpha, int size) {
    auto color_a = Colors::Color(rgba, use_alpha);
    auto color_b = Inkscape::Colors::make_contrasted_color(color_a, 1.0);
    // Once the second color is generated, the original doesn't need alpha
    color_a.enableOpacity(false);
    return create_checkerboard_pattern(color_a.toRGBA(), color_b.toRGBA(), size);
}

/** 
 * Draw drop shadow around the 'rect' with given 'size' and 'color'; shadow extends to the right and bottom of rect.
 */
void ink_cairo_draw_drop_shadow(const Cairo::RefPtr<Cairo::Context> &ctx, const Geom::Rect& rect, double size, guint32 color, double color_alpha) {
    // draw fake drop shadow built from gradients
    const auto r = SP_RGBA32_R_F(color);
    const auto g = SP_RGBA32_G_F(color);
    const auto b = SP_RGBA32_B_F(color);
    const auto a = color_alpha;
    const Geom::Point corners[] = { rect.corner(0), rect.corner(1), rect.corner(2), rect.corner(3) };
    // space for gradient shadow
    double sw = size;
    double half = sw / 2;
    using Geom::X;
    using Geom::Y;
    // 8 gradients total: 4 sides + 4 corners
    auto grad_top    = Cairo::LinearGradient::create(0, corners[0][Y] + half, 0, corners[0][Y] - half);
    auto grad_right  = Cairo::LinearGradient::create(corners[1][X], 0, corners[1][X] + sw, 0);
    auto grad_bottom = Cairo::LinearGradient::create(0, corners[2][Y], 0, corners[2][Y] + sw);
    auto grad_left   = Cairo::LinearGradient::create(corners[0][X] + half, 0, corners[0][X] - half, 0);
    auto grad_btm_right = Cairo::RadialGradient::create(corners[2][X], corners[2][Y], 0, corners[2][X], corners[2][Y], sw);
    auto grad_top_right = Cairo::RadialGradient::create(corners[1][X], corners[1][Y] + half, 0, corners[1][X], corners[1][Y] + half, sw);
    auto grad_btm_left  = Cairo::RadialGradient::create(corners[3][X] + half, corners[3][Y], 0, corners[3][X] + half, corners[3][Y], sw);
    auto grad_top_left  = Cairo::RadialGradient::create(corners[0][X], corners[0][Y], 0, corners[0][X], corners[0][Y], half);
    const int N = 15; // number of gradient stops; stops used to make it non-linear
    // using easing function here: (exp(a*(1-t)) - 1) / (exp(a) - 1);
    // it has a nice property of growing from 0 to 1 for t in [0..1]
    const auto A = 4.0; // this coefficient changes how steep the curve is and controls shadow drop-off
    const auto denominator = exp(A) - 1;
    for (int i = 0; i <= N; ++i) {
        auto pos = static_cast<double>(i) / N;
        // exponential decay for drop shadow - long tail, with values from 100% down to 0% opacity
        auto t = 1 - pos; // reverse 't' so alpha drops from 1 to 0
        auto alpha = (exp(A * t) - 1) / denominator;
        grad_top->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_bottom->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_right->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_left->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_btm_right->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_top_right->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_btm_left->add_color_stop_rgba(pos, r, g, b, alpha * a);
        // this left/top corner is just a silver of the shadow: half of it is "hidden" beneath the page
        if (pos >= 0.5) {
            grad_top_left->add_color_stop_rgba(2 * (pos - 0.5), r, g, b, alpha * a);
        }
    }

    // shadow at the top (faint)
    ctx->rectangle(corners[0][X], corners[0][Y] - half, std::max(corners[1][X] - corners[0][X], 0.0), half);
    ctx->set_source(grad_top);
    ctx->fill();

    // right side
    ctx->rectangle(corners[1][X], corners[1][Y] + half, sw, std::max(corners[2][Y] - corners[1][Y] - half, 0.0));
    ctx->set_source(grad_right);
    ctx->fill();

    // bottom side
    ctx->rectangle(corners[0][X] + half, corners[2][Y], std::max(corners[1][X] - corners[0][X] - half, 0.0), sw);
    ctx->set_source(grad_bottom);
    ctx->fill();

    // left side (faint)
    ctx->rectangle(corners[0][X] - half, corners[0][Y], half, std::max(corners[2][Y] - corners[1][Y], 0.0));
    ctx->set_source(grad_left);
    ctx->fill();

    // bottom corners
    ctx->rectangle(corners[2][X], corners[2][Y], sw, sw);
    ctx->set_source(grad_btm_right);
    ctx->fill();

    ctx->rectangle(corners[3][X] - half, corners[3][Y], std::min(sw, rect.width() + half), sw);
    ctx->set_source(grad_btm_left);
    ctx->fill();

    // top corners
    ctx->rectangle(corners[1][X], corners[1][Y] - half, sw, std::min(sw, rect.height() + half));
    ctx->set_source(grad_top_right);
    ctx->fill();

    ctx->rectangle(corners[0][X] - half, corners[0][Y] - half, half, half);
    ctx->set_source(grad_top_left);
    ctx->fill();
}

/**
 * Converts the Cairo surface to a GdkPixbuf pixel format,
 * without allocating extra memory.
 *
 * This function is intended mainly for creating previews displayed by GTK.
 * For loading images for display on the canvas, use the Inkscape::Pixbuf object.
 *
 * The returned GdkPixbuf takes ownership of the passed surface reference,
 * so it should NOT be freed after calling this function.
 */
GdkPixbuf *ink_pixbuf_create_from_cairo_surface(cairo_surface_t *s)
{
    guchar *pixels = cairo_image_surface_get_data(s);
    int w = cairo_image_surface_get_width(s);
    int h = cairo_image_surface_get_height(s);
    int rs = cairo_image_surface_get_stride(s);

    convert_pixels_argb32_to_pixbuf(pixels, w, h, rs);

    GdkPixbuf *pb = gdk_pixbuf_new_from_data(
        pixels, GDK_COLORSPACE_RGB, TRUE, 8,
        w, h, rs, ink_cairo_pixbuf_cleanup, s);

    return pb;
}

/**
 * Cleanup function for GdkPixbuf.
 * This function should be passed as the GdkPixbufDestroyNotify parameter
 * to gdk_pixbuf_new_from_data when creating a GdkPixbuf backed by
 * a Cairo surface.
 */
void ink_cairo_pixbuf_cleanup(guchar * /*pixels*/, void *data)
{
    cairo_surface_t *surface = static_cast<cairo_surface_t*>(data);
    cairo_surface_destroy(surface);
}

/* The following two functions use "from" instead of "to", because when you write:
   val1 = argb32_from_pixbuf(val1);
   the name of the format is closer to the value in that format. */

guint32 argb32_from_pixbuf(guint32 c)
{
    uint32_t a;
    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
        a = (c & 0xff000000) >> 24;
    } else {
        a = (c & 0x000000ff);
    }

    if (a == 0) {
        return 0;
    }

    // extract color components
    uint32_t r, g, b;
    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
        r = (c & 0x000000ff);
        g = (c & 0x0000ff00) >> 8;
        b = (c & 0x00ff0000) >> 16;
    } else {
        r = (c & 0xff000000) >> 24;
        g = (c & 0x00ff0000) >> 16;
        b = (c & 0x0000ff00) >> 8;
    }

    // premultiply
    r = premul_alpha(r, a);
    b = premul_alpha(b, a);
    g = premul_alpha(g, a);

    // combine into output
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/**
 * Convert one pixel from ARGB to GdkPixbuf format.
 *
 * @param c ARGB color
 * @param bgcolor Color to use if c.alpha is zero (bgcolor.alpha is ignored)
 */
guint32 pixbuf_from_argb32(guint32 c, guint32 bgcolor)
{
    guint32 a = (c & 0xff000000) >> 24;
    if (a == 0) {
        assert(c == 0);
        c = bgcolor;
    }

    // extract color components
    guint32 r = (c & 0x00ff0000) >> 16;
    guint32 g = (c & 0x0000ff00) >> 8;
    guint32 b = (c & 0x000000ff);

    if (a != 0) {
        r = unpremul_alpha(r, a);
        g = unpremul_alpha(g, a);
        b = unpremul_alpha(b, a);
    }

    // combine into output
    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
        return r | (g << 8) | (b << 16) | (a << 24);
    } else {
        return (r << 24) | (g << 16) | (b << 8) | a;
    }
}

/**
 * Convert pixel data from GdkPixbuf format to ARGB.
 * This will convert pixel data from GdkPixbuf format to Cairo's native pixel format.
 * This involves premultiplying alpha and shuffling around the channels.
 * Pixbuf data must have an alpha channel, otherwise the results are undefined
 * (usually a segfault).
 */
void
convert_pixels_pixbuf_to_argb32(guchar *data, int w, int h, int stride)
{
    if (!data || w < 1 || h < 1 || stride < 1) {
        return;
    }

    for (size_t i = 0; i < h; ++i) {
        guint32 *px = reinterpret_cast<guint32*>(data + i*stride);
        for (size_t j = 0; j < w; ++j) {
            *px = argb32_from_pixbuf(*px);
            ++px;
        }
    }
}

/**
 * Convert pixel data from ARGB to GdkPixbuf format.
 * This will convert pixel data from GdkPixbuf format to Cairo's native pixel format.
 * This involves premultiplying alpha and shuffling around the channels.
 */
void
convert_pixels_argb32_to_pixbuf(guchar *data, int w, int h, int stride, guint32 bgcolor)
{
    if (!data || w < 1 || h < 1 || stride < 1) {
        return;
    }
    for (size_t i = 0; i < h; ++i) {
        guint32 *px = reinterpret_cast<guint32*>(data + i*stride);
        for (size_t j = 0; j < w; ++j) {
            *px = pixbuf_from_argb32(*px, bgcolor);
            ++px;
        }
    }
}

guint32 argb32_from_rgba(guint32 in)
{
    guint32 r, g, b, a;
    a = (in & 0x000000ff);
    r = premul_alpha((in & 0xff000000) >> 24, a);
    g = premul_alpha((in & 0x00ff0000) >> 16, a);
    b = premul_alpha((in & 0x0000ff00) >> 8,  a);
    ASSEMBLE_ARGB32(px, a, r, g, b)
    return px;
}


/**
 * Convert one pixel from ARGB to GdkPixbuf format.
 *
 * @param c RGBA color
 */
guint32 rgba_from_argb32(guint32 c)
{
    guint32 a = (c & 0xff000000) >> 24;
    guint32 r = (c & 0x00ff0000) >> 16;
    guint32 g = (c & 0x0000ff00) >> 8;
    guint32 b = (c & 0x000000ff);

    if (a != 0) {
        r = unpremul_alpha(r, a);
        g = unpremul_alpha(g, a);
        b = unpremul_alpha(b, a);
    }

    // combine into output
    guint32 o = (r << 24) | (g << 16) | (b << 8) | (a);

    return o;
}

static constexpr uint16_t get_luminance(uint32_t r, uint32_t g, uint32_t b)
{
    return ((1063 * r + 3576 * g + 361 * b) * 257 + 2500) / 5000;
}

static_assert(
    [] {
        for (int x = 0; x < 256; x++) {
            const uint16_t hex = 0x101 * x;
            assert(get_luminance(x, x, x) == hex);
        }
        return true;
    }(),
    "get_luminance function doesn't produce expected luminance values");

/**
 * Converts a pixbuf to a PNG data structure.
 * For 8-but RGBA png, this is like copying.
 *
 */
const guchar* pixbuf_to_png(guchar const**rows, guchar* px, int num_rows, int num_cols, int stride, int color_type, int bit_depth)
{
    int n_fields = 1 + (color_type&2) + (color_type&4)/4;
    const guchar* new_data = (const guchar*)malloc(((n_fields * bit_depth * num_cols + 7)/8) * num_rows);
    char* ptr = (char*) new_data;
    // Used when we write image data smaller than one byte (for instance in
    // black and white images where 1px = 1bit). Only possible with greyscale.
    int pad = 0;
    for (int row = 0; row < num_rows; ++row) {
        rows[row] = (const guchar*)ptr;
        for (int col = 0; col < num_cols; ++col) {
            guint32 *pixel = reinterpret_cast<guint32*>(px + row*stride)+col;

            guint64 pix3 = (*pixel & 0xff000000) >> 24;
            guint64 pix2 = (*pixel & 0x00ff0000) >> 16;
            guint64 pix1 = (*pixel & 0x0000ff00) >> 8;
            guint64 pix0 = (*pixel & 0x000000ff);

            uint64_t a, r, g, b;
            if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
                a = pix3;
                b = pix2;
                g = pix1;
                r = pix0;
            } else {
                r = pix3;
                g = pix2;
                b = pix1;
                a = pix0;
            }

            // One of possible rgb to greyscale formulas. This one is called "luminance", "luminosity" or "luma"
            uint16_t const gray = get_luminance(r, g, b);

            if (color_type & 2) { // RGB or RGBA
                // for 8bit->16bit transition, I take the FF -> FFFF convention (multiplication by 0x101). 
                // If you prefer FF -> FF00 (multiplication by 0x100), remove the <<8, <<24, <<40 and <<56
                // for little-endian, and remove the <<0, <<16, <<32 and <<48 for big-endian.
                if (color_type & 4) { // RGBA
                    if (bit_depth == 8)
                        *((guint32*)ptr) = *pixel; 
                    else 
                        // This uses the samples in the order they appear in pixel rather than
                        // normalised to abgr or rgba in order to make it endian agnostic,
                        // exploiting the symmetry of the expression (0x101 is the same in both
                        // endiannesses and each sample is multiplied by that).
                        *((guint64*)ptr) = (guint64)((pix3<<56)+(pix3<<48)+(pix2<<40)+(pix2<<32)+(pix1<<24)+(pix1<<16)+(pix0<<8)+(pix0));
                } else { // RGB
                    if (bit_depth == 8) {
                        *ptr = r;
                        *(ptr+1) = g;
                        *(ptr+2) = b;
                    } else {
                        *((guint16*)ptr) = (r<<8)+r;
                        *((guint16*)(ptr+2)) = (g<<8)+g;
                        *((guint16*)(ptr+4)) = (b<<8)+b;
                    }
                }
            } else { // Grayscale
                if (bit_depth == 16) {
                    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
                        *(guint16*)ptr = ((gray & 0xff00)>>8) + ((gray & 0x00ff)<<8);
                    } else {
                        *(guint16*)ptr = gray;
                    }
                    // For 8bit->16bit this mirrors RGB(A), multiplying by
                    // 0x101; if you prefer multiplying by 0x100, remove the
                    // <<8 for little-endian, and remove the unshifted value
                    // for big-endian.
                    if (color_type & 4) // Alpha channel
                        *((guint16*)(ptr+2)) = a + (a<<8);
                } else if (bit_depth == 8) {
                    *ptr = guint8(gray >> 8);
                    if (color_type & 4) // Alpha channel
                        *((guint8*)(ptr+1)) = a;
                } else {
                    if (!pad) *ptr=0;
                    // In PNG numbers are stored left to right, but in most significant bits first, so the first one processed is the ``big'' mask, etc.
                    int realpad = 8 - bit_depth - pad;
                    *ptr += guint8((gray >> (16-bit_depth))<<realpad); // Note the "+="
                    if (color_type & 4) // Alpha channel
                        *(ptr+1) += guint8((a >> (8-bit_depth))<<(bit_depth + realpad));
                }
            }

            pad += bit_depth*n_fields;
            ptr += pad/8;
            pad %= 8;
        }
        // Align bytes on rows
        if (pad) {
            pad = 0;
            ptr++;
        }
    }
    return new_data; 
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
