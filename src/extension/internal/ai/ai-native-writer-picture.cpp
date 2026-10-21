// SPDX-License-Identifier: GPL-2.0-or-later
/** Embedded pictures as Illustrator writes its rasters. See ai-native-writer-picture.h.
 *
 * The form is Illustrator's own, without exception over the owner's 871 copies (2,752 rasters)
 * and the collaborator's pack (2,007), counted 2026-10-20:
 *
 *   %AI5_File:
 *   %AI5_BeginRaster
 *   () 0 XG                                   the file it came from (none), not edited since
 *   /DeviceRGB XN                             its colour space
 *   [matrix] w h 0 Xh
 *   [matrix] 0 0 w h w h 8 type alpha kind 1 0 4 4 0 0
 *   %%BeginData: n                            n counts "XI", its line end and the colour
 *   XI                                        samples, not the alpha plane after them
 *   <colour samples, interleaved><alpha samples>
 *   %%EndData
 *   XH
 *   %AI5_EndRaster
 *   N                                         (VectorCraft found that without it Illustrator
 *                                              drops a clipped picture and what follows)
 *
 * `type` is Adobe's 1998 colour type (1 grey, 3 RGB, 4 CMYK), `alpha` the alpha channel count
 * and `kind`, which the specification reserves, the colour space (0 grey, 1 RGB, 2 CMYK) plus
 * 16 with an alpha channel: so in every one of the 4,759. Samples are straight, CMYK as inks.
 * The matrix carries the picture's pixels, x across and y up from its top-left corner, into art
 * space: PROOF's reader turns the rows over before it (ai-native-import.cpp, Builder::image).
 */
#include "ai-native-writer-picture.h"

#include <cmath>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#include <glib.h>
#include <glib/gstdio.h>

#include "display/cairo-utils.h"
#include "display/psd-pixels.h"
#include "display/raster-pixels.h"
#include "display/tiff-pixels.h"
#include "document.h"
#include "io/link-policy.h"
#include "object/sp-image.h"
#include "object/uri.h"
#include "util/uri.h"
#include "xml/href-attribute-helper.h"
#include "xml/node.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

WrittenPicture refused(std::string why)
{
    WrittenPicture out;
    out.refused.push_back(std::move(why));
    return out;
}

/// To ten places: a picture's scale is small (a 300 dpi picture's is 0.24), and four would
/// put the far side of a picture of thousands of pixels a point out.
std::string fine(double v)
{
    auto t = std::format("{:.10f}", v);
    t.erase(t.find_last_not_of('0') + 1);
    if (t.back() == '.') t.pop_back();
    return t == "-0" ? std::string("0") : t;
}

std::string ps_string(std::string const &s)
{
    std::string out = "(";
    for (unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            out += std::format("\\{:03o}", c);
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + ")";
}

/// Empty on a qualified raster signature; named refusals for I/O and unsupported formats.
std::string check_link(std::string const &path)
{
    auto *file = g_fopen(path.c_str(), "rb");
    if (!file) return "missing or unreadable linked pictures";
    std::array<char, 8> header{};
    auto const n = std::fread(header.data(), 1, header.size(), file);
    bool const failed = std::ferror(file);
    std::fclose(file);
    if (failed) return "missing or unreadable linked pictures";
    std::string_view const bytes(header.data(), n);
    if (bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) ||
        bytes.starts_with(std::string_view("\xff\xd8\xff", 3)) ||
        bytes.starts_with(std::string_view("II\x2a\0", 4)) ||
        bytes.starts_with(std::string_view("MM\0\x2a", 4)) ||
        bytes.starts_with(std::string_view("8BPS\0\1", 6))) return {};
    if (bytes.starts_with("8BPS")) return "linked Photoshop versions other than PSD";
    if (bytes.starts_with(std::string_view("II\x2b\0", 4)) ||
        bytes.starts_with(std::string_view("MM\0\x2b", 4))) return "linked BigTIFF pictures";
    return "linked pictures other than PNG, JPEG, TIFF or Photoshop";
}

