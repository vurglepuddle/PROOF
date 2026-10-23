// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the installed font for a font an Illustrator text document names.
 *
 * A text document names each font by its PostScript name. For an ordinary font that is
 * the name the system knows it by. An instance of a variable font has a name made up for
 * it (Adobe Technical Note 5902): what its family's names start with, then the
 * instance's style (AcuminConcept-ExtraCondUltraBlack) or its value on each axis
 * (AcuminConcept_396.000wght_64.000wdth_4.200slnt). One instance goes by different names
 * in different versions of a font, so the name finds the font, and the instance is the
 * axis values the text document keeps beside it.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_FONTS_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_FONTS_H

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class FontInstance;
class SPDocument;
class SPItem;
class SPStyle;

namespace Inkscape::XML {
class Node;
} // namespace Inkscape::XML

namespace Inkscape::Extension::Internal::AiNative {

/**
 * On a text or span whose font stands in for one that isn't installed: that font's
 * PostScript name.
 *
 * It holds for what the element's own font declarations set: a span inside that declares a
 * font of its own is that font, unless it carries the attribute too. And it holds only
 * while those declarations are the ones the import wrote, which name no
 * -inkscape-font-specification; choosing a font for the text writes one, and the text is
 * then in the font that was chosen.
 */
inline constexpr char const *FONT_MISSING_ATTRIBUTE = "proof:font-missing";
/// Beside it, for an instance of a variable font: the instance's value on each of the
/// font's axes, in the font's order.
inline constexpr char const *FONT_MISSING_AXES_ATTRIBUTE = "proof:font-missing-axes";
/// On a text with characters its font doesn't have, which another font draws: "true".
inline constexpr char const *GLYPHS_MISSING_ATTRIBUTE = "proof:glyphs-missing";

/// An installed font that sets what a text document names.
struct FontMatch
{
    /// Its font properties as style declarations, each ended by a semicolon. An instance
    /// of a variable font gives every axis in font-variation-settings.
    std::string css;
    bool variable = false; ///< An instance of a variable font.
    std::shared_ptr<FontInstance> face;
};

/**
 * Find the installed font with this PostScript name.
 *
 * @param design what the text document keeps for an instance of a variable font: its
 *               value on each axis, in the font's own order. Empty for any other font,
 *               and then a variable font's instance is found by its name alone.
 * @return nothing when the font isn't installed, or isn't the one Pango loads for it.
 */
std::optional<FontMatch> find_font(std::string const &postscript_name, std::vector<double> const &design = {});

/// The installed font that sets type, as a text document names it.
struct PostScriptFont
{
    std::string name;      ///< Its PostScript name.
    bool truetype = false; ///< Its outlines are TrueType's (a text document says 1), else 0.
    std::vector<double> design; ///< Actual loaded coordinates in font axis order; empty for static fonts.
};

/**
 * The font that sets type in `style`, by its PostScript name.
 *
 * @return nothing, with `why` saying so, when the font is not the one the style names (that
 *         family isn't installed and Pango stood another in, or Pango made up its bold or its
 *         slant), has unsupported axis settings, or has no PostScript name. Variable
 *         instances carry their actual loaded coordinates and FreeType's instance name.
 */
std::optional<PostScriptFont> postscript_font(SPStyle const *style, std::string &why);

/**
 * The font that stands in for one that isn't installed: Myriad Pro, as in Illustrator,
 * else Arial, else the system's sans serif. Its declarations name no
 * -inkscape-font-specification (see FONT_MISSING_ATTRIBUTE).
 */
std::optional<FontMatch> substitute_font();

/// A font a document's text names that isn't installed, and the text objects set in a
/// font standing in for it.
struct MissingFont
{
    std::string name; ///< Its PostScript name.
    std::vector<SPItem *> texts;
};

/// The fonts `document` names that aren't installed, in the order its text meets them.
std::vector<MissingFont> missing_fonts(SPDocument *document);

/**
 * Give text its own font back where that font has been installed since the document was
 * made, and drop the names of fonts from text that was given another font by hand.
 *
 * @return how many texts and spans got their font back.
 */
unsigned restore_fonts(SPDocument *document);

/**
 * Set the text that a font stands in for `name` in a font chosen for it, for good.
 *
 * @param css the chosen font's declarations as the font controls write them: they name an
 *            -inkscape-font-specification, by which the text is that font's from then on.
 * @return how many texts and spans were changed.
 */
unsigned replace_font(SPDocument *document, std::string const &name, std::string const &css);

/// Whether this element is set in a font that stands in for the one it names.
bool stands_in(XML::Node const *element);

/**
 * The characters of a text object that are set in a font standing in for another: runs
 * of the places its layout counts, each the first place and the one after the last.
 */
std::vector<std::pair<unsigned, unsigned>> standing_in(SPItem const *text);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_EXTENSION_INTERNAL_AI_NATIVE_FONTS_H
