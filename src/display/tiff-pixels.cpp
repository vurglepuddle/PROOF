// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a TIFF's own pixels, for the screen and for print. See tiff-pixels.h.
 */

#include "tiff-pixels.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <tiffio.h>

namespace Inkscape {
namespace {

/// The most pixels read from one picture: 1 GiB of 8-bit CMYK and more.
constexpr std::uint64_t MAX_PIXELS = std::uint64_t(1) << 28;

struct Memory
{
    std::string_view data;
    toff_t at = 0;
};

tmsize_t read_memory(thandle_t h, void *buf, tmsize_t size)
{
    auto &m = *static_cast<Memory *>(h);
    if (size < 0 || m.at >= m.data.size()) return 0;
    auto const n = std::min<toff_t>(toff_t(size), m.data.size() - m.at);
    std::memcpy(buf, m.data.data() + m.at, n);
    m.at += n;
    return tmsize_t(n);
}

tmsize_t write_memory(thandle_t, void *, tmsize_t) { return 0; }

toff_t seek_memory(thandle_t h, toff_t off, int whence)
{
    auto &m = *static_cast<Memory *>(h);
    toff_t base = whence == SEEK_CUR ? m.at : whence == SEEK_END ? m.data.size() : 0;
    m.at = base + off;
    return m.at;
}

int close_memory(thandle_t) { return 0; }
toff_t size_memory(thandle_t h) { return static_cast<Memory *>(h)->data.size(); }
int map_memory(thandle_t, void **, toff_t *) { return 0; }
void unmap_memory(thandle_t, void *, toff_t) {}

/// libtiff's errors land here; Photoshop's own tags make it warn, which is said nowhere.
int quiet(TIFF *, void *, char const *, char const *, va_list) { return 1; }
int keep_error(TIFF *, void *user, char const *, char const *fmt, va_list ap)
{
    if (auto *error = static_cast<std::string *>(user); error && error->empty()) {
        char text[256];
        std::vsnprintf(text, sizeof(text), fmt, ap);
        *error = text;
    }
    return 1;
}

struct Closer
{
    void operator()(TIFF *t) const { TIFFClose(t); }
};
using Tiff = std::unique_ptr<TIFF, Closer>;

Tiff open(Memory &memory, std::string *error)
{
    auto *options = TIFFOpenOptionsAlloc();
    TIFFOpenOptionsSetErrorHandlerExtR(options, keep_error, error);
    TIFFOpenOptionsSetWarningHandlerExtR(options, quiet, nullptr);
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, tmsize_t(1) << 31);
    Tiff tif(TIFFClientOpenExt("picture", "rm", &memory, read_memory, write_memory, seek_memory, close_memory,
                               size_memory, map_memory, unmap_memory, options));
    TIFFOpenOptionsFree(options);
    return tif;
}

bool fail(std::string *error, char const *why)
{
    if (error && error->empty()) *error = why;
    return false;
}

/// Its pixels, every sample of a pixel together, rows top to bottom, into `out`.
bool read_samples(TIFF *tif, std::uint32_t w, std::uint32_t h, std::size_t pixel_bytes, std::vector<std::uint8_t> &out,
                  std::string *error)
{
    out.assign(std::size_t(w) * h * pixel_bytes, 0);
    std::size_t const row_bytes = std::size_t(w) * pixel_bytes;
    if (TIFFIsTiled(tif)) {
        std::uint32_t tw = 0, th = 0;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
        if (!tw || !th) return fail(error, "its tiles have no size");
        std::vector<std::uint8_t> tile(TIFFTileSize64(tif));
        if (tile.size() < std::size_t(tw) * th * pixel_bytes) return fail(error, "its tiles are smaller than their size");
        for (std::uint32_t y = 0; y < h; y += th) {
            for (std::uint32_t x = 0; x < w; x += tw) {
                if (TIFFReadTile(tif, tile.data(), x, y, 0, 0) < 0) return fail(error, "a tile can't be read");
                std::uint32_t const cw = std::min(tw, w - x), ch = std::min(th, h - y);
                for (std::uint32_t r = 0; r < ch; ++r) {
                    std::memcpy(out.data() + (y + r) * row_bytes + x * pixel_bytes,
                                tile.data() + std::size_t(r) * tw * pixel_bytes, cw * pixel_bytes);
                }
            }
        }
        return true;
    }
    if (std::size_t(TIFFScanlineSize64(tif)) < row_bytes) return fail(error, "its rows are shorter than its width");
    std::vector<std::uint8_t> line(TIFFScanlineSize64(tif));
    for (std::uint32_t y = 0; y < h; ++y) {
        if (TIFFReadScanline(tif, line.data(), y, 0) < 0) return fail(error, "a row can't be read");
        std::memcpy(out.data() + y * row_bytes, line.data(), row_bytes);
    }
    return true;
}

template <typename T>
void split(std::vector<std::uint8_t> const &all, unsigned spp, unsigned colours, int alpha_at, bool premultiplied,
           RasterPixels &out)
{
    auto const *in = reinterpret_cast<T const *>(all.data());
    std::size_t const n = std::size_t(out.width) * out.height;
    out.colour.resize(n * colours * sizeof(T));
    auto *c = reinterpret_cast<T *>(out.colour.data());
    T *a = nullptr;
    if (alpha_at >= 0) {
        out.alpha.resize(n * sizeof(T));
        a = reinterpret_cast<T *>(out.alpha.data());
    }
    constexpr double top = std::numeric_limits<T>::max();
    for (std::size_t i = 0; i < n; ++i, in += spp) {
        T const alpha = alpha_at >= 0 ? in[alpha_at] : T(top);
        for (unsigned k = 0; k < colours; ++k) {
            T v = in[k];
            if (premultiplied) {
                // Premultiplied towards no ink (CMYK) or black (RGB, gray): divided back out.
                v = alpha ? T(std::min(top, std::round(v * top / alpha))) : T(0);
            }
            c[i * colours + k] = v;
        }
        if (a) a[i] = alpha;
    }
}

} // namespace

