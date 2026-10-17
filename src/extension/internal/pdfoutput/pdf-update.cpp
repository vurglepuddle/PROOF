// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: objects added to a PDF just written, in an update appended to it. See pdf-update.h.
 */

#include "pdf-update.h"

#ifdef WITH_POPPLER
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <glib/gstdio.h>
#include <PDFDoc.h>
#include <XRef.h>

namespace Inkscape::Extension::Internal::PdfBuilder {

size_t Appended::write(std::span<unsigned char const> data)
{
    text.append(reinterpret_cast<char const *>(data.data()), data.size());
    return data.size();
}

void Appended::printf(char const *format, ...)
{
    va_list args, again;
    va_start(args, format);
    va_copy(again, args);
    int const n = std::vsnprintf(nullptr, 0, format, args);
    va_end(args);
    if (n > 0) {
        std::string chunk(std::size_t(n) + 1, '\0');
        std::vsnprintf(chunk.data(), chunk.size(), format, again);
        text.append(chunk.data(), std::size_t(n));
    }
    va_end(again);
}

namespace {

/// Where the file's last cross-references begin (its "startxref"), or -1.
Goffset last_xref(std::string const &path, Goffset size)
{
    FILE *f = g_fopen(path.c_str(), "rb");
    if (!f) return -1;
    Goffset const from = std::max<Goffset>(0, size - 2048);
    std::string tail(std::size_t(size - from), '\0');
#ifdef _WIN32
    bool const sought = _fseeki64(f, from, SEEK_SET) == 0;
#else
    bool const sought = fseeko(f, from, SEEK_SET) == 0;
#endif
    bool const read = sought && std::fread(tail.data(), 1, tail.size(), f) == tail.size();
    std::fclose(f);
    auto const at = read ? tail.rfind("startxref") : std::string::npos;
    if (at == std::string::npos) return -1;
    auto const digits = tail.find_first_of("0123456789", at + 9);
    if (digits == std::string::npos) return -1;
    return std::strtoll(tail.c_str() + digits, nullptr, 10);
}

Goffset size_of(std::string const &path)
{
    GStatBuf st;
    return g_stat(path.c_str(), &st) == 0 ? Goffset(st.st_size) : -1;
}

} // namespace

PdfUpdate::PdfUpdate(std::string path, XRef *xref)
    : _path(std::move(path))
    , _xref(xref)
    , _size(size_of(_path))
    , _text(std::max<Goffset>(0, _size))
{
    if (_size < 0) {
        _error = "the PDF just written can't be found";
        return;
    }
    _prev = last_xref(_path, _size);
    if (_prev < 0) {
        _error = "the PDF just written has no cross-references";
        return;
    }
    _text << "\n";
}

// Poppler keeps its own object header and footer to itself.
void PdfUpdate::begin(Ref const &r)
{
    _entries.emplace_back(r.num, r.gen, _text.getPos());
    _text << std::to_string(r.num) << " " << std::to_string(r.gen) << " obj\n";
}

void PdfUpdate::end()
{
    _text << "\nendobj\n";
}

void PdfUpdate::finish(int next)
{
    auto &text = _text;
    auto &entries = _entries;
    Object *trailer = _xref->getTrailerDict();
    auto trailer_entry = [&](char const *key) {
        Object o = trailer->dictLookupNF(key).copy();
        if (o.isNull()) return;
        text << " /" << key << " ";
        PDFDoc::writeObject(&o, &text, _xref, 0, nullptr, cryptRC4, 0, Ref{0, 0}, nullptr);
    };
    bool const stream = _xref->isXRefStream();
    Goffset const at = text.getPos();
    if (stream) entries.emplace_back(next, 0, at);
    std::sort(entries.begin(), entries.end());
    int objects = _xref->getNumObjects();
    if (!entries.empty()) objects = std::max(objects, std::get<0>(entries.back()) + 1);
    // Runs of consecutive numbers.
    std::vector<std::pair<int, int>> runs;
    for (auto const &[num, gen, where] : entries) {
        if (!runs.empty() && runs.back().first + runs.back().second == num) {
            ++runs.back().second;
        } else {
            runs.emplace_back(num, 1);
        }
    }
    if (stream) {
        std::string rows;
        for (auto const &[num, gen, where] : entries) {
            rows += char(1);
            for (int b = 7; b >= 0; --b) rows += char((std::uint64_t(where) >> (8 * b)) & 0xff);
            rows += char((gen >> 8) & 0xff);
            rows += char(gen & 0xff);
        }
        text << std::to_string(next) << " 0 obj\n<< /Type /XRef /Size " << std::to_string(objects) << " /W [1 8 2] /Index [";
        for (auto const &[first, count] : runs) text << std::to_string(first) << " " << std::to_string(count) << " ";
        text << "] /Prev " << std::to_string(_prev);
        trailer_entry("Root");
        trailer_entry("Info");
        trailer_entry("ID");
        text << " /Length " << std::to_string(rows.size()) << " >>\nstream\n" << rows << "\nendstream\nendobj\n";
    } else {
        text << "xref\n";
        std::size_t i = 0;
        for (auto const &[first, count] : runs) {
            text << std::to_string(first) << " " << std::to_string(count) << "\n";
            for (int k = 0; k < count; ++k, ++i) {
                char line[24];
                std::snprintf(line, sizeof line, "%010lld %05d n\r\n", static_cast<long long>(std::get<2>(entries[i])),
                              std::get<1>(entries[i]));
                text << line;
            }
        }
        text << "trailer\n<< /Size " << std::to_string(objects) << " /Prev " << std::to_string(_prev);
        trailer_entry("Root");
        trailer_entry("Info");
        trailer_entry("ID");
        text << " >>\n";
    }
    text << "startxref\n" << std::to_string(at) << "\n%%EOF\n";
}

bool PdfUpdate::write()
{
    FILE *f = g_fopen(_path.c_str(), "ab");
    bool const written = f && std::fwrite(_text.text.data(), 1, _text.text.size(), f) == _text.text.size();
    bool const closed = f && std::fclose(f) == 0;
    return written && closed;
}

} // namespace Inkscape::Extension::Internal::PdfBuilder

#endif // WITH_POPPLER
