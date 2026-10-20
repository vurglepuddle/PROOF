// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Illustrator's own records for a PROOF document: the native half of an .ai file.
 *
 * Writes the subset Illustrator 29.2.1 has been seen to open as editable art
 * (doc/PROOF-ai-interchange.md, "Native writer"): artboards from the document's pages,
 * layers with their options, groups, paths and compound paths with corner/smooth flags,
 * clipping groups, process colour in CMYK, RGB or grey, spot inks with their tints,
 * opaque pad linear and radial process fills and strokes, overprint and stroke style. A
 * gradient-filled shape may have a plain stroke; the modern form also supports gradient
 * strokes with a plain or gradient fill. Gradient stops may be tints of spot inks defined in
 * CMYK; transparent and mixed RGB/CMYK gradients are refused. Pictures the document holds itself are embedded
 * rasters (ai-native-writer-picture.h); readable local PNG/JPEG/classic TIFF/PSD pictures stay
 * linked. Missing links and linked formats outside that subset are refused.
 * Auto/symmetric points become smooth; Illustrator chooses an open path's first-point
 * type because the initial move has no separate flag.
 * Anything else in the document is named in `refused`, and then no records are written:
 * nothing goes to Illustrator that it would draw differently.
 * The optional Illustrator 7 form shares that subset, with a single artboard, ASCII layer
 * and ink names, opaque embedded rasters, and additional refusals where that old format
 * has no supported encoding. A colour-model comment advises newer Illustrator's legacy
 * converter; version 7 ignores that later metadata.
 */

#ifndef SEEN_AI_NATIVE_WRITER_H
#define SEEN_AI_NATIVE_WRITER_H

#include <string>
#include <vector>

class SPDocument;

namespace Inkscape::Extension::Internal::AiNative {

struct WrittenRecords
{
    std::string records;              ///< The records' text, uncompressed; empty when refused.
    std::vector<std::string> refused; ///< What the document has that isn't written yet.
};

enum class RecordsVersion
{
    Modern,       ///< Records for the PDF-compatible container, with named artboards.
    Illustrator7, ///< Standalone records for a single artboard; no PDF representation.
};

WrittenRecords write_records(SPDocument *document, RecordsVersion version = RecordsVersion::Modern);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_H