bool looks_like_tiff(std::string_view data)
{
    return data.size() >= 8 && (data.substr(0, 4) == std::string_view("II*\0", 4) ||
                                data.substr(0, 4) == std::string_view("MM\0*", 4));
}

bool is_cmyk_tiff(std::string_view data)
{
    return cmyk_tiff_profile(data).has_value();
}

std::optional<std::string> cmyk_tiff_profile(std::string_view data)
{
    if (!looks_like_tiff(data)) return {};
    Memory memory{data};
    auto tif = open(memory, nullptr);
    if (!tif) return {};
    std::uint16_t photometric = 0, inkset = INKSET_CMYK;
    TIFFGetField(tif.get(), TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(tif.get(), TIFFTAG_INKSET, &inkset);
    if (photometric != PHOTOMETRIC_SEPARATED || inkset != INKSET_CMYK) return {};
    std::uint32_t icc_size = 0;
    void *icc = nullptr;
    if (TIFFGetField(tif.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc) && icc && icc_size) {
        return std::string(static_cast<char const *>(icc), icc_size);
    }
    return std::string();
}

std::optional<RasterPixels> read_tiff(std::string_view data, std::string *error)
{
    if (!looks_like_tiff(data)) {
        fail(error, "it isn't a TIFF");
        return {};
    }
    Memory memory{data};
    std::string libtiff_error;
    auto tif = open(memory, &libtiff_error);
    if (!tif) {
        fail(error, libtiff_error.empty() ? "it can't be opened" : libtiff_error.c_str());
        return {};
    }
    RasterPixels out;
    std::uint16_t bits = 0, spp = 0, photometric = 0, planar = PLANARCONFIG_CONTIG, format = SAMPLEFORMAT_UINT;
    std::uint16_t inkset = INKSET_CMYK;
    if (!TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &out.width) ||
        !TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &out.height) ||
        !TIFFGetField(tif.get(), TIFFTAG_PHOTOMETRIC, &photometric)) {
        fail(error, "it lacks its size or colour model");
        return {};
    }
    TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLEFORMAT, &format);
    TIFFGetFieldDefaulted(tif.get(), TIFFTAG_INKSET, &inkset);
    unsigned colours = 0;
    switch (photometric) {
        case PHOTOMETRIC_MINISBLACK: out.model = RasterPixels::Model::Gray; colours = 1; break;
        case PHOTOMETRIC_RGB: out.model = RasterPixels::Model::RGB; colours = 3; break;
        case PHOTOMETRIC_SEPARATED:
            if (inkset != INKSET_CMYK) {
                fail(error, "its inks aren't CMYK");
                return {};
            }
            out.model = RasterPixels::Model::CMYK;
            colours = 4;
            break;
        default:
            fail(error, "its colour model isn't gray, RGB or CMYK");
            return {};
    }
    if ((bits != 8 && bits != 16) || format != SAMPLEFORMAT_UINT) {
        fail(error, "its samples aren't 8 or 16 bits");
        return {};
    }
    if (spp < colours || planar != PLANARCONFIG_CONTIG) {
        fail(error, "its samples aren't kept a pixel together");
        return {};
    }
    if (!out.width || !out.height || std::uint64_t(out.width) * out.height > MAX_PIXELS) {
        fail(error, "it is empty or too large");
        return {};
    }
    out.bits = bits;
    // The first extra sample that is its transparency; the others are left out.
    int alpha_at = -1;
    std::uint16_t extras = 0;
    std::uint16_t *kinds = nullptr;
    if (spp > colours && TIFFGetField(tif.get(), TIFFTAG_EXTRASAMPLES, &extras, &kinds)) {
        for (std::uint16_t i = 0; i < extras && colours + i < spp; ++i) {
            if (alpha_at < 0 && (kinds[i] == EXTRASAMPLE_ASSOCALPHA || kinds[i] == EXTRASAMPLE_UNASSALPHA)) {
                alpha_at = int(colours + i);
                out.premultiplied = kinds[i] == EXTRASAMPLE_ASSOCALPHA;
            }
        }
    }
    out.left_out = spp - colours - (alpha_at >= 0 ? 1 : 0);
    std::uint32_t icc_size = 0;
    void *icc = nullptr;
    if (TIFFGetField(tif.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc) && icc && icc_size) {
        out.profile.assign(static_cast<char const *>(icc), icc_size);
    }
    std::vector<std::uint8_t> all;
    if (!read_samples(tif.get(), out.width, out.height, std::size_t(spp) * bits / 8, all, error)) return {};
    if (bits == 8) {
        split<std::uint8_t>(all, spp, colours, alpha_at, out.premultiplied, out);
    } else {
        split<std::uint16_t>(all, spp, colours, alpha_at, out.premultiplied, out);
    }
    return out;
}

} // namespace Inkscape
