// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a Photoshop file's merged image, for the screen and for print (raster-pixels.h).
 *
 * Illustrator places Photoshop files (.psd, and .psb for large ones) as pictures and shows and
 * prints the merged image Photoshop saves beside the layers ("Maximize Compatibility", on by
 * default). PROOF reads that image, not the layers:
 *
 * - Gray, RGB and CMYK, 8 or 16 bits a channel, uncompressed or RLE (PackBits), as Photoshop
 *   writes it.
 * - CMYK, which Photoshop keeps inverted (255 is no ink), as inks.
 * - Its transparency, when the file marks the merged image's first extra channel as such (a
 *   negative layer count). Photoshop mattes the merged image against white where it isn't
 *   opaque; the colour is unmatted here, straight, as a PDF wants it beside its SMask. Other
 *   extra channels (spot channels, saved selections) are counted and left out.
 * - Its own ICC profile (image resource 1039).
 */
#ifndef SEEN_INKSCAPE_DISPLAY_PSD_PIXELS_H
#define SEEN_INKSCAPE_DISPLAY_PSD_PIXELS_H

#include <optional>
#include <string>
#include <string_view>

#include "display/raster-pixels.h"

namespace Inkscape {

/// Whether `data` starts as a Photoshop file does. Reads nothing else.
bool looks_like_psd(std::string_view data);

/// Whether the merged image of the Photoshop file in `data` can be read here; when not, `error`
/// says why. Reads its header and resources only.
bool psd_readable(std::string_view data, std::string *error = nullptr);

/// The ICC profile a CMYK Photoshop file carries (empty when it carries none), or nothing when
/// `data` is no CMYK Photoshop file. Reads its header and resources only.
std::optional<std::string> cmyk_psd_profile(std::string_view data);

/// The merged image of the Photoshop file in `data`, or nothing when it is none or one not read
/// here (then `error` says why, when given).
std::optional<RasterPixels> read_psd(std::string_view data, std::string *error = nullptr);

} // namespace Inkscape

#endif
