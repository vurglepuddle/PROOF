// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_WRITER_PICTURE_H
#define SEEN_AI_NATIVE_WRITER_PICTURE_H

#include <string>
#include <vector>
#include <2geom/affine.h>

class SPImage;

namespace Inkscape::Extension::Internal::AiNative {

/** An embedded raster or a local raster link, as Illustrator writes pictures.
 *
 * For an embedded raster, `records` is the whole object, from `%AI5_File:` to the `N`
 * after `%AI5_EndRaster`: its colour, transparency as an alpha channel, at 8 bits,
 * and where it lies.
 * CMYK numbers are kept: those an Illustrator picture brought with it (proof:samples) or a
 * TIFF's or Photoshop file's own; any other picture goes out as the RGB it shows. Anything
 * else is refused by name and `records` is empty. The caller passes the picture's complete
 * local-to-art transform and handles its effects, clipping and the PDF half.
 * A local PNG/JPEG/classic TIFF/PSD link instead uses a placed-file block with absolute file
 * address and pixel bounds; its file bytes (including CMYK and alpha) remain external.
 * Missing/unreadable links, unsupported formats (including PSB and BigTIFF), and a changed
 * modification time are refused.
 * Illustrator 7 instead writes opaque embedded samples as ASCII hex, without an alpha
 * plane or the modern raster fields. Transparency and external links are refused there.
 */
struct WrittenPicture
{
    std::string records;
    std::vector<std::string> refused;
};

/// Illustrator 7 uses opaque raster samples as comment-prefixed hexadecimal lines.
/// Links and alpha channels remain refused in that form.
WrittenPicture write_picture(SPImage const *image, Geom::Affine const &item_to_art, bool legacy = false);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_PICTURE_H
