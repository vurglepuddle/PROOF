// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Spot (named) inks, stored in the document as PROOF swatches.
 *
 * A spot ink is an ordinary solid Inkscape swatch (a single-stop
 * linearGradient with inkscape:swatch="solid") with three extra attributes:
 *
 *   proof:ink            exact ink name, e.g. "PANTONE 185 C" or "CutContour"
 *   proof:ink-alternate  full-strength alternate colour (CMYK, Lab, RGB...)
 *   proof:ink-tint       tint from 0 to 1; absent means 1
 *
 * The stop colour is the tinted alternate, so anything that does not know
 * about spot inks (stock Inkscape, browsers) still shows the right colour.
 * Objects reference the swatch with fill/stroke url(#id), so redefining the
 * ink updates every user. Each tint is its own self-contained swatch, so
 * copying objects between documents cannot orphan a tint.
 */
#ifndef SEEN_INKSCAPE_SPOT_INK_H
#define SEEN_INKSCAPE_SPOT_INK_H

#include <optional>
#include <string>

#include "colors/color.h"

class SPDocument;

namespace Inkscape::XML {
class Document;
class Node;
} // namespace Inkscape::XML

namespace Inkscape::SpotInk {

struct Ink
{
    std::string name;
    Colors::Color alternate; ///< Full-strength appearance, in the space the source used.
    double tint = 1.0;       ///< 0 = paper, 1 = full ink.
};

/// The colour a tint of this alternate shows on screen and in non-spot output.
/// CMYK scales the inks; Lab and RGB-like spaces blend towards paper white,
/// matching the PDF Separation convention of white at tint 0.
Colors::Color display_color(Colors::Color const &alternate, double tint);

/// Read the ink stored on a swatch element, if it is a PROOF spot swatch.
std::optional<Ink> read(XML::Node const *swatch);

/// Find a swatch in defs for this ink name, alternate and tint, or create one.
/// Returns the swatch element's id.
std::string ensure(XML::Document *xml_doc, XML::Node *defs, SPDocument *doc, Ink const &ink);
std::string ensure(SPDocument *doc, Ink const &ink);

/// Lower-case, hyphenated id stem for an ink name ("PANTONE 185 C" -> "pantone-185-c").
std::string id_stem(std::string const &name);

/// Swatch label for an ink at a tint: "PANTONE 185 C" or "PANTONE 185 C 30%".
std::string label_for(std::string const &name, double tint);

/// The full-strength alternate that shows `shown` at `tint` (inverse of display_color),
/// clamped to the colour space. Used when a tint swatch's colour is edited.
Colors::Color alternate_for(Colors::Color const &shown, double tint);

// Editing an ink. An ink is every swatch with the same name and definition: its full-strength
// swatch and its tint swatches. Each returns the number of swatches changed; callers record undo.

/// Give the ink a new definition; every swatch of it recomputes its shown colour for its tint.
int redefine(SPDocument *doc, Ink const &ink, Colors::Color const &new_alternate);

/// Rename the ink on every swatch of it; tint swatches keep their "NN%" label suffix.
int rename(SPDocument *doc, Ink const &ink, std::string const &new_name);

/// Turn a solid swatch into a full-strength spot ink named `name`, defined by its current colour.
bool make_spot(SPDocument *doc, XML::Node *swatch, std::string const &name);

/// Turn a spot swatch back into an ordinary (process) swatch; its shown colour is kept.
void make_process(XML::Node *swatch);

} // namespace Inkscape::SpotInk

#endif // SEEN_INKSCAPE_SPOT_INK_H
