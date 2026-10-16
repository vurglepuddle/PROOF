// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a placed picture's own pixels, where PROOF reads them itself.
 *
 * The usual picture loader shows a CMYK TIFF through plain arithmetic, without its profile or
 * its transparency, and can't read a Photoshop file at all; the PDF library's own TIFF reader
 * loses the transparency. Those pictures are read here instead (tiff-pixels.h, psd-pixels.h),
 * into one form, and shown and printed from it.
 *
 * CMYK numbers are the document's (Illustrator's "Preserve Numbers"): on screen they show
 * through the document's CMYK profile when it has one, else the picture's own, else the
 * working CMYK profile; in a PDF they go out unchanged as DeviceCMYK. RGB and gray show as
 * their values, as every other RGB picture in PROOF does.
 */
#ifndef SEEN_INKSCAPE_DISPLAY_RASTER_PIXELS_H
#define SEEN_INKSCAPE_DISPLAY_RASTER_PIXELS_H

#include <cairo.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "colors/spaces/enum.h"

namespace Inkscape {
namespace Colors::CMS {
class Profile;
}

struct RasterPixels
{
    enum class Model { Gray, RGB, CMYK };
    Model model = Model::RGB;
    std::uint32_t width = 0, height = 0;
    unsigned bits = 8; ///< Of every sample: 8 or 16.
    /// The colour samples of each pixel, in order (16 bits: native byte order), never
    /// premultiplied. CMYK as inks: 0 is none.
    std::vector<std::uint8_t> colour;
    /// One sample a pixel at `bits`, straight; empty when the picture has no transparency.
    std::vector<std::uint8_t> alpha;
    bool premultiplied = false; ///< How the file kept it (it is straight here).
    std::string profile;        ///< Its own ICC profile, or empty.
    unsigned left_out = 0;      ///< Extra channels that aren't its transparency, not read.

    unsigned channels() const { return model == Model::Gray ? 1 : model == Model::RGB ? 3 : 4; }
};

/// For the screen: a premultiplied ARGB32 surface. CMYK goes through `profile` (or, when that
/// is empty, the picture's own profile, else the working CMYK profile) into sRGB; RGB and gray
/// as their values. Nothing when a CMYK picture has no profile to be seen through.
cairo_surface_t *raster_surface(RasterPixels const &pixels, std::shared_ptr<Colors::CMS::Profile> profile,
                                Colors::RenderingIntent intent = Colors::RenderingIntent::RELATIVE_COLORIMETRIC);

} // namespace Inkscape

#endif
