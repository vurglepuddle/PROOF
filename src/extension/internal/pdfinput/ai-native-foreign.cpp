// SPDX-License-Identifier: GPL-2.0-or-later
/** @file Bounded vector reading of Illustrator's embedded foreign-object PDFs. */
#include "ai-native-foreign.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "pdf-parser.h"
#include "svg-builder.h"
#include <Dict.h>
#include <GlobalParams.h>
#include <Object.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Parser.h>
#include <Stream.h>

#include "document.h"
#include "extension/internal/ai/ai-native-reader.h"
#include "xml/document.h"
#include "xml/node.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

bool number(Object const &v)
{
    return v.isNum() && std::isfinite(v.getNum()) && std::abs(v.getNum()) <= 1e6;
}

bool state_supported(Object const &state)
{
    if (!state.isDict()) return false;
    for (int i = 0; i < state.dictGetLength(); ++i) {
        std::string_view key(state.getDict()->getKey(i));
        auto value = state.dictGetVal(i);
        if (key == "Type" && value.isName("ExtGState")) continue;
        if ((key == "CA" || key == "ca") && number(value) && value.getNum() >= 0 && value.getNum() <= 1) continue;
        if (key == "BM" && (value.isName("Normal") || value.isName("Multiply"))) continue;
        if ((key == "OP" || key == "op" || key == "AIS" || key == "TK") && value.isBool() && !value.getBool()) continue;
        if (key == "SA" && value.isBool()) continue; // Device stroke-adjustment hint.
        if (key == "OPM" && value.isInt() && (value.getInt() == 0 || value.getInt() == 1)) continue; // Overprint itself is refused above.
        if (key == "SMask" && value.isName("None")) continue;
        return false; // No silently omitted transfer, knockout, mask, overprint, etc.
    }
    return true;
}

/// Deliberately narrower than the general PDF importer. No text, XObjects, patterns,
/// annotations or unknown compatibility operators can disappear inside a foreign object.
bool content_supported(Object *contents, XRef *xref, Dict *resources, PDFRectangle const &box)
{
    if (!contents->isStream() && !contents->isArray()) return false;
    if (contents->isArray()) {
        if (contents->arrayGetLength() > 256) return false;
        for (int i = 0; i < contents->arrayGetLength(); ++i) if (!contents->arrayGet(i).isStream()) return false;
    }
    static std::map<std::string_view, unsigned> const numeric = {
        {"cm", 6}, {"m", 2}, {"l", 2}, {"c", 6}, {"v", 4}, {"y", 4}, {"re", 4},
        {"w", 1}, {"J", 1}, {"j", 1}, {"M", 1}, {"i", 1},
        {"g", 1}, {"G", 1}, {"rg", 3}, {"RG", 3}, {"k", 4}, {"K", 4}
    };
    static std::vector<std::string_view> const zero = {
        "h", "n", "f", "F", "f*", "S", "s", "B", "B*", "b", "b*", "W", "W*", "q", "Q", "BX", "EX"
    };
    GfxResources res(xref, resources, nullptr);
    auto state = std::unique_ptr<GfxState>(new _POPPLER_GFX_STATE(72, 72, box, 0, false));
    ::Parser parser(xref, contents, false);
    std::vector<Object> args;
    unsigned tokens = 0, saved = 0, compatibility = 0;
    for (;;) {
        auto obj = parser.getObj();
        if (obj.isEOF()) return args.empty() && saved == 0 && compatibility == 0;
        if (++tokens > 1'000'000 || obj.isError()) return false;
        if (!obj.isCmd()) {
            if (args.size() >= 6 || (!number(obj) && !obj.isName() && !obj.isArray())) return false;
            args.push_back(std::move(obj));
            continue;
        }
        std::string_view op(obj.getCmd());
        if (auto it = numeric.find(op); it != numeric.end()) {
            if (args.size() != it->second || !std::all_of(args.begin(), args.end(), number)) return false;
            if (op == "cm" && std::abs(args[0].getNum() * args[3].getNum() - args[1].getNum() * args[2].getNum()) < 1e-12) return false;
            if ((op == "J" || op == "j") && (!args[0].isInt() || args[0].getInt() < 0 || args[0].getInt() > 2)) return false;
            if ((op == "w" || op == "M" || op == "i") && args[0].getNum() < 0) return false;
        } else if (std::find(zero.begin(), zero.end(), op) != zero.end()) {
            if (!args.empty()) return false;
            if (op == "q" && ++saved > 256) return false;
            if (op == "Q" && (saved == 0 || --saved > 256)) return false;
            if (op == "BX" && ++compatibility > 256) return false;
            if (op == "EX" && (compatibility == 0 || --compatibility > 256)) return false;
        } else if (op == "d") {
            if (args.size() != 2 || !args[0].isArray() || !number(args[1]) || args[0].arrayGetLength() > 256) return false;
            bool positive = args[0].arrayGetLength() == 0;
            for (int i = 0; i < args[0].arrayGetLength(); ++i) {
                auto v = args[0].arrayGet(i);
                if (!number(v) || v.getNum() < 0) return false;
                positive |= v.getNum() > 0;
            }
            if (!positive) return false;
        } else if (op == "gs") {
            if (args.size() != 1 || !args[0].isName() || !state_supported(res.lookupGState(args[0].getName()))) return false;
        } else if (op == "sh") {
            if (args.size() != 1 || !args[0].isName()) return false;
            auto shading = std::unique_ptr<GfxShading>(res.lookupShading(args[0].getName(), nullptr, state.get()));
            if (!shading || (shading->getType() != 2 && shading->getType() != 3)) return false;
            auto const mode = shading->getColorSpace()->getMode();
            if (mode != csDeviceGray && mode != csDeviceRGB && mode != csDeviceCMYK && mode != csICCBased) return false;
        } else if (op == "ri") {
            if (args.size() != 1 || !args[0].isName() ||
                (!args[0].isName("Perceptual") && !args[0].isName("RelativeColorimetric") &&
                 !args[0].isName("Saturation") && !args[0].isName("AbsoluteColorimetric"))) return false;
        } else {
            return false;
        }
        args.clear();
    }
}

} // namespace

