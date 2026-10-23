// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: make a document from Illustrator's native records.
 *
 * The reader (ai-native-reader.h) turns the records into plain structures in art
 * space. This builds the PROOF document from them:
 *
 *   Illustrator                    PROOF
 *   layer, sublayer                layer: name, hidden, locked, layer colour (highlight);
 *                                  print, preview and dim kept as proof:layer-* attributes
 *   group, clipping group          group; a clipping group's path becomes its clip-path
 *   compound path                  one path
 *   path                           path with its fill and stroke; overprint in proof:overprint
 *   process colour                 CMYK (or RGB) in the document's assigned profile
 *   spot colour                    spot ink swatch (spot-ink.h), its Lab, CMYK or RGB kept
 *   [Registration]                 spot ink "All", PDF's registration ink
 *   global process colour          swatch named after it, one per tint
 *   gradient                       gradient named after it, and one per object that places it;
 *                                  midpoints become stops along Illustrator's blend curve
 *   pattern                        pattern holding its tile's art, and one per object that
 *                                  places it
 *   embedded image                 image (PNG for the screen); CMYK and grey samples kept
 *                                  in proof:samples as a TIFF
 *   placed (linked) picture        image linked to the same file, found where the document
 *                                  says it is or beside the document; one that can't be found
 *                                  keeps its link and shows as the document's own preview of it
 *   clipping group                 group with clip-path; a clipping path's own fill is a path
 *                                  under what it clips, its stroke a path over all of it
 *   name, hidden, locked           label, display:none, sodipodi:insensitive
 *   transparency                   opacity, mix-blend-mode, isolation; knockout in proof:knockout
 *   turned box (BBAccumRotation)   proof:box-angle on paths
 *   drawn look                     group of what it draws, marked proof:drawn-look
 *   artboards                      pages; the first artboard's top-left corner is 0,0
 *   guides                         guides
 *
 *   point type, area type          text, a span to a line where Illustrator laid it out; area
 *                                  type keeps its frame in proof:text-frame, its paragraphs'
 *                                  alignment in text-align and which line ends were typed in
 *                                  proof:line-end
 *   an instance of a variable font its family, with every axis in font-variation-settings
 *   All Caps                       text-transform:uppercase on what was typed
 *   small caps                     font-variant-caps:small-caps, which the text layout sets
 *                                  with the font's own or makes of its capitals, at the size
 *                                  the document says (type-settings.h)
 *   underline, strikethrough       text-decoration-line on the span of the run
 *   a paragraph read from the right direction:rtl, each line anchored at the same end
 *   letters read from the right    direction:ltr with unicode-bidi:bidi-override: set from
 *   that Illustrator set as typed  the left one after another, as it shows them
 *   type painted with a pattern    the pattern, placed as on a path; with a gradient, black,
 *                                  as Illustrator draws it
 *   an envelope                    group of the outlines it drew, marked proof:drawn-look;
 *                                  what it distorts isn't kept
 *   a compound shape, a blend,     group of what the plug-in drew of it, with the plug-in's
 *   a brush stroke                 name in proof:plugin; the paths it was made from aren't kept
 *   superscript, subscript         font-variant-position:super or sub at the type's own
 *                                  size, which the text layout sets smaller and off the
 *                                  baseline by what the document says (type-settings.h)
 *   a font that isn't installed    the font that stands in for it (ai-native-fonts.h), and
 *                                  its own name kept in proof:font-missing
 *
 * The document's colours are in its own profile when its file carries one (Source::profile),
 * as Illustrator shows them, and in PROOF's working profile of the document's mode otherwise.
 *
 * User units are points, as in the records; width and height use the file's ruler unit.
 * Type that shows and can't be set as it is (ai-native-text.h) refuses the file, and the
 * caller opens its page; such type that doesn't show is left out. A font that isn't
 * installed doesn't refuse it: as in Illustrator, another font sets the type until the
 * font is there, and the document says which fonts those are. Nor does a linked picture
 * whose file is gone: its link stays as it was written, the small picture Illustrator keeps
 * of it stands in its place, and the document says which pictures those are.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_IMPORT_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_IMPORT_H

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <2geom/rect.h>

#include "ai-native-fonts.h"
#include "ai-native-reader.h"

class SPDocument;

