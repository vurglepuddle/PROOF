// SPDX-License-Identifier: GPL-2.0-or-later
#include "rgb-overprint.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

#include <Dict.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Parser.h>
#include <Stream.h>
#include <XRef.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "poppler-transition-api.h"
#include "util/scope_exit.h"

namespace Inkscape::Extension::Internal {
namespace {

constexpr std::size_t CONTENT_LIMIT = 8u << 20;

bool bounded_contents(Object &contents, bool require_pairs, std::size_t *remaining = nullptr)
{
    std::string bytes;
    auto append = [&](Object &object) {
        if (!object.isStream()) return false;
        auto *stream = object.getStream();
#if POPPLER_CHECK_VERSION(26, 0, 0)
        if (!stream->rewind()) return false;
#else
        stream->reset();
#endif
        for (int c; (c = stream->getChar()) != EOF;) {
            if (bytes.size() == CONTENT_LIMIT || (remaining && !*remaining)) { stream->close(); return false; }
            if (remaining) --*remaining;
            bytes += char(c);
        }
        stream->close();
        return true;
    };
    if (contents.isStream()) {
        if (!append(contents)) return false;
    } else if (contents.isArray() && contents.arrayGetLength() <= 256) {
        for (int i = 0; i < contents.arrayGetLength(); ++i) {
            auto stream = contents.arrayGet(i);
            if (!append(stream)) return false;
        }
    } else return false;
    // Only a fast exclusion: the parser below distinguishes names from strings/comments.
    return !require_pairs || bytes.find("MultiChannelPaint") != bytes.npos;
}

bool full_state(Object const &s, bool overprint)
{
    if (!s.isDict() || !s.dictLookup("BM").isName("Normal")) return false;
    for (auto key : {"op", "OP"}) {
        auto v = s.dictLookup(key);
        if (!v.isBool() || v.getBool() != overprint) return false;
    }
    auto mode = s.dictLookup("OPM");
    if (!mode.isInt() || mode.getInt() != 1) return false;
    for (auto key : {"ca", "CA"}) {
        auto v = s.dictLookup(key);
        if (!v.isNum() || v.getNum() != 1) return false;
    }
    for (int i = 0; i < s.dictGetLength(); ++i) {
        std::string_view key(s.getDict()->getKey(i));
        auto v = s.dictGetVal(i);
        if (key == "Type" && v.isName("ExtGState")) continue;
        if (key == "BM" || key == "op" || key == "OP" || key == "OPM" || key == "ca" || key == "CA") continue;
        if (key == "SA" && v.isBool()) continue;
        if (key == "AIS" && v.isBool() && !v.getBool()) continue;
        if (key == "SMask" && (v.isName("None") || (v.isDict() && v.dictLookup("S").isName("Luminosity") &&
                                                    v.dictLookup("G").isStream()))) continue;
        return false;
    }
    return !s.dictLookup("SMask").isNull();
}

/// Leaf Forms may paint paths, shadings and ordinary images. Collect inherited gs names so
/// the page never changes a state that a Form also uses. Own state dictionaries stay original.
/// Text/patterns, nested Forms, inline images and mask-bearing states need a wider resource walk.
std::optional<std::set<std::string>> leaf_form_states(Object &form, Object const &page_states,
                                                     Object const &page_images, XRef *xref,
                                                     std::size_t &bytes_left, unsigned &tokens_left)
{
    if (!bounded_contents(form, false, &bytes_left)) return {};
    auto resources = form.getStream()->getDict()->lookup("Resources");
    if (!resources.isNull() && !resources.isDict()) return {};
    auto lookup = [&](char const *kind, char const *name, Object const &fallback) {
        if (resources.isDict()) {
            auto local = resources.dictLookup(kind);
            if (local.isDict()) {
                auto object = local.dictLookup(name);
                if (!object.isNull()) return object;
            } else if (!local.isNull()) return local;
        }
        return fallback.isDict() ? fallback.dictLookup(name) : fallback.copy();
    };
    static std::set<std::string_view> const allowed = {
        "q", "Q", "cm", "w", "J", "j", "M", "d", "ri", "i",
        "m", "l", "c", "v", "y", "h", "re", "W", "W*", "n",
        "S", "s", "f", "F", "f*", "B", "B*", "b", "b*",
        "g", "G", "rg", "RG", "k", "K", "sh", "gs", "Do", "BX", "EX"
    };
    ::Parser parser(xref, &form, false);
    std::vector<Object> args;
    std::set<std::string> inherited;
    unsigned saves = 0;
    for (; tokens_left; --tokens_left) {
        auto object = parser.getObj();
        if (object.isEOF()) {
            if (!args.empty() || saves) return {};
            return inherited;
        }
        if (object.isError()) return {};
        if (!object.isCmd()) {
            if (args.size() == 64) return {};
            args.push_back(std::move(object));
            continue;
        }
        std::string_view op(object.getCmd());
        if (!allowed.count(op)) return {};
        if (op == "q") {
            if (!args.empty() || saves == 256) return {};
            ++saves;
        } else if (op == "Q") {
            if (!args.empty() || !saves) return {};
            --saves;
        } else if (op == "gs") {
            if (args.size() != 1 || !args[0].isName()) return {};
            auto const *name = args[0].getName();
            auto state = lookup("ExtGState", name, page_states);
            if ((!full_state(state, false) && !full_state(state, true)) ||
                !state.dictLookup("SMask").isName("None")) return {};
            auto local = resources.isDict() ? resources.dictLookup("ExtGState") : resources.copy();
            if (!local.isDict() || local.dictLookup(name).isNull()) inherited.insert(name);
        } else if (op == "Do") {
            if (args.size() != 1 || !args[0].isName()) return {};
            auto image = lookup("XObject", args[0].getName(), page_images);
            if (!image.isStream() || !image.getStream()->getDict()->lookup("Subtype").isName("Image")) return {};
        }
        args.clear();
    }
    return {};
}

/// Resource names that are used ONLY as the second shading of a supported marked pair.
/// Images cannot look up a page's graphics-state resources; Forms can inherit them.
/// Never change a state that also paints an image or other unmarked artwork.
std::set<std::string> pair_states(Page &page, XRef *xref)
{
    auto group = page.getGroup();
    if (!group || !group->lookup("S").isName("Transparency") || !group->lookup("CS").isName("DeviceRGB")) return {};
    auto knockout = group->lookup("K");
    if (!knockout.isNull() && (!knockout.isBool() || knockout.getBool())) return {};
    auto contents = page.getContents();
    if (!bounded_contents(contents, true)) return {};
    auto *resources = page.getResourceDict();
    if (!resources) return {};
    auto states = resources->lookup("ExtGState");
    auto shadings = resources->lookup("Shading");
    auto xobjects = resources->lookup("XObject");
    if (!states.isDict() || !shadings.isDict()) return {};
    struct Pair { bool valid = true; unsigned shades = 0; std::string second; };
    std::vector<Pair> pairs;
    std::vector<int> marks;
    std::vector<std::string> saves;
    std::vector<Object> forms;
    std::string current;
    std::map<std::string, std::vector<int>> uses;
    ::Parser parser(xref, &contents, false);
    std::vector<Object> args;
    auto active = [&] {
        for (auto it = marks.rbegin(); it != marks.rend(); ++it) if (*it >= 0) return *it;
        return -1;
    };
    static std::set<std::string_view> const allowed = {
        "q", "Q", "cm", "m", "l", "c", "v", "y", "h", "re", "W", "W*", "n", "gs", "sh", "BX", "EX", "ri"
    };
    for (unsigned tokens = 0; tokens < 1'000'000; ++tokens) {
        auto obj = parser.getObj();
        if (obj.isEOF()) {
            if (!args.empty() || !marks.empty() || !saves.empty()) return {};
            // Parse Forms after the page: a malformed file may alias their streams.
            // Bound the extra work across all Forms, including repeated placements.
            std::size_t form_bytes = CONTENT_LIMIT;
            unsigned form_tokens = 1'000'000;
            for (auto &form : forms) {
                auto inherited = leaf_form_states(form, states, xobjects, xref, form_bytes, form_tokens);
                if (!inherited) return {};
                for (auto const &name : *inherited) uses[name].push_back(-1);
            }
            std::set<std::string> result;
            for (auto const &[name, places] : uses) {
                if (std::all_of(places.begin(), places.end(), [&](int i) {
                    return i >= 0 && pairs[i].valid && pairs[i].shades == 2 && pairs[i].second == name;
                })) result.insert(name);
            }
            return result;
        }
        if (obj.isError()) return {};
        if (!obj.isCmd()) {
            if (args.size() == 64) return {};
            args.push_back(std::move(obj));
            continue;
        }
        std::string_view op(obj.getCmd());
        int const in = active();
        if (op == "BI") return {}; // Inline image bytes require their own parser path.
        if (op == "BDC" || op == "BMC") {
            if (args.empty() || !args[0].isName() || marks.size() == 256) return {};
            if (in >= 0) pairs[in].valid = false; // Nested marked paints need separate qualification.
            if (args[0].isName("MultiChannelPaint")) {
                if (pairs.size() == 4096) return {};
                marks.push_back(int(pairs.size()));
                pairs.emplace_back();
                if (in >= 0) pairs.back().valid = false;
            } else marks.push_back(-1);
        } else if (op == "EMC") {
            if (marks.empty() || !args.empty()) return {};
            marks.pop_back();
        } else {
            if (in >= 0 && !allowed.count(op)) pairs[in].valid = false;
            if (op == "q") {
                if (!args.empty() || saves.size() == 256) return {};
                saves.push_back(current);
            } else if (op == "Q") {
                if (!args.empty() || saves.empty()) return {};
                current = std::move(saves.back());
                saves.pop_back();
            } else if (op == "gs") {
                if (args.size() != 1 || !args[0].isName()) return {};
                current = args[0].getName();
                auto full = states.dictLookup(current.c_str());
                if (!full_state(full, false) && !full_state(full, true)) return {};
                uses[current].push_back(in);
            } else if (op == "Do") {
                if (args.size() != 1 || !args[0].isName() || !xobjects.isDict()) return {};
                auto object = xobjects.dictLookup(args[0].getName());
                if (!object.isStream()) return {};
                auto subtype = object.getStream()->getDict()->lookup("Subtype");
                if (subtype.isName("Form")) {
                    if (forms.size() == 64) return {};
                    if (!current.empty() && !states.dictLookup(current.c_str()).dictLookup("SMask").isName("None")) return {};
                    forms.push_back(std::move(object));
                } else if (!subtype.isName("Image")) return {};
                if (!current.empty()) uses[current].push_back(-1);
            } else if (op == "sh" && in >= 0) {
                auto &pair = pairs[in];
                if (args.size() != 1 || !args[0].isName()) return {};
                auto shade = shadings.dictLookup(args[0].getName());
                auto s = states.dictLookup(current.c_str());
                if (!shade.isDict()) return {};
                auto type = shade.dictLookup("ShadingType");
                auto space = shade.dictLookup("ColorSpace");
                bool const first = pair.shades++ == 0;
                bool const separation = space.isArray() && space.arrayGetLength() == 4 &&
                                        space.arrayGet(0).isName("Separation") &&
                                        !space.arrayGet(1).isName("All") && !space.arrayGet(1).isName("None");
                if (!type.isInt() || (type.getInt() != 2 && type.getInt() != 3) ||
                    !full_state(s, !first) || (first ? !separation : !space.isName("DeviceRGB"))) pair.valid = false;
                if (!first) pair.second = current;
            } else if (in < 0 && !current.empty() &&
                       (op == "sh" || op == "f" || op == "F" || op == "f*" || op == "S" || op == "B" || op == "B*" ||
                        op == "b" || op == "b*" || op == "s" || op == "Tj" || op == "TJ" || op == "'" || op == "\"")) {
                uses[current].push_back(-1); // A state that escapes EMC must not change later paint.
            }
        }
        args.clear();
    }
    return {};
}

} // namespace

std::optional<std::string> rgb_overprint_preview(PDFDoc &pdf)
{
    // A disposable appearance PDF is deliberately bounded. Large jobs retain existing handling.
    if (pdf.isEncrypted() || pdf.getNumPages() > 256 || pdf.getBaseStream()->getLength() > (100u << 20)) return {};
    bool changed = false;
    auto *xref = pdf.getXRef();
    for (int i = 1; i <= pdf.getNumPages(); ++i) {
        auto *page = pdf.getPage(i);
        if (!page) continue;
        auto names = pair_states(*page, xref);
        if (names.empty()) continue;
        // Fetch and deep-copy both levels. Inherited resources and the states used by soft-mask
        // Forms must keep their original dictionaries, even when all reference the same objects.
        auto resources = page->getResourceDictObject()->deepCopy();
        auto states = resources.dictLookup("ExtGState").deepCopy();
        for (auto const &name : names) {
            auto state = states.dictLookup(name.c_str()).deepCopy();
#if POPPLER_CHECK_VERSION(26, 4, 0)
            state.getDict()->set("BM", Object::name("Multiply"));
#else
            state.getDict()->set("BM", Object(objName, "Multiply"));
#endif
            states.getDict()->set(name.c_str(), std::move(state));
        }
        resources.getDict()->set("ExtGState", std::move(states));
        auto object = xref->fetch(page->getRef()).deepCopy();
        object.getDict()->set("Resources", std::move(resources));
        xref->setModifiedObject(&object, page->getRef());
        changed = true;
    }
    if (!changed) return {};
    gchar *path = nullptr;
    int fd = g_file_open_tmp("proof-rgb-overprint-XXXXXX", &path, nullptr);
    if (fd < 0) return {};
    auto cleanup = scope_exit([&] { g_unlink(path); g_free(path); });
    std::unique_ptr<FILE, decltype(&std::fclose)> file(fdopen(fd, "w+b"), &std::fclose);
    if (!file) { g_close(fd, nullptr); return {}; }
    {
        FileOutStream output(file.get(), 0);
        if (pdf.saveAs(&output, writeForceRewrite) != 0) return {};
    }
    if (std::fflush(file.get()) || std::fseek(file.get(), 0, SEEK_END)) return {};
    long const size = std::ftell(file.get());
    if (size <= 0 || size > (256L << 20) || std::fseek(file.get(), 0, SEEK_SET)) return {};
    std::string bytes(std::size_t(size), '\0');
    if (std::fread(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) return {};
    return bytes;
}

} // namespace Inkscape::Extension::Internal
