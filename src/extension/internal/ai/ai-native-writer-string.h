// SPDX-License-Identifier: GPL-2.0-or-later
/** Strict name encoding for the standalone Illustrator 7 writer. */
#ifndef SEEN_AI_NATIVE_WRITER_STRING_H
#define SEEN_AI_NATIVE_WRITER_STRING_H

#include <optional>
#include <string>
#include <string_view>

#include <glib.h>

namespace Inkscape::Extension::Internal::AiNative {

/// No transliteration or replacement: an unrepresentable name must refuse the save.
inline std::optional<std::string> legacy_name_bytes(std::string_view utf8)
{
    if (utf8.empty()) return std::string{};
    if (!g_utf8_validate(utf8.data(), utf8.size(), nullptr)) return {};
    gsize written = 0;
    auto *bytes = g_convert(utf8.data(), utf8.size(), "WINDOWS-1252", "UTF-8", nullptr, &written, nullptr);
    if (!bytes) return {};
    std::string out(bytes, written);
    g_free(bytes);
    // These five byte values have no character in Windows-1252, even if an iconv
    // implementation permits the corresponding C1 control to pass through.
    if (out.find_first_of("\x81\x8d\x8f\x90\x9d") != std::string::npos) return {};
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_STRING_H
