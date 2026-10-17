// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Illustrator's own records for a PROOF document: the native half of an .ai file.
 *
 * Writes the subset Illustrator 29.2.1 has been seen to open as editable art
 * (doc/PROOF-ai-interchange.md, "Native writer"): artboards from the document's pages,
 * layers with their options, groups, paths and compound paths, clipping groups, process
 * colour in CMYK, RGB or grey, spot inks with their tints, overprint and stroke style.
 * Anything else in the document is named in `refused`, and then no records are written:
 * nothing goes to Illustrator that it would draw differently.
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

WrittenRecords write_records(SPDocument *document);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_H
