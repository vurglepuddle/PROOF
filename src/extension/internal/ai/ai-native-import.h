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
 *   embedded image                 image (PNG for the screen); CMYK samples kept in
 *                                  proof:samples as a CMYK TIFF
 *   placed (linked) picture        image linked to the same file, found where the document
 *                                  says it is or beside the document
 *   name, hidden, locked           label, display:none, sodipodi:insensitive
 *   transparency                   opacity, mix-blend-mode, isolation; knockout in proof:knockout
 *   turned box (BBAccumRotation)   proof:box-angle on paths
 *   drawn look                     group of what it draws, marked proof:drawn-look
 *   artboards                      pages; the first artboard's top-left corner is 0,0
 *   guides                         guides
 *
 *   point type, area type          text, a span to a line where Illustrator laid it out; area
 *                                  type keeps its frame in proof:text-frame
 *   an instance of a variable font its family, with every axis in font-variation-settings
 *   All Caps                       text-transform:uppercase on what was typed
 *   superscript, subscript         a span at the size and baseline shift the document's
 *                                  settings give them, marked proof:position
 *   a font that isn't installed    the font that stands in for it (ai-native-fonts.h), and
 *                                  its own name kept in proof:font-missing
 *
 * User units are points, as in the records; width and height use the file's ruler unit.
 * Type that shows and can't be set as it is (ai-native-text.h) refuses the file, and the
 * caller opens its page; such type that doesn't show is left out. A font that isn't
 * installed doesn't refuse it: as in Illustrator, another font sets the type until the
 * font is there, and the document says which fonts those are.
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
/// On a span Illustrator set as superscript or subscript: "super" or "sub". Its font size
/// and baseline shift are what that made of the size around it.
inline constexpr char const *TEXT_POSITION_ATTRIBUTE = "proof:position";

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
};

/// Where the records came from, for what they refer to outside themselves.
struct Source
{
    /// The folder of the .ai file (a file name in UTF-8), or empty. Linked pictures that
    /// aren't where the document says are looked for there, as Illustrator does.
    std::string folder;
};

/**
 * Build the document of `ai`.
 *
 * @return the document, or nothing with the reason in `error` when it holds something
 *         the import can't place: type that shows and isn't supported, a linked picture
 *         that shows and can't be found or read.
 */
std::optional<Built> build(Document const &ai, std::string &error, Source const &source = {});

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_EXTENSION_INTERNAL_AI_NATIVE_IMPORT_H
