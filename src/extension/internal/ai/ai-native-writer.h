// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Illustrator's own records for a PROOF document: the native half of an .ai file.
 *
 * Writes editable records for the subset in doc/PROOF-ai-interchange.md ("Native writer");
 * that document records each feature's Illustrator qualification. Supports document artboards,
 * layers with their options, groups, paths and compound paths with corner/smooth flags,
 * clipping groups, process colour in CMYK, RGB or grey, spot inks with their tints (defined
 * in CMYK; in the modern form in Lab or RGB too, ai-native-writer-ink.h), and canonical CMYK
 * Registration in the modern form (reserved document ink All),
 * opaque pad linear and radial process fills and strokes, overprint and stroke style. A
 * gradient-filled shape may have a plain stroke; the modern form also supports gradient
 * strokes with a plain or gradient fill. Gradient stops may be tints of those spot inks, and
 * in the modern form have opacities of their own; mixed RGB/CMYK gradients are
 * refused. The modern form writes an object's and a group's transparency as Illustrator does
 * (`mode opacity isolate knockout shape Xy`; a group's after its end, with `0 0 Xd` and
 * `6 () XW`, or `9 () XW` when it has none): opacity, the sixteen blend modes, isolated
 * blending and knockout, and a fill's or stroke's own opacity when it is the only paint.
 * Transparency on layers and the document, and separate fill and stroke opacities, are
 * refused. The modern form writes patterns on fills and strokes as Illustrator 9 and later do
 * (`%AI3_BeginPattern`, the tile's art on hidden lines, `(name) 0 0 1 1 0 0 0 0 0 [matrix] p`):
 * tiles in the user space of what they paint, with any x, y, width, height and pattern
 * transform; patterns sized to their object, with a view box or inside other patterns, and
 * gradients and pictures in a tile, are refused. The modern form writes opacity masks as
 * Illustrator does, after the object they mask (`/Mask :` in its ArtDictionary on hidden lines,
 * linked, not inverted, the art between `X=` and `X+`): their art is what the body supports, in
 * greys written as Gray (a gradient's stops in an RGB document as neutral RGB), with Normal or
 * Multiply, nested masks too; Clip on, or off where the
 * import's white backdrop covers what the object paints. The import's wrapper about a masked
 * object is written as the object. Colours other than grey, spot inks, patterns and pictures in a
 * mask, and masks sized to their object, on layers, clones or clipping paths, are refused.
 * Pictures the document holds itself are embedded
 * rasters (ai-native-writer-picture.h); readable local PNG/JPEG/classic TIFF/PSD pictures stay
 * linked. Missing links and linked formats outside that subset are refused.
 * Picture slice crops retain the complete raster or link behind an editable viewport clip.
 * The modern form writes point type as Illustrator's stories (ai-native-writer-text.h).
 * Auto/symmetric points become smooth; Illustrator chooses an open path's first-point
 * type because the initial move has no separate flag.
 * Anything else in the document is named in `refused`, and then no records are written:
 * nothing goes to Illustrator that it would draw differently.
 * The optional Illustrator 7 form shares that subset, with a single artboard, strict
 * Windows-1252 names, opaque embedded rasters, and additional refusals where that old format
 * has no supported encoding. A colour-model comment advises newer Illustrator's legacy
 * converter; version 7 ignores that later metadata.
 * Registration is refused in Illustrator 7 until its encoding is qualified.
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
