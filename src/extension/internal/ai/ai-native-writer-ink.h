// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * A spot ink's definition as Illustrator's records carry it, shared by the solid paints and
 * the gradient stops of both writers.
 *
 * Illustrator defines an ink in CMYK, RGB or Lab (its Pantone books are Lab). It writes each
 * with its CMYK first: `c m y k (name) tint x` or `… tint 0 Xx` for CMYK, `c m y k r g b (name)
 * tint 1 Xx` for RGB and `c m y k L a b (name) tint 2 Xx` for Lab, L from 0 to 100. Its header
 * lists CMYK inks by their CMYK (%%CMYKCustomColor) and RGB and Lab inks by their RGB
 * (%%RGBCustomColor), that list after the other. In the owner's files: 382 CMYK, 62 RGB and
 * 741 Lab inks, each in its list without exception.
 */
#ifndef SEEN_AI_NATIVE_WRITER_INK_H
#define SEEN_AI_NATIVE_WRITER_INK_H

#include <array>
#include <string>
#include <variant>

class SPDocument;

namespace Inkscape::Colors {
class Color;
}

namespace Inkscape::Extension::Internal::AiNative {

struct InkDefinition
{
    /// Xx's type operand.
    enum class Model { CMYK = 0, RGB = 1, Lab = 2 };
    Model model = Model::CMYK;
    /// The ink as it is; for RGB and Lab, their process colour at full strength.
    std::array<double, 4> cmyk{};
    /// RGB from 0 to 1, or L from 0 to 100 then a and b. Unused for CMYK.
    std::array<double, 3> values{};
    /// What %%RGBCustomColor lists for an RGB or Lab ink.
    std::array<double, 3> rgb{};

    bool operator==(InkDefinition const &) const = default;
};

/** An ink's definition from its full-strength alternate (SpotInk::Ink::alternate), or why it
 * can't be written. CMYK, and CMYK in a profile, are their numbers; RGB is its numbers (a
 * profile's too), other RGB-like spaces their sRGB; Lab is L, a and b. The CMYK beside an RGB
 * or Lab ink is converted through the document's CMYK profile, or else the working CMYK space,
 * and its RGB for the header in sRGB, clipped as Illustrator's are. */
std::variant<InkDefinition, std::string> ink_definition(Colors::Color const &alternate, SPDocument *document);

/** The operands before the ink's name where it is used at `tint` (0 paper, 1 full): CMYK, then
 * for RGB and Lab their three values. Illustrator writes beside an RGB or Lab ink the CMYK of
 * the tinted colour (SpotInk::display_color), and a CMYK ink's definition at every tint: in the
 * owner's files 55 Lab inks and 1 RGB ink are used at more than one tint, all so, and 38 CMYK
 * inks, none so. Empty when the tinted colour has no CMYK. */
std::string ink_operands(InkDefinition const &ink, Colors::Color const &alternate, double tint, SPDocument *document);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_INK_H
