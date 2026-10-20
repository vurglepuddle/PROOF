// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_WRITER_GRADIENT_H
#define SEEN_AI_NATIVE_WRITER_GRADIENT_H

#include <array>
#include <string>
#include <utility>
#include <vector>
#include <2geom/affine.h>

class SPShape;

namespace Inkscape::Extension::Internal::AiNative {

/** An opaque process linear or radial fill or stroke, prepared independently for one shape.
 *
 * definition belongs in Setup; the caller writes the total gradient count with Bn once.
 * begin opens the instance before the final contour's paint operator, after its path data,
 * and gradient_end() closes it after that operator. Neither may be cached as ordinary paint
 * state. The caller handles compound paths, ancestor effects and the PDF half; it must pass
 * the shape's complete local-to-art transform.
 *
 * Requires an up-to-date document. Supports pad, standard RGB or DeviceCMYK stops, tints of
 * spot inks defined in CMYK (with each other and CMYK stops), stop-only hrefs and nonsingular
 * affine geometry; a radial fill's focal point, and in the modern form its ellipse (Illustrator 7
 * takes circles) and gradients on strokes (Illustrator CS6 brought them; radial strokes must
 * have a centred focal point, and a turned ellipse must be centred on its path bounds).
 * Anything else is
 * refused by name, and both record strings are empty. Neither the document nor its XML is
 * edited.
 */
struct WrittenGradient
{
    std::string definition;
    std::string begin;
    std::vector<std::string> refused;
    /// The spot inks its stops name, with their full-strength CMYK: the caller declares them
    /// with the document's other inks (%%DocumentCustomColors, %%CMYKCustomColor).
    std::vector<std::pair<std::string, std::array<double, 4>>> inks;
};

/// `canvas` is the art origin on the y-down canvas where Illustrator applies a gradient's own
/// matrix (4320 less the template box's x, 4320 plus its y): a turned ellipse is turned there.
/// `stroke` writes the shape's stroke paint, opening with `1 Bb` as Illustrator does.
WrittenGradient write_gradient(SPShape const *shape, Geom::Affine const &item_to_art,
                               std::string const &unique_name, bool legacy = false,
                               Geom::Point const &canvas = {}, bool stroke = false);

/// What closes an instance after the paint operator `op`, by whether that strokes the path:
/// `0 BB` after f or F, `1 BB` after S or B, `2 BB` after s or b (Adobe's 1998 specification,
/// table 10: don't stroke, stroke, close and stroke; and Illustrator's own files throughout).
std::string gradient_end(char op);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_GRADIENT_H
