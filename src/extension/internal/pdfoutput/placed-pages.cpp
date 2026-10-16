// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: placed PDF pages into an exported PDF, as they are. See placed-pages.h.
 */

#include "placed-pages.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <glib/gstdio.h>
#include <zlib.h>

#ifdef WITH_POPPLER
#include <GlobalParams.h>
#include <Object.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Stream.h>
#include <XRef.h>

#include "extension/internal/pdfinput/poppler-transition-api.h"
#endif

namespace Inkscape::Extension::Internal::PdfBuilder {

#ifdef WITH_POPPLER
namespace {

/// The most objects copied from one placed file.
constexpr std::size_t MAX_OBJECTS = 1'000'000;

/// What is appended to the file, kept here until it is whole. Positions count on from the
/// file's end.
class Appended : public OutStream
{
public:
    explicit Appended(Goffset start)
        : _start(start)
    {}
    void close() override {}
    Goffset getPos() override { return _start + Goffset(text.size()); }
    void put(char c) override { text += c; }
    size_t write(std::span<unsigned char const> data) override
    {
        text.append(reinterpret_cast<char const *>(data.data()), data.size());
        return data.size();
    }
    void printf(char const *format, ...) override
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
    Appended &operator<<(std::string_view s)
    {
        text += s;
        return *this;
    }

