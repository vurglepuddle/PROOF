// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: open an .ai file from Illustrator's own copy of its art.
 *
 * The records are read (ai-private-data.h, ai/ai-native-reader.h) and made into a
 * document (ai/ai-native-import.h). Then the safety net: every artboard of that
 * document is drawn and weighed against the file's PDF page for it, drawn by Poppler,
 * the page that every other application shows. Pixels are compared as seen on white,
 * by lightness and by colour, each allowed to match any pixel next to it, so that
 * edges drawn a pixel apart and the two colour conversions don't count; art that is
 * missing, extra, out of place or in the wrong colour does.
 *
 * When the records hold anything that isn't read, or the art draws differently from
 * the page, nothing is returned and the caller imports the page as before.
 */
#ifndef SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_NATIVE_OPEN_H
#define SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_NATIVE_OPEN_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

class SPDocument;

namespace Inkscape::Extension::Internal {

/// How one artboard of the built document compares with its page: the share (0-1) of its
/// pixels that look different.
struct AiPageDifference
{
    double lightness = 0.0;
    double colour = 0.0;
};

/// More of an artboard than this differing, by lightness or by colour, and the page is used.
inline constexpr double AI_NATIVE_MAX_LIGHTNESS = 0.01;
inline constexpr double AI_NATIVE_MAX_COLOUR = 0.02;

struct AiNativeOpen
{
    std::unique_ptr<SPDocument> document;
    std::vector<std::string> notes;
    std::vector<AiPageDifference> differences; ///< Each artboard compared, in order.
};

/**
 * Open the .ai at `path` (a file name in UTF-8) from its native records.
 *
 * @return the document, or nothing with the reason in `reason` when the records can't be
 *         read fully or don't draw like the page. With `keep_differing`, a document that
 *         draws differently is still returned (with its differences), for tests.
 *         A nonempty diagnostic_directory also writes the compared pictures there.
 */
std::optional<AiNativeOpen> open_ai_native(std::string const &path, std::string &reason, bool keep_differing = false,
                                         std::string const &diagnostic_directory = {});

} // namespace Inkscape::Extension::Internal

#endif // SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_NATIVE_OPEN_H