/// The bytes of a data: address, and what it holds; nothing for a link.
std::optional<std::string> data_of(char const *uri, Base64Data &type)
{
    type = Base64Data::NONE;
    if (!uri) return {};
    auto const [base64, kind] = extract_uri_data(uri);
    type = kind;
    if (kind == Base64Data::NONE || !base64) return {};
    gsize length = 0;
    auto *bytes = g_base64_decode(base64, &length);
    std::string out(reinterpret_cast<char const *>(bytes), length);
    g_free(bytes);
    return out;
}

/// The samples as Illustrator keeps a raster's: 8 bits, colour interleaved, alpha apart.
struct Samples
{
    int type = 3; ///< 1 grey, 3 RGB, 4 CMYK.
    std::string colour;
    std::string alpha; ///< Empty when the picture is opaque everywhere.
};

std::optional<Samples> from_raster(RasterPixels const &p)
{
    std::size_t const pixels = std::size_t(p.width) * p.height;
    std::size_t const bytes = p.bits == 16 ? 2 : 1;
    if ((p.bits != 8 && p.bits != 16) || p.colour.size() < pixels * p.channels() * bytes) return {};
    auto eight = [&](std::vector<std::uint8_t> const &from, std::size_t count) {
        std::string out(count, '\0');
        for (std::size_t i = 0; i < count; ++i) {
            if (bytes == 1) {
                out[i] = static_cast<char>(from[i]);
            } else {
                std::uint16_t v;
                std::memcpy(&v, from.data() + 2 * i, 2); // Native byte order.
                out[i] = static_cast<char>((v * 255u + 32767u) / 65535u);
            }
        }
        return out;
    };
    Samples s;
    s.type = p.model == RasterPixels::Model::CMYK ? 4 : p.model == RasterPixels::Model::RGB ? 3 : 1;
    s.colour = eight(p.colour, pixels * p.channels());
    if (!p.alpha.empty()) {
        if (p.alpha.size() < pixels * bytes) return {};
        s.alpha = eight(p.alpha, pixels);
    }
    return s;
}

/// What a picture shows: its decoded RGB and straight alpha.
Samples from_pixbuf(Inkscape::Pixbuf const &shown)
{
    Inkscape::Pixbuf copy(shown);
    copy.ensurePixelFormat(Inkscape::Pixbuf::PF_GDK);
    std::size_t const w = copy.width(), h = copy.height(), stride = copy.rowstride();
    auto const *px = copy.pixels();
    Samples s;
    s.colour.resize(w * h * 3);
    s.alpha.resize(w * h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto const *p = px + y * stride + x * 4;
            std::memcpy(s.colour.data() + (y * w + x) * 3, p, 3);
            s.alpha[y * w + x] = static_cast<char>(p[3]);
        }
    }
    return s;
}

} // namespace