template <typename StreamPointer>
static std::shared_ptr<PDFDoc> memory_pdf(StreamPointer stream)
{
    if constexpr (std::is_constructible_v<PDFDoc, StreamPointer>) return std::make_shared<PDFDoc>(std::move(stream));
    else return std::make_shared<PDFDoc>(stream.release());
}

XML::Node *import_foreign_pdf(ForeignObject const &object, SPDocument *doc, std::string &error)
{
    error = "a foreign object's embedded PDF is outside the supported vector subset";
    if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    Object null;
    null.setToNull();
    auto stream = std::make_unique<MemStream>(object.pdf.data(), 0, object.pdf.size(), std::move(null));
    auto pdf = memory_pdf(std::move(stream));
    if (!pdf->isOk() || pdf->isEncrypted() || pdf->getNumPages() != 1) return nullptr;
    auto page = pdf->getPage(1);
    if (!page) return nullptr;
#if POPPLER_CHECK_VERSION(26, 6, 0)
    auto box = page->getMediaBox(), crop = page->getCropBox();
#else
    auto box = *page->getMediaBox(), crop = *page->getCropBox();
#endif
    auto same = [](double a, double b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 0.01 + 1e-5 * std::abs(b); };
    auto annots = page->getAnnotsObject();
    if (page->getRotate() != 0 || !same(box.x1, crop.x1) || !same(box.y1, crop.y1) ||
        !same(box.x2, crop.x2) || !same(box.y2, crop.y2) ||
        !same(box.x2 - box.x1, object.bounds.width()) || !same(box.y2 - box.y1, object.bounds.height()) ||
        (!annots.isNull() && (!annots.isArray() || annots.arrayGetLength() != 0)) || page->getGroup()) return nullptr;
    auto contents = page->getContents();
    if (!content_supported(&contents, pdf->getXRef(), page->getResourceDict(), box)) return nullptr;

    auto group = doc->getReprDoc()->createElement("svg:g");
    char name[] = "embedded-pdf";
    SvgBuilder parent(doc, name, pdf->getXRef());
    SvgBuilder builder(&parent, group);
    builder.setConvertColors(false);
    ::PdfParser parser(pdf->getXRef(), &builder, page->getResourceDict(), &box);
    // A sub-page parser has no page-builder hook to seed its clip history. Shading
    // fills need the page rectangle even when the content sets no explicit clip.
    auto state = parser.getState();
    state->moveTo(box.x1, box.y1);
    state->lineTo(box.x2, box.y1);
    state->lineTo(box.x2, box.y2);
    state->lineTo(box.x1, box.y2);
    state->closePath();
    builder.setClip(state, clipNormal, true);
    state->clearPath();
    parser.parse(&contents);
    error.clear();
    return group;
}

} // namespace Inkscape::Extension::Internal::AiNative