    std::string text;

private:
    Goffset _start;
};

std::string number(double v)
{
    char buf[40];
    g_ascii_formatd(buf, sizeof buf, "%.10g", v);
    return buf;
}

/// Every object `root` refers to in `xref`, and those they refer to, in the order found. A page
/// tree's parent isn't followed. False when there are too many.
bool reach(Object const &root, XRef *xref, std::set<std::pair<int, int>> &seen, std::vector<Ref> &found)
{
    std::vector<Object> todo;
    todo.push_back(root.copy());
    while (!todo.empty()) {
        Object o = std::move(todo.back());
        todo.pop_back();
        if (o.isRef()) {
            auto const r = o.getRef();
            if (!seen.insert({r.num, r.gen}).second) continue;
            if (seen.size() > MAX_OBJECTS) return false;
            found.push_back(r);
            todo.push_back(xref->fetch(r));
        } else if (o.isArray()) {
            for (int i = 0; i < o.arrayGetLength(); ++i) todo.push_back(o.getArray()->getNF(i).copy());
        } else if (Dict *d = o.isDict() ? o.getDict() : o.isStream() ? o.getStream()->getDict() : nullptr) {
            for (int i = 0; i < d->getLength(); ++i) {
                if (std::string_view(d->getKey(i)) != "Parent") todo.push_back(d->getValNF(i).copy());
            }
        }
    }
    return true;
}

/// A stream's bytes, decoded.
std::string decoded(Object &o)
{
    std::string text;
    if (auto *s = o.isStream() ? o.getStream() : nullptr) {
        s->fillString(text);
        s->close();
    }
    return text;
}

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

// Poppler keeps its own object header and footer to itself.
void begin_object(Appended &text, Ref const &r)
{
    text << std::to_string(r.num) << " " << std::to_string(r.gen) << " obj\n";
}

void end_object(Appended &text)
{
    text << "\nendobj\n";
}

struct Source
{
    std::shared_ptr<PDFDoc> pdf;
    unsigned offset = 0; ///< Added to the number of every object copied from it.
};

} // namespace

bool splice_placed_pages(std::string const &path, std::vector<PlacedPage> const &pages, std::string &error)
{
    error.clear();
    if (pages.empty()) return true;
    if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    auto out = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
    if (!out || !out->isOk()) {
        error = "the PDF just written can't be read back";
        return false;
    }
    XRef *xref = out->getXRef();

    // The stand-ins, by their marker.
    std::map<std::string, Ref> stand_ins;
    for (int num = 1; num < xref->getNumObjects(); ++num) {
        auto *entry = xref->getEntry(num, false);
        if (!entry || entry->type != xrefEntryUncompressed) continue;
        Object o = xref->fetch(num, entry->gen);
        if (!o.isStream() || !o.getStream()->getDict()->lookup("Subtype").isName("Form")) continue;
        auto const length = o.getStream()->getDict()->lookup("Length");
        if (length.isInt() && length.getInt() > 4096) continue;
        auto const content = decoded(o);
        for (auto const &p : pages) {
            if (content.find(p.marker) != std::string::npos) stand_ins[p.marker] = {num, entry->gen};
        }
    }
    for (auto const &p : pages) {
        if (!stand_ins.count(p.marker)) {
            error = "the place of a placed page wasn't found in the PDF";
            return false;
        }
    }

    GStatBuf st;
    if (g_stat(path.c_str(), &st) != 0) {
        error = "the PDF just written can't be found";
        return false;
    }
    Goffset const size = st.st_size;
    Goffset const prev = last_xref(path, size);
    if (prev < 0) {
        error = "the PDF just written has no cross-references";
        return false;
    }

    Appended text(size);
    text << "\n";
    // Object number, generation, where it is.
    std::vector<std::tuple<int, int, Goffset>> entries;
    int next = xref->getNumObjects();

    // Each placed file's objects, renumbered past what is there.
    std::map<std::string, Source> sources;
    for (auto const &p : pages) {
        if (sources.count(p.file)) continue;
        auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(p.file.c_str());
        if (!pdf || !pdf->isOk() || pdf->getNumPages() < 1 || !pdf->getPage(1)) {
            error = "a placed file can't be read: " + p.file;
            return false;
        }
        auto *page = pdf->getPage(1);
        std::set<std::pair<int, int>> seen;
        std::vector<Ref> found;
        bool ok = reach(*page->getResourceDictObject(), pdf->getXRef(), seen, found);
        if (auto *group = page->getGroup()) {
            for (int i = 0; ok && i < group->getLength(); ++i) ok = reach(group->getValNF(i), pdf->getXRef(), seen, found);
        }
        if (!ok) {
            error = "a placed page uses too many objects: " + p.file;
            return false;
        }
        Source source{pdf, unsigned(next - 1)};
        int last = next - 1;
        for (auto const &r : found) {
            Object o = pdf->getXRef()->fetch(r);
            Ref const to{int(r.num + source.offset), r.gen};
            entries.emplace_back(to.num, to.gen, text.getPos());
            begin_object(text, to);
            PDFDoc::writeObject(&o, &text, pdf->getXRef(), source.offset, nullptr, cryptRC4, 0, to, nullptr);
            end_object(text);
            last = std::max(last, to.num);
        }
        next = last + 1;
        sources.emplace(p.file, std::move(source));
    }

    // Each stand-in, now its page.
    for (auto const &p : pages) {
        auto const &source = sources.at(p.file);
        auto *page = source.pdf->getPage(1);
        auto *from = source.pdf->getXRef();
        Ref const at = stand_ins.at(p.marker);
        std::string content;
        Object contents = page->getContents();
        if (contents.isArray()) {
            for (int i = 0; i < contents.arrayGetLength(); ++i) {
                Object part = contents.arrayGet(i);
                content += decoded(part);
                content += "\n";
            }
        } else {
            content = decoded(contents);
        }
        uLongf packed_size = compressBound(uLong(content.size()));
        std::string packed(packed_size, '\0');
        if (compress2(reinterpret_cast<Bytef *>(packed.data()), &packed_size,
                      reinterpret_cast<Bytef const *>(content.data()), uLong(content.size()), 6) != Z_OK) {
            error = "a placed page's content can't be compressed";
            return false;
        }
        packed.resize(packed_size);
        auto const &crop = page->getCropBox();
        entries.emplace_back(at.num, at.gen, text.getPos());
        begin_object(text, at);
        text << "<< /Type /XObject /Subtype /Form /BBox [" << number(crop.x1) << " " << number(crop.y1) << " "
             << number(crop.x2) << " " << number(crop.y2) << "] /Resources ";
        if (auto *resources = page->getResourceDictObject(); resources && resources->isDict()) {
            PDFDoc::writeObject(resources, &text, from, source.offset, nullptr, cryptRC4, 0, at, nullptr);
        } else {
            text << "<< >>";
        }
        if (auto *group = page->getGroup()) {
            text << " /Group ";
            Object copy(group->copy(from));
            PDFDoc::writeObject(&copy, &text, from, source.offset, nullptr, cryptRC4, 0, at, nullptr);
        }
        text << " /Filter /FlateDecode /Length " << std::to_string(packed.size()) << " >>\nstream\n" << packed << "\nendstream";
        end_object(text);
    }

    // The update's cross-references, of the kind the file has, and its trailer.
    Object *trailer = xref->getTrailerDict();
    auto trailer_entry = [&](char const *key) {
        Object o = trailer->dictLookupNF(key).copy();
        if (o.isNull()) return;
        text << " /" << key << " ";
        PDFDoc::writeObject(&o, &text, xref, 0, nullptr, cryptRC4, 0, Ref{0, 0}, nullptr);
    };
    bool const stream = xref->isXRefStream();
    Goffset const at = text.getPos();
    if (stream) entries.emplace_back(next, 0, at);
    std::sort(entries.begin(), entries.end());
    int const objects = std::max(xref->getNumObjects(), std::get<0>(entries.back()) + 1);
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
        text << "] /Prev " << std::to_string(prev);
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
        text << "trailer\n<< /Size " << std::to_string(objects) << " /Prev " << std::to_string(prev);
        trailer_entry("Root");
        trailer_entry("Info");
        trailer_entry("ID");
        text << " >>\n";
    }
    text << "startxref\n" << std::to_string(at) << "\n%%EOF\n";

    // Only now is the file touched.
    out.reset();
    sources.clear();
    FILE *f = g_fopen(path.c_str(), "ab");
    bool const written = f && std::fwrite(text.text.data(), 1, text.text.size(), f) == text.text.size();
    if (f && std::fclose(f) != 0) {
        error = "the placed pages couldn't be written into the PDF";
        return false;
    }
    if (!written) {
        error = "the placed pages couldn't be written into the PDF";
        return false;
    }
    return true;
}
#else
bool splice_placed_pages(std::string const &, std::vector<PlacedPage> const &pages, std::string &error)
{
    if (pages.empty()) return true;
    error = "this build can't put placed pages into a PDF";
    return false;
}
#endif

} // namespace Inkscape::Extension::Internal::PdfBuilder
