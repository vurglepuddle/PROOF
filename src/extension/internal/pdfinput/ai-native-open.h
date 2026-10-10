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
 * missing, extra, out of place or in the wrong colour does. An artboard that differs is
 * weighed once more from larger pictures averaged down, with lines thinner than a pixel
 * kept a pixel wide in both: the two renderers draw such lines differently, and a page
 * of hairlines isn't different art.
 *
 * When the records hold anything that isn't read, or the art draws differently from
 * the page, nothing is returned and the caller imports the page as before.
 */
#ifndef SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_NATIVE_OPEN_H
#define SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_NATIVE_OPEN_H

#include <memory>
#include <optional>
#include <string>
#include <utility>
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
    /// How long each part of opening took, in seconds: "records" (decoding them), "read",
    /// "build" (the document), "compare" (drawing it and the pages).
    std::vector<std::pair<std::string, double>> timings;
};

/// Optional local test caches. The caller supplies trusted directories keyed by source
/// content and the relevant decoder/PDF renderer dependencies. Ordinary imports do not
/// use them. Damaged entries are ignored. Counters describe this call only.
struct AiNativeCache
{
    std::string records_directory;
    std::string reference_directory;
    unsigned records_reused = 0;
    unsigned references_reused = 0;
};

/**
 * Open the .ai at `path` (a file name in UTF-8) from its native records.
 *
 * @return the document, or nothing with the reason in `reason` when the records can't be
 *         read fully or don't draw like the page. With `keep_differing`, a document that
 *         draws differently is still returned (with its differences), for tests.
 *         A nonempty diagnostic_directory also writes the compared pictures there.
 *         cache is for the content-addressed development runner; it is off by default.
 */
std::optional<AiNativeOpen> open_ai_native(std::string const &path, std::string &reason, bool keep_differing = false,
                                         std::string const &diagnostic_directory = {}, AiNativeCache *cache = nullptr);

} // namespace Inkscape::Extension::Internal

#endif // SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_NATIVE_OPEN_H
