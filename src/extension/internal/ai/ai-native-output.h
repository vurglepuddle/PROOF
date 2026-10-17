// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Saving as Illustrator's own .ai: PROOF's PDF of the document, with Illustrator's records for it
 * (ai-native-writer.h) in the page's private data, as Illustrator 29 writes its files.
 *
 * Illustrator reads only the records and ignores the page; every other program reads only the
 * page. Both are made from the same document, at the same save: Illustrator doesn't notice
 * records older than their page (doc/PROOF-ai-interchange.md). A document with anything the
 * writer can't write yet isn't saved, and the message names what.
 */

#ifndef SEEN_AI_NATIVE_OUTPUT_H
#define SEEN_AI_NATIVE_OUTPUT_H

#include <string>

#include "extension/implementation/implementation.h"

namespace Inkscape::Extension::Internal {

namespace AiNative {

/**
 * Illustrator's private data for `records`, added to the PDF at `path` in an update appended to
 * it: the header comments as `AIMetaData`, the records Zstandard-compressed after
 * `%AI24_ZStandard_Data` in `AIPrivateData` blocks of 64 KiB, under `/PieceInfo /Illustrator` on
 * every page, all pages naming one copy. The update is prepared before appending; use a
 * temporary PDF so a write failure cannot damage a previous save. `error` says why it failed.
 */
bool add_private_data(std::string const &path, std::string const &records, std::string &error);

} // namespace AiNative

class AiNativeOutput : public Implementation::Implementation
{
public:
    bool check(Inkscape::Extension::Extension *module) override;
    void save(Inkscape::Extension::Output *mod, SPDocument *doc, char const *filename) override;
    static void init();
};

} // namespace Inkscape::Extension::Internal

#endif // SEEN_AI_NATIVE_OUTPUT_H
