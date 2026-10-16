// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a TIFF's own pixels, for the screen and for print (raster-pixels.h).
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
 */
#ifndef SEEN_INKSCAPE_DISPLAY_TIFF_PIXELS_H
#define SEEN_INKSCAPE_DISPLAY_TIFF_PIXELS_H

#include <optional>
#include <string>
#include <string_view>

#include "display/raster-pixels.h"

namespace Inkscape {

/// Whether `data` starts as a TIFF does (either byte order). Reads nothing else.
bool looks_like_tiff(std::string_view data);

/// Whether `data` is a TIFF whose pixels are CMYK. Reads its tags only.
bool is_cmyk_tiff(std::string_view data);

/// The ICC profile a CMYK TIFF carries (empty when it carries none), or nothing when `data`
/// is no CMYK TIFF. Reads its tags only.
std::optional<std::string> cmyk_tiff_profile(std::string_view data);

/// The pixels of the TIFF in `data`, or nothing when it is no TIFF or one of a kind not read
/// here (then `error` says why, when given).
std::optional<RasterPixels> read_tiff(std::string_view data, std::string *error = nullptr);

} // namespace Inkscape

#endif
