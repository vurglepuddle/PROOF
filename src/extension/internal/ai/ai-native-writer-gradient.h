// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_WRITER_GRADIENT_H
#define SEEN_AI_NATIVE_WRITER_GRADIENT_H

#include <string>
#include <vector>
#include <2geom/affine.h>

class SPShape;

namespace Inkscape::Extension::Internal::AiNative {

/** An opaque process linear fill, prepared independently for one shape.
 *
 * definition belongs in Setup; the caller writes the total gradient count with Bn once.
 * begin/end bracket the final contour's fill operator, after its path data. They must
 * never be cached as ordinary fill state. The caller handles compound paths, ancestor
 * effects and the PDF half; it must pass the shape's complete local-to-art transform.
 *
 * Requires an up-to-date document. Supports pad, standard RGB or DeviceCMYK stops,
 * stop-only hrefs and nonsingular affine geometry. Anything else is refused by name,
 * and all three record strings are empty. Neither the document nor its XML is edited.
 */
struct WrittenGradient
{
    std::string definition;
    std::string begin;
    std::string end;
    std::vector<std::string> refused;
};

WrittenGradient write_gradient(SPShape const *shape, Geom::Affine const &item_to_art,
                               std::string const &unique_name, bool legacy = false);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_GRADIENT_H
