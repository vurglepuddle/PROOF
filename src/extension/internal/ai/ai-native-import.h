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
 *   embedded image                 image (PNG for the screen); CMYK samples kept in
 *                                  proof:samples as a CMYK TIFF
 *   name, hidden, locked           label, display:none, sodipodi:insensitive
 *   transparency                   opacity, mix-blend-mode, isolation; knockout in proof:knockout
 *   turned box (BBAccumRotation)   proof:box-angle on paths
 *   drawn look                     group of what it draws, marked proof:drawn-look
 *   artboards                      pages; the first artboard's top-left corner is 0,0
 *   guides                         guides
 *
 * User units are points, as in the records; width and height use the file's ruler unit.
 * Text objects aren't set yet: a file with type that shows is refused (the caller
 * opens its page), and type that doesn't show is left out.
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

struct Built
{
    std::unique_ptr<SPDocument> document;
    /// Each artboard in the document's user units (points, y down), in the file's order.
    std::vector<Geom::Rect> artboards;
    /// What was left out or approximated, for the person opening the file.
    std::vector<std::string> notes;
};

/**
 * Build the document of `ai`.
 *
 * @return the document, or nothing with the reason in `error` when it holds something
 *         the import can't place yet (type that shows).
 */
std::optional<Built> build(Document const &ai, std::string &error);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_EXTENSION_INTERNAL_AI_NATIVE_IMPORT_H
