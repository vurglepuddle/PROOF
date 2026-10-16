// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a Photoshop file's merged image. See psd-pixels.h.
 *
 * The layout is Adobe's "Photoshop File Formats Specification": a header, colour mode data,
 * image resources, layer and mask information, then the merged image, every channel's plane
 * after the other, big-endian.
 */

#include "psd-pixels.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace Inkscape {
namespace {

/// The most pixels read from one picture, as for TIFFs: 1 GiB of 8-bit CMYK and more.
constexpr std::uint64_t MAX_PIXELS = std::uint64_t(1) << 28;

enum Mode : unsigned { BITMAP = 0, GRAY = 1, INDEXED = 2, RGB = 3, CMYK = 4, DUOTONE = 8, LAB = 9 };

/// Big-endian numbers and spans of `data`, bounded by it: past its end, `ok` turns false.
struct Reader
{
    std::string_view data;
    std::size_t at = 0;
    bool ok = true;

    bool has(std::uint64_t n) const { return ok && at <= data.size() && data.size() - at >= n; }
    std::uint64_t number(unsigned bytes)
    {
        if (!has(bytes)) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i) v = (v << 8) | std::uint8_t(data[at + i]);
        at += bytes;
        return v;
    }
    std::string_view take(std::uint64_t n)
    {
        if (!has(n)) {
            ok = false;
            return {};
        }
        auto const s = data.substr(at, n);
        at += n;
        return s;
    }
};

bool fail(std::string *error, char const *why)
{
    if (error && error->empty()) *error = why;
    return false;
}

/// What is read of a Photoshop file before its merged image.
struct Layout
{
    bool big = false; ///< A .psb.
    unsigned channels = 0, depth = 0, mode = 0;
    std::uint32_t width = 0, height = 0;
    std::string_view profile;
    bool merged = true;        ///< Photoshop saved a merged image ("Maximize Compatibility").
    bool transparency = false; ///< Its first extra channel is the merged image's transparency.
    unsigned compression = 0;
    std::size_t image = 0; ///< Where the merged image's data begins, after its compression.

    unsigned colours() const { return mode == CMYK ? 4 : mode == RGB ? 3 : 1; }
};

/// Whether a block of layer information says that the merged image keeps its transparency:
/// its count of layers is negative.
bool transparent(std::string_view info)
{
    Reader r{info};
    auto const count = r.number(2);
    return r.ok && std::int16_t(count) < 0;
}

std::optional<Layout> layout(std::string_view data, std::string *error)
{
    Reader r{data};
    if (r.take(4) != "8BPS") {
        fail(error, "it isn't a Photoshop file");
        return {};
    }
    Layout l;
    auto const version = r.number(2);
    if (version != 1 && version != 2) {
        fail(error, "its version isn't one Photoshop writes");
        return {};
    }
    l.big = version == 2;
    r.take(6);
    l.channels = unsigned(r.number(2));
    l.height = std::uint32_t(r.number(4));
    l.width = std::uint32_t(r.number(4));
    l.depth = unsigned(r.number(2));
    l.mode = unsigned(r.number(2));
    r.take(r.number(4)); // colour mode data: an indexed or duotone file's
    Reader resources{r.take(r.number(4))};
    while (resources.has(12)) {
        resources.take(4); // "8BIM", or another application's signature
        auto const id = resources.number(2);
        auto const name = resources.number(1);
        resources.take(name + (name % 2 == 0 ? 1 : 0)); // a Pascal string, padded to an even length
        auto const size = resources.number(4);
        auto const body = resources.take(size);
        if (size % 2) resources.take(1);
        if (!resources.ok) break;
        if (id == 1039) l.profile = body;
        if (id == 1057 && body.size() >= 5) l.merged = body[4] != 0;
    }
    unsigned const lengths = l.big ? 8 : 4;
    Reader layers{r.take(r.number(lengths))};
    if (auto const info = layers.take(layers.number(lengths)); !info.empty()) {
        l.transparency = transparent(info);
    } else {
        // At 16 and 32 bits a channel the layers are in a tagged block of their own.
        layers.take(layers.number(4)); // the global layer mask
        // Tagged blocks whose length takes 8 bytes in a .psb.
        static constexpr std::string_view LONG[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                                    "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
        while (layers.has(12)) {
            layers.take(4); // "8BIM" or "8B64"
            auto const key = layers.take(4);
            bool const long_length = l.big && std::find(std::begin(LONG), std::end(LONG), key) != std::end(LONG);
            auto const size = layers.number(long_length ? 8 : 4);
            auto const body = layers.take(size);
            if (size % 2) layers.take(1);
            if (!layers.ok) break;
            if (key == "Lr16" || key == "Lr32") {
                l.transparency = transparent(body);
                break;
            }
        }
    }
    l.compression = unsigned(r.number(2));
    l.image = r.at;
    if (!r.ok) {
        fail(error, "it is cut short before its merged image");
        return {};
    }
    return l;
}

bool check(Layout const &l, std::string *error)
{
    switch (l.mode) {
        case GRAY:
        case RGB:
        case CMYK:
            break;
        case BITMAP: return fail(error, "it is a bitmap (one bit a pixel), which isn't read here");
        case INDEXED: return fail(error, "its colours are indexed, which isn't read here");
        case DUOTONE: return fail(error, "it is a duotone, which isn't read here");
        case LAB: return fail(error, "its colours are Lab, which isn't read here");
        default: return fail(error, "its colour mode isn't gray, RGB or CMYK");
    }
    if (l.depth != 8 && l.depth != 16) return fail(error, "its channels aren't 8 or 16 bits");
    if (l.channels < l.colours() + (l.transparency ? 1 : 0)) return fail(error, "it has fewer channels than colours");
    if (!l.width || !l.height || std::uint64_t(l.width) * l.height > MAX_PIXELS) {
        return fail(error, "it is empty or too large");
    }
    if (!l.merged) return fail(error, "it was saved without its merged image (Photoshop's Maximize Compatibility)");
    if (l.compression > 1) return fail(error, "its merged image is compressed with ZIP, which isn't read here");
    return true;
}

/// One row packed with PackBits into `out`, which it must fill exactly.
bool unpack(std::string_view in, std::uint8_t *out, std::size_t size)
{
    std::size_t i = 0, o = 0;
    while (i < in.size() && o < size) {
        int const n = std::int8_t(in[i++]);
        if (n >= 0) {
            std::size_t const count = std::size_t(n) + 1;
            if (i + count > in.size() || o + count > size) return false;
            std::memcpy(out + o, in.data() + i, count);
            i += count;
            o += count;
        } else if (n != -128) {
            std::size_t const count = std::size_t(1 - n);
            if (i >= in.size() || o + count > size) return false;
            std::memset(out + o, std::uint8_t(in[i++]), count);
            o += count;
        }
    }
    return o == size;
}

/// The planes of the merged image, each `n` big-endian samples, into pixels: colour straight,
/// CMYK as inks.
template <typename T>
void assemble(std::vector<std::uint8_t> const &planes, std::size_t n, unsigned colours, bool transparency, bool cmyk,
              RasterPixels &out)
{
    constexpr std::uint32_t top = std::numeric_limits<T>::max();
    auto sample = [&](unsigned c, std::size_t i) -> std::uint32_t {
        auto const *p = planes.data() + (c * n + i) * sizeof(T);
        return sizeof(T) == 1 ? p[0] : (std::uint32_t(p[0]) << 8) | p[1];
    };
    out.colour.resize(n * colours * sizeof(T));
    auto *colour = reinterpret_cast<T *>(out.colour.data());
    T *alpha = nullptr;
    if (transparency) {
        out.alpha.resize(n * sizeof(T));
        alpha = reinterpret_cast<T *>(out.alpha.data());
    }
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t const a = transparency ? sample(colours, i) : top;
        for (unsigned k = 0; k < colours; ++k) {
            // How far it is from white, which is no ink in CMYK (kept inverted, so white is the
            // largest number in every mode). The matte against white scaled that by the
            // transparency: divided back out.
            std::uint32_t d = top - sample(k, i);
            if (a < top) d = a ? std::min<std::uint32_t>(top, std::uint32_t((std::uint64_t(d) * top + a / 2) / a)) : 0;
            colour[i * colours + k] = T(cmyk ? d : top - d);
        }
        if (alpha) alpha[i] = T(a);
    }
}

} // namespace

