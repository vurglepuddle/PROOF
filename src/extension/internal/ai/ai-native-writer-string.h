// SPDX-License-Identifier: GPL-2.0-or-later
/** Name encodings: strict bytes for the standalone Illustrator 7 writer, XML ids for object names. */
#ifndef SEEN_AI_NATIVE_WRITER_STRING_H
#define SEEN_AI_NATIVE_WRITER_STRING_H

#include <format>
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

/// Whether an XML name holds a UTF-16 unit as it is (XML 1.0, fifth edition), `first` at its start.
inline bool xml_name_unit(gunichar c, bool first)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ':' || (c >= 0xC0 && c <= 0xD6) ||
        (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D) ||
        (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) ||
        (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
        (c >= 0xFDF0 && c <= 0xFFFD)) {
        return true;
    }
    return !first && ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == 0xB7 || (c >= 0x300 && c <= 0x36F) ||
                      c == 0x203F || c == 0x2040);
}

/**
 * An object's name as the XML id Illustrator keeps it by (AI10_ArtUID, read by xml_name()), as
 * Illustrator 29.2.1 writes one (its control, 2026-10-24): a space is `_`; `_` itself and what an
 * XML name can't hold there are `_xHH_`, upper-case hex without leading zeros, a character beyond
 * U+FFFF as its two UTF-16 halves. Spaces at the end go, as Illustrator drops them from a name.
 * Empty for no name.
 */
inline std::string xml_id(std::string_view name)
{
    if (!g_utf8_validate(name.data(), name.size(), nullptr)) return {};
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    std::string out;
    for (auto const *p = name.data(), *end = name.data() + name.size(); p < end; p = g_utf8_next_char(p)) {
        auto const c = g_utf8_get_char(p);
        if (c == ' ') {
            out += '_';
        } else if (c != '_' && xml_name_unit(c, out.empty())) {
            out.append(p, g_utf8_next_char(p) - p);
        } else if (c > 0xFFFF) {
            out += std::format("_x{:X}__x{:X}_", 0xD800 + ((c - 0x10000) >> 10), 0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            out += std::format("_x{:X}_", c);
        }
    }
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_STRING_H
