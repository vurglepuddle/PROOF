// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the stops a gradient is drawn with on the screen and in its previews.
 *
 * Illustrator blends a CMYK document's gradients in CMYK and shows the blend through the
 * document's profile. Cairo, like a browser, blends what it is given in RGB: up to 31 levels from
 * Illustrator mid-ramp for two process stops (PROOF-ai-interchange.md, "Gradient stops of
 * different colour models"). So in a CMYK document the canvas draws a Classic gradient from
 * samples blended in CMYK, and a Perceptual one from its Oklab samples (gradient-perceptual.h).
 *
 * Drawing only. The stops in the document, and what the writers and PDF output take, stay as
 * they are: a PDF shading in a CMYK space blends in CMYK by itself.
 */
#ifndef PROOF_GRADIENT_RENDERING_H
#define PROOF_GRADIENT_RENDERING_H

#include "sp-paint-server-data.h"

class SPDocument;
class SPGradient;

namespace Inkscape::GradientRendering {

/// The gradient's stops as the screen and the gradient previews draw them. An ink beside process
/// colour overprints it, as Illustrator draws it (gradient-rendering.cpp, `ink_blend`).
SPGradientVector canvas_vector(SPGradient const *gradient);

/// The stops PDF output draws a gradient with, so that its page looks as the canvas does: a
/// Perceptual gradient's Oklab samples; an ink beside process colour overprinted, in process
/// colour (the ink on its own plate is the separations work); otherwise the stops themselves, a
/// CMYK shading blending in CMYK by itself.
SPGradientVector pdf_vector(SPGradient const *gradient);

/// In a CMYK document, the stops with samples between them blended in CMYK, every one a colour
/// of the document's CMYK (its profile's, or the working CMYK profile's when it has none). The
/// stops themselves keep their ink values: a CMYK stop its own numbers, others converted as the
/// document converts paint. In an RGB document, or when a stop can't be converted, the vector
/// as it is.
SPGradientVector cmyk_blend(SPDocument *document, SPGradientVector const &vector);

/// A mesh's patches as the screen draws them. In a CMYK document a patch whose corners differ is
/// cut into smaller patches along its own parameters, the same surface exactly, each corner
/// coloured from the four blended in CMYK; Cairo then blends only within the small ones in RGB.
/// Otherwise, or when a corner can't be converted, the patches as they are.
SPGradientMesh canvas_mesh(SPDocument *document, SPGradientMesh const &mesh);

} // namespace Inkscape::GradientRendering

#endif
