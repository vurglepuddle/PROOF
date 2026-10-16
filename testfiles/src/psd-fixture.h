// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: small Photoshop files for tests, written as Photoshop lays them out (Adobe's
 * "Photoshop File Formats Specification"): what display/psd-pixels.h reads, and no more.
 */
#ifndef SEEN_TESTFILES_PSD_FIXTURE_H
#define SEEN_TESTFILES_PSD_FIXTURE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PsdFixture {

struct Psd
{
    unsigned version = 1; ///< 2: a .psb.
    unsigned mode = 4;    ///< 1 gray, 3 RGB, 4 CMYK, 9 Lab.
    unsigned depth = 8;
    std::uint32_t w = 4, h = 1;
    unsigned channels = 5;
    /// Negative: the merged image's first extra channel is its transparency. 0: no layers.
    int layer_count = -1;
    bool layers_tagged = false; ///< The layers in an "Lr16" block, as at 16 bits.
    bool merged = true;         ///< Image resource 1057: Photoshop saved a merged image.
    std::string icc;
    unsigned compression = 1; ///< 0 raw, 1 RLE (PackBits); 2 (ZIP) writes the samples raw.
};

inline void put(std::string &out, std::uint64_t v, unsigned bytes)
{
    for (int i = int(bytes) - 1; i >= 0; --i) out += char((v >> (8 * i)) & 0xff);
}

/// A plane of 8-bit samples.
inline std::string plane8(std::vector<unsigned> const &samples)
{
    std::string out;
    for (auto s : samples) out += char(s);
    return out;
}

/// A plane of 16-bit samples, big-endian.
inline std::string plane16(std::vector<unsigned> const &samples)
{
    std::string out;
    for (auto s : samples) put(out, s, 2);
    return out;
}

/// One row in PackBits: runs of three or more repeated, the rest literal.
inline std::string packbits(std::string_view row)
{
    std::string out;
    std::size_t i = 0;
    while (i < row.size()) {
        std::size_t run = 1;
        while (i + run < row.size() && run < 128 && row[i + run] == row[i]) ++run;
        if (run >= 3) {
            out += char(257 - run);
            out += row[i];
            i += run;
            continue;
        }
        std::size_t const start = i;
        while (i < row.size() && i - start < 128 &&
               !(i + 2 < row.size() && row[i] == row[i + 1] && row[i] == row[i + 2])) {
            ++i;
        }
        out += char(i - start - 1);
        out.append(row.substr(start, i - start));
    }
    return out;
}

/// The file, its merged image made of `planes`, one a channel, each row by row.
inline std::string make_psd(Psd const &p, std::vector<std::string> const &planes)
{
    std::string out = "8BPS";
    put(out, p.version, 2);
    out.append(6, '\0');
    put(out, p.channels, 2);
    put(out, p.h, 4);
    put(out, p.w, 4);
    put(out, p.depth, 2);
    put(out, p.mode, 2);
    put(out, 0, 4); // no colour mode data
    std::string resources;
    auto resource = [&](unsigned id, std::string const &body) {
        resources += "8BIM";
        put(resources, id, 2);
        resources += std::string("\0\0", 2); // an empty name, padded to an even length
        put(resources, body.size(), 4);
        resources += body;
        if (body.size() % 2) resources += '\0';
    };
    std::string version_info;
    put(version_info, 1, 4);
    version_info += char(p.merged ? 1 : 0);
    version_info.append(7, '\0');
    resource(1057, version_info);
    if (!p.icc.empty()) resource(1039, p.icc);
    put(out, resources.size(), 4);
    out += resources;
    unsigned const lengths = p.version == 2 ? 8 : 4;
    std::string layers;
    if (p.layer_count) {
        std::string count;
        put(count, std::uint16_t(std::int16_t(p.layer_count)), 2);
        if (p.layers_tagged) {
            put(layers, 0, lengths); // no layer information here
            put(layers, 0, 4);       // no global layer mask
            layers += "8BIMLr16";
            put(layers, count.size(), lengths);
            layers += count;
        } else {
            put(layers, count.size(), lengths);
            layers += count;
        }
    }
    put(out, layers.size(), lengths);
    out += layers;
    put(out, p.compression, 2);
    std::size_t const row = std::size_t(p.w) * (p.depth / 8);
    if (p.compression == 1) {
        std::vector<std::string> rows;
        for (auto const &plane : planes) {
            for (std::uint32_t y = 0; y < p.h; ++y) rows.push_back(packbits(std::string_view(plane).substr(y * row, row)));
        }
        for (auto const &r : rows) put(out, r.size(), p.version == 2 ? 4 : 2);
        for (auto const &r : rows) out += r;
    } else {
        for (auto const &plane : planes) out += plane;
    }
    return out;
}

/// Ink (0 none) as Photoshop keeps it in a merged image: inverted, matted against white.
inline unsigned stored_ink(unsigned ink, unsigned alpha, unsigned top = 255)
{
    return top - (ink * alpha + top / 2) / top;
}

} // namespace PsdFixture

#endif
