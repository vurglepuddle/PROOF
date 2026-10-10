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
 *
 * User units are points, as in the records; width and height use the file's ruler unit.
 * Type that shows and can't be set as it is (ai-native-text.h; a font that isn't
 * installed) refuses the file, and the caller opens its page; such type that doesn't
 * show is left out.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_IMPORT_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_IMPORT_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <2geom/rect.h>

#include "ai-native-reader.h"

class SPDocument;

namespace Inkscape::Extension::Internal::AiNative {

/// On a layer that doesn't print: "false".
inline constexpr char const *LAYER_PRINT_ATTRIBUTE = "proof:layer-print";
/// On area type: its frame's outline as path data, in the text's own coordinates.
inline constexpr char const *TEXT_FRAME_ATTRIBUTE = "proof:text-frame";
/// On area type whose frame is too small for its text: the text that doesn't show.
inline constexpr char const *TEXT_OVERFLOW_ATTRIBUTE = "proof:text-overflow";

struct Built
{
    std::unique_ptr<SPDocument> document;
    /// Each artboard in the document's user units (points, y down), in the file's order.
    std::vector<Geom::Rect> artboards;
    /// What was left out or approximated, for the person opening the file.
    std::vector<std::string> notes;
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
