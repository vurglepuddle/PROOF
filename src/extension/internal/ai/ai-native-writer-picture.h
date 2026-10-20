// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_WRITER_PICTURE_H
#define SEEN_AI_NATIVE_WRITER_PICTURE_H

#include <string>
#include <vector>
#include <2geom/affine.h>

class SPImage;

namespace Inkscape::Extension::Internal::AiNative {

/** A picture the document holds itself, as Illustrator writes an embedded raster.
 *
 * `records` is the whole object, from `%AI5_File:` to the `N` after `%AI5_EndRaster`: the
 * picture's colour, its transparency as an alpha channel, at 8 bits, and where it lies.
 * CMYK numbers are kept: those an Illustrator picture brought with it (proof:samples) or a
 * TIFF's or Photoshop file's own; any other picture goes out as the RGB it shows. Anything
 * else is refused by name and `records` is empty. The caller passes the picture's complete
 * local-to-art transform and handles its effects, clipping and the PDF half.
 */
struct WrittenPicture
{
    std::string records;
    std::vector<std::string> refused;
};

WrittenPicture write_picture(SPImage const *image, Geom::Affine const &item_to_art);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_PICTURE_H