WrittenPicture write_picture(SPImage const *image, Geom::Affine const &item_to_art, bool legacy)
{
    if (!image) return refused("pictures that can't be read");
    auto const *href = Inkscape::getHrefAttribute(*image->getRepr()).second;
    Base64Data type = Base64Data::NONE;
    auto const bytes = data_of(href, type);
    if (type == Base64Data::SVG) return refused("SVG pictures");
    std::string link;
    if (!bytes) {
        if (legacy) return refused("linked pictures (Illustrator 7)");
        try {
            auto const uri = image->getURI();
            if (!uri.hasScheme("file") || uri.getFragment() || uri.getQuery()) {
                return refused("linked pictures without a local file address");
            }
            link = uri.toNativeFilename();
        } catch (...) {
            return refused("linked pictures without a local file address");
        }
        if (!g_path_is_absolute(link.c_str()) || link.size() > 4096 ||
            link.find_first_of("\r\n") != std::string::npos) return refused("unsupported linked picture paths");
        // Not looked up on another computer (io/link-policy.h), so not checked or written either.
        if (!Inkscape::IO::may_follow_link(link, image->document->getDocumentBase())) {
            return refused("linked pictures on another computer");
        }
        if (!g_file_test(link.c_str(), G_FILE_TEST_IS_REGULAR)) {
            return refused("missing or unreadable linked pictures");
        }
        if (auto const why = check_link(link); !why.empty()) return refused(why);
        if (!image->pixbuf || image->missing) return refused("missing or unreadable linked pictures");
        GStatBuf status;
        if (g_stat(link.c_str(), &status) != 0) return refused("missing or unreadable linked pictures");
        if (image->pixbuf->modificationTime() && status.st_mtime != image->pixbuf->modificationTime()) {
            return refused("linked pictures changed since they were loaded");
        }
    }
    if (!image->pixbuf || image->missing) return refused("pictures that can't be read");
    int const w = image->pixbuf->width(), h = image->pixbuf->height();
    if (w < 1 || h < 1) return refused("pictures that can't be read");

    // Where the pixels lie: as PROOF draws and prints the picture (SPViewBox::get_paintbox).
    auto const box = image->bbox(Geom::identity(), SPItem::GEOMETRIC_BBOX);
    auto const paint = box ? image->get_paintbox(w, h, box) : Geom::OptRect();
    if (!box || !paint || paint->width() <= 0 || paint->height() <= 0) return refused("pictures without a size");
    bool const cropped = paint->left() < box->left() - 1e-6 || paint->top() < box->top() - 1e-6 ||
                         paint->right() > box->right() + 1e-6 || paint->bottom() > box->bottom() + 1e-6;
    auto const a = Geom::Affine(paint->width() / w, 0, 0, paint->height() / h, paint->left(), paint->top()) * item_to_art;
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(a[i])) return refused("singular or nonfinite picture transforms");
    }
    if (std::abs(a.det()) < 1e-12) return refused("singular or nonfinite picture transforms");
    // A slice fills the viewport with a larger picture. Keep all pixels (or the original
    // link) and crop with an editable clipping group, nested inside any explicit clip the
    // caller opened. Transform the four corners, not the rectangle's axis-aligned bounds:
    // the viewport can be turned, reflected or skewed together with its picture.
    // The closing side is written back to the first corner, as Illustrator writes every
    // closed path: it takes a closed path's last point for its first, so without that
    // side the fourth corner became the first anchor's handle and the clip's lower
    // left a curve (Illustrator 29.2.1, 2026-10-21).
    std::string viewport_clip;
    if (cropped) {
        std::array<Geom::Point, 5> const corners{{{box->left(), box->top()}, {box->right(), box->top()},
                                                {box->right(), box->bottom()}, {box->left(), box->bottom()},
                                                {box->left(), box->top()}}};
        viewport_clip = "q\n";
        for (std::size_t i = 0; i < corners.size(); ++i) {
            auto const point = corners[i] * item_to_art;
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                return refused("singular or nonfinite picture transforms");
            }
            viewport_clip += std::format("{} {} {}\n", fine(point.x()), fine(point.y()), i ? 'L' : 'm');
        }
        viewport_clip += "h\nW\nn\n";
    }
    // The modern form ends the clipping group as Illustrator ends one with no transparency of
    // its own, so that the next object's cannot be taken for the group's.
    auto with_crop = [&](WrittenPicture out) {
        if (!viewport_clip.empty()) out.records = viewport_clip + std::move(out.records) + (legacy ? "Q\n" : "Q\n9 () XW\n");
        return out;
    };
    if (!link.empty()) {
        // A placed file uses the stored box directly (unlike a raster's upside-down rows).
        // Pixel-sized boxes work for TIFF too, independently of its resolution; verified by
        // Illustrator 29's open and save of the 300 ppi control (2026-10-21). Keep the absolute
        // file address so Save As into another folder does not change what the link names.
        WrittenPicture out;
        out.records = std::format("%AI5_BeginPlace\n%%FileType: 1\n[{} {} {} {} {} {}] 0 0 {} {} 4 4 0\n"
                                  "{}`\n%%IncludeFile:{}\n~\n%AI5_EndPlace\n",
                                  fine(a[0]), fine(a[1]), fine(a[2]), fine(a[3]), fine(a[4]), fine(a[5]),
                                  w, h, ps_string(link), link);
        return with_crop(std::move(out));
    }

    // CMYK numbers as they are: what an Illustrator picture brought for print, else the
    // picture's own when it is a TIFF or a Photoshop file, which PROOF reads itself.
    std::optional<Samples> samples;
    auto const sized = [&](std::optional<RasterPixels> const &p) {
        return p && int(p->width) == w && int(p->height) == h;
    };
    Base64Data kept_type;
    if (auto const kept = data_of(image->getRepr()->attribute("proof:samples"), kept_type)) {
        if (auto p = read_tiff(*kept); sized(p)) samples = from_raster(*p);
    }
    if (!samples) {
        auto own = read_tiff(*bytes);
        if (!own) own = read_psd(*bytes);
        if (sized(own)) samples = from_raster(*own);
    }
    if (!samples) samples = from_pixbuf(*image->pixbuf);
    if (samples->alpha.find_first_not_of('\xff') == std::string::npos) samples->alpha.clear();

    // The rows count up in the records and down in the picture.
    auto const matrix =
        std::format("[ {} {} {} {} {} {} ]", fine(a[0]), fine(a[1]), fine(-a[2]), fine(-a[3]), fine(a[4]), fine(a[5]));

    bool const alpha = !samples->alpha.empty();
    if (legacy) {
        if (alpha) return refused("picture transparency (Illustrator 7)");
        // Adobe's 1998 specification, section 8: the original twelve operands after the
        // matrix, then XI and ASCII hex samples. DSC brackets let readers skip the data
        // by length; the colour type identifies the space, with reserved fields kept zero.
        std::string encoded;
        auto const &colour = samples->colour;
        encoded.reserve(colour.size() * 2 + ((colour.size() + 31) / 32) * 2);
        constexpr char hex[] = "0123456789ABCDEF";
        for (std::size_t i = 0; i < colour.size(); ++i) {
            if (i % 32 == 0) encoded += '%';
            auto const c = static_cast<unsigned char>(colour[i]);
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
            if (i % 32 == 31 || i + 1 == colour.size()) encoded += '\n';
        }
        WrittenPicture out;
        out.records = std::format("%AI5_File:\n%AI5_BeginRaster\n{} 0 0 {} {} {} {} 8 {} 0 0 0 0\n"
                                  "%%BeginData: {} ASCII Bytes\nXI\n",
                                  matrix, w, h, w, h, samples->type, encoded.size() + 3);
        out.records += encoded;
        out.records += "%%EndData\n%AI5_EndRaster\nN\n";
        return with_crop(std::move(out));
    }
    int const kind = (samples->type == 4 ? 2 : samples->type == 3 ? 1 : 0) + (alpha ? 16 : 0);
    char const *space = samples->type == 4 ? "/DeviceCMYK" : samples->type == 3 ? "/DeviceRGB" : "/DeviceGray";
    WrittenPicture out;
    out.records = std::format("%AI5_File:\n%AI5_BeginRaster\n() 0 XG\n{} XN\n{} {} {} 0 Xh\n"
                              "{} 0 0 {} {} {} {} 8 {} {} {} 1 0 4 4 0 0\n%%BeginData: {}\nXI\n",
                              space, matrix, w, h, matrix, w, h, w, h, samples->type, alpha ? 1 : 0, kind,
                              3 + samples->colour.size());
    out.records += samples->colour;
    out.records += samples->alpha;
    out.records += "\n%%EndData\nXH\n%AI5_EndRaster\nN\n";
    return with_crop(std::move(out));
}

} // namespace Inkscape::Extension::Internal::AiNative