bool looks_like_psd(std::string_view data)
{
    return data.size() >= 26 && data.substr(0, 4) == "8BPS";
}

bool psd_readable(std::string_view data, std::string *error)
{
    auto const l = layout(data, error);
    return l && check(*l, error);
}

std::optional<std::string> cmyk_psd_profile(std::string_view data)
{
    auto const l = layout(data, nullptr);
    if (!l || l->mode != CMYK) return {};
    return std::string(l->profile);
}

std::optional<RasterPixels> read_psd(std::string_view data, std::string *error)
{
    auto const l = layout(data, error);
    if (!l || !check(*l, error)) return {};
    unsigned const colours = l->colours();
    unsigned const used = colours + (l->transparency ? 1 : 0);
    std::size_t const row = std::size_t(l->width) * (l->depth / 8);
    std::size_t const plane = row * l->height;
    std::vector<std::uint8_t> planes(plane * used);
    Reader r{data};
    r.at = l->image;
    if (l->compression == 0) {
        // The channels follow one another; those after the ones used aren't read.
        auto const samples = r.take(plane * used);
        if (!r.ok) {
            fail(error, "its merged image is cut short");
            return {};
        }
        std::memcpy(planes.data(), samples.data(), samples.size());
    } else {
        // The packed size of every row of every channel, then the rows.
        std::vector<std::uint64_t> sizes(std::size_t(l->channels) * l->height);
        for (auto &size : sizes) size = r.number(l->big ? 4 : 2);
        for (std::size_t k = 0; k < std::size_t(used) * l->height; ++k) {
            auto const packed = r.take(sizes[k]);
            if (!r.ok || !unpack(packed, planes.data() + k * row, row)) {
                fail(error, "its merged image is damaged");
                return {};
            }
        }
    }
    RasterPixels out;
    out.model = l->mode == CMYK ? RasterPixels::Model::CMYK
              : l->mode == RGB  ? RasterPixels::Model::RGB
                                : RasterPixels::Model::Gray;
    out.width = l->width;
    out.height = l->height;
    out.bits = l->depth;
    out.profile = std::string(l->profile);
    out.premultiplied = l->transparency;
    out.left_out = l->channels - used;
    std::size_t const n = std::size_t(l->width) * l->height;
    if (l->depth == 8) {
        assemble<std::uint8_t>(planes, n, colours, l->transparency, l->mode == CMYK, out);
    } else {
        assemble<std::uint16_t>(planes, n, colours, l->transparency, l->mode == CMYK, out);
    }
    return out;
}

} // namespace Inkscape
