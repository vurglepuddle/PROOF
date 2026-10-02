// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: read Adobe Illustrator's native records from an .ai file.
 *
 * A PDF-based .ai keeps its native artwork, swatches and settings in page 1's
 * PieceInfo/Illustrator/Private dictionary as AIPrivateData1..N streams. Joined,
 * they are either plain records, "%AI12_CompressedData" + zlib (CS2..CC 2019)
 * or "%AI24_ZStandard_Data" + Zstandard (2020+). PostScript-based .ai files
 * (Illustrator 8 and older) are their own records.
 *
 * Nothing is executed: this only extracts and decompresses bytes, within limits.
 */
#ifndef SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_PRIVATE_DATA_H
#define SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_PRIVATE_DATA_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Inkscape::Extension::Internal {

struct AiNativeRecords
{
    std::string text;     ///< Decoded records (bytes; mostly ASCII PostScript-style lines).
    std::string encoding; ///< "PostScript", "plain", "AI12 zlib" or "AI24 Zstandard".
    bool stopped_early = false; ///< Decoding stopped after stop_marker.
};

/**
 * Read native records from an .ai file.
 *
 * @param stop_marker if not empty, stop decoding once this text has been produced
 *                    (e.g. "%AI5_EndPalette" when only swatches are needed).
 * @param limit       maximum number of decoded bytes.
 * @param error       receives a short reason when nothing could be read.
 */
std::optional<AiNativeRecords> read_ai_native_records(std::string const &path, std::string_view stop_marker,
                                                      std::size_t limit, std::string &error);

} // namespace Inkscape::Extension::Internal

#endif // SEEN_EXTENSION_INTERNAL_PDFINPUT_AI_PRIVATE_DATA_H
