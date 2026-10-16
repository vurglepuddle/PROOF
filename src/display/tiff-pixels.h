// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a TIFF's own pixels, for the screen and for print.
 *
 * A placed TIFF is a print shop's picture for work seen from close up: CMYK, at the full
 * resolution, often with transparency. The usual picture loader shows it through plain
 * arithmetic, without its colour profile and without its transparency, and the PDF library's
 * own TIFF reader loses the transparency. So PROOF reads a TIFF's pixels itself:
 *
 * - Gray, RGB and CMYK (TIFF's "separated" with the CMYK ink set), 8 or 16 bits a sample,
 *   in strips or tiles, the samples of a pixel together (Photoshop's "interleaved").
 * - Its transparency, the first extra sample marked as alpha, premultiplied or not (Photoshop
 *   writes CMYK transparency premultiplied): straight here, as a PDF wants it. Further extra
 *   channels (spot channels, saved selections) are counted and left out.
 * - Its own ICC profile, kept as it is.
 *
 * CMYK numbers are the document's (Illustrator's "Preserve Numbers"): on screen they show
 * through the document's CMYK profile when it has one, else the picture's own, else the
 * working CMYK profile; in a PDF they go out unchanged as DeviceCMYK.
 */
#ifndef SEEN_INKSCAPE_DISPLAY_TIFF_PIXELS_H
#define SEEN_INKSCAPE_DISPLAY_TIFF_PIXELS_H

#include <cairo.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "colors/spaces/enum.h"

namespace Inkscape {
namespace Colors::CMS {
class Profile;
}

struct TiffPixels
{
    enum class Model { Gray, RGB, CMYK };
    Model model = Model::RGB;
    std::uint32_t width = 0, height = 0;
    unsigned bits = 8; ///< Of every sample: 8 or 16.
    /// The colour samples of each pixel, in order (16 bits: native byte order), never
    /// premultiplied.
    std::vector<std::uint8_t> colour;
    /// One sample a pixel at `bits`, straight; empty when the picture has no transparency.
    std::vector<std::uint8_t> alpha;
    bool premultiplied = false; ///< How the file kept it (it is straight here).
    std::string profile;        ///< Its own ICC profile, or empty.
    unsigned left_out = 0;      ///< Extra channels that aren't its transparency, not read.

    unsigned channels() const { return model == Model::Gray ? 1 : model == Model::RGB ? 3 : 4; }
};

/// Whether `data` starts as a TIFF does (either byte order). Reads nothing else.
bool looks_like_tiff(std::string_view data);

/// Whether `data` is a TIFF whose pixels are CMYK. Reads its tags only.
bool is_cmyk_tiff(std::string_view data);

/// The ICC profile a CMYK TIFF carries (empty when it carries none), or nothing when `data`
/// is no CMYK TIFF. Reads its tags only.
std::optional<std::string> cmyk_tiff_profile(std::string_view data);

/// The pixels of the TIFF in `data`, or nothing when it is no TIFF or one of a kind not read
/// here (then `error` says why, when given).
std::optional<TiffPixels> read_tiff(std::string_view data, std::string *error = nullptr);

/// A CMYK TIFF for the screen: a premultiplied ARGB32 surface, through `profile` (or, when
/// that is empty, the picture's own profile, else the working CMYK profile) into sRGB.
/// Nothing when it is no CMYK TIFF or no profile can be had.
cairo_surface_t *cmyk_tiff_surface(TiffPixels const &tiff, std::shared_ptr<Colors::CMS::Profile> profile,
                                   Colors::RenderingIntent intent = Colors::RenderingIntent::RELATIVE_COLORIMETRIC);

} // namespace Inkscape

#endif
