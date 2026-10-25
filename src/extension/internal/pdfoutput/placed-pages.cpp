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
#include "pdf-update.h"
#endif

namespace Inkscape::Extension::Internal::PdfBuilder {

#ifdef WITH_POPPLER
namespace {

/// The most objects copied from one placed file.
constexpr std::size_t MAX_OBJECTS = 1'000'000;

std::string number(double v)
{
    char buf[40];
    g_ascii_formatd(buf, sizeof buf, "%.10g", v);
    return buf;
}

/// Every object `root` refers to in `xref`, and those they refer to, in the order found. A page
/// tree's parent isn't followed. False when there are too many.
bool reach(Object const &root, XRef *xref, std::set<std::pair<int, int>> &seen, std::vector<Ref> &found,
           std::string *incompatible = nullptr)
{
    std::vector<Object> todo;
    todo.push_back(root.copy());
    while (!todo.empty()) {
        Object o = std::move(todo.back());
        todo.pop_back();
        // Poppler writes UTF-8 BOM strings as UTF-16BE, which PDF 1.7 supports.
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
                std::string_view const key = d->getKey(i);
                if (incompatible && (key == "UseBlackPtComp" || key == "AF" || key == "AFRelationship")) {
                    *incompatible = "/" + std::string(key);
                    return false;
                }
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

struct Source
{
    std::shared_ptr<PDFDoc> pdf;
    unsigned offset = 0; ///< Added to the number of every object copied from it.
};

} // namespace

bool placed_pages_pdf17_compatible(std::vector<PlacedPage> const &pages, std::string &error)
{
    error.clear();
    if (pages.empty()) return true;
    if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    std::map<std::string, std::shared_ptr<PDFDoc>> sources;
    std::map<std::string, std::set<std::pair<int, int>>> visited;
    for (auto const &p : pages) {
        auto &pdf = sources[p.file];
        if (!pdf) pdf = _POPPLER_MAKE_SHARED_PDFDOC(p.file.c_str());
        if (!pdf || !pdf->isOk() || p.index < 1 || p.index > pdf->getNumPages()) {
            error = "a placed file's selected page can't be read: " + p.file;
            return false;
        }
        auto page = pdf->getPage(p.index);
        std::vector<Ref> found;
        std::string incompatible;
        auto check = [&](Object const &object) {
            return reach(object, pdf->getXRef(), visited[p.file], found, &incompatible);
        };
        bool ok = true;
        if (auto resources = page->getResourceDictObject()) ok = check(*resources);
        if (auto group = page->getGroup(); ok && group) {
            Object copy(group->copy(pdf->getXRef()));
            ok = check(copy);
        }
        if (!ok) {
            error = incompatible.empty() ? "a placed page uses too many objects: " + p.file
                                        : "a placed PDF uses PDF 2.0 features: " + incompatible;
            return false;
        }
    }
    return true;
}

bool splice_placed_pages(std::string const &path, std::vector<PlacedPage> const &pages, std::string &error, bool pdf17)
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

    PdfUpdate update(path, xref);
    if (!update.ok()) {
        error = update.error();
        return false;
    }
    auto &text = update.text();
    int next = xref->getNumObjects();

    // Each placed file's objects, renumbered past what is there.
    std::map<std::string, Source> sources;
    for (auto const &p : pages) {
        if (sources.count(p.file)) continue;
        auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(p.file.c_str());
        if (!pdf || !pdf->isOk() || pdf->getNumPages() < 1) {
            error = "a placed file can't be read: " + p.file;
            return false;
        }
        std::set<std::pair<int, int>> seen;
        std::vector<Ref> found;
        bool ok = true;
        std::string incompatible;
        // A file may supply several different pages. Copy the union of their resources,
        // once, with one renumbering offset for the file, including shared fonts/pictures.
        for (auto const &selected : pages) {
            if (selected.file != p.file) continue;
            auto *page = selected.index >= 1 && selected.index <= pdf->getNumPages()
                             ? pdf->getPage(selected.index) : nullptr;
            if (!page || page->getRotate() % 360 != 0) {
                error = "a placed file's selected page is missing or turned: " + p.file;
                return false;
            }
            if (auto *resources = page->getResourceDictObject()) {
                ok = ok && reach(*resources, pdf->getXRef(), seen, found, pdf17 ? &incompatible : nullptr);
            }
            if (auto *group = page->getGroup()) {
                Object copy(group->copy(pdf->getXRef()));
                ok = ok && reach(copy, pdf->getXRef(), seen, found, pdf17 ? &incompatible : nullptr);
            }
        }
        if (!ok) {
            error = incompatible.empty() ? "a placed page uses too many objects: " + p.file
                                        : "a placed PDF uses PDF 2.0 features: " + incompatible;
            return false;
        }
        Source source{pdf, unsigned(next - 1)};
        int last = next - 1;
        for (auto const &r : found) {
            Object o = pdf->getXRef()->fetch(r);
            Ref const to{int(r.num + source.offset), r.gen};
            update.begin(to);
            PDFDoc::writeObject(&o, &text, pdf->getXRef(), source.offset, nullptr, cryptRC4, 0, to, nullptr);
            update.end();
            last = std::max(last, to.num);
        }
        next = last + 1;
        sources.emplace(p.file, std::move(source));
    }

    // Each stand-in, now its page.
    for (auto const &p : pages) {
        auto const &source = sources.at(p.file);
        auto *page = source.pdf->getPage(p.index);
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
        update.begin(at);
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
        update.end();
    }

    update.finish(next);
    out.reset();
    sources.clear();
    if (!update.write()) {
        error = "the placed pages couldn't be written into the PDF";
        return false;
    }
    return true;
}
#else
bool placed_pages_pdf17_compatible(std::vector<PlacedPage> const &pages, std::string &error)
{
    if (pages.empty()) return true;
    error = "this build can't put placed pages into a PDF";
    return false;
}

bool splice_placed_pages(std::string const &, std::vector<PlacedPage> const &pages, std::string &error, bool)
{
    if (pages.empty()) return true;
    error = "this build can't put placed pages into a PDF";
    return false;
}
#endif

} // namespace Inkscape::Extension::Internal::PdfBuilder