namespace Inkscape::Extension::Internal::AiNative {

/// On a layer that doesn't print: "false".
inline constexpr char const *LAYER_PRINT_ATTRIBUTE = "proof:layer-print";
/// On area type: its frame's outline as path data, in the text's own coordinates.
inline constexpr char const *TEXT_FRAME_ATTRIBUTE = "proof:text-frame";
/// On area type whose frame is too small for its text: the text that doesn't show.
inline constexpr char const *TEXT_OVERFLOW_ATTRIBUTE = "proof:text-overflow";
/// On a line of type that doesn't end its paragraph: "wrap" where area type's frame broke it
/// with nothing typed, "break" at a line break typed inside the paragraph (Illustrator's
/// forced line break). A line without it ends its paragraph.
inline constexpr char const *LINE_END_ATTRIBUTE = "proof:line-end";
/// On area type, or a line of it, whose paragraphs Illustrator hyphenates: "true".
inline constexpr char const *HYPHENATE_ATTRIBUTE = "proof:hyphenate";
/// Paragraph features 1, 2, 3 as three lengths in the text's coordinates.
inline constexpr char const *PARAGRAPH_INDENTS_ATTRIBUTE = "proof:paragraph-indents";
/// A displayed automatic wrap hyphen, excluded from the native story on save.
inline constexpr char const *AUTOMATIC_HYPHEN_ATTRIBUTE = "proof:automatic-hyphen";
/// On area type whose frame says how it places its first baseline: the rule and the minimum
/// (PointText::first_baseline), as "5 0".
inline constexpr char const *FIRST_BASELINE_ATTRIBUTE = "proof:first-baseline";

/// A linked picture that shows and whose file can't be found.
struct MissingLink
{
    std::string name; ///< The file's name, without its folder.
    /// Where the picture lies: its four corners in the document's user units.
    std::array<Geom::Point, 4> box;
    bool preview = false; ///< Whether the document keeps a picture of it, which then shows there.
};

/// A linked picture that shows, whose file is found, and of which the document keeps the
/// picture Illustrator made of it when saving. A file that no longer looks like that picture
/// changed since; the file's page shows it as it was.
struct FoundLink
{
    std::string name; ///< The file's name, without its folder.
    std::string file; ///< Where it was found (UTF-8).
    std::array<Geom::Point, 4> box; ///< Its four corners in the document's user units.
    /// The kept picture: rows of width * 4 bytes, red, green, blue and a straight alpha.
    int preview_width = 0, preview_height = 0;
    std::string preview;
};

struct Built
{
    std::unique_ptr<SPDocument> document;
    /// Each artboard in the document's user units (points, y down), in the file's order.
    std::vector<Geom::Rect> artboards;
    /// What was left out or approximated, for the person opening the file.
    std::vector<std::string> notes;
    /// The fonts that aren't installed, by PostScript name, in the order met.
    std::vector<std::string> missing_fonts;
    /// Where type that shows is set by a font other than its own (one standing in, or one
    /// drawing characters its font lacks): four-cornered boxes in the document's user units,
    /// both where Illustrator set that type and where it is set here. It can't look like the
    /// file's page there, and a comparison with the page leaves these out.
    std::vector<std::array<Geom::Point, 4>> substituted_type;
    /// The linked pictures that show and can't be found, in the order met. What stands in
    /// for one can't look like the page either, and a comparison leaves its box out.
    std::vector<MissingLink> missing_links;
    /// The linked pictures found, with the picture the document keeps of each; a comparison
    /// with the page leaves out those that no longer look like it (pdfinput/ai-native-open).
    std::vector<FoundLink> found_links;
    /// The document's own colour profile, by its description, when the file carried one and
    /// the document has it; empty when the document has PROOF's working profile.
    std::string profile;
};

/// Where the records came from, for what they refer to outside themselves.
struct Source
{
    /// The folder of the .ai file (a file name in UTF-8), or empty. Linked pictures that
    /// aren't where the document says are looked for there, as Illustrator does.
    std::string folder;
    /// The document's own ICC profile, or empty. The records don't hold it: Illustrator
    /// keeps it in the file's PDF part, as the colour space of its process colours. One of
    /// another colour model than the document's is passed over.
    std::string profile;
};

/**
 * Build the document of `ai`.
 *
 * @return the document, or nothing with the reason in `error` when it holds something
 *         the import can't place: type that shows and isn't supported, a linked file that
 *         shows and isn't a picture that can be read.
 */
std::optional<Built> build(Document const &ai, std::string &error, Source const &source = {});

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_EXTENSION_INTERNAL_AI_NATIVE_IMPORT_H
