// SPDX-License-Identifier: GPL-2.0-or-later
// Hand-written native records and PDF pages: no production artwork in these tests.
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <GlobalParams.h>
// Windows' headers, which Poppler's bring in, make these words nothing.
#undef near
#undef far
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <gtest/gtest.h>
#include <tiffio.h>
#include <zlib.h>
#include <2geom/affine.h>
#include <2geom/bezier-curve.h>
#include <2geom/pathvector.h>

#include "colors/cms/profile.h"
#include "colors/color.h"
#include "display/drawing-item.h"
#include "display/drawing.h"
#include "colors/document-colors.h"
#include "colors/manager.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/cmyk.h"
#include "desktop-style.h"
#include "display/cairo-utils.h"
#include "display/control/font-highlight.h"
#include "doc-per-case-test.h"
#include "document.h"
#include "file.h"
#include "helper/geom.h"
#include "helper/pixbuf-ops.h"
#include "io/link-policy.h"
#include "extension/internal/ai/ai-native-fonts.h"
#include "extension/internal/ai/ai-native-import.h"
#include "extension/internal/ai/ai-native-text.h"
#include "extension/internal/ai/ai-native-writer.h"
#include "extension/internal/ai/ai-native-writer-string.h"
#include "extension/internal/ai/ai-legacy-output.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
#include <Dict.h>
#include <PDFDoc.h>
#include "extension/internal/pdfinput/poppler-transition-api.h"
#include "extension/internal/pdfinput/ai-native-open.h"
#include "extension/internal/pdfinput/rgb-overprint.h"
#endif
#if defined(WITH_POPPLER) && defined(WITH_CAPYPDF)
#include <cstdint>
#include <glib/gstdio.h>
#include <Dict.h>
#include <PDFDoc.h>
#include <Page.h>

#include "extension/internal/ai/ai-native-output.h"
#include "extension/internal/pdfoutput/pdf-output.h"
#include "extension/db.h"
#include "extension/output.h"
#include "extension/internal/pdfinput/ai-private-data.h"
#include "extension/internal/pdfinput/poppler-transition-api.h"
#endif
#include "object/sp-defs.h"
#include "object/sp-image.h"
#include "psd-fixture.h"
#include "object/sp-item.h"
#include "object/sp-gradient.h"
#include "object/gradient-perceptual.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-pattern.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "page-manager.h"
#include "spot-ink.h"
#include "style.h"
#include "svg/svg.h"
#include "text-editing.h"
#include "type-settings.h"
#include "util/units.h"
#include "util/scope_exit.h"
#include "xml/node.h"
#include "xml/repr.h"

using namespace Inkscape;
namespace AN = Extension::Internal::AiNative;
using Colors::Space::Type;

namespace {

std::string rect = "10 20 m 10 60 L 90 60 L 90 20 L 10 20 L\nf\n";

std::string layer(std::string const &art, std::string const &options = "1 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb")
{
    return "%AI5_BeginLayer\n" + options + "\n(Artwork) Ln\n0 A 0 Xw\n" + art + "LB\n%AI5_EndLayer--\n";
}

std::string records(std::string const &art, std::string const &setup = {}, bool cmyk = true)
{
    return "%!PS-Adobe-3.0\n%%BoundingBox: 0 0 200 100\n%AI3_Cropmarks: 0 0 200 100\n%AI5_RulerUnits: 1\n"
           "%AI9_ColorModel: " + std::string(cmyk ? "2" : "1") + "\n%%EndComments\n%%BeginSetup\n" + setup +
           "%%EndSetup\n" + art + "%%PageTrailer\nshowpage\n%%EOF\n";
}

std::string vector_mask(std::string const &art, int clipping = 1)
{
    return "/ArtDictionary :\n/Mask :\n" + std::to_string(clipping) + " /Bool (Clipping) ,\n0 /Bool (Inverted) ,\n0 /Bool (Disabled) ,\n"
           "X=\n" + art + "X+\n(Art) ,\n; (AI9_Mask) ,\n;\n";
}

std::string boards(int count)
{
    std::string out = "%AI9_BeginDocumentData\n%_/Document :\n%_/Dictionary :\n%_/Array :\n";
    for (int i = 0; i < count; ++i) {
        out += "%_/Dictionary :\n%_" + std::to_string(i * 220) + " 100 /RealPointRelToROrigin (PositionPoint1) ,\n%_" +
               std::to_string(i * 220 + 200) + " 0 /RealPointRelToROrigin (PositionPoint2) ,\n%_(Page " +
               std::to_string(i + 1) + ") /UnicodeString (Name) ,\n%_; ,\n";
    }
    return out + "%_; (ArtboardArray) ,\n%_; /Recorded ,\n%_;\n%AI9_EndDocumentData\n";
}

std::string ascii85(std::string const &bytes)
{
    std::string out;
    for (std::size_t at = 0; at < bytes.size(); at += 4) {
        auto const n = std::min<std::size_t>(4, bytes.size() - at);
        std::uint32_t value = 0;
        for (std::size_t j = 0; j < 4; ++j) value = (value << 8) | (j < n ? std::uint8_t(bytes[at + j]) : 0);
        char group[5];
        for (int j = 4; j >= 0; --j) { group[j] = char('!' + value % 85); value /= 85; }
        out.append(group, n + 1);
    }
    return out;
}

// Minimal text dictionaries written independently of the donor and production files.
std::string text_document(std::string const &characters = "(Hello\\r)", int units = 6,
                          std::string const &font = "ArialMT", std::string const &style = {},
                          std::string const &carries = "/2 [1 0 0 1 -90 -10]",
                          std::string const &extra_runs = {})
{
    return "/0 << /1 << /0 [ << /0 << /0 << /0 (" + font + ") >> >> >> ] >> "
           "/8 << /0 [ << /0 << /2 << " + carries + " >> >> >> ] >> >> "
           "/1 << /1 [ << /0 << /0 " + characters +
           " /6 << /0 [ << /1 " + std::to_string(units) + " /0 << /0 << /6 << " + style +
           " >> >> >> >> " + extra_runs + " ] >> >> /1 << /0 [ << /0 0 >> ] "
           "/2 << /99 /F /0 << /0 [8191.5 8191.5] >> "
           "/6 [ << /99 /L /6 [ << /99 /S >> ] >> ] >> >> >> ] "
           "/2 << /0 0 /1 12 /56 true /57 false /53 << /0 << /0 2 /1 [1 0.25 0.5 0.75 0.1] >> >> >> >>";
}

std::string text_setup(std::string const &document)
{
    auto packed = ascii85(document);
    std::string lines;
    for (std::size_t at = 0; at < packed.size(); at += 71) lines += "%" + packed.substr(at, 71) + "\n";
    return "%AI3_TemplateBox: 100 50\n%AI11_BeginTextDocument\n%_/Binary :\n%_/ASCII85Decode ,\n" +
           lines + "%~>\n%_;\n%AI11_EndTextDocument\n";
}

// Illustrator omits this dictionary when the entire story has the default character style.
std::string without_character_runs(std::string document)
{
    auto const start = document.find(" /6 << /0 [");
    auto const end = document.find(" ] >> >> /1", start);
    if (start == document.npos || end == document.npos) return {};
    document.erase(start, end + std::string(" ] >>").size() - start);
    return document;
}

// Independent paragraph resources and run declarations. Names/IDs are arbitrary;
// the style index, sparse properties and parent link are what the reader resolves.
std::string with_paragraphs(std::string document, std::string const &definitions,
                            std::string const &runs, std::string const &defaults = {})
{
    auto at = document.find("/8 <<");
    if (!definitions.empty()) document.insert(at, "/6 << /0 [ " + definitions + " ] >> ");
    if (!runs.empty()) {
        at = document.find(" /6 << /0 [", document.find("/1 << /1 ["));
        if (at == document.npos) at = document.find(" >> /1 << /0 [");
        document.insert(at, " /5 << /0 [ " + runs + " ] >>");
    }
    if (!defaults.empty()) {
        at = document.rfind("/2 << /0 0 /1 12");
        document.insert(at, "/3 << " + defaults + " >> ");
    }
    return document;
}

// The two text documents as Illustrator 2025 writes them: what was typed, then the type
// that appearances drew.
std::string text_setup(std::string const &typed, std::string const &drawn)
{
    auto block = [](std::string const &name, std::string const &document) {
        auto packed = ascii85(document);
        std::string lines;
        for (std::size_t at = 0; at < packed.size(); at += 71) lines += "%" + packed.substr(at, 71) + "\n";
        return "/" + name + " : /ASCII85Decode ,\n" + lines + "%~>\n;\n";
    };
    return "%AI3_TemplateBox: 100 50\n%AI11_BeginTextDocument\n" + block("AI11TextDocument", typed) +
           block("AI11UndoFreeTextDocument", drawn) + "%AI11_EndTextDocument\n";
}

std::string aligned_text_document(unsigned alignment, std::string const &font = "ArialMT")
{
    auto data = text_document("(HHH\\r)", 4, font,
                             "/1 32 /53 << /0 << /0 0 /1 [1 0] >> >>", "/2 [1 0 0 1 -20 -10]");
    auto at = data.find(" /6 << /0 [");
    data.insert(at, " /5 << /0 [ << /1 4 /0 << /0 << /5 << /0 " + std::to_string(alignment) + " >> >> >> >> ] >>");
    at = data.find("/99 /S >>");
    data.replace(at, std::string("/99 /S >>").size(), "/99 /S /0 << /0 [" +
                 std::string(alignment == 2 ? "-28.8" : "-57.6") + " 0] >> >>");
    return data;
}

/// A line of area type: where it starts from the frame's top-left corner, how many
/// characters it holds, and how far its glyphs reach from its segment's start.
struct AreaLine
{
    double x, y;
    int units;
    double start = 0; ///< The segment's offset in the line.
    double width = 0;
};

// Area type as Illustrator 2025 writes it, in a 100 x 40 frame whose top-left corner is the
// layout's 8191.5, 8191.5: the frame's outline (four segments of four points), its kind
// and gutters, and under F the rows holding each line with its box, its segment's
// character count and its glyph run's extent.
std::string area_text_document(std::string const &characters, int units, std::vector<AreaLine> const &lines,
                               std::string const &font = "ArialMT", std::string const &style = {},
                               std::string const &paragraphs = {}, std::string const &layout = {})
{
    auto n = [](double v) { std::ostringstream s; s << v; return s.str(); };
    double const l = 8191.5, t = 8191.5, r = l + 100, b = t + 40;
    std::string outline;
    for (auto const &[x0, y0, x1, y1] : std::vector<std::array<double, 4>>{{r, b, l, b}, {l, b, l, t}, {l, t, r, t}, {r, t, r, b}}) {
        outline += n(x0) + " " + n(y0) + " " + n(x0) + " " + n(y0) + " " + n(x1) + " " + n(y1) + " " + n(x1) + " " + n(y1) + " ";
    }
    std::string laid;
    for (auto const &line : lines) {
        laid += "<< /99 /L /0 << /0 [" + n(line.x) + " " + n(line.y) + "] >> /1 [0 -10 100 3] /6 [ << /99 /S /0 << /0 [" +
                n(line.start) + " 0] >> /15 << /0 " + std::to_string(line.units) + " /2 0 /5 false >> /6 [ << /99 /G /1 [0 -10 " +
                n(line.width) + " 3] >> ] >> ] >> ";
    }
    auto const tree = layout.empty() ? "/2 [ << /99 /PC /6 [ << /99 /F /0 << /0 [8191.5 8191.5] >> /1 [0 0 100 40] /6 [ << /99 /R /6 [ "
                                       "<< /99 /R /6 [ " + laid + "] >> ] >> ] >> ] >> ]"
                                     : layout;
    return "/0 << /1 << /0 [ << /0 << /0 << /0 (" + font + ") >> >> >> ] >> "
           "/8 << /0 [ << /0 << /97 (id) /1 << /0 [ " + outline + "] >> /2 << /0 1 /2 [1 0 0 1 -90 -10] /7 18 /8 18 /10 << /0 5 /1 0 >> >> >> >> ] >> >> "
           "/1 << /1 [ << /0 << /0 " + characters + " " + paragraphs +
           " /6 << /0 [ << /1 " + std::to_string(units) + " /0 << /0 << /6 << " + style + " >> >> >> >> ] >> >> "
           "/1 << /0 [ << /0 0 >> ] " + tree + " >> >> ] "
           "/2 << /0 0 /1 12 /56 true /57 false /53 << /0 << /0 2 /1 [1 0.25 0.5 0.75 0.1] >> >> >> >>";
}

/// `document` with its one line's run of glyphs saying where Illustrator set it: its box in
/// the layout's space (left, top, right, bottom).
std::string with_glyph_run(std::string document, std::string const &box)
{
    std::string const segment = "<< /99 /S >>";
    auto const at = document.find(segment);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos) document.replace(at, segment.size(), "<< /99 /S /6 [ << /99 /G /8 [" + box + "] >> ] >>");
    return document;
}

std::string available_text_font()
{
    for (auto const *name : {"ArialMT", "DejaVuSans", "LiberationSans"}) {
        if (auto *desc = FontFactory::get().parsePostscriptName(name, false)) {
            pango_font_description_free(desc); return name;
        }
    }
    return {};
}

std::vector<XML::Node *> elements(XML::Node *root, char const *name)
{
    std::vector<XML::Node *> out;
    sp_repr_visit_descendants(root, [&](XML::Node *n) {
        if (std::string_view(n->name()) == name) out.push_back(n);
        return true;
    });
    return out;
}

std::string attr(XML::Node *n, char const *key)
{
    auto const *v = n->attribute(key);
    return v ? v : "";
}

/// Where an object draws, in the document's user units (points here; bounds are in pixels).
Geom::OptRect drawn(SPDocument *doc, XML::Node *repr)
{
    doc->ensureUpToDate();
    auto *item = dynamic_cast<SPItem *>(doc->getObjectByRepr(repr));
    auto bounds = item ? item->documentVisualBounds() : Geom::OptRect();
    if (bounds) *bounds *= doc->getDocumentScale().inverse();
    return bounds;
}

class AiNativeImportTest : public DocPerCaseTest
{
protected:
    std::optional<AN::Built> make(std::string const &src)
    {
        std::string error;
        auto ai = AN::read(src, error);
        EXPECT_TRUE(ai) << error;
        if (!ai) return {};
        auto out = AN::build(*ai, error);
        EXPECT_TRUE(out) << error;
        return out;
    }
};

#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
// A minimal PDF carrying independent page content and native artwork. Each page has
// its own box; changing either half exercises the safety net rather than the parser.
std::string fixture(std::string const &native, std::vector<std::string> const &contents,
                    std::string const &boxes = "/MediaBox [0 0 200 100] /TrimBox [0 0 200 100]",
                    std::string const &font = {}, std::string const &profile = {}, std::string const &shading = {},
                    std::string const &extra_resources = {}, std::string const &xobject = {})
{
    std::map<int, std::string> objects;
    objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    std::string kids;
    auto stream = [](std::string const &s) { return "<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "\nendstream"; };
    objects[3] = "<< /AIPrivateData1 4 0 R /NumBlock 1 /ContainerVersion 11 /CreatorVersion 16 /RoundtripVersion 16 >>";
    objects[4] = stream(native);
    std::string resources;
    int next = 5 + int(contents.size()) * 2;
    if (!font.empty()) {
        int const id = next++;
        objects[id] = "<< /Type /Font /Subtype /Type1 /BaseFont /" + font + " /Encoding /WinAnsiEncoding >>";
        resources += " /Font << /F1 " + std::to_string(id) + " 0 R >>";
    }
    if (!profile.empty()) {
        // The document's profile, as Illustrator embeds it: the space of the page's process colours.
        int const id = next++;
        objects[id] = "<< /N 4 /Length " + std::to_string(profile.size()) + " >>\nstream\n" + profile + "\nendstream";
        resources += " /ColorSpace << /CS0 [/ICCBased " + std::to_string(id) + " 0 R] >>";
    }
    if (!shading.empty()) {
        int const id = next++;
        objects[id] = shading;
        resources += " /Shading << /S1 " + std::to_string(id) + " 0 R >>";
    }
    if (!xobject.empty()) {
        int const id = next++;
        objects[id] = xobject;
        resources += " /XObject << /Image " + std::to_string(id) + " 0 R >>";
    }
    resources = "<<" + resources + extra_resources + " >>";
    for (std::size_t i = 0; i < contents.size(); ++i) {
        int const p = 5 + int(i) * 2;
        kids += std::to_string(p) + " 0 R ";
        objects[p] = "<< /Type /Page /Parent 2 0 R " + boxes + " /Resources " + resources + " /Contents " + std::to_string(p + 1) +
                     " 0 R /PieceInfo << /Illustrator << /Private 3 0 R >> >> >>";
        objects[p + 1] = stream(contents[i]);
    }
    objects[2] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(contents.size()) + " >>";
    std::ostringstream pdf;
    pdf << "%PDF-1.5\n";
    std::vector<std::size_t> offsets(objects.size() + 1);
    for (auto const &[n, body] : objects) {
        offsets[n] = std::size_t(pdf.tellp());
        pdf << n << " 0 obj\n" << body << "\nendobj\n";
    }
    auto const xref = pdf.tellp();
    pdf << "xref\n0 " << offsets.size() << "\n0000000000 65535 f \n";
    for (std::size_t n = 1; n < offsets.size(); ++n) pdf << std::setw(10) << std::setfill('0') << offsets[n] << " 00000 n \n";
    pdf << "trailer\n<< /Size " << offsets.size() << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    gchar *dir = g_dir_make_tmp("proof-native-import-XXXXXX", nullptr);
    EXPECT_NE(dir, nullptr);
    if (!dir) return {};
    std::string path = std::string(dir) + "/fixture.ai";
    g_free(dir);
    auto bytes = pdf.str();
    EXPECT_TRUE(g_file_set_contents(path.c_str(), bytes.data(), bytes.size(), nullptr));
    return path;
}
#endif
} // namespace

TEST_F(AiNativeImportTest, LayersPagesUnitsAndYFlip)
{
    auto b = make(records(layer("0 g\n" + rect) + layer("0 g\n" + rect, "0 0 0 0 1 0 0 -1 12 34 56 0 70 0 Lb"), boards(2)));
    ASSERT_TRUE(b);
    auto *doc = b->document.get();
    auto *root = doc->getReprRoot();
    EXPECT_EQ(attr(root, "viewBox"), "0 0 200 100");
    EXPECT_NE(attr(root, "width").find("mm"), std::string::npos);
    EXPECT_NEAR(doc->getWidth().value("pt"), 200, 1e-5);
    EXPECT_NEAR(doc->getHeight().value("pt"), 100, 1e-5);
    auto const pages = doc->getPageManager().getPages();
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_EQ(attr(pages[0]->getRepr(), "inkscape:label"), "Page 1");
    EXPECT_EQ(b->artboards[1], Geom::Rect(220, 0, 420, 100));
    auto const groups = elements(root, "svg:g");
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(attr(groups[0], "inkscape:label"), "Artwork");
    EXPECT_EQ(attr(groups[1], "style"), "display:none");
    EXPECT_EQ(attr(groups[1], "sodipodi:insensitive"), "true");
    EXPECT_EQ(attr(groups[1], "proof:layer-print"), "false");
    EXPECT_EQ(attr(groups[1], "proof:layer-preview"), "false");
    EXPECT_EQ(attr(groups[1], "proof:layer-dim"), "70");
    EXPECT_EQ(attr(groups[1], "inkscape:highlight-color"), "#0c2238");
    auto pv = sp_svg_read_pathv(attr(elements(root, "svg:path")[0], "d").c_str());
    ASSERT_EQ(pv.size(), 1u);
    EXPECT_EQ(pv[0].initialPoint(), Geom::Point(10, 80));
}

namespace {
std::string mesh_art(std::string const &space, std::array<std::string, 4> const &colors,
                     std::string const &extra = {})
{
    return "/Mesh X!\n2 /Version X#\n/Cartesian /Type X#\n[1 1] /Size X#\n/Data X#\n"
           "[0 0] /P X#\n/" + space + " /CS X#\n[" + colors[0] + " 1 10 20 10 40 1 30 20 1] /N X#\n" + extra +
           "[" + colors[1] + " 1 10 80 30 80 1 10 60 1] /N X#\n"
           "[" + colors[2] + " 1 70 80 70 60 1 50 80 1] /N X#\n"
           "[" + colors[3] + " 1 70 20 50 20 1 70 40 1] /N X#\n/E X#\n/End X!\n";
}

/// A mesh of `across` by `up` square patches of 30 pt from 0,0, as Illustrator writes one: patch
/// [a b] from its lower left corner around, each corner's colour `colour(i, j)` at grid point
/// i, j and straight sides; `anchors(a, b)` goes after its first corner (anchors on side 0).
std::string grid_mesh(std::size_t across, std::size_t up, std::function<std::string(std::size_t, std::size_t)> const &colour,
                      std::function<std::string(std::size_t, std::size_t)> const &anchors = {})
{
    std::string s = std::format("/Mesh X!\n2 /Version X#\n/Cartesian /Type X#\n[{} {}] /Size X#\n/Data X#\n", across, up);
    for (std::size_t a = 0; a < across; ++a) {
        for (std::size_t b = 0; b < up; ++b) {
            double const r = b == 0 ? double(a) / across : a == 0 ? double(b) / up : -1;
            s += std::format("[{} {}] /P X#\n[{}] /R X#\n", a, b, r);
            if (a == 0 && b == 0) s += "/DeviceCMYK /CS X#\n";
            double const x0 = 30.0 * a, x1 = x0 + 30, y0 = 30.0 * b, y1 = y0 + 30;
            auto corner = [&](std::size_t i, std::size_t j, double x, double y, double nx, double ny, double px, double py) {
                s += std::format("[{} 1 {} {} {} {} 1 {} {} 1] /N X#\n", colour(i, j), x, y, nx, ny, px, py);
            };
            corner(a, b, x0, y0, x0 + 10, y0, x0, y0 + 10);
            if (anchors) s += anchors(a, b);
            corner(a + 1, b, x1, y0, x1, y0 + 10, x1 - 10, y0);
            corner(a + 1, b + 1, x1, y1, x1 - 10, y1, x1, y1 - 10);
            corner(a, b + 1, x0, y1, x0, y1 - 10, x0 + 10, y1);
            s += "/E X#\n";
        }
    }
    return s + "/End X!\n";
}

/// Grid point i, j of a mesh's corners in CMYK, each different.
std::string grid_colour(std::size_t i, std::size_t j)
{
    return std::format("{} {} {} 0", 0.1 * i, 0.1 * j, 0.05 * (i + j));
}
}

TEST_F(AiNativeImportTest, MeshKeepsEditableCornersAndHandlesAfterReopen)
{
    auto b = make(records(layer(mesh_art("DeviceRGB", {"1 0 0", "0 1 0", "0 0 1", "1 1 1"})), {}, false));
    ASSERT_TRUE(b);
    auto check = [&](SPDocument *doc) {
        doc->ensureUpToDate();
        auto gradients = elements(doc->getDefs()->getRepr(), "svg:meshgradient");
        ASSERT_EQ(gradients.size(), 1u);
        auto const paths = elements(doc->getReprRoot(), "svg:path");
        ASSERT_EQ(paths.size(), 1u);
        EXPECT_EQ(attr(paths[0], "proof:mesh-object"), "true");
        auto *gradient = dynamic_cast<SPMeshGradient *>(doc->getObjectByRepr(gradients[0]));
        ASSERT_TRUE(gradient);
        gradient->ensureArray();
        ASSERT_EQ(gradient->array.patch_rows(), 1u);
        ASSERT_EQ(gradient->array.patch_columns(), 1u);
        SPMeshPatchI p(&gradient->array.nodes, 0, 0);
        EXPECT_EQ(p.getPoint(0, 0), Geom::Point(10, 80));
        EXPECT_EQ(p.getPoint(0, 1), Geom::Point(10, 60));
        EXPECT_EQ(p.getPoint(0, 2), Geom::Point(10, 40));
        EXPECT_EQ(p.getPoint(1, 0), Geom::Point(10, 20));
        EXPECT_EQ(p.getPoint(2, 0), Geom::Point(70, 20));
        EXPECT_EQ(p.getPoint(3, 0), Geom::Point(70, 80));
        auto const color = p.getColor(0);
        ASSERT_TRUE(color);
        auto const values = color->getValues();
        ASSERT_GE(values.size(), 3u);
        EXPECT_EQ(values[0], 1);
        EXPECT_EQ(values[1], 0);
        EXPECT_EQ(values[2], 0);
    };
    check(b->document.get());
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    check(reopened.get());
    auto stops = elements(reopened->getDefs()->getRepr(), "svg:stop");
    stops[0]->setAttribute("style", "stop-color:#123456;stop-opacity:0.5");
    reopened->ensureUpToDate();
    auto edited = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(edited);
    EXPECT_NE(attr(elements(edited->getDefs()->getRepr(), "svg:stop")[0], "style").find("#123456"), std::string::npos);
}

TEST_F(AiNativeImportTest, MeshCmykChannelsAndOpacitySurviveReopen)
{
    auto art = mesh_art("DeviceCMYK", {"0.1 0.2 0.3 0.4", "0 0 0 0", "0 0 0 1", "1 0 0 0"});
    art.replace(art.find("0.4 1 10"), 8, "0.4 0.25 10");
    auto b = make(records(layer(art)));
    ASSERT_TRUE(b);
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto stops = elements(reopened->getDefs()->getRepr(), "svg:stop");
    ASSERT_EQ(stops.size(), 4u);
    auto *stop = reopened->getObjectByRepr(stops[0]);
    auto color = stop->style->stop_color.getColor();
    EXPECT_EQ(color.getSpace()->getComponentType(), Type::CMYK);
    auto const v = color.getValues();
    ASSERT_GE(v.size(), 4u);
    for (unsigned i = 0; i < 4; ++i) EXPECT_NEAR(v[i], 0.1 * (i + 1), 1e-5);
    EXPECT_NE(attr(stops[0], "style").find("stop-opacity:0.25"), std::string::npos);
}

TEST_F(AiNativeImportTest, UncutMeshCornersKeepTheirOwnColourModels)
{
    auto art = mesh_art("DeviceRGB", {"1 0 0", "0 1 0", "0 0 1", "1 1 1"});
    auto const at = art.find("[0 1 0 1 10 80");
    ASSERT_NE(at, std::string::npos);
    art.insert(at, "/DeviceCMYK /CS X#\n");
    art.replace(art.find("[0 1 0 1 10 80"), 7, "[1 0 1 0 ");
    auto const next = art.find("[0 0 1 1 70 80");
    ASSERT_NE(next, std::string::npos);
    art.insert(next, "/DeviceRGB /CS X#\n");
    auto b = make(records(layer(art), {}, false));
    ASSERT_TRUE(b);
    auto stops = elements(b->document->getDefs()->getRepr(), "svg:stop");
    ASSERT_EQ(stops.size(), 4u);
    auto const rgb = b->document->getObjectByRepr(stops[0])->style->stop_color.getColor();
    auto const cmyk = b->document->getObjectByRepr(stops[1])->style->stop_color.getColor();
    EXPECT_EQ(rgb.getSpace()->getComponentType(), Type::RGB);
    EXPECT_EQ(cmyk.getSpace()->getComponentType(), Type::CMYK);
    EXPECT_EQ(cmyk.getValues(), (std::vector<double>{1, 0, 1, 0}));
}

TEST_F(AiNativeImportTest, ABentMeshPatchIsCutAtItsAnchorsWithTheSharesIllustratorGives)
{
    // mesh-1x1-slide, a control saved by Illustrator 29.2.1: an anchor slid up the left edge to
    // 80% of it, its handles kept. Illustrator gives the lower part of the edge 63% of it, as
    // its control polygon is 79.875 pt of 126.42: its colour there is C 63 K 37.
    std::string const art =
        "/Mesh X!\n2 /Version X#\n/Cartesian /Type X#\n[1 1] /Size X#\n/Data X#\n[0 0] /P X#\n"
        "[40 119.875 40 103.2083333333 40 136.5416666667 1] /A X#\n/DeviceCMYK /CS X#\n"
        "[0 0 0 1 1 40 40 73.3333333333 40 1 40 56.6666666667 1] /N X#\n"
        "[0 0 1 0 1 140 40 140 73.3333333333 1 106.6666666667 40 1] /N X#\n"
        "[0 1 0 0 1 140 140 106.6666666667 140 1 140 106.6666666667 1] /N X#\n"
        "[1 0 0 0 1 40 140 40 123.3333333333 1 73.3333333333 140 1] /N X#\n/E X#\n/End X!\n";
    auto b = make(records(layer(art)));
    ASSERT_TRUE(b);
    auto *doc = b->document.get();
    doc->ensureUpToDate();
    auto gradients = elements(doc->getDefs()->getRepr(), "svg:meshgradient");
    ASSERT_EQ(gradients.size(), 1u);
    auto *gradient = dynamic_cast<SPMeshGradient *>(doc->getObjectByRepr(gradients[0]));
    ASSERT_TRUE(gradient);
    gradient->ensureArray();
    ASSERT_EQ(gradient->array.patch_rows(), 2u);
    ASSERT_EQ(gradient->array.patch_columns(), 1u);
    SPMeshPatchI lower(&gradient->array.nodes, 0, 0);
    auto const corner = lower.getPoint(0, 0), anchor = lower.getPoint(3, 0);
    EXPECT_NEAR(anchor.x(), corner.x(), 1e-6);
    EXPECT_NEAR(std::abs(anchor.y() - corner.y()), 79.875, 1e-6);
    double const share = 79.875 / (79.875 + 16.6666666667 + 13.2083333333 + 16.6666666667);
    auto const color = lower.getColor(3);
    ASSERT_TRUE(color);
    auto const v = color->getValues();
    ASSERT_GE(v.size(), 4u);
    // Stop colours are written to three decimals.
    EXPECT_NEAR(v[0], share, 1e-3);
    EXPECT_NEAR(v[3], 1 - share, 1e-3);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](std::string const &s) {
        return s.find("cut into cells") != s.npos;
    }));
}

TEST_F(AiNativeImportTest, UniformMeshKeepsEveryBoundaryNodeAndSaysItIsOutlined)
{
    auto b = make(records(layer(mesh_art("DeviceGray", {"0.3", "0.3", "0.3", "0.3"},
                                        "[4 50 4 40 4 60 1] /S X#\n[6 70 6 60 6 75 1] /A X#\n")), {}, false));
    ASSERT_TRUE(b);
    EXPECT_TRUE(elements(b->document->getDefs()->getRepr(), "svg:meshgradient").empty());
    auto paths = elements(b->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(attr(paths[0], "proof:mesh-outline"), "true");
    auto pv = sp_svg_read_pathv(attr(paths[0], "d").c_str());
    ASSERT_EQ(pv.size(), 1u);
    EXPECT_EQ(pv[0].size(), 6u);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](std::string const &s) {
        return s.find("mesh editing is not kept") != s.npos;
    }));
}

TEST_F(AiNativeImportTest, AMeshComesInAsOneGridOnItsOutline)
{
    // Illustrator edits a mesh as one grid; patch [a b] is the SVG mesh's row b, column a, its
    // corners numbered alike (the page is 100 pt high: art y 30 is 70 down).
    auto b = make(records(layer(grid_mesh(2, 2, grid_colour))));
    ASSERT_TRUE(b);
    auto *doc = b->document.get();
    doc->ensureUpToDate();
    auto paths = elements(doc->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(attr(paths[0], "proof:ai-mesh"), "true");
    EXPECT_EQ(attr(paths[0], "proof:mesh-object"), "true");
    EXPECT_TRUE(elements(doc->getReprRoot(), "svg:g").size() == 1u) << "only the layer";
    auto pv = sp_svg_read_pathv(attr(paths[0], "d").c_str());
    ASSERT_EQ(pv.size(), 1u);
    EXPECT_EQ(pv[0].size_default(), 8u);
    auto gradients = elements(doc->getDefs()->getRepr(), "svg:meshgradient");
    ASSERT_EQ(gradients.size(), 1u);
    auto *gradient = dynamic_cast<SPMeshGradient *>(doc->getObjectByRepr(gradients[0]));
    ASSERT_TRUE(gradient);
    gradient->ensureArray();
    ASSERT_EQ(gradient->array.patch_rows(), 2u);
    ASSERT_EQ(gradient->array.patch_columns(), 2u);
    SPMeshPatchI p(&gradient->array.nodes, 1, 1);
    EXPECT_TRUE(Geom::are_near(p.getPoint(0, 0), Geom::Point(30, 70), 1e-6));
    EXPECT_TRUE(Geom::are_near(p.getPoint(0, 1), Geom::Point(40, 70), 1e-6));
    EXPECT_TRUE(Geom::are_near(p.getPoint(2, 0), Geom::Point(60, 40), 1e-6));
    auto const color = p.getColor(2);
    ASSERT_TRUE(color);
    auto const v = color->getValues();
    ASSERT_GE(v.size(), 4u);
    EXPECT_NEAR(v[0], 0.2, 1e-3);
    EXPECT_NEAR(v[1], 0.2, 1e-3);
    EXPECT_NEAR(v[2], 0.2, 1e-3);

    // Neighbours that don't share a corner's colour aren't one grid: patch by patch, said so.
    auto split = grid_mesh(2, 1, grid_colour);
    split.replace(split.rfind("[0.1 0 0.05 0 1 30 0"), 20, "[0.9 0 0.05 0 1 30 0");
    auto c = make(records(layer(split)));
    ASSERT_TRUE(c);
    EXPECT_EQ(elements(c->document->getReprRoot(), "svg:path").size(), 2u);
    EXPECT_EQ(elements(c->document->getDefs()->getRepr(), "svg:meshgradient").size(), 2u);
    EXPECT_TRUE(std::any_of(c->notes.begin(), c->notes.end(), [](std::string const &s) {
        return s.find("came in patch by patch") != s.npos;
    }));
}

TEST_F(AiNativeImportTest, AnAnchorCutsItsColumnThroughTheWholeGrid)
{
    // An anchor halfway along patch [0 0]'s first side: column 0 is cut there, in both rows, so
    // the grid stays one mesh, the anchor a mesh point with the colour halfway.
    auto anchors = [](std::size_t a, std::size_t b) {
        return a == 0 && b == 0 ? std::string("[15 0 10 0 20 0 1] /A X#\n") : std::string();
    };
    auto art = grid_mesh(2, 2, grid_colour, anchors);
    // The first corner's handle and the second's along the cut side move to the anchor's sides.
    art.replace(art.find("[0 0 0 0 1 0 0 10 0 1 0 10 1]"), 29, "[0 0 0 0 1 0 0 5 0 1 0 10 1]");
    auto const second = art.find("[0.1 0 0.05 0 1 30 0 30 10 1 20 0 1]");
    ASSERT_NE(second, std::string::npos);
    art.replace(second, 36, "[0.1 0 0.05 0 1 30 0 30 10 1 25 0 1]");
    auto b = make(records(layer(art)));
    ASSERT_TRUE(b);
    auto *doc = b->document.get();
    doc->ensureUpToDate();
    ASSERT_EQ(elements(doc->getReprRoot(), "svg:path").size(), 1u);
    auto gradients = elements(doc->getDefs()->getRepr(), "svg:meshgradient");
    ASSERT_EQ(gradients.size(), 1u);
    auto *gradient = dynamic_cast<SPMeshGradient *>(doc->getObjectByRepr(gradients[0]));
    ASSERT_TRUE(gradient);
    gradient->ensureArray();
    ASSERT_EQ(gradient->array.patch_rows(), 2u);
    ASSERT_EQ(gradient->array.patch_columns(), 3u);
    SPMeshPatchI first(&gradient->array.nodes, 0, 0), above(&gradient->array.nodes, 1, 0);
    EXPECT_TRUE(Geom::are_near(first.getPoint(1, 0), Geom::Point(15, 100), 1e-6));
    EXPECT_TRUE(Geom::are_near(above.getPoint(1, 0), Geom::Point(15, 70), 1e-6));
    auto const color = first.getColor(1);
    ASSERT_TRUE(color);
    EXPECT_NEAR(color->getValues()[0], 0.05, 1e-3);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](std::string const &s) {
        return s.find("on a mesh line across its mesh") != s.npos;
    }));
}

#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
namespace {
std::string foreign_pdf(std::string const &content, std::string const &boxes = "/MediaBox [0 0 40 30]",
                        std::string const &shading = {})
{
    auto path = fixture("", {content}, boxes, {}, {}, shading);
    gchar *bytes = nullptr;
    gsize size = 0;
    EXPECT_TRUE(g_file_get_contents(path.c_str(), &bytes, &size, nullptr));
    std::string pdf(bytes ? bytes : "", size);
    g_free(bytes);
    return pdf;
}

std::string foreign_art(std::string const &pdf, std::string const &matrix = "1 0 0 -1 0 0",
                        std::string const &origin = "0 0", std::string const &bounds = "10 20 50 50")
{
    auto data = ascii85(pdf);
    std::string lines;
    for (std::size_t i = 0; i < data.size(); i += 71) lines += "%" + data.substr(i, 71) + "\n";
    return "/ForeignObject :\n1 /Version ,\n" + matrix + " /RTransform ,\n" + origin + " /Origin ,\n" +
        bounds + " /Bounds ,\n/Data ,\n" + lines + "%~>\n;\n";
}

void foreign_fixture(std::string const &name, SPDocument *doc)
{
    if (auto const *dir = g_getenv("PROOF_AI_FOREIGN_DIAG")) {
        g_mkdir_with_parents(dir, 0700);
        auto const svg = sp_repr_save_buf(doc->getReprDoc());
        g_file_set_contents((std::string(dir) + "/" + name + ".svg").c_str(), svg.c_str(), svg.bytes(), nullptr);
    }
}
}

TEST_F(AiNativeImportTest, ForeignPdfKeepsMarginsStrokeAndAffinePlacement)
{
    // Native Bounds locate the whole PDF, not just its smaller painted bounds.
    // The path also extends past the PDF page and must be clipped there.
    auto pdf = foreign_pdf("0.2 0.4 0.6 0.1 k 5 5 12 9 re f 0 G 2 w -5 25 m 45 25 l S");
    unsigned index = 0;
    for (auto const &matrix : {std::string("1 0 0 -1 0 0"), std::string("0 1 1 0 90 10"),
                              std::string("1 0.2 0.3 -1 0 10")}) {
        auto src = records(layer(foreign_art(pdf, matrix)), {}, false);
        // Independently written outer PDF placement: native origin flips y once,
        // then RTransform. Its PDF content starts at (10,20) before that rotation.
        std::string const page = "q " + matrix + " cm 1 0 0 -1 10 -20 cm 0 0 40 30 re W n "
            "0.2 0.4 0.6 0.1 k 5 5 12 9 re f 0 G 2 w -5 25 m 45 25 l S Q";
        std::string reason;
        auto opened = Extension::Internal::open_ai_native(fixture(src, {page}), reason);
        ASSERT_TRUE(opened) << matrix << ": " << reason;
        EXPECT_LT(opened->differences[0].lightness, 0.005);
        auto groups = elements(opened->document->getReprRoot(), "svg:g");
        auto it = std::find_if(groups.begin(), groups.end(), [](auto n) { return attr(n, "proof:ai-foreign") == "pdf"; });
        ASSERT_NE(it, groups.end());
        EXPECT_EQ(attr(*it, "proof:foreign-pdf"), "data:application/pdf;base64," + [&] {
            auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(pdf.data()), pdf.size());
            std::string result(encoded); g_free(encoded); return result;
        }());
        EXPECT_TRUE(elements(*it, "svg:image").empty());
        auto paths = elements(*it, "svg:path");
        ASSERT_EQ(paths.size(), 2u);
        auto color = opened->document->getObjectByRepr(paths[0])->style->fill.getColor();
        EXPECT_EQ(color.getSpace()->getComponentType(), Type::CMYK);
        auto values = color.getValues(); ASSERT_EQ(values.size(), 4u);
        EXPECT_NEAR(values[0], 0.2, 1e-4); EXPECT_NEAR(values[3], 0.1, 1e-4);
        auto stroke = opened->document->getObjectByRepr(paths[1])->style->stroke.getColor();
        EXPECT_EQ(stroke.getSpace()->getComponentType(), Type::CMYK);
        EXPECT_EQ(stroke.getValues(), (std::vector<double>{0, 0, 0, 1}));
        foreign_fixture("foreign-" + std::to_string(index++), opened->document.get());
        auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(opened->document->getReprDoc()).raw());
        ASSERT_TRUE(reopened);
        auto wrappers = elements(reopened->getReprRoot(), "svg:g");
        auto wrapper = std::find_if(wrappers.begin(), wrappers.end(), [](auto n) { return attr(n, "proof:ai-foreign") == "pdf"; });
        ASSERT_NE(wrapper, wrappers.end());
        auto editable = elements(*wrapper, "svg:path");
        ASSERT_EQ(editable.size(), 2u);
        editable[0]->setAttribute("d", "M 6,6 H 18 V 16 H 6 Z");
        auto edited = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
        ASSERT_TRUE(edited);
        EXPECT_EQ(attr(edited->getObjectById(editable[0]->attribute("id"))->getRepr(), "d"), "M 6,6 H 18 V 16 H 6 Z");
    }
}

TEST_F(AiNativeImportTest, ForeignPdfGradientsHaveIndependentResourcesAndSurviveReopen)
{
    auto shading = [](char const *first, char const *last) {
        return std::string("<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 40 0] ") +
            "/Function << /FunctionType 2 /Domain [0 1] /C0 [" + first + "] /C1 [" + last + "] /N 1 >> /Extend [true true] >>";
    };
    auto pdf1 = foreign_pdf("/S1 sh", "/MediaBox [0 0 40 30]", shading("1 0 0", "0 0 1"));
    auto pdf2 = foreign_pdf("/S1 sh", "/MediaBox [0 0 40 30]", shading("0 1 0", "1 1 0"));
    auto b = make(records(layer(foreign_art(pdf1) + foreign_art(pdf2, "1 0 0 -1 60 0")), {}, false));
    ASSERT_TRUE(b);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](auto const &s) { return s.find("original embedded PDF") != s.npos; }));
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto stops = elements(reopened->getDefs()->getRepr(), "svg:stop");
    ASSERT_GE(stops.size(), 4u);
    auto gradients = elements(reopened->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_EQ(gradients.size(), 2u);
    EXPECT_NE(attr(gradients[0], "id"), attr(gradients[1], "id"));
    auto paths = elements(reopened->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_NE(attr(paths[0], "style"), attr(paths[1], "style"));
    foreign_fixture("foreign-gradients", reopened.get());
    stops[0]->setAttribute("style", "stop-color:#123456;stop-opacity:0.5");
    auto edited = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(edited);
    EXPECT_NE(attr(elements(edited->getDefs()->getRepr(), "svg:stop")[0], "style").find("#123456"), std::string::npos);
}

TEST_F(AiNativeImportTest, ForeignPdfNonzeroPageOriginAndHiddenUnsupportedArtwork)
{
    auto pdf = foreign_pdf("0 g -5 -15 12 9 re f", "/MediaBox [-10 -20 30 10]");
    auto src = records(layer(foreign_art(pdf)), {}, false);
    std::string reason;
    auto good = Extension::Internal::open_ai_native(fixture(src, {"0 g 15 25 12 9 re f"}), reason);
    ASSERT_TRUE(good) << reason;
    EXPECT_LT(good->differences[0].lightness, 0.002);
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(src, {"0 g 30 40 12 9 re f"}), reason));

    auto unsupported = foreign_art(foreign_pdf("BX unsupported EX"));
    auto b = make(records(layer(unsupported, "0 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb") + layer("0 g\n" + rect)));
    ASSERT_TRUE(b);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](auto const &note) {
        return note.find("hidden foreign object was left out") != note.npos;
    }));
    // The same unsupported content must be rejected even in a one-point object.
    std::string error;
    auto ai = AN::read(records(layer(foreign_art(foreign_pdf("BX unsupported EX", "/MediaBox [0 0 1 1]"),
                                              "1 0 0 -1 0 0", "0 0", "0 0 1 1"))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
}

TEST_F(AiNativeImportTest, ForeignPdfGrayFillKeepsItsBlackChannel)
{
    auto pdf = foreign_pdf("0.25 g 5 5 12 9 re f");
    auto b = make(records(layer(foreign_art(pdf))));
    ASSERT_TRUE(b);
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto paths = elements(reopened->getReprRoot(), "svg:path"); ASSERT_EQ(paths.size(), 1u);
    auto color = reopened->getObjectByRepr(paths[0])->style->fill.getColor();
    EXPECT_EQ(color.getSpace()->getComponentType(), Type::CMYK);
    auto values = color.getValues(); ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(values[0], 0); EXPECT_EQ(values[1], 0); EXPECT_EQ(values[2], 0);
    EXPECT_NEAR(values[3], 0.75, 1e-4);
}

TEST_F(AiNativeImportTest, ForeignPdfUnsupportedContentCannotHideBelowTheVisualTolerance)
{
    // Tiny foreign objects are rejected structurally even if the artboard comparison
    // would miss their omission. BX/EX must never excuse an unknown operator.
    for (auto const &content : {"0 g 5 5 5 5 re f BX /Bad Do EX", "BT /F1 12 Tf (x) Tj ET",
                               "BX unsupported EX", "/Missing sh", "0 0 1 1 cm", "Q", "1 m", "[0 0] 0 d"}) {
        auto src = records(layer(foreign_art(foreign_pdf(content))));
        std::string error;
        auto ai = AN::read(src, error); ASSERT_TRUE(ai) << error;
        EXPECT_FALSE(AN::build(*ai, error)) << content;
        EXPECT_NE(error.find("foreign object"), std::string::npos);
    }
    for (auto const &boxes : {"/MediaBox [0 0 41 30]", "/MediaBox [0 0 40 30] /Rotate 90",
                             "/MediaBox [0 0 40 30] /CropBox [1 1 39 29]",
                             "/MediaBox [0 0 40 30] /Group << /S /Transparency >>"}) {
        std::string error;
        auto ai = AN::read(records(layer(foreign_art(foreign_pdf("0 g 0 0 40 30 re f", boxes)))), error);
        ASSERT_TRUE(ai) << error;
        EXPECT_FALSE(AN::build(*ai, error)) << boxes;
    }
}

TEST_F(AiNativeImportTest, MeshGradientMatchesIndependentPdfShading)
{
    // Independently encoded PDF Type 6 Coons patch, 8-bit coordinates and colours.
    // Coordinates decode onto [10,70] x [20,80]; 85 and 170 are exactly one and two thirds.
    std::string const hex = "000000005500aa00ff55ffaaffffffffaaff55ff00aa005500"
                            "ff000000ff000000ffffffff>";
    std::string const shading = "<< /ShadingType 6 /ColorSpace /DeviceRGB /BitsPerCoordinate 8 "
        "/BitsPerComponent 8 /BitsPerFlag 8 /Decode [10 70 20 80 0 1 0 1 0 1] "
        "/Filter /ASCIIHexDecode /Length " + std::to_string(hex.size()) + " >>\nstream\n" + hex + "\nendstream";
    auto const native = records(layer(mesh_art("DeviceRGB", {"1 0 0", "0 1 0", "0 0 1", "1 1 1"})), {}, false);
    auto const file = fixture(native, {"/S1 sh"}, "/MediaBox [0 0 200 100]", {}, {}, shading);
    std::string reason;
    auto result = Extension::Internal::open_ai_native(file, reason);
    ASSERT_TRUE(result) << reason;
    ASSERT_EQ(result->differences.size(), 1u);
    EXPECT_LT(result->differences[0].lightness, 0.005);
    EXPECT_LT(result->differences[0].colour, 0.005);
}

TEST_F(AiNativeImportTest, MeshOutlineMatchesIndependentPdfAndDifferentPageIsRefused)
{
    auto native = records(layer(mesh_art("DeviceGray", {"0.3", "0.3", "0.3", "0.3"},
                                          "[10 50 10 40 10 60 1] /S X#\n")), {}, false);
    std::string reason;
    auto file = fixture(native, {"0.3 g 10 20 60 60 re f"});
    auto result = Extension::Internal::open_ai_native(file.c_str(), reason);
    ASSERT_TRUE(result) << reason;
    ASSERT_EQ(result->differences.size(), 1u);
    EXPECT_LT(result->differences[0].lightness, 0.005);
    file = fixture(native, {"0.3 g 110 20 60 60 re f"});
    result = Extension::Internal::open_ai_native(file.c_str(), reason);
    EXPECT_FALSE(result);
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
}

TEST_F(AiNativeImportTest, APostScriptFileWithoutLayersOpensFromItsRecordsUnchecked)
{
    // Photoshop's paths exported to Illustrator: Illustrator 3 PostScript, no layers, no PDF
    // part, every line ending in a bare CR. Nothing else here opens it.
    std::string const ps = "%!PS-Adobe-2.0\r%%Creator: Adobe Photoshop(TM) Pen Path Export 7.0\r"
                           "%%BoundingBox: 0 0 200 100\r%AI3_Cropmarks: 0 0 200 100\r%%EndComments\r"
                           "%%BeginProlog\r%%EndProlog\r%%BeginSetup\rAdobe_IllustratorA_AI3 /initialize get exec\r"
                           "n\r%%EndSetup\r0 0 0 1 k\r0 i 0 J 0 j 1 w 4 M []0 d\r%%Note:\r*u\r1 XR\r10 10 m\r"
                           "10 40 20 50 40 50 C\r60 50 60 10 10 10 C\rn\r*U\r%%Trailer\r%%EOF\r";
    gchar *dir = g_dir_make_tmp("proof-native-import-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    std::string const path = std::string(dir) + "/paths.ai";
    g_free(dir);
    ASSERT_TRUE(g_file_set_contents(path.c_str(), ps.data(), ps.size(), nullptr));
    std::string reason;
    auto result = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(result) << reason;
    EXPECT_TRUE(result->differences.empty());
    ASSERT_FALSE(result->notes.empty());
    EXPECT_NE(result->notes[0].find("wasn't checked against a page"), std::string::npos) << result->notes[0];
    auto *root = result->document->getReprRoot();
    // For the warning when it opens in a window.
    EXPECT_EQ(attr(root, Extension::Internal::AI_UNCHECKED_ATTRIBUTE), "postscript");
    auto const groups = elements(root, "svg:g");
    ASSERT_FALSE(groups.empty());
    EXPECT_EQ(attr(groups[0], "inkscape:label"), "Layer 1");
    auto const paths = elements(root, "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    auto pv = sp_svg_read_pathv(attr(paths[0], "d").c_str());
    ASSERT_EQ(pv.size(), 1u);
    EXPECT_EQ(pv[0].initialPoint(), Geom::Point(10, 90));
}
#endif

TEST_F(AiNativeImportTest, ImportedPagesAndArtworkKeepTheSamePhysicalCoordinates)
{
    auto source = make(records(layer("0 g\n" + rect), boards(2), false));
    ASSERT_TRUE(source);
    auto host = SPDocument::createNewDocFromMem(
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="200" viewBox="0 0 100 50">
             <rect id="existing" x="2" y="3" width="4" height="5"/>
           </svg>)"));
    ASSERT_TRUE(host);
    host->ensureUpToDate();
    auto const existing = cast<SPItem>(host->getObjectById("existing"))->documentGeometricBounds();
    auto path = elements(source->document->getReprRoot(), "svg:path").front();
    auto const original_bounds = cast<SPItem>(source->document->getObjectByRepr(path))->documentGeometricBounds();
    ASSERT_TRUE(original_bounds);
    file_import_pages(host.get(), source->document.get());
    host->ensureUpToDate();
    auto const pages = host->getPageManager().getPages();
    ASSERT_EQ(pages.size(), 3u);
    auto imported_path = elements(host->getReprRoot(), "svg:path").front();
    auto const bounds = cast<SPItem>(host->getObjectByRepr(imported_path))->documentGeometricBounds();
    ASSERT_TRUE(bounds);
    auto const imported_page = pages[1]->getDocumentRect();
    EXPECT_NEAR(imported_page.width(), 200 * 96.0 / 72, 1e-5);
    EXPECT_NEAR(imported_page.height(), 100 * 96.0 / 72, 1e-5);
    EXPECT_NEAR(bounds->left() - imported_page.left(), original_bounds->left(), 1e-5);
    EXPECT_NEAR(bounds->top() - imported_page.top(), original_bounds->top(), 1e-5);
    EXPECT_NEAR(bounds->width(), original_bounds->width(), 1e-5);
    EXPECT_NEAR(bounds->height(), original_bounds->height(), 1e-5);
    EXPECT_EQ(cast<SPItem>(host->getObjectById("existing"))->documentGeometricBounds(), existing);
    EXPECT_STREQ(pages[1]->label(), "Page 1");
    EXPECT_STREQ(pages[2]->label(), "Page 2");
}

TEST_F(AiNativeImportTest, ImportedOverlappingArtboardsRemainSeparate)
{
    auto setup = boards(2);
    setup.replace(setup.find("220 100"), 7, "0 100");
    setup.replace(setup.find("420 0"), 5, "200 0");
    auto source = make(records(layer("0 g\n" + rect), setup, false));
    ASSERT_TRUE(source);
    auto const original_pages = source->document->getPageManager().getPages();
    ASSERT_EQ(original_pages.size(), 2u);
    for (unsigned i : {0u, 1u}) {
        ASSERT_NEAR(original_pages[0]->getDocumentRect().min()[i], original_pages[1]->getDocumentRect().min()[i], 1e-5);
        ASSERT_NEAR(original_pages[0]->getDocumentRect().max()[i], original_pages[1]->getDocumentRect().max()[i], 1e-5);
    }
    auto host = SPDocument::createNewDoc(nullptr, true);
    ASSERT_TRUE(host);
    file_import_pages(host.get(), source->document.get());
    host->ensureUpToDate();
    auto const pages = host->getPageManager().getPages();
    ASSERT_EQ(pages.size(), 3u);
    for (unsigned i : {0u, 1u}) {
        EXPECT_NEAR(pages[1]->getDocumentRect().min()[i], pages[2]->getDocumentRect().min()[i], 1e-5);
        EXPECT_NEAR(pages[1]->getDocumentRect().max()[i], pages[2]->getDocumentRect().max()[i], 1e-5);
    }
    EXPECT_STREQ(pages[1]->label(), "Page 1");
    EXPECT_STREQ(pages[2]->label(), "Page 2");
}

TEST_F(AiNativeImportTest, IllustratorDropOpensBlankCanvasButPreservesExistingArtwork)
{
    auto blank = SPDocument::createNewDocFromMem(
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><g><g/></g></svg>)"));
    ASSERT_TRUE(blank);
    EXPECT_TRUE(file_drop_opens_document(blank.get(), "Map.ai"));
    EXPECT_TRUE(file_drop_opens_document(blank.get(), "Map.AI"));
    EXPECT_FALSE(file_drop_opens_document(blank.get(), "Photo.png"));
    auto existing = SPDocument::createNewDocFromMem(
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><g style="display:none"><rect width="10" height="10"/></g></svg>)"));
    ASSERT_TRUE(existing);
    EXPECT_FALSE(file_drop_opens_document(existing.get(), "Map.ai"));
}

TEST_F(AiNativeImportTest, ProcessChannelsAndBlackOnlyGray)
{
    auto b = make(records(layer("0.05 1 0.45 0.22 k\n" + rect + "0.25 g\n" + rect)));
    ASSERT_TRUE(b);
    auto *doc = b->document.get();
    EXPECT_EQ(Colors::DocumentColors::mode(doc), Type::CMYK);
    auto paths = elements(doc->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 2u);
    auto a = doc->getObjectByRepr(paths[0])->style->fill.getColor();
    auto g = doc->getObjectByRepr(paths[1])->style->fill.getColor();
    EXPECT_EQ(a.getSpace()->getComponentType(), Type::CMYK);
    EXPECT_EQ(a.getValues(), (std::vector<double>{0.05, 1, 0.45, 0.22}));
    EXPECT_EQ(g.getValues(), (std::vector<double>{0, 0, 0, 0.75}));
    if (auto space = Colors::DocumentColors::assignedSpace(doc)) EXPECT_EQ(a.getSpace(), space);
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto again = elements(reopened->getReprRoot(), "svg:path");
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(reopened->getObjectByRepr(again[0])->style->fill.getColor().getValues(), a.getValues());
    EXPECT_EQ(reopened->getObjectByRepr(again[1])->style->fill.getColor().getValues(), g.getValues());
}

TEST_F(AiNativeImportTest, LabSpotTintAndRegistration)
{
    auto b = make(records(layer("0 0 0 0 89 0 76 (PANTONE Test) 0.65 2 Xx\n" + rect +
                                "1 1 1 1 ([Registration]) 0 Xs\n" + rect)));
    ASSERT_TRUE(b);
    auto *defs = b->document->getDefs()->getRepr();
    auto swatches = elements(defs, "svg:linearGradient");
    ASSERT_EQ(swatches.size(), 3u);
    auto spot = SpotInk::read(swatches[2]);
    ASSERT_TRUE(spot);
    EXPECT_EQ(spot->name, "PANTONE Test");
    EXPECT_NEAR(spot->tint, 0.35, 1e-6);
    EXPECT_EQ(spot->alternate.getSpace()->getType(), Type::LAB);
    EXPECT_NEAR(spot->alternate[0], 0.89, 1e-6);
    EXPECT_NEAR(spot->alternate[1], 128.0 / 255, 1e-6);
    EXPECT_NEAR(spot->alternate[2], 204.0 / 255, 1e-6);
    auto reg = SpotInk::read(swatches[1]);
    ASSERT_TRUE(reg);
    EXPECT_EQ(reg->name, "All");
    EXPECT_EQ(attr(swatches[1], "inkscape:label"), "[Registration]");
    EXPECT_EQ(SpotInk::label_for("All", 0.3), "[Registration] 30%");
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto again = elements(reopened->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_EQ(again.size(), 3u);
    auto ink = SpotInk::read(again[2]);
    ASSERT_TRUE(ink);
    EXPECT_TRUE(ink->alternate.isClose(spot->alternate, 1e-6));
    EXPECT_DOUBLE_EQ(ink->tint, spot->tint);
}

TEST_F(AiNativeImportTest, GlobalProcessSwatches)
{
    auto b = make(records(layer("0.1 0.9 0.8 0 (Brand Red) 0.5 0 Xk\n" + rect)));
    ASSERT_TRUE(b);
    auto swatches = elements(b->document->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_EQ(swatches.size(), 2u);
    EXPECT_FALSE(SpotInk::read(swatches[1]));
    EXPECT_EQ(attr(swatches[1], "inkscape:swatch"), "solid");
    auto stops = elements(swatches[1], "svg:stop");
    ASSERT_EQ(stops.size(), 1u);
    auto style = attr(stops[0], "style");
    EXPECT_NE(style.find("0.05"), std::string::npos);
    EXPECT_NE(style.find("0.45"), std::string::npos);
    EXPECT_NE(style.find("0.4"), std::string::npos);
}

TEST_F(AiNativeImportTest, EndpointPaddingUsesIllustratorsFirstEndStop)
{
    std::string setup = "%AI5_BeginGradient: (G)\n(G) 0 3 Bd\n[\n"
                        "0 0 50 100 Bs\n0.25 0 50 100 Bs\n1 0 50 0 Bs\nBD\n%AI5_EndGradient\n";
    auto b = make(records(layer("0 g\n10 20 m 90 20 L 90 60 L 10 60 L h\nBb\n"
                                "0 (G) 10 20 0 80 1 0 0 1 0 0 Bg\nf\n0 BB\n"), setup, false));
    ASSERT_TRUE(b);
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("unused gradient stop at 100% was left out"), std::string::npos);
    auto vectors = elements(b->document->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_FALSE(vectors.empty());
    auto stops = elements(vectors[0], "svg:stop");
    ASSERT_EQ(stops.size(), 2u);
    EXPECT_EQ(attr(stops.back(), "offset"), "1");
    // Linear definitions run last first: .25 is the first visible stop at 100%.
    auto color = b->document->getObjectByRepr(stops.back())->style->stop_color.getColor().converted(Type::RGB);
    ASSERT_TRUE(color);
    EXPECT_NEAR((*color)[0], .25, .001);
}

TEST_F(AiNativeImportTest, GradientMidpointUsesPowerCurve)
{
    std::string setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n1 0 50 100 Bs\n0 0 80 0 Bs\nBD\n%AI5_EndGradient\n";
    auto b = make(records(layer("0 g\n10 20 m 90 20 L 90 60 L 10 60 L h\nBb\n0 (G) 10 20 0 80 1 0 0 1 0 0 Bg\nf\n0 BB\n"), setup, false));
    ASSERT_TRUE(b);
    auto vectors = elements(b->document->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_EQ(vectors.size(), 2u);
    auto stops = elements(vectors[0], "svg:stop");
    ASSERT_GT(stops.size(), 3u);
    for (auto s : stops) {
        if (attr(s, "proof:midpoint-sample") != "true") continue;
        auto u = g_ascii_strtod(s->attribute("offset"), nullptr);
        auto *obj = b->document->getObjectByRepr(s);
        auto color = obj->style->stop_color.getColor().converted(Type::RGB);
        ASSERT_TRUE(color);
        EXPECT_NEAR((*color)[0], std::pow(u, std::log(0.5) / std::log(0.8)), 0.005);
    }
}

TEST_F(AiNativeImportTest, MidpointBetweenModelsBlendsInTheDocumentsMode)
{
    // Cyan in CMYK at 0, red in RGB at 100, the midpoint at 80. Illustrator blends stops of
    // different models in the document's mode: its ramp of the owner's one such gradient, in an
    // RGB document, follows one curve in R, G and B.
    std::string setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0 1 1 0 1 0 0 2 50 100 Bs\n1 0 0 0 1 80 0 Bs\nBD\n"
                        "%AI5_EndGradient\n";
    double const n = std::log(0.5) / std::log(0.8);
    for (bool cmyk : {true, false}) {
        SCOPED_TRACE(cmyk ? "CMYK document" : "RGB document");
        auto b = make(records(layer("0 g\n10 20 m 90 20 L 90 60 L 10 60 L h\nBb\n0 (G) 10 20 0 80 1 0 0 1 0 0 Bg\nf\n0 BB\n"),
                              setup, cmyk));
        ASSERT_TRUE(b);
        auto vectors = elements(b->document->getDefs()->getRepr(), "svg:linearGradient");
        ASSERT_FALSE(vectors.empty());
        auto stops = elements(vectors[0], "svg:stop");
        ASSERT_GT(stops.size(), 3u);
        auto colour_of = [&](XML::Node *s) { return b->document->getObjectByRepr(s)->style->stop_color.getColor(); };
        auto const from = colour_of(stops.front()), to = colour_of(stops.back());
        auto const mode = cmyk ? Type::CMYK : Type::RGB;
        auto values = [&](Colors::Color const &c) {
            return c.getSpace()->getComponentType() == mode ? c.getValues() : c.converted(mode)->getValues();
        };
        auto const a = values(from), z = values(to);
        int samples = 0;
        for (auto s : stops) {
            if (attr(s, "proof:midpoint-sample") != "true") continue;
            ++samples;
            auto const u = g_ascii_strtod(s->attribute("offset"), nullptr);
            auto const colour = colour_of(s);
            ASSERT_EQ(colour.getSpace()->getComponentType(), mode);
            double const v = std::pow(u, n);
            for (std::size_t k = 0; k < (cmyk ? 4u : 3u); ++k) EXPECT_NEAR(colour[k], a[k] + (z[k] - a[k]) * v, 0.005);
        }
        EXPECT_GT(samples, 2);
    }
}

TEST_F(AiNativeImportTest, GradientMatricesPlaceTheWholeRamp)
{
    // As Illustrator writes them since CS4: Bg's values relative to the object's bounds, and
    // matrices from the gradient's unit space onto the art. Xm spans the whole ramp; Bm, for a
    // linear gradient, only the first two stops (10% to 60% here).
    std::string setup = "%AI5_BeginGradient: (L)\n(L) 0 3 Bd\n[\n0 0 50 100 Bs\n0.5 0 50 60 Bs\n1 0 50 10 Bs\nBD\n"
                        "%AI5_EndGradient\n%AI5_BeginGradient: (R)\n(R) 1 2 Bd\n[\n0 0 50 100 Bs\n1 0 50 0 Bs\nBD\n"
                        "%AI5_EndGradient\n";
    auto const path = std::string("10 20 m 90 20 L 90 60 L 10 60 L h\n");
    auto const turned = "Bb\n1 (L) -0.02 0 -90 1.5 1 0 0 1 0 0 1 Bg\n0 -60 -80 0 90 60 Xm\n0 -30 -80 0 90 54 Bm\nf\n0 BB\n";
    auto const older = "Bb\n1 (L) 0 0 0 1 1 0 0 1 0 0 Bg\n40 0 0 -40 18 60 Bm\nf\n0 BB\n";
    auto const round = "Bb\n3 -4 -30 0.5 Bh\n1 (R) -0.2 -0.7 0 0.9 0.6 0.8 -0.8 0.6 5000 -1400 1 Bg\n"
                       "20 0 0 -40 50 40 Bm\nf\n0 BB\n";
    auto b = make(records(layer("0 g\n" + path + turned + path + older + path + round), setup, false));
    ASSERT_TRUE(b);
    auto const to_doc = Geom::Affine(1, 0, 0, -1, 0, 100); // The 200 x 100 artboard, y turned down.
    auto matrix = [](XML::Node *n) {
        Geom::Affine m;
        EXPECT_TRUE(sp_svg_transform_read(n->attribute("gradientTransform"), &m));
        return m;
    };
    auto number = [](XML::Node *n, char const *key) { return g_ascii_strtod(attr(n, key).c_str(), nullptr); };
    std::vector<XML::Node *> placed;
    for (auto *g : elements(b->document->getDefs()->getRepr(), "svg:linearGradient")) {
        if (g->attribute("gradientTransform")) placed.push_back(g);
    }
    ASSERT_EQ(placed.size(), 2u);
    // Xm alone places it: the ramp is the unit of x, whatever Bg's angle and length say.
    EXPECT_TRUE(Geom::are_near(matrix(placed[0]), Geom::Affine(0, -60, -80, 0, 90, 60) * to_doc, 1e-6));
    EXPECT_NEAR(number(placed[0], "x1"), 0.0, 1e-9);
    EXPECT_NEAR(number(placed[0], "x2"), 1.0, 1e-9);
    EXPECT_NEAR(number(placed[0], "y2"), 0.0, 1e-9);
    // Bm alone: its unit is 10% to 60% of the ramp, so the ramp runs from -0.2 to 1.8.
    EXPECT_TRUE(Geom::are_near(matrix(placed[1]), Geom::Affine(40, 0, 0, -40, 18, 60) * to_doc, 1e-6));
    EXPECT_NEAR(number(placed[1], "x1"), -0.2, 1e-9);
    EXPECT_NEAR(number(placed[1], "x2"), 1.8, 1e-9);
    // Radial: the unit circle through Bm, and Bh's angle and length for the highlight.
    auto radial = elements(b->document->getDefs()->getRepr(), "svg:radialGradient");
    ASSERT_EQ(radial.size(), 1u);
    EXPECT_TRUE(Geom::are_near(matrix(radial[0]), Geom::Affine(20, 0, 0, -40, 50, 40) * to_doc, 1e-6));
    EXPECT_NEAR(number(radial[0], "cx"), 0.0, 1e-9);
    EXPECT_NEAR(number(radial[0], "cy"), 0.0, 1e-9);
    EXPECT_NEAR(number(radial[0], "r"), 1.0, 1e-9);
    EXPECT_NEAR(number(radial[0], "fx"), 0.5 * std::cos(-M_PI / 6), 1e-6);
    EXPECT_NEAR(number(radial[0], "fy"), -0.5 * std::sin(-M_PI / 6), 1e-6);
}

TEST_F(AiNativeImportTest, CmykImageKeepsPngAndExactTiffSamples)
{
    std::string art = "%AI5_BeginRaster\n() 1 XG\n/DeviceCMYK XN\n[1 0 0 1 10 20] 0 0 2 1 2 1 8 4 0 0 1 0 0\n"
                      "%%BeginData: 12\nXI\n\x10\x20\x30\x40\x50\x60\x70\x80\n%%EndData\nXH\n%AI5_EndRaster\n";
    auto b = make(records(layer(art)));
    ASSERT_TRUE(b);
    auto images = elements(b->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(attr(images[0], "xlink:href").find("data:image/png;base64,"), 0u);
    auto data = attr(images[0], "proof:samples");
    ASSERT_EQ(data.find("data:image/tiff;base64,"), 0u);
    gsize size = 0;
    auto *bytes = g_base64_decode(data.substr(data.find(',') + 1).c_str(), &size);
    ASSERT_GT(size, 16u);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(bytes), 4), std::string("II\x2a\0", 4));
    auto u16 = [&](int at) { return unsigned(bytes[at]) | unsigned(bytes[at + 1]) << 8; };
    auto u32 = [&](int at) { return u16(at) | u16(at + 2) << 16; };
    unsigned offset = 0, length = 0;
    for (unsigned i = 0; i < u16(8); ++i) {
        int const at = 10 + i * 12;
        if (u16(at) == 273) offset = u32(at + 8);
        if (u16(at) == 279) length = u32(at + 8);
    }
    ASSERT_LE(offset + length, size);
    unsigned char raw[8];
    uLongf len = sizeof raw;
    EXPECT_EQ(uncompress(raw, &len, bytes + offset, length), Z_OK);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(raw), len), std::string("\x10\x20\x30\x40\x50\x60\x70\x80", 8));
    g_free(bytes);
    auto m = Geom::Affine();
    EXPECT_TRUE(sp_svg_transform_read(images[0]->attribute("transform"), &m));
    EXPECT_EQ(Geom::Point(0, 0) * m, Geom::Point(10, 80));
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto again = elements(reopened->getReprRoot(), "svg:image");
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(attr(again[0], "proof:samples"), data);
    EXPECT_EQ(attr(again[0], "xlink:href"), attr(images[0], "xlink:href"));
}

TEST_F(AiNativeImportTest, TurnedImagesLieWhereTheirMatrixPutsThem)
{
    // Two pixels by one, each two points across, turned a quarter each way as Illustrator
    // writes the corner tiles of a pattern brush. On a 100 point artboard, y counted down.
    auto box = [&](std::string const &matrix) {
        std::string art = "%AI5_BeginRaster\n() 1 XG\n/DeviceCMYK XN\n[" + matrix + "] 0 0 2 1 2 1 8 4 0 0 1 0 0\n"
                          "%%BeginData: 12\nXI\n\x10\x20\x30\x40\x50\x60\x70\x80\n%%EndData\nXH\n%AI5_EndRaster\n";
        auto b = make(records(layer(art)));
        EXPECT_TRUE(b);
        auto images = b ? elements(b->document->getReprRoot(), "svg:image") : std::vector<XML::Node *>();
        EXPECT_EQ(images.size(), 1u);
        auto m = Geom::Affine();
        if (images.empty() || !sp_svg_transform_read(images[0]->attribute("transform"), &m)) return std::array<Geom::Point, 2>{};
        // Where the picture's first pixel starts, and where its last one ends.
        return std::array<Geom::Point, 2>{Geom::Point(0, 0) * m, Geom::Point(2, 1) * m};
    };
    auto near = [](Geom::Point const &a, Geom::Point const &b) { return Geom::are_near(a, b, 1e-9); };
    auto upright = box("2 0 0 -2 50 40");
    EXPECT_TRUE(near(upright[0], {50, 60}) && near(upright[1], {54, 58})) << upright[0] << " " << upright[1];
    auto half = box("-2 0 0 2 50 40");
    EXPECT_TRUE(near(half[0], {50, 60}) && near(half[1], {46, 62})) << half[0] << " " << half[1];
    auto quarter = box("0 2 2 0 50 40");
    EXPECT_TRUE(near(quarter[0], {50, 60}) && near(quarter[1], {48, 56})) << quarter[0] << " " << quarter[1];
    auto other = box("0 -2 -2 0 50 40");
    EXPECT_TRUE(near(other[0], {50, 60}) && near(other[1], {52, 64})) << other[0] << " " << other[1];
}

TEST_F(AiNativeImportTest, AppearanceAndTurnedBox)
{
    auto b = make(records(layer("1 Xw 1 A 1 O 1 R 0 g 0 G\n1 0.4 1 2 0 Xy\n10 20 m 90 60 L B\n"
                                "%_/ArtDictionary :\n%_(0.5) /String (BBAccumRotation) ,\n%_;\n")));
    ASSERT_TRUE(b);
    auto paths = elements(b->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    auto style = attr(paths[0], "style");
    EXPECT_NE(style.find("opacity:0.4"), std::string::npos);
    EXPECT_NE(style.find("mix-blend-mode:multiply"), std::string::npos);
    EXPECT_NE(style.find("isolation:isolate"), std::string::npos);
    EXPECT_NE(style.find("display:none"), std::string::npos);
    EXPECT_EQ(attr(paths[0], "sodipodi:insensitive"), "true");
    EXPECT_EQ(attr(paths[0], "proof:overprint"), "fill stroke");
    EXPECT_EQ(attr(paths[0], "proof:knockout"), "neutral");
    EXPECT_NEAR(g_ascii_strtod(paths[0]->attribute("proof:box-angle"), nullptr), -0.5 * 180 / M_PI, 1e-5);
}

TEST_F(AiNativeImportTest, ClippingAndCompoundPaths)
{
    auto b = make(records(layer("q\n0 g\n10 20 m 90 20 L 90 60 L h W n\n" + rect + "Q\n*u\n1 XR\n" + rect + rect + "*U\n")));
    ASSERT_TRUE(b);
    auto clips = elements(b->document->getDefs()->getRepr(), "svg:clipPath");
    ASSERT_EQ(clips.size(), 1u);
    EXPECT_EQ(attr(clips[0], "clipPathUnits"), "userSpaceOnUse");
    auto groups = elements(b->document->getReprRoot(), "svg:g");
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_FALSE(attr(groups[1], "clip-path").empty());
    auto paths = elements(groups[0], "svg:path");
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(sp_svg_read_pathv(paths[1]->attribute("d")).size(), 2u);
    EXPECT_NE(attr(paths[1], "style").find("fill-rule:evenodd"), std::string::npos);
}

TEST_F(AiNativeImportTest, ACompoundPathWithItsPathsInAGroupIsPainted)
{
    // As Illustrator writes a compound path that has a style of its own: a frame with a hole.
    auto b = make(records(layer("*u\nu\n1 D\n0 0 0 1 k\n0 0 m 0 100 L 100 100 L 100 0 L 0 0 L\nf\n0 D\n"
                                "20 20 m 80 20 L 80 80 L 20 80 L 20 20 L\nf\nU\n9 () XW\n*U\n")));
    ASSERT_TRUE(b);
    auto paths = elements(b->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    auto const style = attr(paths[0], "style");
    EXPECT_EQ(style.find("fill:none"), std::string::npos) << style;
    EXPECT_NE(style.find("fill:"), std::string::npos) << style;
    // One path of two outlines.
    auto const d = attr(paths[0], "d");
    EXPECT_EQ(std::count_if(d.begin(), d.end(), [](char c) { return c == 'm' || c == 'M'; }), 2) << d;
}

TEST_F(AiNativeImportTest, VectorOpacityMaskIsEditableAndSurvivesReopen)
{
    // The mask is in art coordinates and uses coverage, even in a CMYK document.
    auto b = make(records(layer("0 0 0 1 k\n" + rect + vector_mask("0 0 0 0.5 k\n" + rect))));
    ASSERT_TRUE(b);
    auto masks = elements(b->document->getDefs()->getRepr(), "svg:mask"); ASSERT_EQ(masks.size(), 1u);
    EXPECT_EQ(attr(masks[0], "maskUnits"), "userSpaceOnUse");
    EXPECT_EQ(attr(masks[0], "x"), "10");
    EXPECT_EQ(attr(masks[0], "y"), "40");
    auto paths = elements(masks[0], "svg:path"); ASSERT_EQ(paths.size(), 1u);
    auto *path = b->document->getObjectByRepr(paths[0]); ASSERT_TRUE(path);
    EXPECT_EQ(path->style->fill.getColor().getSpace()->getComponentType(), Type::RGB);
    EXPECT_EQ(path->style->fill.getColor().getValues(), (std::vector<double>{0.5, 0.5, 0.5}));
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto again = elements(reopened->getDefs()->getRepr(), "svg:mask"); ASSERT_EQ(again.size(), 1u);
    auto edited = elements(again[0], "svg:path"); ASSERT_EQ(edited.size(), 1u);
    auto const space = reopened->getObjectByRepr(edited[0])->style->fill.getColor().getSpace();
    edited[0]->setAttribute("style", "fill:" + Colors::Color(space, {1, 1, 1}).toString(false));
    reopened->ensureUpToDate();
    auto saved = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(saved);
    auto final_mask = elements(saved->getDefs()->getRepr(), "svg:mask"); ASSERT_EQ(final_mask.size(), 1u);
    auto final_paths = elements(final_mask[0], "svg:path"); ASSERT_EQ(final_paths.size(), 1u);
    EXPECT_EQ(saved->getObjectByRepr(final_paths[0])->style->fill.getColor().getValues(),
              (std::vector<double>{1, 1, 1}));
}

TEST_F(AiNativeImportTest, OpacityMaskGradientsHaveTheirOwnCoverageColorsAndSurviveReopen)
{
    // The same CMYK gradient paints ordinary artwork and a mask. Only the mask's
    // vector belongs to embedded sRGB, so its K values remain exact coverage.
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0 0 0 1 1 50 100 Bs\n"
                              "0 0 0 0 1 50 0 Bs\nBD\n%AI5_EndGradient\n";
    auto const gradient = "10 20 m 90 20 L 90 60 L 10 60 L h\nBb\n0 (G) 10 20 0 80 1 0 0 1 0 0 Bg\nf\n0 BB\n";
    auto const mask_art = std::string(gradient) + vector_mask("", 0) + "0 0 0 0.5 k\n" + rect;
    auto b = make(records(layer(std::string(gradient) + "0 g\n" + rect + vector_mask(mask_art, 0) + gradient), setup));
    ASSERT_TRUE(b);
    auto check = [&](SPDocument *doc) {
        auto masks = elements(doc->getDefs()->getRepr(), "svg:mask"); ASSERT_EQ(masks.size(), 2u);
        unsigned rgb = 0, cmyk = 0;
        for (auto stop : elements(doc->getDefs()->getRepr(), "svg:stop")) {
            auto const color = doc->getObjectByRepr(stop)->style->stop_color.getColor();
            auto const type = color.getSpace()->getComponentType();
            if (type == Type::CMYK) ++cmyk;
            else if (type == Type::RGB) ++rgb;
        }
        EXPECT_EQ(rgb, 2u);
        EXPECT_EQ(cmyk, 2u);
        for (auto mask : masks) {
            auto backgrounds = elements(mask, "svg:rect"); ASSERT_EQ(backgrounds.size(), 1u);
            EXPECT_EQ(attr(backgrounds[0], "proof:mask-backdrop"), "white");
        }
        // Siblings after a nested mask must still use coverage colours.
        auto paths = elements(masks[1], "svg:path"); ASSERT_EQ(paths.size(), 2u);
        auto color = doc->getObjectByRepr(paths.back())->style->fill.getColor();
        EXPECT_EQ(color.getSpace()->getComponentType(), Type::RGB);
        EXPECT_EQ(color.getValues(), (std::vector<double>{0.5, 0.5, 0.5}));
    };
    check(b->document.get());
    if (auto const *dir = g_getenv("PROOF_AI_MASK_DIAG")) {
        g_mkdir_with_parents(dir, 0700);
        auto const svg = sp_repr_save_buf(b->document->getReprDoc());
        g_file_set_contents((std::string(dir) + "/cmyk-gradient.svg").c_str(), svg.c_str(), svg.bytes(), nullptr);
    }
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    check(reopened.get());
    auto stops = elements(reopened->getDefs()->getRepr(), "svg:stop");
    auto const space = reopened->getObjectByRepr(stops[2])->style->stop_color.getColor().getSpace();
    stops[2]->setAttribute("style", "stop-color:" + Colors::Color(space, {0.25, 0.25, 0.25}).toString(false));
    reopened->ensureUpToDate();
    auto edited = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(edited);
    auto final = elements(edited->getDefs()->getRepr(), "svg:stop");
    EXPECT_EQ(edited->getObjectByRepr(final[2])->style->stop_color.getColor().getValues(),
              (std::vector<double>{0.25, 0.25, 0.25}));
}

TEST_F(AiNativeImportTest, VisibleTextRefusedHiddenTextNoted)
{
    auto source = records(layer("/AI11Text :\n3 /StoryIndex ,\n;\n"));
    std::string error;
    auto ai = AN::read(source, error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("visible text"), std::string::npos);
    ai->layers[0].visible = false;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("text object"), std::string::npos);
}

TEST_F(AiNativeImportTest, PointTextReadsUnicodeRunLengthsAndCanvasPlacement)
{
    auto data = text_setup(text_document("(\\376\\377\\000A\\330\\075\\336\\000\\000\\015)", 4));
    auto texts = AN::read_text_document(data, Geom::Point(100, 50));
    ASSERT_TRUE(texts->error.empty()) << texts->error;
    ASSERT_EQ(texts->stories.size(), 1u);
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    auto const &story = *texts->stories[0];
    ASSERT_EQ(story.lines.size(), 1u);
    ASSERT_EQ(story.lines[0].runs.size(), 1u);
    EXPECT_EQ(story.lines[0].runs[0].text, "A\xf0\x9f\x98\x80");
    auto point = story.lines[0].origin * story.to_art;
    EXPECT_NEAR(point.x(), 10, 1e-6);
    EXPECT_NEAR(point.y(), 60, 1e-6);
    auto const &s = story.lines[0].runs[0].style;
    ASSERT_TRUE(s.fill);
    EXPECT_EQ(s.fill->model, AN::Color::Model::Cmyk);
    EXPECT_EQ(s.fill->v, (std::array<double, 4>{0.25, 0.5, 0.75, 0.1}));

    // The section's own ruler origin says where the art lies on the canvas, and counts
    // before the document's centre. One that agrees with the centre changes nothing; one of
    // an artboard that isn't in the middle of the canvas moves the type with it.
    auto with_origin = [&](std::string const &entry) {
        auto moved = data;
        auto const end = moved.find("%AI11_EndTextDocument");
        EXPECT_NE(end, std::string::npos);
        moved.insert(end, entry);
        return moved;
    };
    auto where = [](std::shared_ptr<AN::TextDocument const> const &document) {
        EXPECT_TRUE(document->error.empty()) << document->error;
        EXPECT_EQ(document->stories.size(), 1u);
        if (document->stories.size() != 1 || !document->stories[0]) return Geom::Point(-1, -1);
        return document->stories[0]->lines[0].origin * document->stories[0]->to_art;
    };
    EXPECT_TRUE(Geom::are_near(where(AN::read_text_document(with_origin("8091.5 8241.5 /RulerOrigin ,\n"), Geom::Point(100, 50))),
                               Geom::Point(10, 60), 1e-6));
    EXPECT_TRUE(Geom::are_near(where(AN::read_text_document(with_origin("8071.5 8251.5 /RulerOrigin ,\n"), Geom::Point(100, 50))),
                               Geom::Point(30, 70), 1e-6));
    EXPECT_TRUE(Geom::are_near(where(AN::read_text_document(with_origin("%_8071.5 8251.5 /RulerOrigin ,\n"), {})),
                               Geom::Point(30, 70), 1e-6));
    // What isn't two numbers and the name isn't that entry.
    EXPECT_TRUE(Geom::are_near(where(AN::read_text_document(with_origin("(a) 1 2 /RulerOrigin ,\n"), Geom::Point(100, 50))),
                               Geom::Point(10, 60), 1e-6));
    EXPECT_FALSE(AN::read_text_document(data, {})->error.empty());
}

TEST_F(AiNativeImportTest, PointTextRefusesBrokenUnicodeFramesStylesAndEncodings)
{
    for (auto const &doc : {
             text_document("(\\376\\377\\000A\\330\\075\\336\\000\\000\\015)", 2),
             text_document("(Hi\\r)", 3, "ArialMT", "/6 0"),
             text_document("(Hi\\r)", 3, "ArialMT", "/9 1e9"),
             // Type on a path, and a frame carrying what isn't known.
             text_document("(Hi\\r)", 3, "ArialMT", {}, "/0 2 /2 [1 0 0 1 -90 -10]"),
             text_document("(Hi\\r)", 3, "ArialMT", {}, "/7 18 /2 [1 0 0 1 -90 -10]"),
             text_document("(Hi\\r)", 1, "ArialMT", {}, "/2 [1 0 0 1 -90 -10]",
                           "<< /1 2 /0 << /0 << /6 << /6 1.2 >> >> >> >>"),
             text_document("(Hi\\r)", 3, "ArialMT", "/2 true"),
             // A capitals setting and a position that aren't known, and lines under or
             // through type other than the plain ones.
             text_document("(Hi\\r)", 3, "ArialMT", "/12 3"),
             text_document("(Hi\\r)", 3, "ArialMT", "/13 3"),
             text_document("(Hi\\r)", 3, "ArialMT", "/15 2"),
             text_document("(Hi\\r)", 3, "ArialMT", "/16 2"),
             text_document("(Hi\\r)", 3, "ArialMT", {}, "/1 [1 2]"),
             text_document("(Hi\\r)", 3, "ArialMT", {}, "/2 [1 0 0 0 0 0]")}) {
        auto texts = AN::read_text_document(text_setup(doc), Geom::Point(100, 50));
        ASSERT_EQ(texts->stories.size(), 1u);
        EXPECT_FALSE(texts->stories[0]); EXPECT_FALSE(texts->errors[0].empty());
    }
    EXPECT_FALSE(AN::read_text_document(text_setup(text_document()), {})->error.empty());
    EXPECT_FALSE(AN::read_text_document("%AI11_BeginTextDocument\n/ASCII85Decode , z~>\n%AI11_EndTextDocument", Geom::Point(0, 0))->error.empty());
    EXPECT_FALSE(AN::read_text_document(text_setup("/0 " + std::string(100, '[')), Geom::Point(0, 0))->error.empty());
    auto bad = text_setup(text_document());
    bad.replace(bad.find("~>"), 2, "~!");
    EXPECT_FALSE(AN::read_text_document(bad, Geom::Point(0, 0))->error.empty());
}

TEST_F(AiNativeImportTest, StoriesWithoutCharacterRunsUseDocumentDefaults)
{
    // Count UTF-16 units, including a supplementary character and the paragraph end.
    for (auto const &document : {text_document(),
             text_document("(\\376\\377\\000A\\330\\075\\336\\000\\000\\015)", 4)}) {
        auto explicit_runs = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
        auto inherited = AN::read_text_document(text_setup(without_character_runs(document)), Geom::Point(100, 50));
        ASSERT_EQ(inherited->stories.size(), 1u) << inherited->error;
        ASSERT_TRUE(explicit_runs->stories[0]) << explicit_runs->errors[0];
        ASSERT_TRUE(inherited->stories[0]) << inherited->errors[0];
        EXPECT_TRUE(AN::same_drawing(*explicit_runs->stories[0], *inherited->stories[0]));
    }
    // A present but incomplete/empty run list is not a request to inherit defaults.
    for (auto const &runs : {" /6 << >>", " /6 << /0 [] >>"}) {
        auto document = without_character_runs(text_document());
        document.insert(document.find(" >> /1 << /0 ["), runs);
        auto bad = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
        ASSERT_EQ(bad->stories.size(), 1u);
        EXPECT_FALSE(bad->stories[0]);
    }
    auto tabs = AN::read_text_document(text_setup(text_document("(A\\tB\\r)", 4)), Geom::Point(100, 50));
    ASSERT_EQ(tabs->stories.size(), 1u);
    ASSERT_TRUE(tabs->stories[0]) << tabs->errors[0];
    ASSERT_TRUE(tabs->stories[0]->lines[0].tabs);
    EXPECT_EQ(tabs->stories[0]->lines[0].tabs->interval, 36);
}

TEST_F(AiNativeImportTest, ParagraphTabStopsResolveDefaultsStylesAndScale)
{
    for (double scale : {0.5, 1.0, 2.0}) {
        auto document = text_document("(A\\tB\\r)", 4, "ArialMT", "/6 " + std::to_string(scale));
        document = with_paragraphs(document,
            "<< /0 << /5 << /31 60 /30 << /0 [ << /0 50 /1 0 /2 () /3 (:) >> ] >> >> >> >>",
            "<< /1 4 /0 << /0 0 >> >>", "/31 36");
        auto texts = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
        ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
        auto const &line = texts->stories[0]->lines[0];
        ASSERT_TRUE(line.tabs);
        EXPECT_DOUBLE_EQ(line.tabs->origin, 8191.5 / scale);
        EXPECT_DOUBLE_EQ(line.tabs->interval, 60 / scale);
        ASSERT_EQ(line.tabs->positions.size(), 1u);
        EXPECT_DOUBLE_EQ(line.tabs->positions[0], 50 / scale);
        EXPECT_EQ(line.runs[0].text, "A\tB");
    }
}

TEST_F(AiNativeImportTest, ParagraphTabStopsRefuseUnsupportedAndMalformedSettings)
{
    for (auto features : {"/31 0", "/31 -36", "/31 1e99", "/30 []", "/30 << /9 [] >>",
                          "/30 << /0 [ << /0 50 /1 1 >> ] >>",
                          "/30 << /0 [ << /0 50 /1 2 >> ] >>",
                          "/30 << /0 [ << /0 50 /1 3 /3 (:) >> ] >>",
                          "/30 << /0 [ << /0 50 /1 0 /2 (.) >> ] >>",
                          "/30 << /0 [ << /0 50 /1 0 /4 true >> ] >>",
                          "/30 << /0 [ << /0 -1 /1 0 >> ] >>",
                          "/30 << /0 [ << /0 50 /1 0 >> << /0 40 /1 0 >> ] >>",
                          "/0 1", "/0 2", "/35 1"}) {
        auto document = with_paragraphs(text_document("(A\\tB\\r)", 4), {},
            "<< /1 4 /0 << /0 << /5 << " + std::string(features) + " >> >> >> >>");
        auto texts = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
        ASSERT_EQ(texts->stories.size(), 1u);
        EXPECT_FALSE(texts->stories[0]) << features;
        EXPECT_FALSE(texts->errors[0].empty());
    }
    auto consecutive = AN::read_text_document(text_setup(text_document("(A\\t\\tB\\r)", 5)), Geom::Point(100, 50));
    ASSERT_TRUE(consecutive->stories[0]) << consecutive->errors[0];
    EXPECT_EQ(consecutive->stories[0]->lines[0].runs[0].text, "A\t\tB");
}

TEST_F(AiNativeImportTest, ParagraphTabsKeepUnicodeAndPerParagraphAreaRulers)
{
    auto unicode = AN::read_text_document(text_setup(text_document(
        "(\\376\\377\\330\\075\\336\\000\\000\\011\\000\\102\\000\\015)", 5)), Geom::Point(100, 50));
    ASSERT_TRUE(unicode->stories[0]) << unicode->errors[0];
    EXPECT_EQ(unicode->stories[0]->lines[0].runs[0].text, "\xF0\x9F\x98\x80\tB");
    ASSERT_TRUE(unicode->stories[0]->lines[0].tabs);
    auto document = area_text_document("(A\\tB\\rC\\tD\\r)", 8, {{0, 10, 4}, {0, 24, 4}}, "ArialMT", {},
        "/5 << /0 [ << /1 4 /0 << /0 << /5 << /31 36 >> >> >> >> "
        "<< /1 4 /0 << /0 << /5 << /31 72 >> >> >> >> ] >>");
    auto texts = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    ASSERT_EQ(texts->stories[0]->lines.size(), 2u);
    ASSERT_TRUE(texts->stories[0]->lines[0].tabs);
    ASSERT_TRUE(texts->stories[0]->lines[1].tabs);
    EXPECT_EQ(texts->stories[0]->lines[0].tabs->interval, 36);
    EXPECT_EQ(texts->stories[0]->lines[1].tabs->interval, 72);
}

TEST_F(AiNativeImportTest, ParagraphTabsKeepLetterSpaceKerningAndConsecutiveStops)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto anchors = [&](std::string const &characters, unsigned units, std::string const &style = std::string()) {
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                              text_setup(text_document(characters, units, font, style))));
        std::vector<double> out;
        if (!b) return out;
        b->document->ensureUpToDate();
        auto reprs = elements(b->document->getReprRoot(), "svg:text");
        if (reprs.empty()) return out;
        auto *text = cast<SPText>(b->document->getObjectByRepr(reprs[0]));
        for (auto it = text->layout.begin();; it.nextCharacter()) {
            out.push_back(text->layout.characterAnchorPoint(it).x());
            if (it == text->layout.end()) break;
        }
        return out;
    };
    // Derive a nonzero pair from ordinary text, whose existing shaping is
    // independent of the tab implementation; do not assume a font version.
    for (std::string letter : {"A", "T", "V", "Y"}) {
        auto single = anchors("(" + letter + "\\r)", 2);
        auto spaced = anchors("(" + letter + " \\r)", 3);
        ASSERT_EQ(single.size(), 2u); ASSERT_EQ(spaced.size(), 3u);
        auto const kern = spaced[1] - single[1];
        if (kern >= -0.01) continue;
        auto tab = anchors("(" + letter + "\\tB\\r)", 4);
        ASSERT_EQ(tab.size(), 4u);
        EXPECT_NEAR(tab[2], 8191.5 + 36 + kern, 0.002);
        auto twice = anchors("(" + letter + "\\t\\tB\\r)", 5);
        ASSERT_EQ(twice.size(), 5u);
        EXPECT_NEAR(twice[3], 8191.5 + 36, 0.002);
        auto scaled = anchors("(" + letter + "\\tB\\r)", 4, "/6 2");
        ASSERT_EQ(scaled.size(), 4u);
        EXPECT_NEAR(scaled[2], 8191.5 / 2 + 18 + kern, 0.002);
        auto no_kern = anchors("(" + letter + "\\tB\\r)", 4, "/11 0");
        ASSERT_EQ(no_kern.size(), 4u);
        EXPECT_NEAR(no_kern[2], 8191.5 + 36, 0.002);
        auto tracked = anchors("(" + letter + "\\tB\\r)", 4, "/8 500");
        ASSERT_EQ(tracked.size(), 4u);
        EXPECT_NEAR(tracked[2], 8191.5 + 36 + kern + 6, 0.002);
        auto tracked_twice = anchors("(" + letter + "\\t\\tB\\r)", 5, "/8 500");
        ASSERT_EQ(tracked_twice.size(), 5u);
        EXPECT_NEAR(tracked_twice[3], 8191.5 + 72, 0.002);
        auto negative = anchors("(" + letter + "\\tB\\r)", 4, "/8 -100");
        ASSERT_EQ(negative.size(), 4u);
        EXPECT_NEAR(negative[2], 8191.5 + 36 + kern - 1.2, 0.002);
        auto leading = anchors("(\\t\\tB\\r)", 4, "/8 500");
        ASSERT_EQ(leading.size(), 4u);
        EXPECT_NEAR(leading[2], 8191.5 + 72, 0.002);
        return;
    }
    GTEST_SKIP() << "available font has no negative letter-space kerning";
}

// Ordinary Illustrator path text, independently written as one cubic and one layout
// line. The path's parameter bounds must be converted before any straightening.
std::string path_text_document(std::string const &characters = "(ABCD\\r)", int units = 5,
                               std::string const &font = "ArialMT", std::string const &style = {},
                               std::string const &options = {}, std::string const &bounds = "0 0.99")
{
    auto data = text_document(characters, units, font, style, "/0 2 /6 [" + bounds + "] " + options);
    auto at = data.find("/2 << /0 2");
    data.insert(at, "/1 << /0 [8191.5 8191.5 8191.5 8191.5 8391.5 8191.5 8391.5 8191.5] >> ");
    at = data.find("/99 /F /0 << /0 [8191.5 8191.5] >>");
    data.replace(at, std::string("/99 /F /0 << /0 [8191.5 8191.5] >>").size(),
                 "/99 /F /1 [0 0 200 0] /5 2 /10 0");
    return data;
}

TEST_F(AiNativeImportTest, PathTypeUsesCubicParametersAndKeepsFullGeometry)
{
    auto read = [](std::string const &bounds) {
        return AN::read_text_document(text_setup(path_text_document("(ABCD\\r)", 5, "ArialMT", {}, {}, bounds)), Geom::Point(100, 50));
    };
    auto text = read("0.25 0.99");
    ASSERT_EQ(text->stories.size(), 1u) << text->error;
    ASSERT_TRUE(text->stories[0]) << text->errors[0];
    auto const &s = *text->stories[0]; ASSERT_TRUE(s.path);
    EXPECT_FALSE(s.area);
    EXPECT_NEAR(s.path->start, 31.25, 0.001);
    EXPECT_NEAR(s.path->end, 199.9404, 0.001);
    EXPECT_NEAR(s.path->outline[0].length(), 200, 0.001);
    EXPECT_EQ(s.lines[0].runs[0].text, "ABCD");
    EXPECT_TRUE(AN::same_drawing(s, s));
    auto changed = read("0.3 0.99"); ASSERT_TRUE(changed->stories[0]);
    EXPECT_FALSE(AN::same_drawing(s, *changed->stories[0]));
    for (auto bounds : {"-1 0.99", "0.5 0.2", "0 0", "0 2", "0", "0 1e100"}) {
        auto invalid = read(bounds);
        ASSERT_EQ(invalid->stories.size(), 1u);
        EXPECT_FALSE(invalid->stories[0]) << bounds;
    }
}

TEST_F(AiNativeImportTest, PathTypeRefusesUnverifiedOptionsAndCharacterSettings)
{
    auto read = [](std::string const &style, std::string const &options) {
        return AN::read_text_document(text_setup(path_text_document("(ABCD\\r)", 5, "ArialMT", style, options)), Geom::Point(100, 50));
    };
    auto ordinary = read({}, "/11 << /4 10 /18 10 /22 0.025 /6 false /7 false >>");
    ASSERT_TRUE(ordinary->stories[0]) << ordinary->errors[0];
    for (auto options : {"/11 << /4 12 >>", "/11 << /18 -12.5 >>", "/11 << /22 0.05 >>", "/11 << /6 true >>",
                         "/11 << /0 false /1 0 /2 0 /7 false >>", "/11 << /4 1e300 /18 -1e300 /22 0 >>"}) {
        auto accepted = read({}, options);
        ASSERT_EQ(accepted->stories.size(), 1u);
        ASSERT_TRUE(accepted->stories[0]) << options << ": " << accepted->errors[0];
        ASSERT_TRUE(accepted->stories[0]->path);
        EXPECT_FALSE(accepted->stories[0]->path->source_options.empty());
    }
    for (auto options : {"/11 << /0 true >>", "/11 << /1 1 >>", "/11 << /2 1 >>", "/11 << /7 true >>", "/11 0",
                         "/11 << /4 false >>", "/11 << /18 [] >>", "/11 << /22 false >>", "/11 << /6 1 >>",
                         "/11 << /0 0 >>", "/11 << /1 (0) >>", "/11 << /123 0 >>"}) {
        auto invalid = read({}, options);
        ASSERT_EQ(invalid->stories.size(), 1u);
        EXPECT_FALSE(invalid->stories[0]) << options;
    }
    for (auto style : {"/6 1.5", "/7 1.5", "/9 6", "/16 1", "/15 1", "/13 1"}) {
        auto invalid = read(style, {});
        ASSERT_EQ(invalid->stories.size(), 1u);
        EXPECT_FALSE(invalid->stories[0]) << style;
    }
    auto tab = AN::read_text_document(text_setup(path_text_document("(A\\tB\\r)", 4)), Geom::Point(100, 50));
    EXPECT_FALSE(tab->stories[0]);
}

TEST_F(AiNativeImportTest, PathTypeAlignmentAndGeometryRemainEditableAfterReopen)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    for (unsigned alignment : {0u, 1u, 2u}) {
        auto document = with_paragraphs(path_text_document("(ABCD\\r)", 5, font), {},
            "<< /1 5 /0 << /0 << /5 << /0 " + std::to_string(alignment) + " >> >> >> >>");
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(document)));
        ASSERT_TRUE(b);
        EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](auto const &note) {
            return note.find("parameter brackets were left out") != std::string::npos;
        }));
        auto doc = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        auto paths = elements(doc->getDefs()->getRepr(), "svg:path"); ASSERT_EQ(paths.size(), 1u);
        auto texts = elements(doc->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
        auto spans = elements(texts[0], "svg:textPath"); ASSERT_EQ(spans.size(), 1u);
        EXPECT_EQ(attr(spans[0], "style"), alignment == 2 ? "text-anchor:middle;" : alignment == 1 ? "text-anchor:end;" : "text-anchor:start;");
        EXPECT_NEAR(std::stod(attr(spans[0], "startOffset")), alignment == 2 ? 99.9702 : alignment == 1 ? 199.9404 : 0, 0.001);
        EXPECT_TRUE(attr(spans[0], "x").empty()); EXPECT_TRUE(attr(spans[0], "y").empty());
        auto *text = cast<SPText>(doc->getObjectByRepr(texts[0])); ASSERT_TRUE(text);
        auto before = text->layout.convertToCurves().boundsFast(); ASSERT_TRUE(before);
        spans[0]->firstChild()->setContent("ABCDEFG"); doc->ensureUpToDate();
        EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 7u);
        auto longer = text->layout.convertToCurves().boundsFast(); ASSERT_TRUE(longer);
        EXPECT_GT(longer->width(), before->width());
        // Editing the referenced path relays out the same text object.
        paths[0]->setAttribute("d", "M8191.5,8241.5 h200"); doc->ensureUpToDate();
        auto moved = text->layout.convertToCurves().boundsFast(); ASSERT_TRUE(moved);
        EXPECT_NEAR(moved->top() - longer->top(), 50, 0.001);
        Geom::Point cursor; double height, rotation;
        text->layout.queryCursorShape(text->layout.end(), cursor, height, rotation);
        EXPECT_TRUE(std::isfinite(cursor.x())); EXPECT_GT(height, 0);
        text->layout.characterBoundingBox(text->layout.end(), &rotation);
        auto saved = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw()); ASSERT_TRUE(saved);
        auto again = elements(saved->getReprRoot(), "svg:textPath"); ASSERT_EQ(again.size(), 1u);
        EXPECT_EQ(attr(again[0], "proof:path-end"), attr(spans[0], "proof:path-end"));
        EXPECT_EQ(std::string(again[0]->firstChild()->content()), "ABCDEFG");
    }
}

TEST_F(AiNativeImportTest, PathTypeEndBracketHidesOverflowAndCanBeEdited)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(path_text_document("(AAAA\\r)", 5, font))));
    ASSERT_TRUE(b);
    auto doc = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw()); ASSERT_TRUE(doc);
    auto texts = elements(doc->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    auto *text = cast<SPText>(doc->getObjectByRepr(texts[0])); ASSERT_TRUE(text);
    auto spans = elements(texts[0], "svg:textPath"); ASSERT_EQ(spans.size(), 1u);
    doc->ensureUpToDate();
    auto const width = text->layout.glyphs()[0].advance;
    spans[0]->setAttributeSvgDouble("proof:path-end", width * 1.1); doc->ensureUpToDate();
    EXPECT_TRUE(text->layout.inputTruncated());
    EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 4u); // Hidden text remains editable.
    auto one = text->layout.convertToCurves(); EXPECT_FALSE(one.empty());
    spans[0]->setAttribute("proof:path-end", "199.9404"); doc->ensureUpToDate();
    EXPECT_FALSE(text->layout.inputTruncated());
    EXPECT_GT(text->layout.convertToCurves().size(), one.size());
    for (auto invalid : {"bad", "nan", "-1", "1e1000"}) {
        spans[0]->setAttribute("proof:path-end", invalid); doc->ensureUpToDate();
        EXPECT_TRUE(text->layout.inputTruncated());
        EXPECT_TRUE(text->layout.convertToCurves().empty()) << invalid;
    }
    spans[0]->setAttribute("proof:path-end", "199.9404"); doc->ensureUpToDate();
    EXPECT_FALSE(text->layout.inputTruncated());
    // End-bracket metadata is preserved when SPTextPath builds a new representation.
    auto fresh = doc->getObjectByRepr(spans[0])->updateRepr(doc->getReprDoc(), nullptr, SP_OBJECT_WRITE_BUILD | SP_OBJECT_WRITE_EXT);
    ASSERT_TRUE(fresh); EXPECT_EQ(attr(fresh, "proof:path-end"), "199.9404");
    Inkscape::GC::release(fresh);
}

TEST_F(AiNativeImportTest, PathTypeWhollyClippedStorySurvivesAbsentLayoutAndReopens)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    for (auto const *options : {"", "/11 << /6 false >>"}) {
        auto data = path_text_document("(WWWW\\r)", 5, font, {}, options, "0 0.1");
        // Illustrator omits the entire layout frame when no whole letter fits. Keep its
        // independent geometry, style runs and story, rather than needing a cached /F.
        auto const at = data.find("/2 << /99 /F"); ASSERT_NE(at, std::string::npos);
        std::size_t end = at + 3; int depth = 0;
        do {
            if (data.compare(end, 2, "<<") == 0) { ++depth; end += 2; }
            else if (data.compare(end, 2, ">>") == 0) { --depth; end += 2; }
            else ++end;
        } while (depth && end < data.size());
        ASSERT_EQ(depth, 0);
        data.replace(at, end - at, "/2 [ << /99 /PC /5 0 >> ]");
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(data)));
        ASSERT_TRUE(b);
        auto *text = cast<SPText>(b->document->getObjectByRepr(elements(b->document->getReprRoot(), "svg:text").at(0)));
        ASSERT_TRUE(text); b->document->ensureUpToDate();
        EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 4u);
        EXPECT_TRUE(text->layout.inputTruncated());
        EXPECT_TRUE(text->layout.convertToCurves().empty());
        auto const written = AN::write_records(b->document.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto again = make(written.records); ASSERT_TRUE(again);
        auto *reopened = cast<SPText>(again->document->getObjectByRepr(elements(again->document->getReprRoot(), "svg:text").at(0)));
        ASSERT_TRUE(reopened); again->document->ensureUpToDate();
        EXPECT_EQ(reopened->layout.iteratorToCharIndex(reopened->layout.end()), 4u);
        EXPECT_TRUE(reopened->layout.convertToCurves().empty());
        auto span = elements(reopened->getRepr(), "svg:textPath").at(0);
        span->setAttribute("proof:path-end", "199.9404"); again->document->ensureUpToDate();
        EXPECT_FALSE(reopened->layout.inputTruncated());
        EXPECT_FALSE(reopened->layout.convertToCurves().empty());
    }
}

TEST_F(AiNativeImportTest, PathTypeClosedPathDoesNotRepeatOverflowAroundTheLoop)
{
    std::string const svg =
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' xmlns:proof='urn:x-proof:document:1'>"
        "<defs><path id='p' d='M0,30 h20 v20 h-20 z'/></defs><text style='font-size:12px'>"
        "<textPath xlink:href='#p' proof:path-start='0' proof:path-end='10'>AAAAAAAAAAAAAAAAAAAA</textPath></text></svg>";
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc); doc->ensureUpToDate();
    auto texts = elements(doc->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    auto *text = cast<SPText>(doc->getObjectByRepr(texts[0])); ASSERT_TRUE(text);
    EXPECT_TRUE(text->layout.inputTruncated());
    auto bounds = text->layout.convertToCurves().boundsFast(); ASSERT_TRUE(bounds);
    EXPECT_LT(bounds->right(), 15);
    EXPECT_LT(bounds->bottom(), 35);
}

// Path type on a closed square, 100 a side: four lines kept as cubics, from the top left
// corner round to the right.
std::string loop_text_document(std::string const &characters, int units, std::string const &font, std::string const &bounds)
{
    auto data = path_text_document(characters, units, font, {}, {}, bounds);
    std::string const line = "/1 << /0 [8191.5 8191.5 8191.5 8191.5 8391.5 8191.5 8391.5 8191.5] >> ";
    data.replace(data.find(line), line.size(),
                 "/1 << /0 [8191.5 8191.5 8191.5 8191.5 8291.5 8191.5 8291.5 8191.5 8291.5 8191.5 8291.5 8191.5 "
                 "8291.5 8291.5 8291.5 8291.5 8291.5 8291.5 8291.5 8291.5 8191.5 8291.5 8191.5 8291.5 "
                 "8191.5 8291.5 8191.5 8291.5 8191.5 8191.5 8191.5 8191.5] >> ");
    return data;
}

TEST_F(AiNativeImportTest, PathTypeOnAClosedPathRunsOnPastItsStart)
{
    auto read = [](std::string const &bounds) {
        return AN::read_text_document(text_setup(loop_text_document("(ABCD\\r)", 5, "ArialMT", bounds)), Geom::Point(100, 50));
    };
    // The end written past the last segment, or before the start, or the start below zero: the
    // same range, from halfway along the fourth side round to just short of it. Time 0.49 on a
    // line kept as a cubic is 48.5002% along it.
    for (auto bounds : {"3.5 7.49", "3.5 3.49", "-0.5 3.49"}) {
        auto text = read(bounds);
        ASSERT_TRUE(text->stories.at(0)) << bounds << ": " << text->errors[0];
        auto const &p = *text->stories[0]->path;
        EXPECT_NEAR(p.start, 350, 0.001) << bounds;
        EXPECT_NEAR(p.end, 748.5002, 0.001) << bounds;
    }
    // Brackets that meet are a whole turn; a range inside the loop is as it was.
    for (auto [bounds, start, end] : {std::tuple{"0 4", 0.0, 400.0}, std::tuple{"1 5", 100.0, 500.0}, std::tuple{"0.5 2.5", 50.0, 250.0}}) {
        auto text = read(bounds);
        ASSERT_TRUE(text->stories.at(0)) << bounds << ": " << text->errors[0];
        EXPECT_NEAR(text->stories[0]->path->start, start, 0.001) << bounds;
        EXPECT_NEAR(text->stories[0]->path->end, end, 0.001) << bounds;
    }
    // Decimal brackets a full turn apart can differ by a few ulps after normalization.
    // They must not become a zero-length interval (an Illustrator centre-on-loop return).
    for (auto const *bounds : {"2.63949 6.63949", "0.63949 4.63949", "-1.36051 2.63949"}) {
        auto text = read(bounds);
        ASSERT_TRUE(text->stories.at(0)) << bounds << ": " << text->errors[0];
        EXPECT_NEAR(text->stories[0]->path->end - text->stories[0]->path->start, 400, 1e-6);
    }
    for (auto bounds : {"1 1", "0 9", "-9 1", "1 9.5"}) {
        auto invalid = read(bounds);
        ASSERT_EQ(invalid->stories.size(), 1u);
        EXPECT_FALSE(invalid->stories[0]) << bounds;
    }

    // Laid out, the type runs on round the corner where the path starts.
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(loop_text_document("(AAAAAAAAAA\\r)", 11, font, "3.5 7.49"))));
    ASSERT_TRUE(b);
    auto texts = elements(b->document->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    auto spans = elements(texts[0], "svg:textPath"); ASSERT_EQ(spans.size(), 1u);
    EXPECT_NEAR(std::stod(attr(spans[0], "proof:path-end")), 748.5002, 0.001);
    auto *text = cast<SPText>(b->document->getObjectByRepr(texts[0])); ASSERT_TRUE(text);
    b->document->ensureUpToDate();
    EXPECT_FALSE(text->layout.inputTruncated());
    auto it = text->layout.end();
    it.prevCharacter();
    auto const last = text->layout.convertToCurves(it, text->layout.end()).boundsFast();
    ASSERT_TRUE(last);
    EXPECT_GT(last->midpoint().x(), 8191.5); EXPECT_LT(last->midpoint().x(), 8250);
    EXPECT_NEAR(last->midpoint().y(), 8191.5, 10);
    // So do its box (selecting it, clicking it) and the cursor after it.
    auto const box = text->layout.characterBoundingBox(it);
    EXPECT_NEAR(box.midpoint().x(), last->midpoint().x(), 1);
    EXPECT_NEAR(box.midpoint().y(), last->midpoint().y(), 4);
    EXPECT_EQ(text->layout.getLetterAt(last->midpoint().x(), last->midpoint().y()), it);
    Geom::Point cursor;
    double height, rotation;
    text->layout.queryCursorShape(text->layout.end(), cursor, height, rotation);
    // Its foot is the font's descent below the top side.
    EXPECT_GT(cursor.y(), 8191.5); EXPECT_LT(cursor.y(), 8191.5 + 4);
    EXPECT_NEAR(rotation, 0, 1e-6);
    EXPECT_GT(cursor.x(), last->midpoint().x()); EXPECT_LT(cursor.x(), last->right() + 1);
    auto const near = text->layout.getNearestCursorPositionTo(Geom::Point(last->midpoint().x() + 1, 8191.5));
    EXPECT_GE(text->layout.iteratorToCharIndex(near), 9u);
}


TEST_F(AiNativeImportTest, ParagraphTabsStayAlignedAfterTextAndRulerEditsAndReopen)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto document = with_paragraphs(text_document("(A\\tB\\tC\\r)", 6, font, "/11 0"), {},
        "<< /1 6 /0 << /0 << /5 << /31 36 /30 << /0 [ << /0 50 /1 0 >> ] >> >> >> >> >>");
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(document)));
    ASSERT_TRUE(b);
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto texts = elements(reopened->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    auto *text = cast<SPText>(reopened->getObjectByRepr(texts[0])); ASSERT_TRUE(text);
    EXPECT_TRUE(attr(texts[0], "proof:glyphs-missing").empty());
    EXPECT_TRUE(b->substituted_type.empty());
    auto spans = elements(texts[0], "svg:tspan"); ASSERT_EQ(spans.size(), 1u);
    auto point = [&](unsigned index) {
        reopened->ensureUpToDate();
        auto it = text->layout.begin();
        for (unsigned i = 0; i < index; ++i) it.nextCharacter();
        return text->layout.characterAnchorPoint(it).x();
    };
    EXPECT_NEAR(point(2), 8191.5 + 50, 0.002);
    EXPECT_NEAR(point(4), 8191.5 + 72, 0.002);
    // A tab occupies a selectable character and advance, without a drawn glyph.
    EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 5u);
    EXPECT_EQ(text->layout.glyphs().size(), 3u);
    auto tab = text->layout.begin(); tab.nextCharacter();
    auto after_tab = tab; after_tab.nextCharacter();
    auto selection = text->layout.createSelectionShape(tab, after_tab, Geom::Affine());
    ASSERT_EQ(selection.size(), 4u);
    EXPECT_GT(Geom::distance(selection[0], selection[1]), 0);
    Geom::Point before, after; double height, rotation;
    text->layout.queryCursorShape(tab, before, height, rotation);
    EXPECT_GT(height, 0);
    text->layout.queryCursorShape(after_tab, after, height, rotation);
    EXPECT_GT(after.x(), before.x());
    spans[0]->firstChild()->setContent("AA\tB\tC");
    EXPECT_NEAR(point(3), 8191.5 + 50, 0.002);
    EXPECT_NEAR(point(5), 8191.5 + 72, 0.002);
    // Updating the ruler invalidates layout without requiring a text edit.
    spans[0]->setAttribute("proof:tab-interval", "80");
    EXPECT_NEAR(point(5), 8191.5 + 80, 0.002);
    auto again = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(again);
    auto again_spans = elements(again->getReprRoot(), "svg:tspan"); ASSERT_EQ(again_spans.size(), 1u);
    EXPECT_EQ(attr(again_spans[0], "proof:tab-interval"), "80");
    EXPECT_EQ(attr(again_spans[0], "proof:tab-stops"), "50");
    // Selecting a story made entirely of tabs must work without any glyphs.
    again_spans[0]->firstChild()->setContent("\t\t");
    again->ensureUpToDate();
    auto *only_tabs = cast<SPText>(again->getObjectByRepr(elements(again->getReprRoot(), "svg:text")[0]));
    ASSERT_TRUE(only_tabs->layout.glyphs().empty());
    EXPECT_EQ(only_tabs->layout.createSelectionShape(only_tabs->layout.begin(), only_tabs->layout.end(), Geom::Affine()).size(), 8u);
    only_tabs->layout.characterBoundingBox(only_tabs->layout.end(), &rotation);
    EXPECT_EQ(rotation, 0);
    // Later attaching imported type to a path must not index a nonexistent tab glyph.
    auto fitted = SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
        "xmlns:proof='urn:x-proof:document:1'><defs><path id='p' d='M0,30 h200'/></defs>"
        "<text proof:tab-origin='0' proof:tab-interval='36' xml:space='preserve'>"
        "<textPath xlink:href='#p'>A\tB</textPath></text></svg>");
    ASSERT_TRUE(fitted); fitted->ensureUpToDate();
    auto *fitted_text = cast<SPText>(fitted->getObjectByRepr(elements(fitted->getReprRoot(), "svg:text")[0]));
    auto path_tab = fitted_text->layout.begin(); path_tab.nextCharacter();
    auto box = fitted_text->layout.characterBoundingBox(path_tab);
    EXPECT_TRUE(std::isfinite(box.midpoint().x()));
    EXPECT_TRUE(std::isfinite(box.midpoint().y()));
}

TEST_F(AiNativeImportTest, InheritedCharacterStyleRemainsEditableAfterReopen)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(without_character_runs(text_document("(Hello\\r)", 6, font))));
    auto b = make(source); ASSERT_TRUE(b);
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto texts = elements(reopened->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    auto spans = elements(texts[0], "svg:tspan"); ASSERT_EQ(spans.size(), 1u);
    ASSERT_TRUE(spans[0]->firstChild());
    EXPECT_STREQ(spans[0]->firstChild()->content(), "Hello");
    auto *text = reopened->getObjectByRepr(texts[0]);
    EXPECT_EQ(text->style->fill.getColor().getValues(), (std::vector<double>{0.25, 0.5, 0.75, 0.1}));
    spans[0]->firstChild()->setContent("Edited!");
    reopened->ensureUpToDate();
    auto edited = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(edited);
    auto again = elements(elements(edited->getReprRoot(), "svg:text")[0], "svg:tspan");
    ASSERT_EQ(again.size(), 1u);
    EXPECT_STREQ(again[0]->firstChild()->content(), "Edited!");
}

TEST_F(AiNativeImportTest, NamedParagraphStylesKeepAlignmentAndDrawing)
{
    for (unsigned alignment : {1u, 2u}) {
        auto inline_document = aligned_text_document(alignment);
        auto named = inline_document;
        std::string const declaration = "/0 << /0 << /5 << /0 " + std::to_string(alignment) + " >> >> >>";
        auto const at = named.find(declaration); ASSERT_NE(at, named.npos);
        named.replace(at, declaration.size(), "/0 << /0 1 >>");
        named = with_paragraphs(named, "<< /0 << /0 (Base) /5 << /0 0 >> >> >> "
            "<< /0 << /0 (Child) /6 0 /5 << /0 " + std::to_string(alignment) + " >> >> >>", {});
        auto a = AN::read_text_document(text_setup(inline_document), Geom::Point(100, 50));
        auto b = AN::read_text_document(text_setup(named), Geom::Point(100, 50));
        ASSERT_EQ(b->stories.size(), 1u) << b->error;
        ASSERT_TRUE(b->stories[0]) << b->errors[0]; ASSERT_TRUE(a->stories[0]);
        EXPECT_TRUE(b->stories[0]->paragraph_styles_expanded);
        EXPECT_FALSE(a->stories[0]->paragraph_styles_expanded);
        EXPECT_EQ(b->stories[0]->lines[0].alignment, alignment);
        EXPECT_TRUE(AN::same_drawing(*a->stories[0], *b->stories[0]));
    }
}

TEST_F(AiNativeImportTest, ParagraphStylesInheritSparseCharacterDefaultsAndOverrides)
{
    std::string const definitions =
        "<< /0 << /5 << /33 1 /35 1 /32 << /1 18 /8 100 /16 1 /18 false "
        "/53 << /0 << /0 0 /1 [1 0] >> >> >> >> >> >> "
        "<< /0 << /6 0 /5 << /32 << /1 24 >> >> >> >> "
        "<< /0 << /6 1 /5 << /33 0 /35 0 /32 << /8 150 >> >> >> >>";
    std::string const runs = "<< /1 6 /0 << /0 2 >> >>";
    auto document = with_paragraphs(without_character_runs(text_document()), definitions, runs);
    auto inherited = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
    ASSERT_EQ(inherited->stories.size(), 1u) << inherited->error;
    ASSERT_TRUE(inherited->stories[0]) << inherited->errors[0];
    auto const &s = inherited->stories[0]->lines[0].runs[0].style;
    EXPECT_EQ(s.size, 24); EXPECT_EQ(s.tracking, 150);
    EXPECT_TRUE(s.underline); EXPECT_FALSE(s.ligatures);
    EXPECT_EQ(s.fill->model, AN::Color::Model::Gray); EXPECT_EQ(s.fill->v[0], 0);
    EXPECT_FALSE(inherited->stories[0]->rtl); // Explicit child zero overrides parent one.
    EXPECT_FALSE(inherited->stories[0]->world_ready);
    document = with_paragraphs(text_document("(Hello\\r)", 6, "ArialMT",
        "/1 30 /8 0 /16 0 /18 true"), definitions, runs);
    auto overridden = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
    ASSERT_TRUE(overridden->stories[0]) << overridden->errors[0];
    auto const &o = overridden->stories[0]->lines[0].runs[0].style;
    EXPECT_EQ(o.size, 30); EXPECT_EQ(o.tracking, 0);
    EXPECT_FALSE(o.underline); EXPECT_TRUE(o.ligatures);
    EXPECT_EQ(o.fill->v[0], 0); // Unoverridden paint still inherits the ancestor.
    auto from_document = AN::read_text_document(text_setup(with_paragraphs(
        without_character_runs(text_document()), {}, {}, "/32 << /1 22 /8 50 >>")), Geom::Point(100, 50));
    ASSERT_TRUE(from_document->stories[0]) << from_document->errors[0];
    EXPECT_EQ(from_document->stories[0]->lines[0].runs[0].style.size, 22);
    EXPECT_EQ(from_document->stories[0]->lines[0].runs[0].style.tracking, 50);
    EXPECT_FALSE(from_document->stories[0]->paragraph_styles_expanded);
}

TEST_F(AiNativeImportTest, ParagraphCharacterDefaultsSplitRunsAtUtf16Boundaries)
{
    auto document = area_text_document("(\\376\\377\\000A\\330\\075\\336\\000\\000\\015\\000B\\000\\015)", 6,
                                      {{0, 10, 4}, {0, 25, 2}});
    std::string const definitions = "<< /0 << /5 << /32 << /1 10 >> >> >> >> "
                                    "<< /0 << /5 << /32 << /1 20 >> >> >> >>";
    document = with_paragraphs(document, definitions, "<< /1 4 /0 << /0 0 >> >> << /1 2 /0 << /0 1 >> >>");
    auto texts = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    auto const &lines = texts->stories[0]->lines; ASSERT_EQ(lines.size(), 2u);
    ASSERT_EQ(lines[0].runs.size(), 1u); ASSERT_EQ(lines[1].runs.size(), 1u);
    EXPECT_EQ(lines[0].runs[0].text, "A\xf0\x9f\x98\x80");
    EXPECT_EQ(lines[1].runs[0].text, "B");
    EXPECT_EQ(lines[0].runs[0].style.size, 10); EXPECT_EQ(lines[1].runs[0].style.size, 20);
    // Paragraph and character runs cannot bisect a surrogate pair.
    document.replace(document.find("<< /1 4 /0 << /0 0 >> >> << /1 2"),
                     std::string("<< /1 4 /0 << /0 0 >> >> << /1 2").size(), "<< /1 2 /0 << /0 0 >> >> << /1 4");
    auto bad = AN::read_text_document(text_setup(document), Geom::Point(100, 50));
    EXPECT_FALSE(bad->stories[0]);
    EXPECT_NE(bad->errors[0].find("Unicode character"), std::string::npos);
}

TEST_F(AiNativeImportTest, ParagraphStylesRefuseInvalidReferencesCyclesAndInheritedFeatures)
{
    auto bad_story = [&](std::string const &definitions, std::string const &reference,
                         std::string const &defaults = {}) {
        auto texts = AN::read_text_document(text_setup(with_paragraphs(
            without_character_runs(text_document()), definitions,
            "<< /1 6 /0 << /0 " + reference + " >> >>", defaults)), Geom::Point(100, 50));
        ASSERT_EQ(texts->stories.size(), 1u) << texts->error;
        EXPECT_FALSE(texts->stories[0]) << reference;
        EXPECT_FALSE(texts->errors[0].empty());
    };
    std::string const base = "<< /0 << /5 << >> >> >>";
    for (auto const *reference : {"0", "-1", "0.5", "3", "(Normal)"}) {
        bad_story(reference == std::string("0") ? "" : base, reference);
    }
    for (auto const *definitions : {
        "<< /0 << /5 << >> /6 0 >> >>", // Self-cycle.
        "<< /0 << /5 << >> /6 1 >> >> << /0 << /5 << >> /6 0 >> >>",
        "<< /0 << /5 << >> /6 (Base) >> >>",
        "<< /0 << /5 [] >> >>",
        "<< /0 << /5 << /32 0 >> >> >>",
        "<< /0 << /5 << /32 << /2 true >> >> >> >>", // Unsupported inherited feature.
        "<< /0 << /5 << /0 7 >> >> >>"}) bad_story(definitions, "0");
    bad_story(base, "0", "/32 false");
    std::string deep;
    for (unsigned i = 0; i < 65; ++i) {
        deep += "<< /0 << /5 << >>" + (i < 64 ? " /6 " + std::to_string(i + 1) : "") + " >> >> ";
    }
    bad_story(deep, "0");
    // Justification a style gives is the paragraph's, and so are the spacings it allows.
    auto justified = with_paragraphs(area_text_document("(Hi\\r)", 3, {{0, 10, 3}}),
        "<< /0 << /5 << /0 3 >> >> >>", "<< /1 3 /0 << /0 0 >> >>");
    auto read = AN::read_text_document(text_setup(justified), Geom::Point(100, 50));
    ASSERT_TRUE(read->stories[0]) << read->errors[0];
    EXPECT_EQ(read->stories[0]->lines[0].justification, 3u);
    auto spaced = with_paragraphs(area_text_document("(Hi\\r)", 3, {{0, 10, 3}}),
        "<< /0 << /5 << /0 3 /18 [0 0.05 0.1] >> >> >>", "<< /1 3 /0 << /0 0 >> >>");
    auto refused = AN::read_text_document(text_setup(spaced), Geom::Point(100, 50));
    EXPECT_FALSE(refused->stories[0]);
    EXPECT_NE(refused->errors[0].find("justified"), std::string::npos);
}

TEST_F(AiNativeImportTest, ParagraphResourcesAreLocalToTypedAndDrawnDocuments)
{
    auto document = [&](unsigned size) {
        return with_paragraphs(without_character_runs(text_document()),
            "<< /0 << /5 << /32 << /1 " + std::to_string(size) + " >> >> >> >>",
            "<< /1 6 /0 << /0 0 >> >>");
    };
    auto texts = AN::read_text_document(text_setup(document(18), document(24)), Geom::Point(100, 50));
    ASSERT_EQ(texts->stories.size(), 1u); ASSERT_EQ(texts->drawn.size(), 1u);
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0]; ASSERT_TRUE(texts->drawn[0]) << texts->drawn_errors[0];
    EXPECT_EQ(texts->stories[0]->lines[0].runs[0].style.size, 18);
    EXPECT_EQ(texts->drawn[0]->lines[0].runs[0].style.size, 24);
}

TEST_F(AiNativeImportTest, NamedParagraphFormattingRemainsEditableAndReportsLostLinks)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto document = with_paragraphs(without_character_runs(text_document("(Hello\\r)", 6, font)),
        "<< /0 << /0 (Body) /5 << /32 << /1 22 /8 100 "
        "/53 << /0 << /0 2 /1 [1 0.1 0.2 0.3 0.4] >> >> >> >> >> >>",
        "<< /1 6 /0 << /0 0 >> >>");
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(document)));
    ASSERT_TRUE(b);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](auto const &note) {
        return note.find("paragraph styles came in as inline formatting") != std::string::npos;
    }));
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto texts = elements(reopened->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    EXPECT_NE(attr(texts[0], "style").find("letter-spacing:2.2"), std::string::npos);
    EXPECT_EQ(reopened->getObjectByRepr(texts[0])->style->fill.getColor().getValues(),
              (std::vector<double>{0.1, 0.2, 0.3, 0.4}));
    auto spans = elements(texts[0], "svg:tspan"); ASSERT_EQ(spans.size(), 1u);
    ASSERT_TRUE(spans[0]->firstChild()); EXPECT_STREQ(spans[0]->firstChild()->content(), "Hello");
    spans[0]->firstChild()->setContent("Edited!"); reopened->ensureUpToDate();
    auto edited = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(edited);
    auto again = elements(elements(edited->getReprRoot(), "svg:text")[0], "svg:tspan");
    ASSERT_EQ(again.size(), 1u); EXPECT_STREQ(again[0]->firstChild()->content(), "Edited!");
}

TEST_F(AiNativeImportTest, PointTextBuildsEditableSvgAndSurvivesReopen)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(text_document("(Hello\\r)", 6, font, "/8 100")));
    auto b = make(source); ASSERT_TRUE(b);
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_EQ(attr(texts[0], "proof:ai-story"), "0");
    // One style throughout: it is the text object's own, and the line's span adds nothing.
    auto spans = elements(texts[0], "svg:tspan"); ASSERT_EQ(spans.size(), 1u);
    EXPECT_NE(attr(texts[0], "style").find("letter-spacing:1.2"), std::string::npos);
    EXPECT_NE(attr(texts[0], "style").find("icc-color"), std::string::npos);
    EXPECT_EQ(attr(spans[0], "style"), "text-anchor:start;");
    ASSERT_TRUE(spans[0]->firstChild());
    EXPECT_STREQ(spans[0]->firstChild()->content(), "Hello");
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto again = elements(reopened->getReprRoot(), "svg:text"); ASSERT_EQ(again.size(), 1u);
    auto again_spans = elements(again[0], "svg:tspan"); ASSERT_EQ(again_spans.size(), 1u);
    ASSERT_TRUE(again_spans[0]->firstChild());
    EXPECT_STREQ(again_spans[0]->firstChild()->content(), "Hello");
    EXPECT_EQ(attr(again[0], "transform"), attr(texts[0], "transform"));
    // The object's own fill is set, and its text draws with it.
    auto *text = reopened->getObjectByRepr(again[0]);
    EXPECT_TRUE(text->style->fill.set);
    EXPECT_EQ(text->style->fill.getColor().getValues(), (std::vector<double>{0.25, 0.5, 0.75, 0.1}));
    EXPECT_EQ(reopened->getObjectByRepr(again_spans[0])->style->fill.getColor().getValues(),
              (std::vector<double>{0.25, 0.5, 0.75, 0.1}));
    again_spans[0]->firstChild()->setContent("Edited!");
    reopened->ensureUpToDate();
    auto edited = SPDocument::createNewDocFromMem(sp_repr_save_buf(reopened->getReprDoc()).raw());
    ASSERT_TRUE(edited);
    auto edited_spans = elements(elements(edited->getReprRoot(), "svg:text")[0], "svg:tspan");
    ASSERT_EQ(edited_spans.size(), 1u);
    EXPECT_STREQ(edited_spans[0]->firstChild()->content(), "Edited!");
}

TEST_F(AiNativeImportTest, PointTextRunsCarryOnlyWhatDiffersFromTheObject)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    std::string error;
    auto ai = AN::read(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                               text_setup(text_document("(Hello\\r)", 6, font))), error);
    ASSERT_TRUE(ai) << error;
    ASSERT_TRUE(ai->texts && ai->texts->stories.size() == 1 && ai->texts->stories[0]);
    auto styled = std::make_shared<AN::TextDocument>(*ai->texts);
    auto &line = styled->stories[0]->lines.at(0);
    ASSERT_EQ(line.runs.size(), 1u);
    // "He" as it is, "llo" larger and red, then "!" as the first again.
    auto first = line.runs[0];
    auto second = first, third = first;
    first.text = "He";
    second.text = "llo";
    second.style.size = first.style.size * 2;
    second.style.fill = AN::Color::rgb(1, 0, 0);
    third.text = "!";
    line.runs = {first, second, third};
    ai->texts = styled;
    auto b = AN::build(*ai, error); ASSERT_TRUE(b) << error;
    auto texts = elements(b->document->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    auto spans = elements(texts[0], "svg:tspan"); ASSERT_EQ(spans.size(), 2u);
    auto const style = attr(spans[1], "style");
    EXPECT_NE(style.find("font-size:"), std::string::npos) << style;
    EXPECT_NE(style.find("fill:"), std::string::npos) << style;
    EXPECT_EQ(style.find("font-family"), std::string::npos) << style;
    EXPECT_EQ(style.find("stroke"), std::string::npos) << style;
    EXPECT_STREQ(spans[1]->firstChild()->content(), "llo");
    EXPECT_STREQ(spans[0]->firstChild()->content(), "He");
    EXPECT_STREQ(spans[0]->lastChild()->content(), "!");
    b->document->ensureUpToDate();
    auto *text = b->document->getObjectByRepr(texts[0]);
    EXPECT_TRUE(text->style->fill.set);
    EXPECT_NE(b->document->getObjectByRepr(spans[1])->style->fill.getColor().toString(),
              text->style->fill.getColor().toString());
}

TEST_F(AiNativeImportTest, FontThatIsNotInstalledIsStoodInFor)
{
    std::string const missing = "PROOF-Missing-Unique-Font-734";
    auto source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(with_glyph_run(text_document("(Hello\\r)", 6, missing), "8191.5 8180 8220 8195")));
    auto b = make(source);
    ASSERT_TRUE(b);
    // The text is there, set in the font that stands in, and says whose place that takes.
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_EQ(attr(texts[0], "proof:font-missing"), missing);
    EXPECT_EQ(attr(texts[0], "proof:font-missing-axes"), "");
    EXPECT_NE(attr(texts[0], "style").find("font-family:"), std::string::npos);
    EXPECT_EQ(attr(texts[0], "style").find(missing), std::string::npos);
    auto bounds = drawn(b->document.get(), texts[0]);
    ASSERT_TRUE(bounds);
    EXPECT_GT(bounds->width(), 10);
    EXPECT_EQ(b->missing_fonts, std::vector<std::string>{missing});
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("1 font isn't installed (" + missing + "): 1 text object is set in "), std::string::npos) << b->notes[0];
    // Where a page would show its type, and where it is set here: neither can be compared.
    ASSERT_EQ(b->substituted_type.size(), 2u);
    auto const &there = b->substituted_type[0];
    EXPECT_TRUE(Geom::are_near(there[0], Geom::Point(10, 28.5), 1e-9)) << there[0];
    EXPECT_TRUE(Geom::are_near(there[2], Geom::Point(38.5, 43.5), 1e-9)) << there[2];
    auto const &here = b->substituted_type[1];
    EXPECT_TRUE(Geom::are_near(here[0], bounds->min(), 1e-6) && Geom::are_near(here[2], bounds->max(), 1e-6));

    // Type that doesn't show is stood in for as well, and isn't a place to leave out.
    std::string error;
    auto ai = AN::read(source, error);
    ASSERT_TRUE(ai) << error;
    ai->layers[0].visible = false;
    auto hidden = AN::build(*ai, error);
    ASSERT_TRUE(hidden) << error;
    EXPECT_EQ(elements(hidden->document->getReprRoot(), "svg:text").size(), 1u);
    EXPECT_TRUE(hidden->substituted_type.empty());
    EXPECT_EQ(hidden->missing_fonts.size(), 1u);
}

TEST_F(AiNativeImportTest, FontsThatAreNotInstalledKeepTheirOwnNames)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    std::string error;
    auto ai = AN::read(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(text_document("(Hello!\\r)", 7, font))), error);
    ASSERT_TRUE(ai) << error;
    auto styled = std::make_shared<AN::TextDocument>(*ai->texts);
    auto &line = styled->stories.at(0)->lines.at(0);
    // "He" in one font that isn't installed, "l" in another, "lo" in an instance of a
    // variable font that isn't, and "!" in one that is.
    auto first = line.runs.at(0);
    auto second = first, third = first, fourth = first;
    first.text = "He";
    first.style.font = "PROOF-Missing-One";
    second.text = "l";
    second.style.font = "PROOF-Missing-Two";
    third.text = "lo";
    third.style.font = "PROOFMissing_350.000wght";
    third.style.design = {350, 87.5};
    fourth.text = "!";
    line.runs = {first, second, third, fourth};
    ai->texts = styled;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_EQ(attr(texts[0], "proof:font-missing"), "PROOF-Missing-One");
    // The line's span, then one for each run whose font isn't the object's.
    auto spans = elements(texts[0], "svg:tspan");
    ASSERT_EQ(spans.size(), 4u);
    EXPECT_STREQ(spans[0]->firstChild()->content(), "He");
    EXPECT_EQ(attr(spans[1], "proof:font-missing"), "PROOF-Missing-Two");
    EXPECT_NE(attr(spans[1], "style").find("font-family:"), std::string::npos) << attr(spans[1], "style");
    EXPECT_EQ(attr(spans[2], "proof:font-missing"), "PROOFMissing_350.000wght");
    EXPECT_EQ(attr(spans[2], "proof:font-missing-axes"), "350 87.5");
    // The one that is installed says its font, and isn't one standing in.
    EXPECT_EQ(attr(spans[3], "proof:font-missing"), "");
    EXPECT_NE(attr(spans[3], "style").find("font-family:"), std::string::npos) << attr(spans[3], "style");
    EXPECT_EQ(b->missing_fonts, (std::vector<std::string>{"PROOF-Missing-One", "PROOF-Missing-Two", "PROOFMissing_350.000wght"}));
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("3 fonts aren't installed"), std::string::npos) << b->notes[0];
    EXPECT_NE(b->notes[0].find("until they are"), std::string::npos) << b->notes[0];
}

TEST_F(AiNativeImportTest, CharactersAFontLacksAreDrawnByAnother)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // A private-use character no text font has.
    auto source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
        text_setup(text_document("(\\376\\377\\333\\377\\337\\375\\000\\015)", 3, font)));
    auto b = make(source);
    ASSERT_TRUE(b);
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_EQ(attr(texts[0], "proof:glyphs-missing"), "true");
    EXPECT_EQ(attr(texts[0], "proof:font-missing"), "");
    EXPECT_TRUE(b->missing_fonts.empty());
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("1 text object has characters that its font doesn't have"), std::string::npos) << b->notes[0];
    // Text whose font has all its characters says nothing.
    auto plain = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(text_document("(Hello\\r)", 6, font))));
    ASSERT_TRUE(plain);
    EXPECT_EQ(attr(elements(plain->document->getReprRoot(), "svg:text").at(0), "proof:glyphs-missing"), "");
    EXPECT_TRUE(plain->notes.empty());
    EXPECT_TRUE(plain->substituted_type.empty());
}

TEST_F(AiNativeImportTest, GlyphRunsSayWhereTheyWereSet)
{
    auto read = [&](std::string const &style) {
        auto texts = AN::read_text_document(text_setup(with_glyph_run(text_document("(Hi\\r)", 3, "ArialMT", style), "8191.5 8180 8220 8195")),
                                            Geom::Point(100, 50));
        EXPECT_TRUE(texts->stories.at(0)) << texts->errors.at(0);
        return *texts->stories[0];
    };
    auto story = read({});
    ASSERT_EQ(story.set.size(), 1u);
    EXPECT_TRUE(Geom::are_near(story.set[0].min() * story.to_art, Geom::Point(10, 71.5), 1e-9));
    EXPECT_TRUE(Geom::are_near(story.set[0].max() * story.to_art, Geom::Point(38.5, 56.5), 1e-9));
    // Under a character scale the box is still where the type is on the art.
    auto scaled = read("/6 0.5 /7 2");
    ASSERT_EQ(scaled.set.size(), 1u);
    EXPECT_TRUE(Geom::are_near(scaled.set[0].min() * scaled.to_art, Geom::Point(10, 71.5), 1e-9));
    EXPECT_TRUE(Geom::are_near(scaled.set[0].max() * scaled.to_art, Geom::Point(38.5, 56.5), 1e-9));
    // A run without a box, or with one that isn't four numbers, has none.
    auto none = AN::read_text_document(text_setup(text_document()), Geom::Point(100, 50));
    ASSERT_TRUE(none->stories.at(0));
    EXPECT_TRUE(none->stories[0]->set.empty());
    auto bad = AN::read_text_document(text_setup(with_glyph_run(text_document(), "1 2 (x) 4")), Geom::Point(100, 50));
    ASSERT_TRUE(bad->stories.at(0));
    EXPECT_TRUE(bad->stories[0]->set.empty());
}

TEST_F(AiNativeImportTest, PointTextPreservesStyledLinesAndAffinePlacement)
{
    auto data = text_document("(Hi\\rBye\\r)", 3, "ArialMT", {}, "/2 [0 1 -1 0 -90 -10]",
                             "<< /1 4 /0 << /0 << /6 << /1 24 /8 50 >> >> >> >>");
    auto const at = data.find("/6 [ << /99 /L /6 [ << /99 /S >> ] >> ]");
    ASSERT_NE(at, std::string::npos);
    data.replace(at, std::string("/6 [ << /99 /L /6 [ << /99 /S >> ] >> ]").size(),
                 "/6 [ << /99 /L /6 [ << /99 /S >> ] >> "
                 "<< /99 /L /0 << /0 [2 18] >> /6 [ << /99 /S /0 << /0 [3 0] >> >> ] >> ]");
    auto texts = AN::read_text_document(text_setup(data), Geom::Point(100, 50));
    ASSERT_EQ(texts->stories.size(), 1u);
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    auto const &s = *texts->stories[0];
    ASSERT_EQ(s.lines.size(), 2u);
    ASSERT_EQ(s.lines[0].runs.size(), 1u); ASSERT_EQ(s.lines[1].runs.size(), 1u);
    EXPECT_EQ(s.lines[0].runs[0].text, "Hi"); EXPECT_EQ(s.lines[1].runs[0].text, "Bye");
    EXPECT_EQ(s.lines[0].runs[0].style.size, 12); EXPECT_EQ(s.lines[1].runs[0].style.size, 24);
    EXPECT_EQ(s.lines[1].runs[0].style.tracking, 50);
    auto p0 = s.lines[0].origin * s.to_art;
    auto p1 = s.lines[1].origin * s.to_art;
    EXPECT_EQ(p1 - p0, Geom::Point(-18, -5));
}

TEST_F(AiNativeImportTest, PointTextAlignedParagraphsKeepEditableAnchors)
{
    for (unsigned align : {1u, 2u}) {
        auto texts = AN::read_text_document(text_setup(aligned_text_document(align)), Geom::Point(100, 50));
        ASSERT_EQ(texts->stories.size(), 1u);
        ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
        auto const &s = *texts->stories[0];
        ASSERT_EQ(s.lines.size(), 1u); EXPECT_EQ(s.lines[0].alignment, align);
        EXPECT_EQ(s.lines[0].origin * s.to_art, Geom::Point(80, 60));
    }
    // A space at the line's end is part of what Illustrator centred, and of what is centred
    // here when the story says so: the line stays anchored at its middle.
    auto spaced = aligned_text_document(2);
    spaced.replace(spaced.find("(HHH\\r)"), 7, "(HH \\r)");
    auto texts = AN::read_text_document(text_setup(spaced), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_EQ(texts->stories[0]->lines.at(0).alignment, 2u);
    EXPECT_TRUE(texts->stories[0]->illustrator_alignment);
    EXPECT_TRUE(Geom::are_near(texts->stories[0]->lines[0].origin * texts->stories[0]->to_art, Geom::Point(80, 60), 1e-9));
    // Not in a paragraph read from the right, whose line ends are elsewhere: set from its start.
    auto backwards = spaced;
    backwards.replace(backwards.find("/5 << /0 2 >>"), 13, "/5 << /0 2 /33 1 >>");
    texts = AN::read_text_document(text_setup(backwards), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_EQ(texts->stories[0]->lines.at(0).alignment, 0u);
    EXPECT_FALSE(texts->stories[0]->illustrator_alignment);
    // A justified paragraph of point type has nothing to be justified to: its line is set
    // from its left end, where its glyphs start.
    auto justified = AN::read_text_document(text_setup(aligned_text_document(3)), Geom::Point(100, 50));
    ASSERT_EQ(justified->stories.size(), 1u);
    ASSERT_TRUE(justified->stories[0]) << justified->errors[0];
    EXPECT_EQ(justified->stories[0]->lines.at(0).alignment, 0u);
    // An alignment nobody knows refuses the story.
    auto bad = AN::read_text_document(text_setup(aligned_text_document(7)), Geom::Point(100, 50));
    ASSERT_EQ(bad->stories.size(), 1u); EXPECT_FALSE(bad->stories[0]);
    auto data = aligned_text_document(2);
    auto const at = data.find("/1 4 /0"); ASSERT_NE(at, std::string::npos);
    data.replace(at, 4, "/1 3");
    bad = AN::read_text_document(text_setup(data), Geom::Point(100, 50));
    ASSERT_EQ(bad->stories.size(), 1u); EXPECT_FALSE(bad->stories[0]);
}

TEST_F(AiNativeImportTest, UnusedPaletteInksSurvive)
{
    auto b = make(records(layer("0 g\n" + rect), "%AI5_BeginPalette\n0 0 Pb\n"
                          "0 0 0 0 89 0 76 (Unused Spot) 0 2 Xx\n(Unused Spot) Pc\nPB\n%AI5_EndPalette\n"));
    ASSERT_TRUE(b);
    auto swatches = elements(b->document->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_EQ(swatches.size(), 1u);
    auto ink = SpotInk::read(swatches[0]);
    ASSERT_TRUE(ink);
    EXPECT_EQ(ink->name, "Unused Spot");
}

TEST_F(AiNativeImportTest, TilesAreDrawnWithoutSeams)
{
    auto box = [](double x0, double y0, double x1, double y1, char const *paint = "f") {
        auto s = [](double v) { return std::to_string(v); };
        return s(x0) + " " + s(y0) + " m " + s(x0) + " " + s(y1) + " L " + s(x1) + " " + s(y1) + " L " + s(x1) + " " +
               s(y0) + " L " + s(x0) + " " + s(y0) + " L\n" + paint + "\n";
    };
    auto crisp = [](XML::Node *n) { return attr(n, "style").find("shape-rendering:crispEdges") != std::string::npos; };
    // An expanded gradient: strips that meet along whole edges. A box apart from them and
    // a stroked one are no tiles, and a strip of no width isn't one either.
    auto strips = make(records(layer("u\n0 0 0 1 k\n" + box(10, 20, 10, 60) + box(10, 20, 30, 60) +
                                     "0 0 0 0.8 k\n" + box(30, 20, 50, 60) + "0 0 0 0.6 k\n" + box(50, 20, 70, 60) +
                                     box(100, 20, 120, 60) + "0 0 0 1 K 1 w\n" + box(70, 20, 90, 60, "b") + "U\n")));
    ASSERT_TRUE(strips);
    auto paths = elements(strips->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 6u);
    EXPECT_FALSE(crisp(paths[0]));
    EXPECT_TRUE(crisp(paths[1]));
    EXPECT_TRUE(crisp(paths[2]));
    EXPECT_TRUE(crisp(paths[3]));
    EXPECT_FALSE(crisp(paths[4]));
    EXPECT_FALSE(crisp(paths[5]));

    // Slanted strips would show ragged ends, unless a clipping path cuts those ends away.
    auto slanted = [](double x, double lift) {
        auto s = [](double v) { return std::to_string(v); };
        return s(x) + " " + s(0 + lift) + " m " + s(x) + " " + s(90 + lift) + " L " + s(x + 30) + " " + s(95 + lift) +
               " L " + s(x + 30) + " " + s(5 + lift) + " L " + s(x) + " " + s(0 + lift) + " L\nf\n";
    };
    auto const pair = "0 0 0 1 k\n" + slanted(40, 0) + "0 0 0 0.5 k\n" + slanted(70, 5);
    auto open = make(records(layer("u\n" + pair + "U\n")));
    ASSERT_TRUE(open);
    for (auto *p : elements(open->document->getReprRoot(), "svg:path")) EXPECT_FALSE(crisp(p));
    auto clipped = make(records(layer("q\n" + box(50, 30, 90, 70, "h W n") + pair + "Q\n")));
    ASSERT_TRUE(clipped);
    auto inside = elements(clipped->document->getReprRoot()->lastChild(), "svg:path");
    ASSERT_EQ(inside.size(), 2u);
    EXPECT_TRUE(crisp(inside[0]));
    EXPECT_TRUE(crisp(inside[1]));
    // The same strips reaching into view under a larger clipping path stay antialiased.
    auto showing = make(records(layer("q\n" + box(20, 0, 120, 100, "h W n") + pair + "Q\n")));
    ASSERT_TRUE(showing);
    for (auto *p : elements(showing->document->getReprRoot()->lastChild(), "svg:path")) EXPECT_FALSE(crisp(p));
}

TEST_F(AiNativeImportTest, UnplaceableImagesRefused)
{
    std::string error;
    auto ai = AN::read(records(layer("0 g\n" + rect)), error);
    ASSERT_TRUE(ai);
    AN::Node image;
    image.kind = AN::Node::Kind::Image;
    image.image = 0;
    ai->layers[0].children.push_back(image);
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("image couldn't be placed"), std::string::npos);
}

TEST_F(AiNativeImportTest, ReaderOmissionsRefusedEvenForSmallArtwork)
{
    std::string error;
    auto ai = AN::read(records(layer("0 g\n" + rect + "Bb\n0 (Missing) 0 0 0 10 1 0 0 1 0 0 Bg\n" + rect + "0 BB\n")), error);
    ASSERT_TRUE(ai) << error;
    ASSERT_FALSE(ai->warnings.empty());
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("gradient isn't defined"), std::string::npos);
}

TEST_F(AiNativeImportTest, GuidesAndDrawnLooks)
{
    auto b = make(records(layer("0 g 10 20 m 90 20 L (N) *\nu\n" + rect +
                                "U\n%_0 g\n%_10 20 m 10 60 L 90 60 L 90 20 L h\n%_f\n1 (Style) XW\n")));
    ASSERT_TRUE(b);
    EXPECT_EQ(elements(b->document->getNamedView()->getRepr(), "sodipodi:guide").size(), 1u);
    auto groups = elements(b->document->getReprRoot(), "svg:g");
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(attr(groups[1], "proof:drawn-look"), "true");
}

TEST_F(AiNativeImportTest, AClippingPathsFillLiesUnderWhatItClipsAndItsStrokeOverIt)
{
    // What is clipped, then the clipping path: filled, itself, stroked.
    std::string const frame = "10 20 m\n10 60 L\n90 60 L\n90 20 L\n10 20 L\nh\nW\n";
    auto b = make(records(layer("q\n0 1 0 0 k\n0 30 m\n100 30 L\n100 50 L\n0 50 L\nf\nu\n0 0 0 1 k\n" + frame + "f\n" + frame +
                                "n\n0 R\n1 0 0 0 K\n4 w\n" + frame + "s\nU\n9 () XW\nQ\n9 () XW\n")));
    ASSERT_TRUE(b);
    auto *root = b->document->getReprRoot();
    auto const paths = elements(root, "svg:path");
    // In the clipping path of the document, and three drawn: the fill, the clipped art, the stroke.
    ASSERT_EQ(elements(b->document->getDefs()->getRepr(), "svg:clipPath").size(), 1u);
    std::vector<XML::Node *> drawn;
    for (auto *p : paths) {
        if (!p->parent() || std::string_view(p->parent()->name()) != "svg:clipPath") drawn.push_back(p);
    }
    ASSERT_EQ(drawn.size(), 3u);
    auto style = [&](XML::Node *n) { return attr(n, "style"); };
    // The fill and what is clipped lie in the clipped group, the fill first; the stroke after the group.
    EXPECT_FALSE(attr(drawn[0]->parent(), "clip-path").empty());
    EXPECT_EQ(drawn[0]->parent(), drawn[1]->parent());
    EXPECT_NE(style(drawn[0]).find("stroke:none"), std::string::npos) << style(drawn[0]);
    EXPECT_EQ(style(drawn[0]).find("fill:none"), std::string::npos) << style(drawn[0]);
    EXPECT_TRUE(attr(drawn[2]->parent(), "clip-path").empty());
    EXPECT_EQ(drawn[2]->parent(), drawn[0]->parent()->parent());
    EXPECT_NE(style(drawn[2]).find("fill:none"), std::string::npos) << style(drawn[2]);
    EXPECT_NE(style(drawn[2]).find("stroke-width:4"), std::string::npos) << style(drawn[2]);
}

TEST_F(AiNativeImportTest, ABrushStrokeIsWhatTheBrushDrewWithoutThePathUnderIt)
{
    // The path a brush follows is written on %_ lines beside what the brush drew of it.
    // Drawn as well, it would lie over the stroke as a line of its own.
    auto b = make(records(layer("u\n%_u\n%_0 R\n%_0 G\n%_1 w\n%_10 40 m\n%_90 40 L\n%_S\n%_U\n%_9 () XW\n"
                                "u\n0 O\n0 g\n" + rect + "U\n9 () XW\n(Adobe ArtOnPath Brush Tool) 1 0 34 XP\n"
                                "%0403020100\n9 () XW\nU\n0 0 Xd\n6 () XW\n")));
    ASSERT_TRUE(b);
    auto const paths = elements(b->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(attr(paths[0]->parent(), "proof:plugin"), "Adobe ArtOnPath Brush Tool");
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_EQ(b->notes[0], "1 brush stroke came in as what it draws: the paths it was made from aren't kept");
}

// ---- area type, baseline shifts -----------------------------------------------------------

TEST_F(AiNativeImportTest, AreaTypeKeepsTheLinesIllustratorLaidOut)
{
    // One paragraph that wrapped: the story has no line end where the frame broke it.
    auto texts = AN::read_text_document(text_setup(area_text_document("(Hello world\\r)", 12, {{0, 12, 6}, {0, 26, 6}})),
                                        Geom::Point(100, 50));
    ASSERT_TRUE(texts->error.empty()) << texts->error;
    ASSERT_EQ(texts->stories.size(), 1u);
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    auto const &s = *texts->stories[0];
    EXPECT_TRUE(s.area);
    EXPECT_FALSE(s.empty);
    EXPECT_TRUE(s.overflow.empty());
    ASSERT_EQ(s.lines.size(), 2u);
    ASSERT_EQ(s.lines[0].runs.size(), 1u);
    ASSERT_EQ(s.lines[1].runs.size(), 1u);
    EXPECT_EQ(s.lines[0].runs[0].text, "Hello ");
    EXPECT_EQ(s.lines[1].runs[0].text, "world");
    // The frame's top-left corner is at 10, 60 of the art (y up); baselines 12 and 26 below it.
    EXPECT_EQ(s.lines[0].origin * s.to_art, Geom::Point(10, 48));
    EXPECT_EQ(s.lines[1].origin * s.to_art, Geom::Point(10, 34));
    auto const frame = (s.frame * s.to_art).boundsExact();
    ASSERT_TRUE(frame);
    EXPECT_EQ(*frame, Geom::Rect(Geom::Point(10, 20), Geom::Point(110, 60)));
    ASSERT_EQ(s.frame.size(), 1u);
    EXPECT_TRUE(s.frame[0].closed());
    EXPECT_EQ(s.frame[0].size(), 4u);

    // Rows and columns hold their lines at their own offsets.
    auto columns = area_text_document("(ab\\rcd\\r)", 6, {}, "ArialMT", {}, {},
        "/2 [ << /99 /F /0 << /0 [8191.5 8191.5] >> /6 [ "
        "<< /99 /R /6 [ << /99 /L /0 << /0 [0 12] >> /6 [ << /99 /S /15 << /0 3 >> >> ] >> ] >> "
        "<< /99 /R /0 << /0 [50 2] >> /6 [ << /99 /L /0 << /0 [1 12] >> /6 [ << /99 /S /0 << /0 [3 0] >> /15 << /0 3 >> >> ] >> ] >> ] >> ]");
    texts = AN::read_text_document(text_setup(columns), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    ASSERT_EQ(texts->stories[0]->lines.size(), 2u);
    EXPECT_EQ(texts->stories[0]->lines[1].runs.at(0).text, "cd");
    EXPECT_EQ(texts->stories[0]->lines[1].origin * texts->stories[0]->to_art, Geom::Point(10 + 54, 60 - 14));
}

TEST_F(AiNativeImportTest, AreaTypeAnchorsOverflowAndEmptyFrames)
{
    auto const centred = " /5 << /0 [ << /1 6 /0 << /0 << /5 << /0 2 >> >> >> >> ] >>";
    // Centred, and its glyphs sit about the middle of the line's box: anchored there.
    auto texts = AN::read_text_document(text_setup(area_text_document("(Hello\\r)", 6, {{0, 12, 6, 30, 40}}, "ArialMT", {}, centred)),
                                        Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_EQ(texts->stories[0]->lines[0].alignment, 2u);
    EXPECT_EQ(texts->stories[0]->lines[0].origin * texts->stories[0]->to_art, Geom::Point(60, 48));
    // Glyphs that sit elsewhere (an indent, a space left hanging): the line stays where it starts.
    texts = AN::read_text_document(text_setup(area_text_document("(Hello\\r)", 6, {{0, 12, 6, 20, 40}}, "ArialMT", {}, centred)),
                                   Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_EQ(texts->stories[0]->lines[0].alignment, 0u);
    EXPECT_EQ(texts->stories[0]->lines[0].origin * texts->stories[0]->to_art, Geom::Point(30, 48));
    // A justified paragraph's last line is set at its own width, from its start.
    auto const justified = " /5 << /0 [ << /1 6 /0 << /0 << /5 << /0 5 >> >> >> >> ] >>";
    texts = AN::read_text_document(text_setup(area_text_document("(Hello\\r)", 6, {{0, 12, 6, 30, 40}}, "ArialMT", {}, justified)),
                                   Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_EQ(texts->stories[0]->lines[0].alignment, 0u);
    EXPECT_EQ(texts->stories[0]->lines[0].justification, 5u);
    EXPECT_FALSE(texts->stories[0]->lines[0].justified_width);
    EXPECT_EQ(texts->stories[0]->lines[0].origin * texts->stories[0]->to_art, Geom::Point(40, 48));

    // A paragraph with nothing in it past the frame's end holds nothing; real text is kept.
    texts = AN::read_text_document(text_setup(area_text_document("(Hello \\r\\r)", 8, {{0, 12, 7}})), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_TRUE(texts->stories[0]->overflow.empty());
    EXPECT_EQ(texts->stories[0]->lines.at(0).runs.at(0).text, "Hello ");
    texts = AN::read_text_document(text_setup(area_text_document("(Hello \\rworld\\r)", 13, {{0, 12, 7}})), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_EQ(texts->stories[0]->overflow, "world\n");
    // Counts that reach past the story, or a line without one.
    texts = AN::read_text_document(text_setup(area_text_document("(Hello\\r)", 6, {{0, 12, 9}})), Geom::Point(100, 50));
    EXPECT_FALSE(texts->stories[0]);
    auto uncounted = area_text_document("(Hello\\r)", 6, {{0, 12, 6}});
    uncounted.replace(uncounted.find("/15 << /0 6 /2 0 /5 false >>"), 28, "");
    texts = AN::read_text_document(text_setup(uncounted), Geom::Point(100, 50));
    EXPECT_FALSE(texts->stories[0]);

    // A frame nothing was typed in has no layout at all.
    auto blank = area_text_document("(\\r)", 1, {}, "ArialMT", {}, {}, " ");
    texts = AN::read_text_document(text_setup(blank), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_TRUE(texts->stories[0]->empty);
    std::string error;
    auto ai = AN::read(records(layer("0 g\n" + rect + "/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(blank)), error);
    ASSERT_TRUE(ai) << error;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    EXPECT_TRUE(elements(b->document->getReprRoot(), "svg:text").empty());
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("nothing typed"), std::string::npos) << b->notes[0];
}

std::vector<std::pair<gunichar, Geom::Point>> set_characters(SPDocument *doc, XML::Node *repr);
void expect_characters_where_they_were(SPDocument *was_doc, XML::Node *was, SPDocument *now_doc, XML::Node *now);

/// A paragraph run of `units` aligned `alignment` (Illustrator's paragraph feature 0), with `more`.
std::string aligned_paragraph(int units, int alignment, std::string const &more = {})
{
    return " /5 << /0 [ << /1 " + std::to_string(units) + " /0 << /0 << /5 << /0 " + std::to_string(alignment) + more +
           " >> >> >> >> ] >>";
}

TEST_F(AiNativeImportTest, JustifiedAreaTypeKeepsEachLinesMeasure)
{
    using End = AN::TextLine::End;
    auto read = [](std::string const &characters, int units, std::vector<AreaLine> const &lines, std::string const &paragraphs) {
        return AN::read_text_document(text_setup(area_text_document(characters, units, lines, "ArialMT", {}, paragraphs)),
                                      Geom::Point(100, 50));
    };
    // Justified, its last line left: the line the frame wrapped reaches across the frame, 100
    // from its start; the last is set at its own width. Both from their starts.
    auto texts = read("(Hello world\\r)", 12, {{0, 12, 6}, {0, 26, 6}}, aligned_paragraph(12, 3));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    auto const *lines = &texts->stories[0]->lines;
    ASSERT_EQ(lines->size(), 2u);
    for (auto const &l : *lines) {
        EXPECT_EQ(l.justification, 3u);
        EXPECT_EQ(l.alignment, 0u);
        EXPECT_EQ(l.paragraph_alignment, 0u);
    }
    ASSERT_TRUE((*lines)[0].justified_width);
    EXPECT_DOUBLE_EQ(*(*lines)[0].justified_width, 100);
    EXPECT_FALSE((*lines)[1].justified_width);

    // Less its indents: the start one where the line starts, the end one off its box.
    texts = read("(Hello world\\r)", 12, {{0, 12, 6, 10}, {0, 26, 6, 10}}, aligned_paragraph(12, 3, " /2 10 /3 5"));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    ASSERT_TRUE(texts->stories[0]->lines[0].justified_width);
    EXPECT_DOUBLE_EQ(*texts->stories[0]->lines[0].justified_width, 85);

    // Justified to the last line too; a line break typed inside the paragraph is stretched as well.
    texts = read("(Hello world\\r)", 12, {{0, 12, 6}, {0, 26, 6}}, aligned_paragraph(12, 6));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_TRUE(texts->stories[0]->lines[1].justified_width);
    texts = read("(Hello\\003world\\r)", 12, {{0, 12, 6}, {0, 26, 6}}, aligned_paragraph(12, 5));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    lines = &texts->stories[0]->lines;
    EXPECT_EQ((*lines)[0].end, End::Break);
    EXPECT_TRUE((*lines)[0].justified_width);
    EXPECT_FALSE((*lines)[1].justified_width);
    EXPECT_EQ((*lines)[1].paragraph_alignment, 2u);
    EXPECT_EQ((*lines)[1].alignment, 0u);

    // What PROOF doesn't justify alike is refused by name: a paragraph that reads from the right,
    // and Illustrator's spacing settings other than its defaults.
    for (auto const *more : {" /33 1", " /17 [0.8 0.9 1.33]", " /19 [0.9 1 1.1]"}) {
        SCOPED_TRACE(more);
        texts = read("(Hello world\\r)", 12, {{0, 12, 6}, {0, 26, 6}}, aligned_paragraph(12, 3, more));
        EXPECT_FALSE(texts->stories[0]);
        EXPECT_NE(texts->errors[0].find("justified"), std::string::npos) << texts->errors[0];
    }
}

/// Area type in the 100 x 40 frame of area_text_document, its layout tree written out: each
/// line's character count, its glyph run (`glyphs` glyphs, which `older` counts as Illustrator
/// before 2025 does) with what Illustrator added to them (the body of its 6), and the
/// segment's clusters (the body of its count's 1).
struct GlyphLine
{
    int units, glyphs;
    std::string added, clusters = {};
    bool older = false;
};
std::string justified_layout(std::vector<GlyphLine> const &lines)
{
    std::string laid;
    int y = 12;
    for (auto const &l : lines) {
        std::string ids;
        for (int i = 0; i < l.glyphs; ++i) ids += " 0";
        laid += "<< /99 /L /0 << /0 [0 " + std::to_string(y) + "] >> /1 [0 -10 100 3] /6 [ << /99 /S /15 << /0 " +
                std::to_string(l.units) + (l.clusters.empty() ? "" : " /1 [ " + l.clusters + " ]") +
                " /2 0 /5 false >> /6 [ << /99 /G /1 [0 -10 100 3] " +
                (l.older ? "/5 [" + ids + " ]" : "/21 << /0 [" + ids + " ] >>") +
                (l.added.empty() ? "" : " /6 << " + l.added + " >>") + " >> ] >> ] >> ";
        y += 14;
    }
    return "/2 [ << /99 /PC /6 [ << /99 /F /0 << /0 [8191.5 8191.5] >> /1 [0 0 100 40] /6 [ << /99 /R /6 [ "
           "<< /99 /R /6 [ " + laid + "] >> ] >> ] >> ] >> ]";
}

TEST_F(AiNativeImportTest, JustifiedLinesAreCheckedAgainstHowIllustratorWidenedTheirSpaces)
{
    auto read = [](std::string const &characters, int units, std::vector<GlyphLine> const &lines, int alignment = 3) {
        auto const doc = area_text_document(characters, units, {}, "ArialMT", {}, aligned_paragraph(units, alignment),
                                            justified_layout(lines));
        return AN::read_text_document(text_setup(doc), Geom::Point(100, 50));
    };
    auto refused_for = [](auto const &texts, char const *why) {
        EXPECT_FALSE(texts->stories[0]);
        EXPECT_NE(texts->errors[0].find(why), std::string::npos) << texts->errors[0];
    };
    // "aa bb " then "cc": the space between the words widened by a quarter of its width, the one
    // at the line's end left alone, as Illustrator 29.2.1 does.
    auto const widened = "/0 [ << >> << /1 0.25 >> << >> ] /1 [2 1 3]";
    auto texts = read("(aa bb cc\\r)", 9, {{6, 6, widened}, {3, 3, ""}});
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_TRUE(texts->stories[0]->lines[0].justified_width);
    // As older files count their glyphs, too.
    texts = read("(aa bb cc\\r)", 9, {{6, 6, widened, "", true}, {3, 3, ""}});
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    // Tracking on every glyph doesn't move what is justified.
    texts = read("(aa bb cc\\r)", 9, {{6, 6, "/0 [ << /3 0.5 >> << /1 0.25 /3 0.5 >> << /3 0.5 >> ] /1 [2 1 3]"}, {3, 3, ""}});
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    // A ligature is one glyph for its letters: the space after "ffi" is the second glyph.
    texts = read("(ffi aa bb\\r)", 10, {{7, 5, "/0 [ << >> << /1 0.3 >> << >> ] /1 [1 1 3]", "<< /0 1 /1 0 /2 3 >>"}, {3, 3, ""}});
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];

    // Counted without its cluster, that glyph would be a letter: Illustrator spacing letters.
    refused_for(read("(ffi aa bb\\r)", 10, {{7, 5, "/0 [ << >> << /1 0.3 >> << >> ] /1 [1 1 3]"}, {3, 3, ""}}),
                "spaces its letters");
    refused_for(read("(aa bb cc\\r)", 9, {{6, 6, "/0 [ << /1 0.25 >> << >> ] /1 [1 5]"}, {3, 3, ""}}), "spaces its letters");
    // Word spaces widened by different shares, or one left alone.
    refused_for(read("(aa bb cc dd\\r)", 12, {{9, 9, "/0 [ << >> << /1 0.2 >> << >> << /1 0.3 >> << >> ] /1 [2 1 2 1 3]"}, {3, 3, ""}}),
                "unequally");
    refused_for(read("(aa bb cc dd\\r)", 12, {{9, 9, "/0 [ << >> << /1 0.2 >> << >> ] /1 [2 1 6]"}, {3, 3, ""}}), "unequally");
    // A last line, set at its own width, with spaces of another width.
    refused_for(read("(aa bb cc dd\\r)", 12, {{6, 6, widened}, {6, 6, "/0 [ << >> << /1 0.1 >> << >> ] /1 [2 1 3]"}}),
                "wider or narrower");
    // Glyph counts that disagree.
    refused_for(read("(aa bb cc\\r)", 9, {{6, 5, widened}, {3, 3, ""}}), "glyph adjustments");
}

TEST_F(AiNativeImportTest, JustifiedAreaTypeIsStretchedAcrossItsFrame)
{
    // Courier New's advance is 1229/2048 em: at 12 points "aaa bbb" is seven of them, its space
    // one. To reach 100 the space takes the rest: its word spacing.
    double const advance = 12 * 1229.0 / 2048, extra = 100 - 7 * advance;
    auto *courier = FontFactory::get().parsePostscriptName("CourierNewPSMT", false);
    if (!courier) GTEST_SKIP() << "CourierNewPSMT isn't installed";
    pango_font_description_free(courier);
    auto const source = [](int alignment, std::string const &style = {}) {
        return records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                       text_setup(area_text_document("(aaa bbb ccc ddd\\r)", 16, {{0, 12, 8}, {0, 26, 8}}, "CourierNewPSMT",
                                                     style, aligned_paragraph(16, alignment))));
    };
    auto b = make(source(3));
    ASSERT_TRUE(b);
    auto const text = elements(b->document->getReprRoot(), "svg:text").at(0);
    EXPECT_NE(attr(text, "style").find("text-align:justify;"), std::string::npos) << attr(text, "style");
    EXPECT_EQ(attr(text, AN::TEXT_ALIGN_LAST_ATTRIBUTE), "start");
    auto const spans = elements(text, "svg:tspan");
    ASSERT_EQ(spans.size(), 2u);
    auto const spacing = [](XML::Node *span) -> std::optional<double> {
        auto const style = attr(span, "style");
        auto const at = style.find("word-spacing:");
        if (at == std::string::npos) return {};
        return g_ascii_strtod(style.c_str() + at + 13, nullptr);
    };
    ASSERT_TRUE(spacing(spans[0]));
    EXPECT_NEAR(*spacing(spans[0]), extra, 1e-4);
    EXPECT_FALSE(spacing(spans[1]));
    // Its second word starts so far along, in advances (the document's units aren't points).
    auto const chars = set_characters(b->document.get(), text);
    ASSERT_EQ(chars.size(), 15u);
    double const one = chars[1].second.x() - chars[0].second.x();
    EXPECT_NEAR((chars[4].second.x() - chars[0].second.x()) / one, 4 + extra / advance, 1e-4);

    // Justified to the last line too, the last line takes its share as well.
    b = make(source(6));
    ASSERT_TRUE(b);
    auto const all = elements(elements(b->document->getReprRoot(), "svg:text").at(0), "svg:tspan");
    EXPECT_EQ(attr(elements(b->document->getReprRoot(), "svg:text").at(0), AN::TEXT_ALIGN_LAST_ATTRIBUTE), "justify");
    ASSERT_TRUE(spacing(all.at(1)));
    EXPECT_NEAR(*spacing(all.at(1)), extra, 1e-4);

    // Tracked, the letters reach the measure without the tracking after the last of them, and
    // the space is widened with its own: 0.6 after each of the six characters before the last.
    b = make(source(3, "/8 50"));
    ASSERT_TRUE(b);
    auto const tracked = elements(elements(b->document->getReprRoot(), "svg:text").at(0), "svg:tspan");
    ASSERT_TRUE(spacing(tracked.at(0)));
    EXPECT_NEAR(*spacing(tracked.at(0)), 100 - 7 * advance - 6 * 0.6, 1e-4);
}

TEST_F(AiNativeImportTest, JustifiedAreaTypeMatchesIndependentPdf)
{
    // At 24 points Courier New's letters are 14.4 apart: "bb" ends at the frame's right side,
    // 100 from its left at 10, so it starts at 81.2.
    auto *font = FontFactory::get().parsePostscriptName("CourierNewPSMT", false);
    if (!font) GTEST_SKIP() << "CourierNewPSMT isn't installed";
    pango_font_description_free(font);
    auto src = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                       text_setup(area_text_document("(aa bb cc\\r)", 9, {{0, 22, 6}, {0, 46, 3}}, "CourierNewPSMT",
                                                     "/1 24 /53 << /0 << /0 0 /1 [1 0] >> >>", aligned_paragraph(9, 3))));
    std::string reason;
    auto good = Extension::Internal::open_ai_native(
        fixture(src, {"0 g BT /F1 24 Tf 10 38 Td (aa) Tj ET BT /F1 24 Tf 81.2 38 Td (bb) Tj ET BT /F1 24 Tf 10 14 Td (cc) Tj ET"},
                "/MediaBox [0 0 200 100]", "CourierNewPSMT"), reason);
    ASSERT_TRUE(good) << reason;
    // Where the words would be with their space as the font has it.
    EXPECT_FALSE(Extension::Internal::open_ai_native(
        fixture(src, {"0 g BT /F1 24 Tf 10 38 Td (aa) Tj ET BT /F1 24 Tf 53.2 38 Td (bb) Tj ET BT /F1 24 Tf 10 14 Td (cc) Tj ET"},
                "/MediaBox [0 0 200 100]", "CourierNewPSMT"), reason));
}

TEST_F(AiNativeImportTest, WrittenJustifiedAreaTypeKeepsItsJustification)
{
    auto *courier = FontFactory::get().parsePostscriptName("CourierNewPSMT", false);
    if (!courier) GTEST_SKIP() << "CourierNewPSMT isn't installed";
    pango_font_description_free(courier);
    for (int alignment : {3, 4, 5, 6}) {
        SCOPED_TRACE(alignment);
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                              text_setup(area_text_document("(aaa bbb\\003ccc ddd eee\\r)", 20, {{0, 12, 8}, {0, 26, 12}},
                                                            "CourierNewPSMT", {}, aligned_paragraph(20, alignment)))));
        ASSERT_TRUE(b);
        auto const written = AN::write_records(b->document.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto const texts = AN::read_text_document(written.records, {});
        ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
        auto const &lines = texts->stories[0]->lines;
        ASSERT_EQ(lines.size(), 2u);
        EXPECT_EQ(lines[0].end, AN::TextLine::End::Break);
        for (auto const &l : lines) EXPECT_EQ(l.justification, unsigned(alignment));
        // The reader finds its measure from the frame: PROOF writes no line boxes.
        ASSERT_TRUE(lines[0].justified_width);
        EXPECT_NEAR(*lines[0].justified_width, 100, 1e-3);
        EXPECT_EQ(bool(lines[1].justified_width), alignment == 6);
        // Opened again, every character is where it was.
        auto again = make(written.records);
        ASSERT_TRUE(again);
        expect_characters_where_they_were(b->document.get(), elements(b->document->getReprRoot(), "svg:text").at(0),
                                          again->document.get(), elements(again->document->getReprRoot(), "svg:text").at(0));
    }
    // Word spacing on a line that isn't stretched is the characters' own, which Illustrator has not.
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(area_text_document("(aaa bbb ccc ddd\\r)", 16, {{0, 12, 8}, {0, 26, 8}}, "CourierNewPSMT",
                                                        {}, aligned_paragraph(16, 3)))));
    ASSERT_TRUE(b);
    auto const last = elements(elements(b->document->getReprRoot(), "svg:text").at(0), "svg:tspan").at(1);
    last->setAttribute("style", attr(last, "style") + "word-spacing:3;");
    b->document->ensureUpToDate();
    auto const written = AN::write_records(b->document.get());
    ASSERT_FALSE(written.refused.empty());
    EXPECT_NE(written.refused.front().find("word spacing"), std::string::npos) << written.refused.front();
}

TEST_F(AiNativeImportTest, AreaTypeBuildsPositionedLinesAndKeepsItsFrame)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(area_text_document("(Hello \\rworld\\rmore\\r)", 18, {{0, 12, 7}, {0, 26, 6}}, font)));
    auto b = make(source);
    ASSERT_TRUE(b);
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    auto spans = elements(texts[0], "svg:tspan");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_STREQ(spans[0]->firstChild()->content(), "Hello ");
    EXPECT_STREQ(spans[1]->firstChild()->content(), "world");
    EXPECT_EQ(attr(spans[0], "y"), "8203.5");
    EXPECT_EQ(attr(spans[1], "y"), "8217.5");
    // The frame, in the text's own coordinates, and what it is too small to show.
    auto const frame = sp_svg_read_pathv(attr(texts[0], AN::TEXT_FRAME_ATTRIBUTE).c_str());
    ASSERT_EQ(frame.size(), 1u);
    EXPECT_EQ(*frame.boundsExact(), Geom::Rect(Geom::Point(8191.5, 8191.5), Geom::Point(8291.5, 8231.5)));
    EXPECT_EQ(attr(texts[0], AN::TEXT_OVERFLOW_ATTRIBUTE), "more\n");
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("more text than fits"), std::string::npos) << b->notes[0];
    // Drawn inside its frame on the page: 10..110 across, 40..80 down (the page is 100 high).
    auto bounds = drawn(b->document.get(), texts[0]);
    ASSERT_TRUE(bounds);
    EXPECT_GT(bounds->left(), 9.0);
    EXPECT_LT(bounds->right(), 110.0);
    EXPECT_GT(bounds->top(), 40.0);
    EXPECT_LT(bounds->bottom(), 72.0);
    // Both survive a save and reopen.
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto again = elements(reopened->getReprRoot(), "svg:text");
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(attr(again[0], AN::TEXT_FRAME_ATTRIBUTE), attr(texts[0], AN::TEXT_FRAME_ATTRIBUTE));
    EXPECT_EQ(attr(again[0], AN::TEXT_OVERFLOW_ATTRIBUTE), "more\n");
}

/// Area type of three paragraphs: a centred one Illustrator hyphenates, which the frame wrapped
/// after "Hello ", then two right-aligned ones, the first with a line break typed in it (U+0003,
/// where Illustrator ends a paragraph run too), and `extra` after them.
std::string paragraphs_in_a_frame(std::string const &font, std::string const &extra = {}, int more = 0)
{
    auto const run = [](int units, std::string const &own) {
        return "<< /1 " + std::to_string(units) + " /0 << /0 << /5 << " + own + " >> >> >> >> ";
    };
    std::string runs = run(12, "/0 2 /9 true") + run(5, "/0 1") + run(5, "/0 1");
    if (more) runs += run(more, "/0 1");
    return area_text_document("(Hello world\\rNext\\003line\\r" + extra + ")", 22 + more,
                              {{0, 12, 6}, {0, 26, 6}, {0, 40, 5}, {0, 54, 5}}, font, {}, " /5 << /0 [ " + runs + "] >>");
}

TEST_F(AiNativeImportTest, AreaTypeKeepsItsParagraphsWhereTheFrameWrappedThem)
{
    auto texts = AN::read_text_document(text_setup(paragraphs_in_a_frame("ArialMT")), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
    auto const &lines = texts->stories[0]->lines;
    ASSERT_EQ(lines.size(), 4u);
    using End = AN::TextLine::End;
    EXPECT_EQ(lines[0].end, End::Wrap);
    EXPECT_EQ(lines[1].end, End::Paragraph);
    EXPECT_EQ(lines[2].end, End::Break);
    EXPECT_EQ(lines[3].end, End::Paragraph);
    for (std::size_t i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(lines[i].paragraph_alignment, i < 2 ? 2u : 1u);
        EXPECT_EQ(lines[i].hyphenate, i < 2);
    }
    EXPECT_EQ(lines[2].runs.at(0).text, "Next");

    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // On the page: each line says how it ended, and its paragraph's alignment and hyphenation,
    // which differ here, are each line's own.
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(paragraphs_in_a_frame(font))));
    ASSERT_TRUE(b);
    auto const text = elements(b->document->getReprRoot(), "svg:text").at(0);
    auto const spans = elements(text, "svg:tspan");
    ASSERT_EQ(spans.size(), 4u);
    char const *const ends[] = {"wrap", "", "break", ""};
    for (std::size_t i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(attr(spans[i], AN::LINE_END_ATTRIBUTE), ends[i]);
        EXPECT_NE(attr(spans[i], "style").find(i < 2 ? "text-align:center" : "text-align:end"), std::string::npos);
        EXPECT_EQ(attr(spans[i], AN::HYPHENATE_ATTRIBUTE), i < 2 ? "true" : "");
    }
    EXPECT_EQ(attr(text, "style").find("text-align"), std::string::npos);
    // How the frame places its first baseline (5, the em box), for laying it out again.
    EXPECT_EQ(attr(text, AN::FIRST_BASELINE_ATTRIBUTE), "5 0");
    // What all its paragraphs share is the text's.
    auto const one = area_text_document("(Hello world\\r)", 12, {{0, 12, 6}, {0, 26, 6}}, font, {},
                                        " /5 << /0 [ << /1 12 /0 << /0 << /5 << /0 2 /9 true >> >> >> >> ] >>");
    b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(one)));
    ASSERT_TRUE(b);
    auto const shared = elements(b->document->getReprRoot(), "svg:text").at(0);
    EXPECT_NE(attr(shared, "style").find("text-align:center"), std::string::npos);
    EXPECT_EQ(attr(shared, AN::HYPHENATE_ATTRIBUTE), "true");
    for (auto *span : elements(shared, "svg:tspan")) {
        EXPECT_EQ(attr(span, "style").find("text-align"), std::string::npos);
        EXPECT_EQ(attr(span, AN::HYPHENATE_ATTRIBUTE), "");
    }
}

TEST_F(AiNativeImportTest, BaselineShiftMovesOnlyItsOwnCharacters)
{
    auto shifted = AN::read_text_document(text_setup(text_document("(Hi\\r)", 3, "ArialMT", "/9 -1.5")), Geom::Point(100, 50));
    ASSERT_TRUE(shifted->stories[0]) << shifted->errors[0];
    EXPECT_EQ(shifted->stories[0]->lines.at(0).runs.at(0).style.baseline_shift, -1.5);

    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto top = [&](std::string const &style, std::string *span_style = nullptr, std::string *text_style = nullptr) {
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                              text_setup(text_document("(HHH\\r)", 4, font, "/1 20 " + style))));
        EXPECT_TRUE(b);
        if (!b) return 0.0;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        auto spans = elements(texts[0], "svg:tspan");
        if (span_style && !spans.empty()) *span_style = attr(spans.back(), "style");
        if (text_style) *text_style = attr(texts[0], "style");
        auto bounds = drawn(b->document.get(), texts[0]);
        EXPECT_TRUE(bounds);
        return bounds ? bounds->top() : 0.0;
    };
    std::string span, text;
    double const plain = top({});
    double const raised = top("/9 6", &span, &text);
    double const lowered = top("/9 -4");
    // Above the baseline is up the page; the document's y points down.
    EXPECT_NEAR(raised, plain - 6, 0.05);
    EXPECT_NEAR(lowered, plain + 4, 0.05);
    // The span the characters are in carries the shift, not the text object.
    EXPECT_NE(span.find("baseline-shift:6"), std::string::npos) << span;
    EXPECT_EQ(text.find("baseline-shift"), std::string::npos) << text;
}

TEST_F(AiNativeImportTest, TypeAnAppearanceDrewHasStoriesOfItsOwn)
{
    // An object with an appearance: what that draws (type marked FreeUndo, numbered among the
    // drawn stories), then the object itself on %_ lines, numbered among the typed ones.
    auto const art = std::string("u\n/AI11Text :\n1 /FreeUndo ,\n0 /StoryIndex ,\n;\nU\n"
                                 "%_/AI11Text :\n%_0 /FreeUndo ,\n%_0 /StoryIndex ,\n%_;\n1 (Anon) XW\n");
    auto const red = "/53 << /0 << /0 1 /1 [1 1 0 0] >> >>";
    auto documents = AN::read_text_document(text_setup(text_document(), text_document("(World\\r)", 6, "ArialMT", red)),
                                            Geom::Point(100, 50));
    ASSERT_TRUE(documents->error.empty()) << documents->error;
    ASSERT_EQ(documents->stories.size(), 1u);
    ASSERT_EQ(documents->drawn.size(), 1u);
    ASSERT_TRUE(documents->stories[0] && documents->drawn[0]) << documents->drawn_errors[0];
    EXPECT_EQ(documents->stories[0]->lines.at(0).runs.at(0).text, "Hello");
    EXPECT_EQ(documents->drawn[0]->lines.at(0).runs.at(0).text, "World");
    EXPECT_FALSE(AN::same_drawing(*documents->stories[0], *documents->drawn[0]));
    EXPECT_TRUE(AN::same_drawing(*documents->stories[0], *documents->stories[0]));
    // A second document that can't be read leaves drawn type without stories; the first stands.
    auto broken = text_setup(text_document(), "/0 [ nonsense");
    documents = AN::read_text_document(broken, Geom::Point(100, 50));
    EXPECT_TRUE(documents->error.empty());
    EXPECT_EQ(documents->stories.size(), 1u);
    EXPECT_TRUE(documents->drawn.empty());

    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // The appearance drew nothing but the type as it was typed: the object stands, editable.
    auto b = make(records(layer(art), text_setup(text_document("(Hello\\r)", 6, font), text_document("(Hello\\r)", 6, font))));
    ASSERT_TRUE(b);
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_EQ(attr(texts[0], "proof:ai-story"), "0");
    EXPECT_EQ(attr(texts[0], "proof:ai-drawn-story"), "");
    EXPECT_STREQ(texts[0]->parent()->attribute("inkscape:groupmode"), "layer");
    EXPECT_TRUE(b->notes.empty());

    // The appearance paints it red: the look stands, set from its own story.
    b = make(records(layer(art), text_setup(text_document("(Hello\\r)", 6, font), text_document("(Hello\\r)", 6, font, red))));
    ASSERT_TRUE(b);
    texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_EQ(attr(texts[0], "proof:ai-drawn-story"), "0");
    EXPECT_EQ(attr(texts[0], "proof:ai-story"), "");
    EXPECT_NE(attr(texts[0], "style").find("fill:#ff0000"), std::string::npos) << attr(texts[0], "style");
    EXPECT_EQ(attr(texts[0]->parent(), "proof:drawn-look"), "true");
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("came in as groups of what they draw"), std::string::npos) << b->notes[0];

    // Drawn type whose number is past the drawn stories has nothing to say what it sets.
    std::string error;
    auto const lone = std::string("/AI11Text :\n1 /FreeUndo ,\n3 /StoryIndex ,\n;\n");
    EXPECT_FALSE(AN::read(records(layer(lone), text_setup(text_document("(Hello\\r)", 6, font), text_document("(Hello\\r)", 6, font))), error));
    EXPECT_NE(error.find("type drawn by an appearance"), std::string::npos) << error;
}

TEST_F(AiNativeImportTest, StoriesCanHoldACharacterThatSetsNothing)
{
    // A NUL among the characters (a paste leaves one): it counts as a character and sets none.
    auto texts = AN::read_text_document(text_setup(text_document("(\\376\\377\\000H\\000i\\000\\000\\000\\015)", 4)),
                                        Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    ASSERT_EQ(texts->stories[0]->lines.size(), 1u);
    EXPECT_EQ(texts->stories[0]->lines[0].runs.at(0).text, "Hi");
}

TEST_F(AiNativeImportTest, KerningAndLigaturesFollowTheRun)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    std::vector<std::string> notes;
    auto width = [&](std::string const &style, std::string *text_style = nullptr) {
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                              text_setup(text_document("(AVAVAVAV\\r)", 9, font, "/1 40 " + style))));
        EXPECT_TRUE(b);
        if (!b) return 0.0;
        notes = b->notes;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        if (text_style) *text_style = attr(texts[0], "style");
        auto bounds = drawn(b->document.get(), texts[0]);
        EXPECT_TRUE(bounds);
        return bounds ? bounds->width() : 0.0;
    };
    std::string plain, off, substitutions, optical;
    double const kerned = width({}, &plain);
    // The font's own kerning is what a run has unless it says otherwise: nothing is written.
    EXPECT_EQ(plain.find("font-feature-settings"), std::string::npos) << plain;
    EXPECT_EQ(plain.find("font-variant-ligatures"), std::string::npos) << plain;
    EXPECT_TRUE(notes.empty());
    // Kerning switched off: A and V stand their full widths apart.
    double const unkerned = width("/11 0", &off);
    EXPECT_NE(off.find("font-feature-settings:'kern' 0"), std::string::npos) << off;
    EXPECT_GT(unkerned, kerned + 5.0);
    width("/18 false /20 false", &substitutions);
    EXPECT_NE(substitutions.find("font-variant-ligatures:no-common-ligatures no-contextual"), std::string::npos) << substitutions;
    EXPECT_NE(substitutions.find("font-feature-settings:normal"), std::string::npos) << substitutions;
    // Optical kerning is Illustrator's own: the font's stands in, and the import says so.
    EXPECT_NEAR(width("/11 2", &optical), kerned, 1e-6);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("kerned optically"), std::string::npos) << notes[0];
    auto bad = AN::read_text_document(text_setup(text_document("(Hi\\r)", 3, "ArialMT", "/11 7")), Geom::Point(100, 50));
    EXPECT_FALSE(bad->stories[0]);
}

// ---- instances of variable fonts ----------------------------------------------------------

namespace {

/// `document` with this font's entry given axis values, in 16.16 fixed point as a text
/// document keeps them.
std::string with_design(std::string document, std::string const &font, std::string const &values)
{
    auto const entry = "/0 (" + font + ")";
    auto const at = document.find(entry);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos) document.insert(at + entry.size(), " /4 [" + values + "]");
    return document;
}

std::string fixed(std::vector<double> const &design)
{
    std::string out;
    for (auto v : design) out += " " + std::to_string(std::llround(v * 65536));
    return out + " ";
}

/// Bahnschrift comes with Windows 10 and later: weight 300 to 700 (400 by default) and
/// width 75 to 100 (100 by default), in that order.
bool has_bahnschrift()
{
    return bool(AN::find_font("Bahnschrift"));
}

} // namespace

TEST_F(AiNativeImportTest, VariableFontInstanceKeepsItsAxisValues)
{
    std::string const name = "AcuminConcept_396.000wght_64.000wdth_4.200slnt";
    auto read = [&](std::string const &values) {
        auto texts = AN::read_text_document(text_setup(with_design(text_document("(Hi\\r)", 3, name), name, values)),
                                            Geom::Point(100, 50));
        EXPECT_TRUE(texts->stories.at(0)) << texts->errors.at(0);
        return texts->stories[0] ? texts->stories[0]->lines.at(0).runs.at(0).style : AN::TextStyle();
    };
    auto const style = read(fixed({396, 64, 4.2}));
    EXPECT_EQ(style.font, name);
    ASSERT_EQ(style.design.size(), 3u);
    EXPECT_NEAR(style.design[0], 396, 1e-4);
    EXPECT_NEAR(style.design[1], 64, 1e-4);
    EXPECT_NEAR(style.design[2], 4.2, 1e-4);
    // Values that aren't numbers leave the name to say which instance it is.
    EXPECT_TRUE(read(" 25952256 (wide) ").design.empty());
    // An ordinary font has none.
    auto plain = AN::read_text_document(text_setup(text_document()), Geom::Point(100, 50));
    ASSERT_TRUE(plain->stories.at(0));
    EXPECT_TRUE(plain->stories[0]->lines.at(0).runs.at(0).style.design.empty());
    // Two instances of one font aren't the same drawing.
    auto a = AN::read_text_document(text_setup(with_design(text_document("(Hi\\r)", 3, name), name, fixed({396, 64, 4.2}))),
                                    Geom::Point(100, 50));
    auto b = AN::read_text_document(text_setup(with_design(text_document("(Hi\\r)", 3, name), name, fixed({400, 64, 4.2}))),
                                    Geom::Point(100, 50));
    ASSERT_TRUE(a->stories.at(0) && b->stories.at(0));
    EXPECT_TRUE(AN::same_drawing(*a->stories[0], *a->stories[0]));
    EXPECT_FALSE(AN::same_drawing(*a->stories[0], *b->stories[0]));
}

TEST_F(AiNativeImportTest, VariableFontInstancesAreFoundByNameAndAxisValues)
{
    if (!has_bahnschrift()) GTEST_SKIP() << "Bahnschrift isn't installed";
    auto has = [](std::optional<AN::FontMatch> const &font, char const *declaration) {
        return font && font->css.find(declaration) != std::string::npos;
    };
    // The font itself: an instance with every axis at its default.
    auto itself = AN::find_font("Bahnschrift");
    ASSERT_TRUE(itself);
    EXPECT_TRUE(itself->variable);
    EXPECT_TRUE(itself->face);
    EXPECT_TRUE(has(itself, "font-family:Bahnschrift;")) << itself->css;
    EXPECT_TRUE(has(itself, "font-variation-settings:'wdth' 100, 'wght' 400;")) << itself->css;
    EXPECT_TRUE(has(itself, "font-weight:400;")) << itself->css;
    // A named instance, which the system lists under its name.
    auto condensed = AN::find_font("Bahnschrift-SemiBoldCondensed");
    ASSERT_TRUE(condensed);
    EXPECT_TRUE(has(condensed, "font-variation-settings:'wdth' 75, 'wght' 600;")) << condensed->css;
    EXPECT_TRUE(has(condensed, "font-weight:600;")) << condensed->css;
    EXPECT_TRUE(has(condensed, "font-stretch:condensed;")) << condensed->css;
    // One it doesn't list: Regular is the font itself to the system.
    auto regular = AN::find_font("Bahnschrift-Regular");
    ASSERT_TRUE(regular);
    EXPECT_EQ(regular->css, itself->css);
    // An instance between the named ones, by the values the document keeps.
    auto between = AN::find_font("Bahnschrift_350.000wght_80.000wdth", {350, 80});
    ASSERT_TRUE(between);
    EXPECT_TRUE(has(between, "font-variation-settings:'wdth' 80, 'wght' 350;")) << between->css;
    EXPECT_TRUE(has(between, "font-weight:350;")) << between->css;
    // Without them its name spells them out, an axis at its default left unsaid.
    auto spelled = AN::find_font("Bahnschrift_350.000wght_80.000wdth");
    ASSERT_TRUE(spelled);
    EXPECT_EQ(spelled->css, between->css);
    auto light = AN::find_font("Bahnschrift_300.000wght");
    ASSERT_TRUE(light);
    EXPECT_TRUE(has(light, "font-variation-settings:'wdth' 100, 'wght' 300;")) << light->css;
    // Another version of a font names an instance otherwise: the values still say which.
    auto renamed = AN::find_font("Bahnschrift-SemiboldCond", {600, 75});
    ASSERT_TRUE(renamed);
    EXPECT_EQ(renamed->css, condensed->css);
    // The values win over the name, and stay within the font's axes.
    auto moved = AN::find_font("Bahnschrift-SemiBoldCondensed", {900, 10});
    ASSERT_TRUE(moved);
    EXPECT_TRUE(has(moved, "font-variation-settings:'wdth' 75, 'wght' 700;")) << moved->css;
    // Not this font's instances.
    EXPECT_FALSE(AN::find_font("Bahnschrift-NoSuchStyle"));
    EXPECT_FALSE(AN::find_font("Bahnschriftx_350.000wght"));
    EXPECT_FALSE(AN::find_font("Bahnschrift_350.000zzzz"));
    EXPECT_FALSE(AN::find_font("Bahnschrift_wght"));
    // As many values as another font's axes: the name has to say.
    EXPECT_FALSE(AN::find_font("Bahnschrift-NoSuchStyle", {350, 80, 3}));
    // An ordinary font is as it was: no axes.
    if (auto font = available_text_font(); !font.empty()) {
        auto plain = AN::find_font(font, {350, 80});
        ASSERT_TRUE(plain);
        EXPECT_FALSE(plain->variable);
        EXPECT_EQ(plain->css.find("font-variation-settings"), std::string::npos) << plain->css;
    }
}

TEST_F(AiNativeImportTest, VariableFontInstanceSetsTextInItsOwnShapes)
{
    if (!has_bahnschrift()) GTEST_SKIP() << "Bahnschrift isn't installed";
    auto width = [&](std::string const &font, std::vector<double> const &design, std::string *style = nullptr) {
        auto data = text_document("(HHHHHHHH\\r)", 9, font, "/1 40");
        if (!design.empty()) data = with_design(data, font, fixed(design));
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(data)));
        EXPECT_TRUE(b);
        if (!b) return 0.0;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        if (style) *style = attr(texts[0], "style");
        auto bounds = drawn(b->document.get(), texts[0]);
        EXPECT_TRUE(bounds);
        return bounds ? bounds->width() : 0.0;
    };
    std::string style;
    double const regular = width("Bahnschrift", {});
    double const narrow = width("Bahnschrift-LightCondensed", {});
    double const bold = width("Bahnschrift-Bold", {700, 100});
    double const between = width("Bahnschrift_350.000wght_80.000wdth", {350, 80}, &style);
    EXPECT_NE(style.find("font-variation-settings:'wdth' 80, 'wght' 350"), std::string::npos) << style;
    EXPECT_GT(regular, 100);
    EXPECT_LT(narrow, regular - 20);
    // Its heavier stems reach a little further at both ends; the letters stand as far apart.
    EXPECT_GT(bold, regular + 0.5);
    EXPECT_GT(between, narrow + 2);
    EXPECT_LT(between, regular - 20);
    // It survives being saved and opened again.
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(with_design(text_document("(HHHHHHHH\\r)", 9, "Bahnschrift-X", "/1 40"), "Bahnschrift-X",
                                                 fixed({350, 80})))));
    ASSERT_TRUE(b);
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    auto again = elements(reopened->getReprRoot(), "svg:text");
    ASSERT_EQ(again.size(), 1u);
    auto bounds = drawn(reopened.get(), again[0]);
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), between, 1e-6);
}

TEST_F(AiNativeImportTest, VariableFontAxesStopAtTheirOwnRuns)
{
    auto plain = available_text_font();
    if (!has_bahnschrift() || plain.empty()) GTEST_SKIP() << "Bahnschrift or the plain test font isn't installed";
    auto build = [&](std::string const &first_font, std::vector<double> const &first_design, std::string const &second_font,
                     std::vector<double> const &second_design) {
        std::string error;
        auto ai = AN::read(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(text_document("(Hello\\r)", 6, plain))), error);
        EXPECT_TRUE(ai) << error;
        auto styled = std::make_shared<AN::TextDocument>(*ai->texts);
        auto &line = styled->stories.at(0)->lines.at(0);
        auto first = line.runs.at(0);
        auto second = first;
        first.text = "He";
        first.style.font = first_font;
        first.style.design = first_design;
        second.text = "llo";
        second.style.font = second_font;
        second.style.design = second_design;
        line.runs = {first, second};
        ai->texts = styled;
        auto b = AN::build(*ai, error);
        EXPECT_TRUE(b) << error;
        return b;
    };
    // An ordinary font after an instance says it has no axes: it would inherit the object's.
    auto b = build("Bahnschrift-Light", {300, 100}, plain, {});
    ASSERT_TRUE(b);
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    auto spans = elements(texts[0], "svg:tspan");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_NE(attr(texts[0], "style").find("font-variation-settings:'wdth' 100, 'wght' 300"), std::string::npos) << attr(texts[0], "style");
    EXPECT_NE(attr(spans[1], "style").find("font-variation-settings:normal"), std::string::npos) << attr(spans[1], "style");
    // An instance after an ordinary font carries its own; so does one after another.
    b = build(plain, {}, "Bahnschrift-Light", {300, 100});
    ASSERT_TRUE(b);
    texts = elements(b->document->getReprRoot(), "svg:text");
    spans = elements(texts.at(0), "svg:tspan");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_NE(attr(texts[0], "style").find("font-variation-settings:normal"), std::string::npos) << attr(texts[0], "style");
    EXPECT_NE(attr(spans[1], "style").find("font-variation-settings:'wdth' 100, 'wght' 300"), std::string::npos) << attr(spans[1], "style");
    b = build("Bahnschrift-Light", {300, 100}, "Bahnschrift-Bold", {700, 100});
    ASSERT_TRUE(b);
    spans = elements(elements(b->document->getReprRoot(), "svg:text").at(0), "svg:tspan");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_NE(attr(spans[1], "style").find("font-variation-settings:'wdth' 100, 'wght' 700"), std::string::npos) << attr(spans[1], "style");
    // Text with no instance in it says nothing about axes.
    b = build(plain, {}, plain, {});
    ASSERT_TRUE(b);
    EXPECT_EQ(attr(elements(b->document->getReprRoot(), "svg:text").at(0), "style").find("font-variation-settings"), std::string::npos);
}

TEST_F(AiNativeImportTest, TextGetsItsFontBackOnceItIsInstalled)
{
    auto font = available_text_font();
    if (font.empty() || !has_bahnschrift()) GTEST_SKIP() << "the test fonts aren't installed";
    // A document saved while two fonts weren't installed, as the import writes it: the
    // object's font and one span's stand in for an ordinary font and for an instance of a
    // variable one, another span's for a font that still isn't there, and a last span was
    // given a font by hand since.
    auto stand_in = AN::substitute_font();
    ASSERT_TRUE(stand_in);
    EXPECT_EQ(stand_in->css.find("-inkscape-font-specification"), std::string::npos) << stand_in->css;
    // Font names are quoted with apostrophes: the attributes take the other quotes.
    auto q = [](std::string const &value) { return "\"" + value + "\""; };
    auto doc = SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:proof='urn:x-proof:document:1' width='400' height='100'>"
        "<text xml:space='preserve' proof:font-missing='" + font + "' style=" + q("font-size:40px;fill:#ff0000;" + stand_in->css) + ">"
        "<tspan x='10' y='60'>ab"
        "<tspan proof:font-missing='Bahnschrift_350.000wght' proof:font-missing-axes='350 80' style=" + q(stand_in->css) + ">cd</tspan>"
        "<tspan proof:font-missing='PROOF-Missing-Unique-Font-734' style=" + q(stand_in->css) + ">ef</tspan>"
        "<tspan proof:font-missing='PROOF-Missing-Unique-Font-735' style='font-family:Arial;-inkscape-font-specification:Arial'>gh</tspan>"
        "</tspan></text></svg>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto before = AN::missing_fonts(doc.get());
    // The one given a font by hand isn't standing in for anything.
    ASSERT_EQ(before.size(), 3u);
    EXPECT_EQ(before[0].name, font);
    EXPECT_EQ(before[1].name, "Bahnschrift_350.000wght");
    EXPECT_EQ(before[2].name, "PROOF-Missing-Unique-Font-734");
    ASSERT_EQ(before[2].texts.size(), 1u);
    EXPECT_EQ(before[0].texts, before[2].texts);

    EXPECT_EQ(AN::restore_fonts(doc.get()), 2u);
    auto texts = elements(doc->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    auto spans = elements(texts[0], "svg:tspan");
    ASSERT_EQ(spans.size(), 4u);
    // Its own font again, with the rest of its style as it was. On a machine where the font
    // that stands in is this very font (Arial, without Myriad Pro), the declarations are the
    // same ones as before: what says the text has its font is the specification beside them.
    EXPECT_EQ(attr(texts[0], "proof:font-missing"), "");
    EXPECT_NE(attr(texts[0], "style").find("-inkscape-font-specification"), std::string::npos) << attr(texts[0], "style");
    EXPECT_NE(attr(texts[0], "style").find("font-size:40px"), std::string::npos) << attr(texts[0], "style");
    EXPECT_NE(attr(texts[0], "style").find("fill:#ff0000"), std::string::npos) << attr(texts[0], "style");
    EXPECT_EQ(attr(spans[1], "proof:font-missing"), "");
    EXPECT_EQ(attr(spans[1], "proof:font-missing-axes"), "");
    EXPECT_NE(attr(spans[1], "style").find("font-variation-settings:'wdth' 80, 'wght' 350"), std::string::npos) << attr(spans[1], "style");
    // The font that still isn't installed keeps its name; the one chosen by hand drops its.
    EXPECT_EQ(attr(spans[2], "proof:font-missing"), "PROOF-Missing-Unique-Font-734");
    EXPECT_EQ(attr(spans[3], "proof:font-missing"), "");
    EXPECT_NE(attr(spans[3], "style").find("font-family:Arial"), std::string::npos) << attr(spans[3], "style");
    auto after = AN::missing_fonts(doc.get());
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].name, "PROOF-Missing-Unique-Font-734");
    // Nothing more to give back.
    EXPECT_EQ(AN::restore_fonts(doc.get()), 0u);
}

TEST_F(AiNativeImportTest, AFontThatIsNotInstalledIsReplacedAcrossTheDocument)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial isn't installed";
    auto stand_in = AN::substitute_font();
    ASSERT_TRUE(stand_in);
    auto q = [](std::string const &value) { return "\"" + value + "\""; };
    // Two texts in one missing font, one of them with a span in another and one in a font
    // chosen by hand; and a text whose spans alone say their fonts.
    auto doc = SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:proof='urn:x-proof:document:1' width='400' height='200'>"
        "<text id='a' xml:space='preserve' proof:font-missing='PROOF-Missing-One' style=" + q("font-size:40px;fill:#ff0000;" + stand_in->css) + ">"
        "<tspan x='10' y='60'>ab"
        "<tspan proof:font-missing='PROOF-Missing-Two' style=" + q(stand_in->css) + ">cd</tspan>"
        "<tspan style='font-family:Arial;-inkscape-font-specification:Arial'>ef</tspan>gh</tspan></text>"
        "<text id='b' xml:space='preserve' proof:font-missing='PROOF-Missing-One' proof:font-missing-axes='350 80' style=" +
        q("font-size:20px;font-variation-settings:'wght' 350;" + stand_in->css) + "><tspan x='10' y='120'>ij</tspan></text>"
        "<text id='c' xml:space='preserve' style='font-size:20px;font-family:Arial'><tspan x='10' y='160'>kl"
        "<tspan proof:font-missing='PROOF-Missing-Two' style=" + q(stand_in->css) + ">mn</tspan></tspan></text></svg>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto item = [&](char const *id) { return cast<SPItem>(doc->getObjectById(id)); };
    using Runs = std::vector<std::pair<unsigned, unsigned>>;
    // What a font stands in for, as the layout counts its characters: all of the first text
    // but the span given a font by hand.
    EXPECT_EQ(AN::standing_in(item("a")), (Runs{{0, 4}, {6, 8}}));
    EXPECT_EQ(AN::standing_in(item("b")), (Runs{{0, 2}}));
    EXPECT_EQ(AN::standing_in(item("c")), (Runs{{2, 4}}));
    auto before = AN::missing_fonts(doc.get());
    ASSERT_EQ(before.size(), 2u);
    EXPECT_EQ(before[0].name, "PROOF-Missing-One");
    EXPECT_EQ(before[0].texts.size(), 2u);
    EXPECT_EQ(before[1].texts.size(), 2u);

    // Declarations that don't say which font was chosen replace nothing.
    EXPECT_EQ(AN::replace_font(doc.get(), "PROOF-Missing-One", "font-family:Arial;"), 0u);
    EXPECT_EQ(AN::replace_font(doc.get(), "PROOF-Missing-None", "font-family:Arial;-inkscape-font-specification:'Arial Bold';"), 0u);
    std::string const chosen = "font-family:Arial;-inkscape-font-specification:'Arial Bold';font-weight:bold;font-style:normal;"
                               "font-stretch:normal;font-variant:normal;";
    auto const plain = drawn(doc.get(), item("b")->getRepr());
    EXPECT_EQ(AN::replace_font(doc.get(), "PROOF-Missing-One", chosen), 2u);
    for (auto const *id : {"a", "b"}) {
        auto *node = item(id)->getRepr();
        auto const style = attr(node, "style");
        EXPECT_EQ(attr(node, "proof:font-missing"), "") << id;
        EXPECT_EQ(attr(node, "proof:font-missing-axes"), "") << id;
        EXPECT_NE(style.find("-inkscape-font-specification:'Arial Bold'"), std::string::npos) << style;
        EXPECT_NE(style.find("font-weight:bold"), std::string::npos) << style;
        // An ordinary font has no axes: the instance's are gone.
        EXPECT_NE(style.find("font-variation-settings:normal"), std::string::npos) << style;
        EXPECT_EQ(style.find("'wght' 350"), std::string::npos) << style;
    }
    // The rest of the style stays, and so does the other missing font.
    EXPECT_NE(attr(item("a")->getRepr(), "style").find("font-size:40px"), std::string::npos);
    EXPECT_NE(attr(item("a")->getRepr(), "style").find("fill:#ff0000"), std::string::npos);
    auto const bold = drawn(doc.get(), item("b")->getRepr());
    ASSERT_TRUE(plain && bold);
    EXPECT_GT(bold->width(), plain->width() + 0.2);
    EXPECT_EQ(AN::standing_in(item("a")), (Runs{{2, 4}}));
    EXPECT_TRUE(AN::standing_in(item("b")).empty());
    auto after = AN::missing_fonts(doc.get());
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].name, "PROOF-Missing-Two");
    EXPECT_EQ(after[0].texts.size(), 2u);
    // The other font likewise, in both texts that have it.
    EXPECT_EQ(AN::replace_font(doc.get(), "PROOF-Missing-Two", chosen), 2u);
    EXPECT_TRUE(AN::missing_fonts(doc.get()).empty());
    EXPECT_TRUE(AN::standing_in(item("a")).empty());
    EXPECT_TRUE(AN::standing_in(item("c")).empty());
}

TEST_F(AiNativeImportTest, TheHighlightLiesOnTheTypeAFontStandsInFor)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial isn't installed";
    auto stand_in = AN::substitute_font();
    ASSERT_TRUE(stand_in);
    auto q = [](std::string const &value) { return "\"" + value + "\""; };
    // Two lines. In the first a span has a font chosen by hand; the second is one style
    // from end to end, in two spans.
    auto doc = SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:proof='urn:x-proof:document:1' width='400' height='200'>"
        "<text id='a' xml:space='preserve' proof:font-missing='PROOF-Missing-One' style=" + q("font-size:40px;" + stand_in->css) + ">"
        "<tspan x='10' y='60'>ab<tspan style='font-family:Arial;-inkscape-font-specification:Arial'>cd</tspan>ef</tspan>"
        "<tspan x='10' y='120'>gh<tspan style='fill:#ff0000'>ij</tspan></tspan></text>"
        "<text id='b' xml:space='preserve' style='font-size:40px;font-family:Arial'><tspan x='10' y='180'>kl</tspan></text></svg>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto *a = cast<SPText>(doc->getObjectById("a"));
    ASSERT_TRUE(a);
    auto const shape = Inkscape::Display::font_highlight_shape(a);
    // "ab", "ef", and the whole second line as one stretch.
    ASSERT_EQ(shape.size(), 12u);
    auto box = [&](unsigned i) { return Geom::Rect(shape[i * 4], shape[i * 4 + 2]); };
    // Each lies on its line, from above the letters to below the baseline.
    for (unsigned i = 0; i < 3; ++i) {
        double const baseline = i < 2 ? 60 : 120;
        EXPECT_LT(box(i).top(), baseline - 25) << i;
        EXPECT_GT(box(i).bottom(), baseline + 2) << i;
        EXPECT_LT(box(i).bottom(), baseline + 20) << i;
        // Corners go round from the top left.
        EXPECT_EQ(shape[i * 4], box(i).corner(0)) << i;
        EXPECT_EQ(shape[i * 4 + 1], box(i).corner(1)) << i;
        EXPECT_EQ(shape[i * 4 + 3], box(i).corner(3)) << i;
    }
    EXPECT_NEAR(box(0).left(), 10, 1e-6);
    // The span with a font of its own lies between the first two, uncovered.
    EXPECT_GT(box(1).left(), box(0).right() + 20);
    EXPECT_NEAR(box(2).left(), 10, 1e-6);
    // The type itself is inside what covers it.
    auto const drawn_second = a->layout.bounds(Geom::identity(), false, 6, 3);
    ASSERT_TRUE(drawn_second);
    EXPECT_TRUE(box(2).contains(*drawn_second));
    // Text in its own font has none, nor has text whose font was replaced.
    EXPECT_TRUE(Inkscape::Display::font_highlight_shape(cast<SPItem>(doc->getObjectById("b"))).empty());
    EXPECT_EQ(AN::replace_font(doc.get(), "PROOF-Missing-One", "font-family:Arial;-inkscape-font-specification:Arial;"), 1u);
    doc->ensureUpToDate();
    EXPECT_TRUE(Inkscape::Display::font_highlight_shape(a).empty());
}

TEST_F(AiNativeImportTest, TheCharacterButtonsReadWhatTheTextIs)
{
    auto doc = SPDocument::createNewDocFromMem(std::string(
        "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='200'>"
        "<text id='caps' style='font-size:20px;text-transform:uppercase'><tspan x='10' y='30'>ab</tspan></text>"
        "<text id='caps2' style='font-size:20px;text-transform:uppercase'><tspan x='10' y='60'>cd"
        "<tspan id='none' style='text-transform:none;font-variant-position:super'>ef</tspan></tspan></text>"
        "<text id='small' style='font-size:20px;font-variant-caps:small-caps;font-variant-position:sub'><tspan x='10' y='90'>gh</tspan></text>"
        "<text id='plain' style='font-size:20px'><tspan x='10' y='120'>ij</tspan></text></svg>"));
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto item = [&](char const *id) { return cast<SPItem>(doc->getObjectById(id)); };
    auto query = [&](std::vector<SPItem *> const &items, SPStyle &style) {
        return sp_desktop_query_style_from_list(items, &style, QUERY_STYLE_PROPERTY_FONTVARIANTS);
    };
    {
        // Texts that agree: all caps, on the line.
        SPStyle style(doc.get());
        EXPECT_EQ(query({item("caps"), item("caps2")}, style), QUERY_STYLE_MULTIPLE_SAME);
        EXPECT_TRUE(style.text_transform.set);
        EXPECT_EQ(style.text_transform.computed, SP_CSS_TEXT_TRANSFORM_UPPERCASE);
        EXPECT_EQ(style.font_variant_position.value, 0);
        EXPECT_EQ(style.font_variant_position.computed, SP_CSS_FONT_VARIANT_POSITION_NORMAL);
    }
    {
        // A span that undoes its text's capitals, above the line.
        SPStyle style(doc.get());
        EXPECT_EQ(query({item("none")}, style), QUERY_STYLE_SINGLE);
        EXPECT_TRUE(style.text_transform.set);
        EXPECT_EQ(style.text_transform.computed, SP_CSS_TEXT_TRANSFORM_NONE);
        EXPECT_EQ(style.font_variant_position.value, 0);
        EXPECT_EQ(style.font_variant_position.computed, SP_CSS_FONT_VARIANT_POSITION_SUPER);
    }
    {
        // Texts that differ in both: neither button is down.
        SPStyle style(doc.get());
        query({item("caps"), item("small")}, style);
        EXPECT_FALSE(style.text_transform.set);
        EXPECT_NE(style.font_variant_caps.value, 0);
        EXPECT_NE(style.font_variant_position.value, 0);
    }
    {
        SPStyle style(doc.get());
        EXPECT_EQ(query({item("small")}, style), QUERY_STYLE_SINGLE);
        EXPECT_TRUE(style.text_transform.set);
        EXPECT_EQ(style.text_transform.computed, SP_CSS_TEXT_TRANSFORM_NONE);
        EXPECT_EQ(style.font_variant_caps.value, 0);
        EXPECT_EQ(style.font_variant_caps.computed, SP_CSS_FONT_VARIANT_CAPS_SMALL);
        EXPECT_EQ(style.font_variant_position.computed, SP_CSS_FONT_VARIANT_POSITION_SUB);
    }
    {
        // Nothing that is text: nothing is said.
        SPStyle style(doc.get());
        EXPECT_EQ(query({}, style), QUERY_STYLE_NOTHING);
        EXPECT_FALSE(style.text_transform.set);
    }
}

// ---- capitals -----------------------------------------------------------------------------

TEST_F(AiNativeImportTest, TextTransformSetsCapitalsOneCharacterForOne)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial isn't installed";
    auto document = [](std::string const &style, std::string const &content) {
        return SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='100'><text xml:space='preserve' "
            "style='font-family:Arial;font-size:40px;" + style + "'><tspan x='10' y='60'>" + content + "</tspan></text></svg>");
    };
    auto bounds = [&](SPDocument *doc) {
        auto texts = elements(doc->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        return texts.empty() ? Geom::OptRect() : drawn(doc, texts[0]);
    };
    auto same = [&](std::string const &style, std::string const &typed, std::string const &set) {
        auto a = document(style, typed), b = document({}, set);
        auto const ra = bounds(a.get()), rb = bounds(b.get());
        EXPECT_TRUE(ra && rb);
        if (!ra || !rb) return false;
        EXPECT_NEAR(ra->left(), rb->left(), 1e-6) << typed;
        EXPECT_NEAR(ra->width(), rb->width(), 1e-6) << typed;
        EXPECT_NEAR(ra->height(), rb->height(), 1e-6) << typed;
        return true;
    };
    // Capitals draw as capitals typed.
    EXPECT_TRUE(same("text-transform:uppercase;", "Hello, world", "HELLO, WORLD"));
    EXPECT_TRUE(same("text-transform:lowercase;", "Hello, WORLD", "hello, world"));
    EXPECT_TRUE(same("text-transform:capitalize;", "hello wORLD it's", "Hello WORLD It's"));
    // Letters whose capitals take another number of bytes (a long s, a dotless i), Cyrillic,
    // and a sharp s, which has no one capital and stays.
    EXPECT_TRUE(same("text-transform:uppercase;",
                     "\xc5\xbft\xc4\xb1r \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 stra\xc3\x9f" "e",
                     "STIR \xd0\x9f\xd0\xa0\xd0\x98\xd0\x92\xd0\x95\xd0\xa2 STRA\xc3\x9f" "E"));
    // Inherited by a span, and undone by one.
    auto mixed = SPDocument::createNewDocFromMem(std::string(
        "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='100'><text xml:space='preserve' "
        "style='font-family:Arial;font-size:40px;text-transform:uppercase'><tspan x='10' y='60'>ab<tspan "
        "style='text-transform:none'>cd</tspan>ef</tspan></text></svg>"));
    auto plain = document({}, "ABcdEF");
    auto const rm = bounds(mixed.get()), rp = bounds(plain.get());
    ASSERT_TRUE(rm && rp);
    EXPECT_NEAR(rm->width(), rp->width(), 1e-6);
    // A word that runs through a span is still one word.
    auto through = SPDocument::createNewDocFromMem(std::string(
        "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='100'><text xml:space='preserve' "
        "style='font-family:Arial;font-size:40px;text-transform:capitalize'><tspan x='10' y='60'>hel<tspan "
        "style='fill:#ff0000'>lo wor</tspan>ld</tspan></text></svg>"));
    auto typed = document({}, "Hello World");
    auto const rt = bounds(through.get()), ry = bounds(typed.get());
    ASSERT_TRUE(rt && ry);
    EXPECT_NEAR(rt->width(), ry->width(), 1e-6);
    // Each character of the layout is a character of the source: the cursor's places.
    auto doc = document("text-transform:uppercase;", "\xc5\xbft\xc4\xb1r");
    doc->ensureUpToDate();
    auto *text = cast<SPText>(doc->getObjectByRepr(elements(doc->getReprRoot(), "svg:text").at(0)));
    ASSERT_TRUE(text);
    EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 4);
    auto it = text->layout.begin();
    it.nextCharacter();
    it.nextCharacter();
    SPObject *source = nullptr;
    Glib::ustring::iterator at;
    text->layout.getSourceOfCharacter(it, &source, &at);
    ASSERT_TRUE(source);
    // The third character typed, a dotless i, not the I that is drawn.
    EXPECT_EQ(*at, gunichar(0x131));
}

TEST_F(AiNativeImportTest, AllCapsKeepsWhatWasTyped)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto build = [&](std::string const &characters, std::string const &style, std::string *text_style, std::string *content) {
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                              text_setup(text_document(characters, 6, font, "/1 40 " + style))));
        EXPECT_TRUE(b);
        if (!b) return 0.0;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        if (text_style) *text_style = attr(texts[0], "style");
        auto spans = elements(texts[0], "svg:tspan");
        if (content && !spans.empty() && spans[0]->firstChild()) *content = spans[0]->firstChild()->content();
        auto bounds = drawn(b->document.get(), texts[0]);
        EXPECT_TRUE(bounds);
        return bounds ? bounds->width() : 0.0;
    };
    std::string caps_style, typed, plain_style;
    double const caps = build("(Hello\\r)", "/12 2", &caps_style, &typed);
    double const capitals = build("(HELLO\\r)", {}, &plain_style, nullptr);
    double const lower = build("(Hello\\r)", {}, nullptr, nullptr);
    EXPECT_NE(caps_style.find("text-transform:uppercase;"), std::string::npos) << caps_style;
    EXPECT_EQ(typed, "Hello");
    EXPECT_NEAR(caps, capitals, 1e-6);
    EXPECT_GT(caps, lower + 5);
    // Text without it says nothing of capitals.
    EXPECT_EQ(plain_style.find("text-transform"), std::string::npos) << plain_style;

    // A run of capitals beside one as typed: each says which it is.
    std::string error;
    auto ai = AN::read(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(text_document("(Hello\\r)", 6, font))), error);
    ASSERT_TRUE(ai) << error;
    auto styled = std::make_shared<AN::TextDocument>(*ai->texts);
    auto &line = styled->stories.at(0)->lines.at(0);
    auto first = line.runs.at(0);
    auto second = first;
    first.text = "He";
    first.style.all_caps = true;
    second.text = "llo";
    line.runs = {first, second};
    EXPECT_FALSE(AN::same_drawing(*styled->stories[0], *ai->texts->stories[0]));
    ai->texts = styled;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    auto spans = elements(texts.at(0), "svg:tspan");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_NE(attr(texts[0], "style").find("text-transform:uppercase;"), std::string::npos);
    EXPECT_EQ(attr(spans[1], "style"), "text-transform:none;");
}

TEST_F(AiNativeImportTest, SuperscriptAndSubscriptTakeTheDocumentsSizes)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // "x" then "2" above the line, at 20 points; and the same below it.
    auto build = [&](int position, std::string const &settings, std::string *style, std::string *root) {
        auto data = text_document("(x2\\r)", 1, font, "/1 20", "/2 [1 0 0 1 -90 -10]",
                                  "<< /1 2 /0 << /0 << /6 << /1 20 /13 " + std::to_string(position) + " >> >> >> >>");
        if (!settings.empty()) {
            auto const at = data.find("/1 << /1 [ << /0 << /0 (x2");
            EXPECT_NE(at, std::string::npos);
            if (at != std::string::npos) data.insert(at + 6, "/0 << " + settings + " >> ");
        }
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(data)));
        EXPECT_TRUE(b);
        if (!b) return Geom::OptRect();
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        auto spans = elements(texts.at(0), "svg:tspan");
        EXPECT_EQ(spans.size(), 2u);
        if (spans.size() != 2) return Geom::OptRect();
        // The type says which it is and keeps its size: the layout sets it smaller and moved.
        EXPECT_NE(attr(texts[0], "style").find("font-size:20;"), std::string::npos) << attr(texts[0], "style");
        EXPECT_NE(attr(texts[0], "style").find("font-variant-position:normal;"), std::string::npos) << attr(texts[0], "style");
        if (style) *style = attr(spans[1], "style");
        if (root) {
            *root = attr(b->document->getReprRoot(), "proof:superscript") + "|" + attr(b->document->getReprRoot(), "proof:subscript");
        }
        EXPECT_STREQ(spans[1]->firstChild()->content(), "2");
        return drawn(b->document.get(), spans[1]);
    };
    // The figure on the line, for its size and place.
    auto on_line = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(text_document("(x2\\r)", 3, font, "/1 20"))));
    ASSERT_TRUE(on_line);
    auto line_text = elements(on_line->document->getReprRoot(), "svg:text").at(0);
    ASSERT_EQ(elements(line_text, "svg:tspan").size(), 1u);
    // Nothing says where it stands, nor does the document say how such type is set.
    EXPECT_EQ(attr(line_text, "style").find("font-variant-position"), std::string::npos) << attr(line_text, "style");
    EXPECT_EQ(attr(on_line->document->getReprRoot(), "proof:superscript"), "");
    auto whole = drawn(on_line->document.get(), line_text);
    ASSERT_TRUE(whole);
    // The figure's height at 20 points: both are set from the same baseline.
    auto const figure = [&] {
        auto doc = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(text_document("(2\\r)", 2, font, "/1 20"))));
        auto bounds = doc ? drawn(doc->document.get(), elements(doc->document->getReprRoot(), "svg:text").at(0)) : Geom::OptRect();
        EXPECT_TRUE(bounds);
        return bounds ? *bounds : Geom::Rect();
    }();
    double const baseline = figure.bottom(); // A 2 stands on the line.

    std::string style, root;
    // Illustrator's own settings: 58.3% of the size, a third of the size up.
    auto super = build(1, {}, &style, &root);
    EXPECT_EQ(style, "font-variant-position:super;");
    EXPECT_EQ(root, "|");
    auto sub = build(2, {}, &style, &root);
    EXPECT_EQ(style, "font-variant-position:sub;");
    ASSERT_TRUE(super && sub);
    // The same figure, 13.32 points apart.
    EXPECT_NEAR(sub->top() - super->top(), 13.32, 1e-6);
    EXPECT_NEAR(super->height(), sub->height(), 1e-6);
    EXPECT_NEAR(super->height(), figure.height() * 0.583, 1e-6);
    EXPECT_NEAR(super->bottom(), baseline - 6.66, 0.02);
    EXPECT_NEAR(sub->bottom(), baseline + 6.66, 0.02);
    // A document with settings of its own says them, and its type is set by them.
    auto own = build(1, "/3 0.5 /4 0.4 /5 0.6 /6 0.2", &style, &root);
    EXPECT_EQ(style, "font-variant-position:super;");
    EXPECT_EQ(root, "0.5 0.4|0.6 0.2");
    ASSERT_TRUE(own);
    EXPECT_NEAR(own->height(), figure.height() * 0.5, 1e-6);
    EXPECT_NEAR(own->bottom(), baseline - 8, 0.02);
    auto own_sub = build(2, "/3 0.5 /4 0.4 /5 0.6 /6 0.2", &style, &root);
    ASSERT_TRUE(own_sub);
    EXPECT_NEAR(own_sub->height(), figure.height() * 0.6, 1e-6);
    EXPECT_NEAR(own_sub->bottom(), baseline + 4, 0.02);
    // A setting that isn't a share of the size refuses the document's stories.
    auto data = text_document("(x2\\r)", 3, font, "/1 20 /13 1");
    data.insert(data.find("/1 << /1 [ << /0 << /0 (x2") + 6, "/0 << /3 7 >> ");
    EXPECT_FALSE(AN::read_text_document(text_setup(data), Geom::Point(100, 50))->error.empty());
}

TEST_F(AiNativeImportTest, SmallCapitalsAreMadeOfCapitalsForAFontWithoutThem)
{
    // A font without small capitals of its own, of those Windows comes with.
    std::optional<AN::FontMatch> font;
    for (auto const *name : {"Verdana", "Tahoma", "CourierNewPSMT", "Impact", "ArialMT", "Bahnschrift"}) {
        auto found = AN::find_font(name);
        if (!font && found && found->face && !found->face->get_opentype_tables().contains("smcp")) font = found;
    }
    if (!font) GTEST_SKIP() << "every test font that is installed has small capitals of its own";
    auto document = [&](std::string const &style, std::string const &content) {
        return SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='100'><text xml:space='preserve' "
            "style=\"font-size:40px;" + font->css + style + "\"><tspan x='10' y='60'>" + content + "</tspan></text></svg>");
    };
    auto bounds = [&](SPDocument *doc) {
        auto texts = elements(doc->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        return texts.empty() ? Geom::OptRect() : drawn(doc, texts[0]);
    };
    auto same = [&](std::string const &style, std::string const &typed, std::string const &set) {
        auto a = document(style, typed), b = document({}, set);
        auto const ra = bounds(a.get()), rb = bounds(b.get());
        EXPECT_TRUE(ra && rb);
        if (!ra || !rb) return;
        EXPECT_NEAR(ra->left(), rb->left(), 1e-6) << typed;
        EXPECT_NEAR(ra->width(), rb->width(), 1e-6) << typed;
        EXPECT_NEAR(ra->top(), rb->top(), 1e-6) << typed;
        EXPECT_NEAR(ra->bottom(), rb->bottom(), 1e-6) << typed;
    };
    // Small letters are capitals at seven tenths of the size; capitals and figures are as typed.
    same("font-variant-caps:small-caps;", "Test", "T<tspan style='font-size:28px'>EST</tspan>");
    same("font-variant-caps:small-caps;", "a B2c \xc3\xa9",
         "<tspan style='font-size:28px'>A</tspan> B2<tspan style='font-size:28px'>C</tspan> <tspan style='font-size:28px'>\xc3\x89</tspan>");
    // All caps over it: nothing is a small letter any more.
    same("font-variant-caps:small-caps;text-transform:uppercase;", "Test", "TEST");
    // Type without small letters is untouched.
    same("font-variant-caps:small-caps;", "TEST 12", "TEST 12");
    // The size is the document's to say.
    {
        auto a = SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:proof='urn:x-proof:document:1' width='400' height='100' "
            "proof:small-caps='0.5'><text xml:space='preserve' style=\"font-size:40px;" + font->css +
            "font-variant-caps:small-caps;\"><tspan x='10' y='60'>Test</tspan></text></svg>");
        auto b = document({}, "T<tspan style='font-size:20px'>EST</tspan>");
        auto const ra = bounds(a.get()), rb = bounds(b.get());
        ASSERT_TRUE(ra && rb);
        EXPECT_NEAR(ra->width(), rb->width(), 1e-6);
        EXPECT_NEAR(ra->top(), rb->top(), 1e-6);
    }
    // Small capitals above the line: each makes the letters smaller in its turn.
    same("font-variant-caps:small-caps;font-variant-position:super;", "Test",
         "<tspan style='font-size:23.32px;baseline-shift:13.32px'>T</tspan>"
         "<tspan style='font-size:16.324px;baseline-shift:13.32px'>EST</tspan>");
    // The text is what was typed, a place in the layout a place in it.
    auto doc = document("font-variant-caps:small-caps;", "Test");
    doc->ensureUpToDate();
    auto *text = cast<SPText>(doc->getObjectByRepr(elements(doc->getReprRoot(), "svg:text").at(0)));
    ASSERT_TRUE(text);
    EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 4);
    auto it = text->layout.begin();
    it.nextCharacter();
    SPObject *source = nullptr;
    Glib::ustring::iterator at;
    text->layout.getSourceOfCharacter(it, &source, &at);
    ASSERT_TRUE(source);
    EXPECT_EQ(*at, gunichar('e'));
}

TEST_F(AiNativeImportTest, SmallCapsUnderlineAndStrikethroughFollowTheRun)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto build = [&](std::string const &style, std::string *text_style, std::string *span_style) {
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                              text_setup(text_document("(Hello\\r)", 6, font, "/1 40 " + style))));
        EXPECT_TRUE(b);
        if (!b) return 0.0;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        if (text_style) *text_style = attr(texts[0], "style");
        auto spans = elements(texts[0], "svg:tspan");
        if (span_style) *span_style = spans.size() == 2 ? attr(spans[1], "style") : std::string("(no span)");
        auto bounds = drawn(b->document.get(), texts[0]);
        EXPECT_TRUE(bounds);
        return bounds ? bounds->width() : 0.0;
    };
    std::string text_style, span_style;
    double const plain = build({}, &text_style, &span_style);
    EXPECT_EQ(text_style.find("font-variant-caps"), std::string::npos) << text_style;
    EXPECT_EQ(text_style.find("text-decoration"), std::string::npos) << text_style;
    EXPECT_EQ(span_style, "(no span)");
    // Small caps: the story keeps what was typed; the layout sets it.
    double const small = build("/12 1", &text_style, &span_style);
    EXPECT_NE(text_style.find("font-variant-caps:small-caps;"), std::string::npos) << text_style;
    double const capitals = build("/12 2", nullptr, nullptr);
    EXPECT_GT(small, plain + 5);
    EXPECT_LT(small, capitals - 5);
    // Lines through and under a run are its span's own.
    build("/15 1", &text_style, &span_style);
    EXPECT_EQ(text_style.find("text-decoration"), std::string::npos) << text_style;
    EXPECT_EQ(span_style, "text-decoration-line:line-through;");
    build("/16 1", nullptr, &span_style);
    EXPECT_EQ(span_style, "text-decoration-line:underline;");
    build("/15 1 /16 1", nullptr, &span_style);
    EXPECT_EQ(span_style, "text-decoration-line:underline line-through;");
    // A document whose small caps are another size than Illustrator's own says so, and the
    // layout sets them at it (for a font that has none of its own).
    auto data = text_document("(Hello\\r)", 6, font, "/1 40 /12 1");
    data.insert(data.find("/1 << /1 [ << /0 << /0 (Hello") + 6, "/0 << /7 0.5 >> ");
    auto texts = AN::read_text_document(text_setup(data), Geom::Point(100, 50));
    ASSERT_EQ(texts->stories.size(), 1u);
    ASSERT_TRUE(texts->stories[0]) << texts->errors.at(0);
    EXPECT_EQ(texts->type.small_caps_size, 0.5);
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(data)));
    ASSERT_TRUE(b);
    EXPECT_EQ(attr(b->document->getReprRoot(), "proof:small-caps"), "0.5");
    auto found = AN::find_font(font);
    ASSERT_TRUE(found && found->face);
    auto bounds = drawn(b->document.get(), elements(b->document->getReprRoot(), "svg:text").at(0));
    ASSERT_TRUE(bounds);
    if (!found->face->get_opentype_tables().contains("smcp")) {
        EXPECT_LT(bounds->width(), small - 5);
        EXPECT_GT(bounds->width(), plain * 0.5);
    }
    // Type an appearance drew is in a text document of its own. Where that one says another
    // size than the document's, its small caps can't be set.
    auto typed = text_document("(Hello\\r)", 6, font, "/1 40");
    auto other = text_document("(Hello\\r)", 6, font, "/1 40 /12 1");
    other.insert(other.find("/1 << /1 [ << /0 << /0 (Hello") + 6, "/0 << /7 0.5 >> ");
    auto both = AN::read_text_document(text_setup(typed, other), Geom::Point(100, 50));
    ASSERT_TRUE(both->error.empty()) << both->error;
    ASSERT_EQ(both->drawn.size(), 1u);
    EXPECT_FALSE(both->drawn[0]);
    EXPECT_NE(both->drawn_errors.at(0).find("small caps"), std::string::npos) << both->drawn_errors.at(0);
    EXPECT_EQ(both->type.small_caps_size, 0.7);
}

TEST_F(AiNativeImportTest, SuperscriptAndSubscriptAreMadeByTheLayout)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial isn't installed";
    auto document = [](std::string const &root, std::string const &content) {
        return SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:proof='urn:x-proof:document:1' width='400' height='100' " + root +
            "><text xml:space='preserve' style='font-family:Arial;font-size:40px'><tspan x='10' y='60'>" + content +
            "</tspan></text></svg>");
    };
    auto bounds = [&](SPDocument *doc) {
        auto texts = elements(doc->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        return texts.empty() ? Geom::OptRect() : drawn(doc, texts[0]);
    };
    auto same = [&](std::string const &root, std::string const &made, std::string const &set) {
        auto a = document(root, made), b = document({}, set);
        auto const ra = bounds(a.get()), rb = bounds(b.get());
        EXPECT_TRUE(ra && rb);
        if (!ra || !rb) return;
        EXPECT_NEAR(ra->left(), rb->left(), 1e-6) << made;
        EXPECT_NEAR(ra->width(), rb->width(), 1e-6) << made;
        EXPECT_NEAR(ra->top(), rb->top(), 1e-6) << made;
        EXPECT_NEAR(ra->bottom(), rb->bottom(), 1e-6) << made;
    };
    // Illustrator's own: 58.3% of the size, a third of the size off the baseline. The type
    // keeps its size; what follows is back on the line.
    same({}, "H<tspan style='font-variant-position:super'>2x</tspan>H",
         "H<tspan style='font-size:23.32px;baseline-shift:13.32px'>2x</tspan>H");
    same({}, "H<tspan style='font-variant-position:sub'>2x</tspan>H",
         "H<tspan style='font-size:23.32px;baseline-shift:-13.32px'>2x</tspan>H");
    // The size is the type's own, whatever holds it.
    same({}, "H<tspan style='font-size:20px;font-variant-position:super'>2x</tspan>",
         "H<tspan style='font-size:11.66px;baseline-shift:6.66px'>2x</tspan>");
    // A span inside one is the same, not smaller again; one that says normal is on the line.
    same({}, "H<tspan style='font-variant-position:super'>2<tspan style='fill:#ff0000'>x</tspan><tspan "
             "style='font-variant-position:normal'>y</tspan></tspan>",
         "H<tspan style='font-size:23.32px;baseline-shift:13.32px'>2x</tspan>y");
    // A baseline shift of the type's own is added to it.
    same({}, "H<tspan style='font-variant-position:super;baseline-shift:5px'>2x</tspan>",
         "H<tspan style='font-size:23.32px;baseline-shift:18.32px'>2x</tspan>");
    // What the document says.
    same("proof:superscript='0.5 0.25' proof:subscript='0.75 0.1'",
         "H<tspan style='font-variant-position:super'>2</tspan><tspan style='font-variant-position:sub'>x</tspan>",
         "H<tspan style='font-size:20px;baseline-shift:10px'>2</tspan><tspan style='font-size:30px;baseline-shift:-4px'>x</tspan>");
    // What it says wrongly is Illustrator's own.
    same("proof:superscript='0 1' proof:subscript='big'",
         "H<tspan style='font-variant-position:super'>2</tspan><tspan style='font-variant-position:sub'>x</tspan>",
         "H<tspan style='font-size:23.32px;baseline-shift:13.32px'>2</tspan><tspan style='font-size:23.32px;baseline-shift:-13.32px'>x</tspan>");

    // Its settings read and written; Illustrator's own aren't written.
    auto doc = document({}, "H<tspan style='font-variant-position:super'>2</tspan>");
    auto const before = bounds(doc.get());
    ASSERT_TRUE(before);
    EXPECT_EQ(Inkscape::TypeSettings::of(doc.get()), Inkscape::TypeSettings());
    Inkscape::TypeSettings settings;
    settings.write(doc.get());
    EXPECT_EQ(attr(doc->getReprRoot(), "proof:superscript"), "");
    settings.superscript_size = 0.8;
    settings.superscript_shift = 0;
    settings.small_caps_size = 0.6;
    settings.write(doc.get());
    EXPECT_EQ(attr(doc->getReprRoot(), "proof:superscript"), "0.8 0");
    EXPECT_EQ(attr(doc->getReprRoot(), "proof:subscript"), "");
    EXPECT_EQ(attr(doc->getReprRoot(), "proof:small-caps"), "0.6");
    EXPECT_EQ(Inkscape::TypeSettings::of(doc.get()), settings);
    // The document's text is laid out again by them.
    auto const after = bounds(doc.get());
    ASSERT_TRUE(after);
    EXPECT_GT(after->width(), before->width() + 2);
    auto reference = document({}, "H<tspan style='font-size:32px'>2</tspan>");
    EXPECT_NEAR(after->width(), bounds(reference.get())->width(), 1e-6);
    // Settings no document can have aren't written.
    settings.subscript_size = 0;
    EXPECT_FALSE(settings.valid());
    settings.write(doc.get());
    EXPECT_EQ(attr(doc->getReprRoot(), "proof:superscript"), "0.8 0");
    Inkscape::TypeSettings().write(doc.get());
    EXPECT_EQ(attr(doc->getReprRoot(), "proof:superscript"), "");
    EXPECT_EQ(attr(doc->getReprRoot(), "proof:small-caps"), "");

    // Each character of the layout is still a character of the source.
    auto *text = cast<SPText>(doc->getObjectByRepr(elements(doc->getReprRoot(), "svg:text").at(0)));
    ASSERT_TRUE(text);
    doc->ensureUpToDate();
    EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 2);
}

TEST_F(AiNativeImportTest, ParagraphsReadFromTheRightKeepTheirEnds)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial, which has Hebrew, isn't installed";
    // Four Hebrew letters at 32 points whose line starts, as Illustrator has it, at its left
    // end: 10 points in, 40 from the top.
    auto hebrew = [](std::string const &direction, int alignment = 0, bool ordered = true) {
        auto data = text_document("(\\376\\377\\005\\342\\005\\321\\005\\350\\005\\331\\000\\015)", 5, "ArialMT", "/1 32");
        int const physical = direction == " /33 1" && alignment < 2 ? 1 - alignment : alignment;
        auto at = data.find(" /6 << /0 [");
        data.insert(at, " /5 << /0 [ << /1 5 /0 << /0 << /5 << /0 " + std::to_string(physical) + direction + " /35 " + (ordered ? "1" : "0") + " >> >> >> >> ] >>");
        // What Illustrator's composer for these scripts leaves with the segment.
        std::string const segment = "<< /99 /S >>";
        at = data.find(segment);
        if (ordered) data.replace(at, segment.size(), "<< /99 /S /15 << /0 5 /2 5 /5 true /7 << /0 5 >> >> >>");
        return data;
    };
    auto read = AN::read_text_document(text_setup(hebrew(" /33 1")), Geom::Point(100, 50));
    ASSERT_TRUE(read->stories.at(0)) << read->errors.at(0);
    EXPECT_TRUE(read->stories[0]->rtl);
    EXPECT_EQ(read->stories[0]->lines.at(0).alignment, 0u);
    auto build = [&](std::string const &direction, std::string *text_style, std::string *span_style) {
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(hebrew(direction))));
        EXPECT_TRUE(b);
        if (!b) return Geom::OptRect();
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        auto spans = elements(texts.at(0), "svg:tspan");
        EXPECT_EQ(spans.size(), 1u);
        if (text_style) *text_style = attr(texts[0], "style");
        if (span_style && !spans.empty()) *span_style = attr(spans[0], "style");
        return drawn(b->document.get(), texts[0]);
    };
    std::string text_style, span_style;
    auto rtl = build(" /33 1", &text_style, &span_style);
    EXPECT_NE(text_style.find("direction:rtl;"), std::string::npos) << text_style;
    EXPECT_EQ(span_style, "text-anchor:end;");
    auto ltr = build(" /33 0", &text_style, &span_style);
    EXPECT_EQ(text_style.find("direction"), std::string::npos) << text_style;
    EXPECT_EQ(span_style, "text-anchor:start;");
    // Either way the line lies to the right of where it starts, in the same place: these
    // letters read from the right whatever their paragraph says.
    ASSERT_TRUE(rtl && ltr);
    EXPECT_GT(rtl->left(), 9.0);
    EXPECT_LT(rtl->left(), 14.0);
    EXPECT_NEAR(rtl->left(), ltr->left(), 0.01);
    EXPECT_NEAR(rtl->width(), ltr->width(), 0.01);
    // A direction that isn't known refuses the story.
    auto bad = AN::read_text_document(text_setup(hebrew(" /33 2")), Geom::Point(100, 50));
    ASSERT_EQ(bad->stories.size(), 1u);
    EXPECT_FALSE(bad->stories[0]);
    // Where Illustrator didn't put such letters in order it shows them backwards, set from
    // the left as they were typed. The story says so, and they are set that way here.
    auto backwards = AN::read_text_document(text_setup(hebrew(" /33 0", 0, false)), Geom::Point(100, 50));
    ASSERT_EQ(backwards->stories.size(), 1u);
    ASSERT_TRUE(backwards->stories[0]) << backwards->errors.at(0);
    EXPECT_TRUE(backwards->stories[0]->unordered);
    EXPECT_FALSE(backwards->stories[0]->rtl);
    EXPECT_FALSE(read->stories[0]->unordered);
    EXPECT_FALSE(AN::same_drawing(*backwards->stories[0], *read->stories[0]));
    // Nothing says how a paragraph read from the right sets them unordered.
    auto odd = AN::read_text_document(text_setup(hebrew(" /33 1", 0, false)), Geom::Point(100, 50));
    ASSERT_EQ(odd->stories.size(), 1u);
    EXPECT_FALSE(odd->stories[0]);
    EXPECT_NE(odd->errors.at(0).find("read from the right"), std::string::npos) << odd->errors.at(0);

    // Where each letter of the line lies, as the layout counts them.
    auto places = [&](std::string const &data, std::string *style, std::vector<std::string> *notes) {
        std::vector<double> out;
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(data)));
        EXPECT_TRUE(b);
        if (!b) return out;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        if (texts.empty()) return out;
        if (style) *style = attr(texts[0], "style");
        if (notes) *notes = b->notes;
        b->document->ensureUpToDate();
        auto *text = cast<SPText>(b->document->getObjectByRepr(texts[0]));
        for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
            out.push_back(text->layout.characterBoundingBox(it).midpoint().x());
        }
        return out;
    };
    std::vector<std::string> notes;
    auto const as_typed = places(hebrew(" /33 0", 0, false), &text_style, &notes);
    EXPECT_NE(text_style.find("direction:ltr;unicode-bidi:bidi-override;"), std::string::npos) << text_style;
    ASSERT_EQ(as_typed.size(), 4u);
    // The first letter typed is the leftmost, and so on.
    EXPECT_TRUE(std::is_sorted(as_typed.begin(), as_typed.end())) << as_typed[0] << " " << as_typed[3];
    EXPECT_TRUE(std::any_of(notes.begin(), notes.end(), [](std::string const &n) { return n.find("show backwards") != std::string::npos; }));
    // In order, it is the rightmost; and nothing is said.
    auto const in_order = places(hebrew(" /33 0"), &text_style, &notes);
    EXPECT_EQ(text_style.find("unicode-bidi"), std::string::npos) << text_style;
    ASSERT_EQ(in_order.size(), 4u);
    EXPECT_TRUE(std::is_sorted(in_order.rbegin(), in_order.rend())) << in_order[0] << " " << in_order[3];
    EXPECT_TRUE(std::none_of(notes.begin(), notes.end(), [](std::string const &n) { return n.find("show backwards") != std::string::npos; }));
    // The same letters either way, the other way round.
    EXPECT_NEAR(as_typed.back() - as_typed.front(), in_order.front() - in_order.back(), 0.5);

    // A paragraph of point type that is justified has nothing to be justified to. Its line
    // is set as its paragraph's last one is: from the left (3, and 6, justified all through),
    // from the right (4) or about its middle (5). These letters lie where they lay.
    for (auto const &[alignment, anchor] : {std::pair{3, "start"}, std::pair{6, "start"}}) {
        auto const reference = build(" /33 0", &text_style, &span_style);
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(hebrew(" /33 0", alignment))));
        ASSERT_TRUE(b) << alignment;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        ASSERT_EQ(texts.size(), 1u);
        EXPECT_EQ(attr(elements(texts[0], "svg:tspan").at(0), "style"), std::string("text-anchor:") + anchor + ";") << alignment;
        auto const drawn_here = drawn(b->document.get(), texts[0]);
        ASSERT_TRUE(reference && drawn_here);
        EXPECT_NEAR(drawn_here->left(), reference->left(), 0.01) << alignment;
    }
    // Justified area type is another matter: its lines are stretched to its frame.
    auto justified_point = AN::read_text_document(text_setup(hebrew(" /33 0", 4)), Geom::Point(100, 50));
    ASSERT_TRUE(justified_point->stories.at(0)) << justified_point->errors.at(0);
    auto unknown = AN::read_text_document(text_setup(hebrew(" /33 0", 7)), Geom::Point(100, 50));
    EXPECT_FALSE(unknown->stories.at(0));
}

TEST_F(AiNativeImportTest, TypePaintedByNameIsBlackForAGradientAndPatternedForAPattern)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // The document's styles, as Illustrator writes them among the records: a plain colour,
    // a gradient and a pattern, each under a name a character's paint can give.
    std::string const styles =
        "%AI9_BeginArtStyles\n"
        "/KnownStyle :\n(Anon plain) /Name ,\n/SimpleStyle :\n0 O\n0 0 0 1 k\n0 1 Xd\n/Paint ;\n /Def ;\n"
        "/KnownStyle :\n(Anon gradient) /Name ,\n/SimpleStyle :\n0 O\nBb\n2 (Unnamed gradient 1) 0 0 0 1 1 0 0 1 0 0 1 Bg\n0 BB\n"
        "0 1 Xd\n/Paint ;\n /Def ;\n"
        "/KnownStyle :\n(Anon pattern) /Name ,\n/SimpleStyle :\n0 O\n(Dots) 0 0 1 1 0 0 0 0 0 [2 0 0 2 30 40] p\n0 1 Xd\n/Paint ;\n"
        " /Def ;\n%AI9_EndArtStyles\n";
    // Beside the name the text document keeps a colour: black, for a gradient or a pattern.
    auto painted = [&](std::string const &name, bool stroke = false) {
        std::string const paint = " << /99 /CAITextPaint /5 (" + name + ") /0 << /0 0 /1 [1 0] >> >>";
        return text_document("(Hello\\r)", 6, font, "/1 40 " + (stroke ? "/57 true /54" + paint : "/56 true /53" + paint));
    };
    auto read = [&](std::string const &name, bool stroke = false) {
        return AN::read_text_document(styles + text_setup(painted(name, stroke)), Geom::Point(100, 50));
    };
    for (auto const *name : {"Anon plain", "Anon nowhere"}) {
        auto texts = read(name);
        ASSERT_TRUE(texts->stories.at(0)) << texts->errors.at(0);
        EXPECT_FALSE(texts->stories[0]->lines.at(0).runs.at(0).style.gradient) << name;
    }
    // A gradient isn't drawn on type: the characters are black in Illustrator.
    for (bool stroke : {false, true}) {
        auto texts = read("Anon gradient", stroke);
        ASSERT_TRUE(texts->stories.at(0)) << texts->errors.at(0);
        auto const &style = texts->stories[0]->lines.at(0).runs.at(0).style;
        EXPECT_TRUE(style.gradient);
        ASSERT_TRUE(stroke ? style.stroke : style.fill);
    }
    // A pattern is drawn: the story says which, and where its tile lies.
    for (bool stroke : {false, true}) {
        auto texts = read("Anon pattern", stroke);
        ASSERT_TRUE(texts->stories.at(0)) << texts->errors.at(0);
        auto const &style = texts->stories[0]->lines.at(0).runs.at(0).style;
        EXPECT_FALSE(style.gradient);
        auto const &pattern = stroke ? style.stroke_pattern : style.fill_pattern;
        ASSERT_TRUE(pattern);
        EXPECT_FALSE(stroke ? style.fill_pattern : style.stroke_pattern);
        EXPECT_EQ(pattern->name, "Dots");
        EXPECT_EQ(pattern->matrix, Geom::Affine(2, 0, 0, 2, 30, 40));
    }
    // One placed the way Illustrator 8 placed them isn't read, on type as on a path.
    auto moved = styles;
    moved.replace(moved.find("(Dots) 0 0 1 1"), 14, "(Dots) 5 0 1 1");
    auto old = AN::read_text_document(moved + text_setup(painted("Anon pattern")), Geom::Point(100, 50));
    EXPECT_FALSE(old->stories.at(0));
    EXPECT_NE(old->errors.at(0).find("pattern"), std::string::npos) << old->errors.at(0);
    // Nor is type painted with a pattern the file doesn't define: it is refused, not set in black.
    {
        std::string error;
        auto ai = AN::read(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), styles + text_setup(painted("Anon pattern"))), error);
        ASSERT_TRUE(ai) << error;
        EXPECT_FALSE(AN::build(*ai, error));
        EXPECT_NE(error.find("pattern that isn't in the file"), std::string::npos) << error;
    }

    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), styles + text_setup(painted("Anon gradient"))));
    ASSERT_TRUE(b);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(),
                            [](std::string const &n) { return n.find("painted with a gradient") != std::string::npos; }));
    auto plain = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), styles + text_setup(painted("Anon plain"))));
    ASSERT_TRUE(plain);
    EXPECT_TRUE(std::none_of(plain->notes.begin(), plain->notes.end(),
                             [](std::string const &n) { return n.find("painted with a gradient") != std::string::npos; }));
}

TEST_F(AiNativeImportTest, TypeInAnInkIsTheInkItsArtStyleNames)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // The styles as Illustrator 29.2.1 wrote them for type in inks (2026-10-24): a CMYK ink
    // whole and at 40%, a Lab ink at 70%, an RGB ink at 60% and a global process colour.
    auto style = [](std::string const &name, std::string const &line) {
        return "/KnownStyle :\n(" + name + ") /Name ,\n/SimpleStyle :\n0 O\n" + line +
               "\n0 1 0 0 0 Xy\n0 J 0 j 1 w 10 M []0 d\n0 XR\n0 1 Xd\n/Paint ;\n /Def ;\n";
    };
    std::string const styles = "%AI9_BeginArtStyles\n" +
        style("Anon whole", "0 0.800000011920929 0.899999976158142 0 (PROOF Spot CMYK) 0 x") +
        style("Anon tint", "0 0.800000011920929 0.899999976158142 0 (PROOF Spot CMYK) 0.600000023841858 x") +
        style("Anon lab", "0.452307909727097 0.582391083240509 0 0 40 50 -60 (PROOF Spot Lab) 0.300000011920929 2 Xx") +
        style("Anon rgb", "0.565667152404785 0 0.477851510047913 0 0.078431375324726 0.627451002597809 0.352941185235977 "
                          "(PROOF Spot RGB) 0.400000005960465 1 Xx") +
        style("Anon global", "0.600000023841858 0.100000001490116 0 0.200000002980232 (PROOF Global) 0 0 Xk") +
        style("Anon registration", "1 1 1 1 ([Registration]) 0.5 Xs") +
        "%AI9_EndArtStyles\n";
    // Beside the name, what the ink looks like: for a Lab ink RGB, whatever its tint.
    auto painted = [&](std::string const &name, bool stroke = false) {
        std::string const paint = " << /99 /CAITextPaint /5 (" + name + ") /0 << /0 1 /1 [1 0.5 0.2 0.76] >> >>";
        return text_document("(Hello\\r)", 6, font, "/1 40 " + (stroke ? "/57 true /63 0.5 /54" + paint : "/56 true /53" + paint));
    };
    auto run_of = [&](std::string const &name, bool stroke = false) {
        auto texts = AN::read_text_document(styles + text_setup(painted(name, stroke)), Geom::Point(100, 50));
        EXPECT_TRUE(texts->stories.at(0)) << texts->errors.at(0);
        return texts->stories.at(0) ? texts->stories[0]->lines.at(0).runs.at(0).style : AN::TextStyle{};
    };
    struct Expected
    {
        char const *style;
        char const *ink;
        AN::Ink::Kind kind;
        AN::Color color;
        double tint;
    };
    Expected const cases[] = {
        {"Anon whole", "PROOF Spot CMYK", AN::Ink::Kind::Spot, AN::Color::cmyk(0, 0.800000011920929, 0.899999976158142, 0), 1},
        {"Anon tint", "PROOF Spot CMYK", AN::Ink::Kind::Spot, AN::Color::cmyk(0, 0.800000011920929, 0.899999976158142, 0), 0.4},
        {"Anon lab", "PROOF Spot Lab", AN::Ink::Kind::Spot, AN::Color::lab(40, 50, -60), 0.7},
        {"Anon rgb", "PROOF Spot RGB", AN::Ink::Kind::Spot, AN::Color::rgb(0.078431375324726, 0.627451002597809, 0.352941185235977), 0.6},
        {"Anon global", "PROOF Global", AN::Ink::Kind::Process, AN::Color::cmyk(0.600000023841858, 0.100000001490116, 0, 0.200000002980232), 1},
        {"Anon registration", "[Registration]", AN::Ink::Kind::Registration, AN::Color::cmyk(1, 1, 1, 1), 0.5},
    };
    for (auto const &c : cases) {
        for (bool stroke : {false, true}) {
            SCOPED_TRACE(std::string(c.style) + (stroke ? " stroke" : " fill"));
            auto const s = run_of(c.style, stroke);
            auto const &ink = stroke ? s.stroke_ink : s.fill_ink;
            ASSERT_TRUE(ink);
            EXPECT_FALSE(stroke ? s.fill_ink : s.stroke_ink);
            EXPECT_FALSE(s.gradient);
            EXPECT_EQ(ink->ink.name, c.ink);
            EXPECT_EQ(ink->ink.kind, c.kind);
            EXPECT_EQ(ink->ink.color, c.color);
            EXPECT_NEAR(ink->tint, c.tint, 1e-6);
            // What it looks like stays beside it.
            ASSERT_TRUE(stroke ? s.stroke : s.fill);
        }
    }
    // A plain colour's style is no ink.
    EXPECT_FALSE(run_of("Anon nowhere").fill_ink);

    // The import paints the characters with the ink's swatch, as a path in it is painted. An ink
    // only type uses, which the Swatches panel doesn't list, gets one too.
    for (auto const &c : cases) {
        SCOPED_TRACE(c.style);
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), styles + text_setup(painted(c.style))));
        ASSERT_TRUE(b);
        auto *doc = b->document.get();
        doc->ensureUpToDate();
        auto const texts = elements(doc->getReprRoot(), "svg:text");
        ASSERT_EQ(texts.size(), 1u);
        auto *text = cast<SPText>(doc->getObjectByRepr(texts[0]));
        ASSERT_TRUE(text && text->style->fill.isPaintserver());
        auto *swatch = cast<SPGradient>(text->style->getFillPaintServer());
        ASSERT_TRUE(swatch);
        if (c.kind == AN::Ink::Kind::Process) {
            // A global process colour is a swatch of its name, in process colour.
            EXPECT_FALSE(SpotInk::read(swatch->getRepr()));
            EXPECT_NE(attr(swatch->getRepr(), "inkscape:label").find("PROOF Global"), std::string::npos);
            continue;
        }
        auto const ink = SpotInk::read(swatch->getRepr());
        ASSERT_TRUE(ink);
        // Registration is PDF's ink All, as on a path.
        EXPECT_EQ(ink->name, c.kind == AN::Ink::Kind::Registration ? std::string("All") : std::string(c.ink));
        EXPECT_NEAR(ink->tint, c.tint, 1e-4);
    }
}

TEST_F(AiNativeImportTest, DirectionOverridesTheWayLettersRead)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial, which has Hebrew, isn't installed";
    // Where the layout puts each character of a line, in the order they were typed.
    auto places = [&](std::string const &style, std::string const &content) {
        std::vector<double> out;
        auto doc = SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' width='600' height='100'><text xml:space='preserve' "
            "style='font-family:Arial;font-size:40px;" + style + "'><tspan x='300' y='60'>" + content + "</tspan></text></svg>");
        EXPECT_TRUE(doc);
        if (!doc) return out;
        doc->ensureUpToDate();
        auto *text = cast<SPText>(doc->getObjectByRepr(elements(doc->getReprRoot(), "svg:text").at(0)));
        for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
            out.push_back(text->layout.characterBoundingBox(it).midpoint().x());
        }
        return out;
    };
    auto rising = [](std::vector<double> const &v, std::size_t from, std::size_t to) {
        return std::is_sorted(v.begin() + from, v.begin() + to);
    };
    auto falling = [](std::vector<double> const &v, std::size_t from, std::size_t to) {
        return std::is_sorted(v.begin() + from, v.begin() + to, std::greater<>());
    };
    std::string const alef_to_dalet = "\xd7\x90\xd7\x91\xd7\x92\xd7\x93";
    // Hebrew reads from the right by itself, also in a line that reads from the left.
    auto v = places({}, alef_to_dalet);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_TRUE(falling(v, 0, 4));
    // Overridden, every letter follows the last the way the line reads.
    v = places("unicode-bidi:bidi-override;", alef_to_dalet);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_TRUE(rising(v, 0, 4));
    v = places("direction:rtl;unicode-bidi:bidi-override;", "abcd");
    ASSERT_EQ(v.size(), 4u);
    EXPECT_TRUE(falling(v, 0, 4));
    // The other values change nothing.
    for (auto const *value : {"normal", "embed", "isolate", "plaintext"}) {
        v = places(std::string("unicode-bidi:") + value + ";", alef_to_dalet);
        ASSERT_EQ(v.size(), 4u);
        EXPECT_TRUE(falling(v, 0, 4)) << value;
    }
    // It holds for what an element contains, down to one inside that says otherwise; and
    // only there: letters of one script and one font are cut where it begins and ends.
    v = places({}, "\xd7\x90\xd7\x91<tspan style='unicode-bidi:bidi-override'>\xd7\x92\xd7\x93<tspan style='fill:#ff0000'>"
                   "\xd7\x94\xd7\x95</tspan></tspan>\xd7\x96\xd7\x97");
    ASSERT_EQ(v.size(), 8u);
    EXPECT_TRUE(falling(v, 0, 2));
    EXPECT_TRUE(rising(v, 2, 6));
    EXPECT_TRUE(falling(v, 6, 8));
    v = places("unicode-bidi:bidi-override;", "\xd7\x90\xd7\x91<tspan style='unicode-bidi:normal'>\xd7\x92\xd7\x93</tspan>");
    ASSERT_EQ(v.size(), 4u);
    EXPECT_TRUE(rising(v, 0, 2));
    EXPECT_TRUE(falling(v, 2, 4));
    // The letters are the same ones, as wide.
    auto width = [](std::vector<double> const &p) { return *std::max_element(p.begin(), p.end()) - *std::min_element(p.begin(), p.end()); };
    EXPECT_NEAR(width(places({}, alef_to_dalet)), width(places("unicode-bidi:bidi-override;", alef_to_dalet)), 0.01);
}

TEST_F(AiNativeImportTest, LinesUnderAndThroughTypeStopAtTheLastLetter)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial isn't installed";
    // How long the line through "HHHH" is drawn, with this spacing between its letters.
    auto length = [&](std::string const &spacing) {
        auto doc = SPDocument::createNewDocFromMem(std::string(
            "<svg xmlns='http://www.w3.org/2000/svg' width='600' height='100'><text xml:space='preserve' "
            "style='font-family:Arial;font-size:40px;text-decoration-line:line-through;") + spacing +
            "'><tspan x='10' y='60'>HHHH</tspan></text></svg>");
        doc->ensureUpToDate();
        auto texts = elements(doc->getReprRoot(), "svg:text");
        EXPECT_EQ(texts.size(), 1u);
        // The layout says where the lines go when the text is shown.
        Inkscape::Drawing drawing;
        auto const key = SPItem::display_key_new(1);
        drawing.setRoot(doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
        auto *span = doc->getObjectByRepr(elements(texts.at(0), "svg:tspan").at(0));
        double const out = span->style->text_decoration_data.tspan_width;
        doc->getRoot()->invoke_hide(key);
        return out;
    };
    // From the first letter's start to the last one's end: three spaces between four
    // letters, and not the fourth that follows the last.
    double const plain = length("");
    EXPECT_GT(plain, 80);
    EXPECT_NEAR(length("letter-spacing:20px;"), plain + 60, 1e-6);
    EXPECT_NEAR(length("letter-spacing:-4px;"), plain - 12, 1e-6);
}

// ---- patterns -----------------------------------------------------------------------------

namespace {

/// A pattern as Illustrator CS and later write it: every line of the tile's art hidden.
std::string pattern(std::string const &name, std::string const &box, std::string const &art)
{
    std::string hidden;
    for (std::size_t at = 0; at < art.size();) {
        auto const end = art.find('\n', at);
        hidden += "%_" + art.substr(at, end - at) + "\n";
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return "%AI3_BeginPattern: (" + name + ")\n(" + name + ") " + box + "\n" + hidden + "E\n%AI3_EndPattern\n";
}

} // namespace

TEST_F(AiNativeImportTest, APatternPaintsTypeWhereItWouldPaintAPath)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto const tile = "0 g\n0 0 m 0 5 L 10 5 L 10 0 L 0 0 L\nf\n";
    // A path and a text object painted with the same pattern, placed alike; the text names
    // it through a style of the document's.
    std::string const styles =
        "%AI9_BeginArtStyles\n/KnownStyle :\n(Anon dots) /Name ,\n/SimpleStyle :\n0 O\n"
        "(Dots) 0 0 1 1 0 0 0 0 0 [2 0 0 2 30 40] p\n0 1 Xd\n/Paint ;\n /Def ;\n%AI9_EndArtStyles\n";
    auto const data = text_document("(Hello\\r)", 6, font,
                                    "/1 40 /56 true /53 << /99 /CAITextPaint /5 (Anon dots) /0 << /0 0 /1 [1 0] >> >>");
    auto b = make(records(layer("(Dots) 0 0 1 1 0 0 0 0 0 [2 0 0 2 30 40] p\n" + rect + "/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          pattern("Dots", "0 0 20 10", tile) + styles + text_setup(data)));
    ASSERT_TRUE(b);
    auto texts = elements(b->document->getReprRoot(), "svg:text");
    auto paths = elements(b->document->getReprRoot(), "svg:path");
    ASSERT_EQ(texts.size(), 1u);
    auto url = [](std::string const &style) {
        auto const at = style.find("fill:url(#");
        return at == std::string::npos ? std::string() : style.substr(at + 10, style.find(')', at) - at - 10);
    };
    auto const text_paint = url(attr(texts[0], "style"));
    ASSERT_FALSE(text_paint.empty()) << attr(texts[0], "style");
    // The path of the layer (the tile's own path is in the pattern).
    std::string path_paint;
    for (auto *p : paths) {
        if (!url(attr(p, "style")).empty()) path_paint = url(attr(p, "style"));
    }
    ASSERT_FALSE(path_paint.empty());
    EXPECT_NE(text_paint, path_paint);
    auto matrix_of = [&](std::string const &id) {
        auto *node = b->document->getObjectById(id) ? b->document->getObjectById(id)->getRepr() : nullptr;
        Geom::Affine m;
        EXPECT_TRUE(node);
        if (node) {
            EXPECT_FALSE(attr(node, "xlink:href").empty());
            sp_svg_transform_read(attr(node, "patternTransform").c_str(), &m);
        }
        return m;
    };
    Geom::Affine text_matrix;
    ASSERT_TRUE(sp_svg_transform_read(attr(texts[0], "transform").c_str(), &text_matrix));
    // Through the text's own matrix, its pattern lies where the path's does.
    EXPECT_TRUE(Geom::are_near(matrix_of(text_paint) * text_matrix, matrix_of(path_paint), 1e-6));
    // And both use the one tile.
    auto *text_pattern = b->document->getObjectById(text_paint)->getRepr();
    auto *path_pattern = b->document->getObjectById(path_paint)->getRepr();
    EXPECT_EQ(attr(text_pattern, "xlink:href"), attr(path_pattern, "xlink:href"));

    // A pattern whose tile can't be read refuses the type it paints, as it does a path.
    std::string error;
    auto ai = AN::read(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                               pattern("Dots", "0 0 4 4", "7 Qz\n") + styles + text_setup(data)), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("painted with a pattern that has"), std::string::npos) << error;
}

TEST_F(AiNativeImportTest, PatternFillsAreTilesPlacedPerObject)
{
    // A 20 x 10 tile with a square in its lower left corner (the pattern's y points up).
    auto const tile = "0 g\n0 0 m 0 5 L 10 5 L 10 0 L 0 0 L\nf\n";
    auto b = make(records(layer("(Dots) 0 0 1 1 0 0 0 0 0 [2 0 0 2 30 40] p\n" + rect +
                                "0 0 1 0 K\n(Dots) 0 0 1 1 0 0 0 0 0 [1 0 0 1 0 0] P\n" + rect.substr(0, rect.size() - 2) + "B\n"),
                          pattern("Dots", "0 0 20 10", tile) + pattern("Unused", "5 5 9 9", tile) +
                              pattern("Odd", "0 0 4 4", "7 Qz\n")));
    ASSERT_TRUE(b);
    auto patterns = elements(b->document->getReprRoot(), "svg:pattern");
    // The tile, one for each of the three places it paints (a fill, and the next object's
    // fill and stroke), and the pattern nothing uses: Illustrator lists it among the
    // swatches. The one that can't be read isn't there.
    ASSERT_EQ(patterns.size(), 5u);
    auto find = [&](std::string const &label) -> XML::Node * {
        for (auto *p : patterns) if (attr(p, "inkscape:label") == label) return p;
        return nullptr;
    };
    auto *dots = find("Dots");
    ASSERT_TRUE(dots);
    EXPECT_TRUE(find("Unused"));
    EXPECT_FALSE(find("Odd"));
    EXPECT_EQ(attr(dots, "patternUnits"), "userSpaceOnUse");
    EXPECT_EQ(attr(dots, "width"), "20");
    EXPECT_EQ(attr(dots, "height"), "10");
    // The tile's art in the tile's space: its top-left corner at 0,0, y down.
    auto art = elements(dots, "svg:path");
    ASSERT_EQ(art.size(), 1u);
    EXPECT_EQ(*sp_svg_read_pathv(attr(art[0], "d").c_str()).boundsExact(), Geom::Rect(Geom::Point(0, 5), Geom::Point(10, 10)));
    // The Unused one's tile starts at 5,5 of its own space: its art is moved with it.
    auto unused = elements(find("Unused"), "svg:path");
    ASSERT_EQ(unused.size(), 1u);
    EXPECT_EQ(*sp_svg_read_pathv(attr(unused[0], "d").c_str()).boundsExact(), Geom::Rect(Geom::Point(-5, 4), Geom::Point(5, 9)));

    auto paths = elements(b->document->getReprRoot()->lastChild(), "svg:path");
    ASSERT_EQ(paths.size(), 2u);
    auto placed = [&](XML::Node *path, char const *property) -> XML::Node * {
        auto const style = attr(path, "style");
        auto const at = style.find(std::string(property) + ":url(#");
        if (at == std::string::npos) return nullptr;
        auto const from = at + std::string(property).size() + 6;
        return b->document->getObjectById(style.substr(from, style.find(')', from) - from))->getRepr();
    };
    auto *fill = placed(paths[0], "fill");
    ASSERT_TRUE(fill);
    EXPECT_EQ(attr(fill, "xlink:href"), "#" + attr(dots, "id"));
    Geom::Affine m;
    ASSERT_TRUE(sp_svg_transform_read(attr(fill, "patternTransform").c_str(), &m));
    // Tile to pattern (flip in the 10 high tile), the object's matrix, art to page (flip in the 100 high page).
    EXPECT_TRUE(Geom::are_near(m, Geom::Affine(2, 0, 0, 2, 30, 40), 1e-9)) << attr(fill, "patternTransform");
    auto *stroke = placed(paths[1], "stroke");
    ASSERT_TRUE(stroke);
    EXPECT_NE(stroke, fill);
    ASSERT_TRUE(sp_svg_transform_read(attr(stroke, "patternTransform").c_str(), &m));
    EXPECT_TRUE(Geom::are_near(m, Geom::Affine(1, 0, 0, 1, 0, 90), 1e-9)) << attr(stroke, "patternTransform");
    // The second object still has the first's fill: a pattern stays the paint like a colour.
    EXPECT_NE(attr(paths[1], "style").find("fill:url(#"), std::string::npos);
    // They survive a save and reopen, and the tile draws.
    auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(reopened);
    EXPECT_EQ(elements(reopened->getReprRoot(), "svg:pattern").size(), 5u);
}

// ---- linked pictures ----------------------------------------------------------------------

namespace {

/// A picture written to `file`, 4 x 2 unless said.
void write_picture(std::string const &file, int w = 4, int h = 2)
{
    auto *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, w, h);
    ASSERT_NE(pb, nullptr);
    gdk_pixbuf_fill(pb, 0x336699ff);
    EXPECT_TRUE(gdk_pixbuf_save(pb, file.c_str(), "png", nullptr, nullptr));
    g_object_unref(pb);
}

/// A file name as a string of the records.
std::string escaped(std::string const &name)
{
    std::string out;
    for (char c : name) {
        if (c == '\\' || c == '(' || c == ')') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

/// One pixel of the picture a document keeps of a linked one: alpha first.
std::string argb(int a, int r, int g, int b)
{
    return {char(a), char(r), char(g), char(b)};
}

/// That picture, `pixels` its rows one after another.
std::string kept_preview(int w, int h, std::string const &pixels)
{
    return "%AI17_Begin_Content_if_version_gt:24 5\n\n%AI26_BeginPlacedObjectPreview\n" + std::to_string(w) + " " +
           std::to_string(h) + " " + std::to_string(w * 4) + " 32 17 " + pixels +
           "\r\n%AI26_EndPlacedObjectPreview\n%AI17_End_Versioned_Content\n";
}

/// The pixels of a PNG in a data: address, as rows of red, green, blue, alpha.
std::string pixels_of(std::string const &address, int &w, int &h)
{
    std::string out;
    w = h = 0;
    auto const at = address.find("base64,");
    if (address.substr(0, 15) != "data:image/png;" || at == std::string::npos) return out;
    gsize size = 0;
    guchar *bytes = g_base64_decode(address.c_str() + at + 7, &size);
    auto *loader = gdk_pixbuf_loader_new();
    bool const ok = gdk_pixbuf_loader_write(loader, bytes, size, nullptr) && gdk_pixbuf_loader_close(loader, nullptr);
    auto *pb = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    if (pb && gdk_pixbuf_get_has_alpha(pb) && gdk_pixbuf_get_n_channels(pb) == 4) {
        w = gdk_pixbuf_get_width(pb);
        h = gdk_pixbuf_get_height(pb);
        auto const *rows = gdk_pixbuf_get_pixels(pb);
        for (int y = 0; y < h; ++y) {
            out.append(reinterpret_cast<char const *>(rows) + y * gdk_pixbuf_get_rowstride(pb), w * 4);
        }
    }
    g_object_unref(loader);
    g_free(bytes);
    return out;
}

/// A linked picture as Illustrator 2025 writes it, in the box 10..50 x 20..40 of the page.
/// `preview` is the picture the document keeps of it, if it keeps one.
std::string linked(std::string const &file, std::string const &folder = {}, std::string const &preview = {})
{
    return "%AI5_BeginPlace\n%%FileType: 1\n[1 0 0 -1 0 100] 10 20 50 40 4 4 0\n(" + escaped(file) + ")`\n"
           "%%IncludeFile:" + file + "\n" + preview + "%AI17_Begin_Content_if_version_gt:24 12\n\n%AI28_BeginPlacedRelativePath\n(" +
           escaped(folder) + ")\n%AI28_EndPlacedRelativePath\n%AI17_Alternate_Content\n%AI17_End_Versioned_Content\n\n~\n"
           "%AI5_EndPlace\n%_/ArtDictionary :\n%_(Photo) /UnicodeString (AIArtName) ,\n%_;\n";
}

std::string temporary_folder()
{
    gchar *dir = g_dir_make_tmp("proof-linked-XXXXXX", nullptr);
    EXPECT_NE(dir, nullptr);
    std::string out = dir ? dir : "";
    g_free(dir);
    return out;
}

} // namespace

TEST_F(AiNativeImportTest, LinkedPictureStaysALinkToItsFile)
{
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    auto const file = folder + G_DIR_SEPARATOR_S + "photo (1).png";
    write_picture(file);
    std::string error;
    auto ai = AN::read(records(layer(linked(file))), error);
    ASSERT_TRUE(ai) << error;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    EXPECT_TRUE(b->notes.empty());
    auto images = elements(b->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    // A link, not a copy of the picture.
    auto const href = attr(images[0], "xlink:href");
    EXPECT_EQ(href.substr(0, 8), "file:///");
    gchar *named = g_filename_from_uri(href.c_str(), nullptr, nullptr);
    ASSERT_NE(named, nullptr);
    EXPECT_TRUE(g_file_test(named, G_FILE_TEST_IS_REGULAR)) << named;
    g_free(named);
    EXPECT_EQ(attr(images[0], "sodipodi:absref"), file);
    EXPECT_EQ(attr(images[0], "inkscape:label"), "Photo");
    EXPECT_EQ(attr(images[0], "preserveAspectRatio"), "none");
    // Where the document puts it, and drawn.
    b->document->ensureUpToDate();
    auto *image = dynamic_cast<SPImage *>(b->document->getObjectByRepr(images[0]));
    ASSERT_TRUE(image);
    EXPECT_NE(image->pixbuf, nullptr);
    auto bounds = drawn(b->document.get(), images[0]);
    ASSERT_TRUE(bounds);
    EXPECT_TRUE(Geom::are_near(bounds->min(), Geom::Point(10, 20), 1e-6)) << bounds->min();
    EXPECT_TRUE(Geom::are_near(bounds->max(), Geom::Point(50, 40), 1e-6)) << bounds->max();
}

TEST_F(AiNativeImportTest, LinkedPictureIsLookedForBesideTheDocument)
{
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    auto const links = folder + G_DIR_SEPARATOR_S + "Links";
    ASSERT_EQ(g_mkdir_with_parents(links.c_str(), 0700), 0);
    write_picture(folder + G_DIR_SEPARATOR_S + "beside.png");
    write_picture(links + G_DIR_SEPARATOR_S + "below.png");
    AN::Source source;
    source.folder = folder;
    std::string error;
    // The machine that saved the document kept them somewhere this one doesn't have.
    auto const elsewhere = std::string("Q:\\Jobs\\2026\\");
    for (auto const &[name, relative] : {std::pair<std::string, std::string>{"beside.png", ""}, {"below.png", "Links\\"}}) {
        auto ai = AN::read(records(layer(linked(elsewhere + name, relative))), error);
        ASSERT_TRUE(ai) << error;
        // Without the document's folder there is nowhere to look.
        auto nowhere = AN::build(*ai, error);
        ASSERT_TRUE(nowhere) << error;
        ASSERT_EQ(nowhere->missing_links.size(), 1u);
        EXPECT_EQ(nowhere->missing_links[0].name, name);
        auto b = AN::build(*ai, error, source);
        ASSERT_TRUE(b) << error;
        EXPECT_TRUE(b->missing_links.empty());
        EXPECT_TRUE(b->notes.empty());
        auto images = elements(b->document->getReprRoot(), "svg:image");
        ASSERT_EQ(images.size(), 1u);
        gchar *named = g_filename_from_uri(attr(images[0], "xlink:href").c_str(), nullptr, nullptr);
        ASSERT_NE(named, nullptr) << attr(images[0], "xlink:href");
        EXPECT_TRUE(g_file_test(named, G_FILE_TEST_IS_REGULAR)) << named;
        g_free(named);
    }
}

TEST(LinkPolicy, PathsNameTheirNetworkShare)
{
    using Inkscape::IO::network_share;
    EXPECT_EQ(network_share("C:\\Jobs\\logo.png"), "");
    EXPECT_EQ(network_share("Links\\logo.png"), "");
    EXPECT_EQ(network_share("\\\\?\\C:\\Jobs\\logo.png"), "");
    EXPECT_EQ(network_share("\\\\NAS\\Jobs\\2026\\logo.png"), "\\\\nas\\jobs");
    EXPECT_EQ(network_share("//nas/Jobs/logo.png"), "\\\\nas\\jobs");
    EXPECT_EQ(network_share("\\\\?\\UNC\\Nas\\JOBS\\logo.png"), "\\\\nas\\jobs");
    EXPECT_EQ(network_share("\\\\НАС\\Работы\\logo.png"), "\\\\нас\\работы");
    // A computer without a share, and devices, match no share.
    EXPECT_EQ(network_share("\\\\nas"), "\\\\");
    EXPECT_EQ(network_share("\\\\nas\\"), "\\\\");
    EXPECT_EQ(network_share("\\\\.\\pipe\\logo"), "\\\\");
    EXPECT_EQ(network_share("\\\\?\\GLOBALROOT\\Device\\logo"), "\\\\");
}

#ifdef _WIN32
TEST(LinkPolicy, OnlyTheDocumentsOwnShareOrAChosenOneIsFollowed)
{
    using Inkscape::IO::may_follow_link;
    // Nothing here touches a network: the rule reads the paths alone (C: isn't a mapped drive).
    EXPECT_TRUE(may_follow_link("C:\\Jobs\\logo.png", nullptr));
    EXPECT_TRUE(may_follow_link("Links\\logo.png", "\\\\elsewhere\\share"));
    EXPECT_FALSE(may_follow_link("\\\\proof-test-a\\jobs\\logo.png", nullptr));
    EXPECT_FALSE(may_follow_link("\\\\proof-test-a\\jobs\\logo.png", "C:\\Work"));
    EXPECT_FALSE(may_follow_link("\\\\proof-test-a\\jobs\\logo.png", "\\\\proof-test-a\\other"));
    EXPECT_FALSE(may_follow_link("\\\\proof-test-a\\jobs\\logo.png", "\\\\proof-test-b\\jobs"));
    EXPECT_TRUE(may_follow_link("\\\\proof-test-a\\jobs\\logo.png", "\\\\PROOF-TEST-A\\Jobs\\2026"));
    EXPECT_TRUE(may_follow_link("//proof-test-a/jobs/logo.png", "\\\\?\\UNC\\proof-test-a\\jobs"));
    EXPECT_FALSE(may_follow_link("\\\\.\\pipe\\logo", "\\\\proof-test-a\\jobs"));
    // A share the user chose a file from is followed from then on; only that share.
    EXPECT_FALSE(may_follow_link("\\\\proof-test-c\\assets\\a.png", nullptr));
    Inkscape::IO::trust_link_share("\\\\proof-test-c\\Assets\\chosen.png");
    EXPECT_TRUE(may_follow_link("\\\\proof-test-c\\assets\\a.png", nullptr));
    EXPECT_FALSE(may_follow_link("\\\\proof-test-c\\other\\a.png", nullptr));
}

TEST_F(AiNativeImportTest, ALinkedPictureOnAnotherComputerIsNotLookedUpAndIsNamed)
{
    // Looked up, it would have Windows offer the user's sign-in to that computer.
    auto joined = [](auto const &notes) {
        std::string all;
        for (auto const &note : notes) all += note + "\n";
        return all;
    };
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    AN::Source source;
    source.folder = folder;
    std::string error;
    auto ai = AN::read(records(layer(linked("\\\\proof-test-nowhere\\share\\logo.png"))), error);
    ASSERT_TRUE(ai) << error;
    auto b = AN::build(*ai, error, source);
    ASSERT_TRUE(b) << error;
    ASSERT_EQ(b->missing_links.size(), 1u);
    EXPECT_EQ(b->missing_links[0].name, "logo.png");
    auto const notes = joined(b->notes);
    EXPECT_NE(notes.find("on another computer weren't looked up (\\\\proof-test-nowhere\\share)"), std::string::npos)
        << notes;
    // The link is kept as the document has it, and isn't loaded when the picture is drawn either.
    auto images = elements(b->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    b->document->ensureUpToDate();
    auto *image = dynamic_cast<SPImage *>(b->document->getObjectByRepr(images[0]));
    ASSERT_TRUE(image);
    EXPECT_TRUE(image->linkMissing());
    // One found beside the document is used, and needs no word.
    write_picture(folder + G_DIR_SEPARATOR_S + "logo.png");
    auto beside = AN::build(*ai, error, source);
    ASSERT_TRUE(beside) << error;
    EXPECT_TRUE(beside->missing_links.empty());
    EXPECT_EQ(joined(beside->notes).find("another computer"), std::string::npos) << joined(beside->notes);
}
#endif

TEST_F(AiNativeImportTest, ALinkedPictureThatIsGoneKeepsItsLinkAndShowsWhatTheDocumentKeepsOfIt)
{
    // Four pixels by two: red, green, blue and a white that is half clear, over a row of one colour.
    auto const rows = argb(255, 255, 0, 0) + argb(255, 0, 255, 0) + argb(255, 0, 0, 255) + argb(128, 255, 255, 255) +
                      argb(255, 0x33, 0x66, 0x99) + argb(255, 0x33, 0x66, 0x99) + argb(255, 0x33, 0x66, 0x99) +
                      argb(255, 0x33, 0x66, 0x99);
    auto const gone = std::string("Q:\\Jobs\\gone.png");
    std::string error;
    auto ai = AN::read(records(layer("0 g\n" + rect + linked(gone, {}, kept_preview(4, 2, rows)))), error);
    ASSERT_TRUE(ai) << error;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;

    // The document says which picture, and where it lies.
    ASSERT_EQ(b->missing_links.size(), 1u);
    EXPECT_EQ(b->missing_links[0].name, "gone.png");
    EXPECT_TRUE(b->missing_links[0].preview);
    Geom::Rect box(b->missing_links[0].box[0], b->missing_links[0].box[0]);
    for (auto const &corner : b->missing_links[0].box) box.expandTo(corner);
    EXPECT_TRUE(Geom::are_near(box.min(), Geom::Point(10, 20), 1e-6)) << box.min();
    EXPECT_TRUE(Geom::are_near(box.max(), Geom::Point(50, 40), 1e-6)) << box.max();
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("1 linked picture can't be found (gone.png)"), std::string::npos) << b->notes[0];
    EXPECT_NE(b->notes[0].find("the small picture the document keeps of it shows in its place"), std::string::npos) << b->notes[0];

    // The link is kept as it was written, and the kept picture beside it.
    auto images = elements(b->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(attr(images[0], "sodipodi:absref"), gone);
    EXPECT_NE(attr(images[0], "xlink:href").substr(0, 5), "data:");
    EXPECT_NE(attr(images[0], "xlink:href").find("gone.png"), std::string::npos);
    int w = 0, h = 0;
    auto const kept = pixels_of(attr(images[0], SPImage::LINK_PREVIEW_ATTRIBUTE), w, h);
    EXPECT_EQ(w, 4);
    EXPECT_EQ(h, 2);
    auto rgba = [](int r, int g, int bl, int a) { return std::string{char(r), char(g), char(bl), char(a)}; };
    EXPECT_EQ(kept, rgba(255, 0, 0, 255) + rgba(0, 255, 0, 255) + rgba(0, 0, 255, 255) + rgba(255, 255, 255, 128) +
                        rgba(0x33, 0x66, 0x99, 255) + rgba(0x33, 0x66, 0x99, 255) + rgba(0x33, 0x66, 0x99, 255) +
                        rgba(0x33, 0x66, 0x99, 255));

    // It shows that picture in the box the file's own lay in, and counts as missing.
    b->document->ensureUpToDate();
    auto *image = cast<SPImage>(b->document->getObjectByRepr(images[0]));
    ASSERT_TRUE(image);
    EXPECT_TRUE(image->missing);
    EXPECT_TRUE(image->linkMissing());
    ASSERT_NE(image->pixbuf, nullptr);
    EXPECT_EQ(image->pixbuf->width(), 4);
    EXPECT_EQ(image->pixbuf->height(), 2);
    EXPECT_NE(image->linkedFile().find("gone.png"), std::string::npos) << image->linkedFile();
    auto bounds = drawn(b->document.get(), images[0]);
    ASSERT_TRUE(bounds);
    EXPECT_TRUE(Geom::are_near(bounds->min(), Geom::Point(10, 20), 1e-6)) << bounds->min();
    EXPECT_TRUE(Geom::are_near(bounds->max(), Geom::Point(50, 40), 1e-6)) << bounds->max();
    EXPECT_EQ(sp_images_missing(b->document.get()), std::vector<SPImage *>{image});
    auto const mark = Inkscape::Display::link_highlight_shape(image);
    ASSERT_EQ(mark.size(), 4u);
    EXPECT_EQ(mark[0], Geom::Point(10, 20));
    EXPECT_EQ(mark[2], Geom::Point(50, 40));

    // A long name is cut short in what is said, and whole in what is kept.
    auto const wordy = std::string("Q:\\Jobs\\a picture with a name that goes on for a good deal longer than a note has room for.png");
    ai = AN::read(records(layer(linked(wordy))), error);
    ASSERT_TRUE(ai) << error;
    auto named = AN::build(*ai, error);
    ASSERT_TRUE(named) << error;
    ASSERT_EQ(named->missing_links.size(), 1u);
    EXPECT_EQ(named->missing_links[0].name, wordy.substr(8));
    ASSERT_EQ(named->notes.size(), 1u);
    EXPECT_NE(named->notes[0].find("(a picture with a name that goes ... room for.png)"), std::string::npos) << named->notes[0];

    // A document that keeps no picture of it: a marked box, and it says so.
    ai = AN::read(records(layer(linked(gone) + linked(gone))), error);
    ASSERT_TRUE(ai) << error;
    auto plain = AN::build(*ai, error);
    ASSERT_TRUE(plain) << error;
    ASSERT_EQ(plain->missing_links.size(), 2u);
    EXPECT_FALSE(plain->missing_links[0].preview);
    ASSERT_EQ(plain->notes.size(), 1u);
    EXPECT_NE(plain->notes[0].find("2 linked pictures can't be found (gone.png): a marked box shows in the place of each"),
              std::string::npos) << plain->notes[0];
    images = elements(plain->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 2u);
    EXPECT_TRUE(attr(images[0], SPImage::LINK_PREVIEW_ATTRIBUTE).empty());
    plain->document->ensureUpToDate();
    image = cast<SPImage>(plain->document->getObjectByRepr(images[0]));
    ASSERT_TRUE(image);
    EXPECT_TRUE(image->linkMissing());
    EXPECT_NE(image->pixbuf, nullptr);

    // In a pattern's tile it shows wherever the pattern paints, with no one place to leave
    // out of a comparison: such a document is still refused. A pattern nothing paints with
    // is taken out, as before.
    auto const tile = pattern("Dots", "0 0 20 10", linked(gone));
    ai = AN::read(records(layer("(Dots) 0 0 1 1 0 0 0 0 0 [1 0 0 1 0 0] p\n" + rect), tile), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("in a pattern can't be found (gone.png)"), std::string::npos) << error;
    ai = AN::read(records(layer("0 g\n" + rect), tile), error);
    ASSERT_TRUE(ai) << error;
    auto unused = AN::build(*ai, error);
    ASSERT_TRUE(unused) << error;
    EXPECT_TRUE(unused->missing_links.empty());
    EXPECT_TRUE(elements(unused->document->getReprRoot(), "svg:image").empty());
}

TEST_F(AiNativeImportTest, ALinkedCmykTiffWithAnotherProfileIsNamedAndKeepsItsNumbers)
{
    // Preserve Numbers (owner, 2026-10-16): the TIFF's numbers are the document's; a profile of
    // its own that differs is said, as most TIFFs share the document's and some don't.
    std::string gracol, fogra;
    auto read_file = [](char const *path, std::string &out) {
        gchar *data = nullptr;
        gsize len = 0;
        if (!g_file_get_contents(path, &data, &len, nullptr)) return false;
        out.assign(data, len);
        g_free(data);
        return true;
    };
    if (!read_file("C:/Windows/System32/spool/drivers/color/CoatedGRACoL2006.icc", gracol) ||
        !read_file("C:/Windows/System32/spool/drivers/color/CoatedFOGRA39.icc", fogra)) {
        GTEST_SKIP() << "CoatedGRACoL2006.icc and CoatedFOGRA39.icc are needed";
    }
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    auto write_tiff = [&](char const *name, std::string const &icc) {
        auto const path = folder + G_DIR_SEPARATOR_S + name;
        TIFF *t = TIFFOpen(path.c_str(), "w");
        EXPECT_NE(t, nullptr);
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, 2);
        TIFFSetField(t, TIFFTAG_IMAGELENGTH, 1);
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
        TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        if (!icc.empty()) TIFFSetField(t, TIFFTAG_ICCPROFILE, std::uint32_t(icc.size()), icc.data());
        std::uint8_t row[8] = {0, 255, 150, 0, 0, 0, 0, 0};
        TIFFWriteScanline(t, row, 0, 0);
        TIFFClose(t);
        return path;
    };
    auto const other = write_tiff("other.tif", fogra);
    auto const same = write_tiff("same.tif", gracol);
    auto const plain = write_tiff("plain.tif", {});
    std::string error;
    auto ai = AN::read(records(layer(linked(other) + linked(same) + linked(plain))), error);
    ASSERT_TRUE(ai) << error;
    AN::Source source;
    source.profile = gracol;
    auto b = AN::build(*ai, error, source);
    ASSERT_TRUE(b) << error;
    std::string note;
    for (auto const &n : b->notes) {
        if (n.find("a profile other than the document's") != std::string::npos) note = n;
    }
    EXPECT_NE(note.find("1 linked CMYK picture carries"), std::string::npos) << note;
    EXPECT_NE(note.find("other.tif"), std::string::npos) << note;
    EXPECT_EQ(note.find("same.tif"), std::string::npos) << note;
    EXPECT_EQ(note.find("plain.tif"), std::string::npos) << note;
    EXPECT_NE(note.find("kept"), std::string::npos) << note;
    // All three are in the document, linked as they are.
    EXPECT_EQ(elements(b->document->getReprRoot(), "svg:image").size(), 3u);
}

TEST_F(AiNativeImportTest, ALinkedPhotoshopFileShowsItsMergedImage)
{
    // The collaborator's linked Photoshop files (2026-10-16): CMYK with transparency, saved with
    // their merged image. One saved without it says why it can't be shown.
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    auto write = [&](char const *name, PsdFixture::Psd const &p) {
        using PsdFixture::plane8;
        // Full cyan, opaque; nothing at all.
        auto const data = PsdFixture::make_psd(
            p, {plane8({0, 255}), plane8({255, 255}), plane8({255, 255}), plane8({255, 255}), plane8({255, 0})});
        auto const path = folder + G_DIR_SEPARATOR_S + name;
        EXPECT_TRUE(g_file_set_contents(path.c_str(), data.data(), gssize(data.size()), nullptr));
        return path;
    };
    PsdFixture::Psd p;
    p.w = 2;
    auto const gifts = write("gifts.psd", p);
    std::string error;
    auto ai = AN::read(records(layer(linked(gifts))), error);
    ASSERT_TRUE(ai) << error;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    auto const images = elements(b->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    b->document->ensureUpToDate();
    auto *image = cast<SPImage>(b->document->getObjectByRepr(images[0]));
    ASSERT_TRUE(image && image->pixbuf);
    EXPECT_EQ(image->pixbuf->width(), 2);
    auto const *row = reinterpret_cast<std::uint32_t const *>(cairo_image_surface_get_data(image->pixbuf->getSurfaceRaw()));
    EXPECT_EQ(row[0] >> 24, 255u);
    EXPECT_LT((row[0] >> 16) & 0xff, row[0] & 0xff) << "cyan";
    EXPECT_EQ(row[1], 0u) << "transparent";

    p.merged = false;
    auto const flat = write("flat.psd", p);
    ai = AN::read(records(layer(linked(flat))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("Maximize Compatibility"), std::string::npos) << error;
    EXPECT_NE(error.find("flat.psd"), std::string::npos) << error;
}

TEST_F(AiNativeImportTest, ALinkedPdfPageShowsTheSelectedPageCroppedToItsCropBox)
{
    // The collaborator's linked .ai and .pdf files (2026-10-17): Illustrator places a page and
    // keeps it as it is. Each page must retain its own drawing, also in the screen cache.
    auto const placed = fixture(records(layer("0 g\n" + rect)),
                                {"0 0 1 0 k 0 0 200 100 re f", "0 1 0 rg 0 0 200 100 re f"});
    // As Illustrator writes the placement: the box the size of the page box it is cropped to.
    auto page = [](std::string const &file, double w, double h, int index) {
        auto num = [](double v) {
            std::ostringstream os;
            os << v;
            return os.str();
        };
        return "%AI5_BeginPlace\n%%FileType: 1\n[1 0 0 -1 0 100] 7893 " + num(7770 - h) + " " + num(7893 + w) +
               " 7770 4 4 0\n(" + escaped(file) + ")`\n%%IncludeFile:" + file + "\n~\n%AI5_EndPlace\n"
               "%_/ArtDictionary :\n%_3 /Int (AI11 PlacedPDFCropTo) ,\n%_" + std::to_string(index) +
               " /Int (AI11 PlacedPDFPageIndex) ,\n%_;\n";
    };
    std::string error;
    auto ai = AN::read(records(layer(page(placed, 200, 100, 1))), error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->placed.size(), 1u);
    EXPECT_EQ(ai->placed[0].page, 1);
    EXPECT_EQ(ai->placed[0].crop_to, 3);
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    auto const images = elements(b->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    b->document->ensureUpToDate();
    auto *image = cast<SPImage>(b->document->getObjectByRepr(images[0]));
    ASSERT_TRUE(image && image->pixbuf);
    // At 300 pixels an inch: 200 by 100 points.
    EXPECT_EQ(image->pixbuf->width(), 833);
    EXPECT_EQ(image->pixbuf->height(), 417);
    auto *surface = image->pixbuf->getSurfaceRaw();
    auto const *row = cairo_image_surface_get_data(surface) + 200 * cairo_image_surface_get_stride(surface);
    auto const yellow = reinterpret_cast<std::uint32_t const *>(row)[400];
    EXPECT_EQ(yellow >> 24, 255u);
    EXPECT_GT((yellow >> 16) & 0xff, 200u);
    EXPECT_GT((yellow >> 8) & 0xff, 200u);
    EXPECT_LT(yellow & 0xff, 60u);

    // Select page 2, save/reopen, then change back and forth as Undo/Redo would.
    ai = AN::read(records(layer(page(placed, 200, 100, 2))), error);
    ASSERT_TRUE(ai) << error;
    b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    auto saved = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(saved);
    auto const second = elements(saved->getReprRoot(), "svg:image");
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(attr(second[0], SPImage::PDF_PAGE_ATTRIBUTE), "2");
    image = cast<SPImage>(saved->getObjectByRepr(second[0]));
    ASSERT_TRUE(image);
    auto centre = [&]() {
        saved->ensureUpToDate();
        if (!image->pixbuf) return std::uint32_t(0);
        auto *s = image->pixbuf->getSurfaceRaw();
        return reinterpret_cast<std::uint32_t const *>(cairo_image_surface_get_data(s) +
                   200 * cairo_image_surface_get_stride(s))[400];
    };
    EXPECT_EQ(centre(), 0xff00ff00u);
    second[0]->removeAttribute(SPImage::PDF_PAGE_ATTRIBUTE);
    EXPECT_EQ(centre(), yellow);
    second[0]->setAttribute(SPImage::PDF_PAGE_ATTRIBUTE, "2");
    EXPECT_EQ(centre(), 0xff00ff00u);
    for (auto const *invalid : {"0", "-1", "2.5", "2147483648", "2junk"}) {
        second[0]->setAttribute(SPImage::PDF_PAGE_ATTRIBUTE, invalid);
        saved->ensureUpToDate();
        EXPECT_EQ(image->pdfPage(), 0) << invalid;
        EXPECT_TRUE(image->missing) << invalid;
    }
    second[0]->setAttribute(SPImage::PDF_PAGE_ATTRIBUTE, "3");
    saved->ensureUpToDate();
    EXPECT_TRUE(image->missing) << "an unavailable page must never load page 1";
    second[0]->setAttribute(SPImage::PDF_PAGE_ATTRIBUTE, "2");
    EXPECT_EQ(centre(), 0xff00ff00u);
    // A missing page or another box than its crop box is refused, saying why.
    ai = AN::read(records(layer(page(placed, 200, 100, 3))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("page 3"), std::string::npos) << error;
    for (auto const *invalid : {"0", "-1", "2.5", "2147483648"}) {
        auto record = page(placed, 200, 100, 2);
        auto const at = record.find("%_2 /Int (AI11 PlacedPDFPageIndex)");
        ASSERT_NE(at, std::string::npos);
        record.replace(at + 2, 1, invalid);
        EXPECT_FALSE(AN::read(records(layer(record)), error)) << invalid;
        EXPECT_NE(error.find("invalid page number"), std::string::npos) << error;
    }
    ai = AN::read(records(layer(page(placed, 150, 100, 1))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("crop box"), std::string::npos) << error;
    auto const rotated = fixture("", {"", ""}, "/MediaBox [0 0 200 100] /Rotate 90");
    ai = AN::read(records(layer(page(rotated, 200, 100, 2))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("page is turned"), std::string::npos) << error;
    // An .ai of Illustrator 8 or older is PostScript, not a page.
    auto const folder = temporary_folder();
    auto const old = folder + G_DIR_SEPARATOR_S + "old.ai";
    std::string const postscript = "%!PS-Adobe-3.0\n%%BoundingBox: 0 0 200 100\n%%EOF\n";
    ASSERT_TRUE(g_file_set_contents(old.c_str(), postscript.data(), gssize(postscript.size()), nullptr));
    ai = AN::read(records(layer(page(old, 200, 100, 1))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("can't be read as a PDF"), std::string::npos) << error;
}

TEST_F(AiNativeImportTest, PicturesThatAreGoneAreLinkedAgainFromOneFolder)
{
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    auto const in_folder = [&](char const *name) { return folder + G_DIR_SEPARATOR_S + name; };
    write_picture(in_folder("a.png"));
    write_picture(in_folder("b.png"));
    write_picture(in_folder("tall.png"), 2, 4);
    write_picture(in_folder("here.png"));
    ASSERT_TRUE(g_file_set_contents(in_folder("notes.txt").c_str(), "not a picture", -1, nullptr));
    auto const q = std::string("Q:\\Jobs\\");
    auto const kept = kept_preview(1, 1, argb(255, 1, 2, 3));
    std::string error;
    auto ai = AN::read(records(layer(linked(q + "a.png", {}, kept) + linked(q + "b.png", {}, kept) + linked(q + "c.png") +
                                     linked(in_folder("here.png")))),
                       error);
    ASSERT_TRUE(ai) << error;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    auto *doc = b->document.get();
    auto missing = sp_images_missing(doc);
    ASSERT_EQ(missing.size(), 3u);
    EXPECT_EQ(b->missing_links.size(), 3u);
    auto *a = missing[0];
    EXPECT_NE(a->linkedFile().find("a.png"), std::string::npos);

    // What isn't a picture links nothing.
    EXPECT_EQ(sp_images_relink(a, in_folder("notes.txt")), 0u);
    EXPECT_EQ(sp_images_relink(a, in_folder("nothing.png")), 0u);
    EXPECT_EQ(sp_images_missing(doc).size(), 3u);

    // One is linked by hand, and the other that has a file of its name there with it.
    auto const gone_href = attr(a->getRepr(), "xlink:href"), gone_absref = attr(a->getRepr(), "sodipodi:absref");
    auto const gone_preview = attr(a->getRepr(), SPImage::LINK_PREVIEW_ATTRIBUTE);
    EXPECT_EQ(sp_images_relink(a, in_folder("a.png")), 2u);
    missing = sp_images_missing(doc);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_NE(missing[0]->linkedFile().find("c.png"), std::string::npos);
    EXPECT_FALSE(a->missing);
    ASSERT_NE(a->pixbuf, nullptr);
    EXPECT_EQ(a->pixbuf->width(), 4);
    EXPECT_EQ(attr(a->getRepr(), "sodipodi:absref"), in_folder("a.png"));
    EXPECT_EQ(a->linkedFile(), in_folder("a.png"));
    EXPECT_TRUE(attr(a->getRepr(), SPImage::LINK_PREVIEW_ATTRIBUTE).empty());
    // The same shape as the box it lay in: it fills it as before.
    EXPECT_EQ(attr(a->getRepr(), "preserveAspectRatio"), "none");
    EXPECT_TRUE(Inkscape::Display::link_highlight_shape(a).empty());

    // Taken back, as Undo takes it: the kept picture shows again, and though it is of
    // another shape than the file's, the box stays as it was.
    a->getRepr()->setAttribute("xlink:href", gone_href);
    a->getRepr()->setAttribute("sodipodi:absref", gone_absref);
    a->getRepr()->setAttribute(SPImage::LINK_PREVIEW_ATTRIBUTE, gone_preview);
    doc->ensureUpToDate();
    EXPECT_TRUE(a->linkMissing());
    ASSERT_NE(a->pixbuf, nullptr);
    EXPECT_EQ(a->pixbuf->width(), 1);
    EXPECT_EQ(attr(a->getRepr(), "width"), "40");
    EXPECT_EQ(attr(a->getRepr(), "height"), "20");
    EXPECT_EQ(sp_images_relink(a, in_folder("a.png")), 1u);
    EXPECT_EQ(attr(a->getRepr(), "width"), "40");
    EXPECT_EQ(attr(a->getRepr(), "height"), "20");
    missing = sp_images_missing(doc);
    ASSERT_EQ(missing.size(), 1u);

    // A picture of another shape keeps its own, inside the box.
    auto *c = missing[0];
    EXPECT_EQ(Inkscape::Display::link_highlight_shape(c).size(), 4u);
    EXPECT_EQ(sp_images_relink(c, in_folder("tall.png")), 1u);
    EXPECT_EQ(attr(c->getRepr(), "preserveAspectRatio"), "xMidYMid");
    EXPECT_EQ(attr(c->getRepr(), "width"), "40");
    EXPECT_EQ(attr(c->getRepr(), "height"), "20");
    EXPECT_TRUE(sp_images_missing(doc).empty());
}

TEST_F(AiNativeImportTest, LinkedPictureThatDoesNotShowOrCannotBeRead)
{
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    AN::Source source;
    source.folder = folder;
    std::string error;
    // Not there, in a layer that doesn't show: kept as a link, and noted as one that doesn't show.
    auto const gone = std::string("Q:\\Jobs\\gone.png");
    auto ai = AN::read(records(layer("0 g\n" + rect + linked(gone))), error);
    ASSERT_TRUE(ai) << error;
    ai->layers[0].visible = false;
    auto hidden = AN::build(*ai, error, source);
    ASSERT_TRUE(hidden) << error;
    EXPECT_TRUE(hidden->missing_links.empty());
    auto images = elements(hidden->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(attr(images[0], "sodipodi:absref"), gone);
    ASSERT_EQ(hidden->notes.size(), 1u);
    EXPECT_NE(hidden->notes[0].find("linked picture that doesn't show"), std::string::npos) << hidden->notes[0];
    // A hidden object in a layer that shows doesn't show either.
    ai->layers[0].visible = true;
    ai->layers[0].children.back().visible = false;
    hidden = AN::build(*ai, error, source);
    ASSERT_TRUE(hidden) << error;
    EXPECT_TRUE(hidden->missing_links.empty());

    // A file nothing here draws (Photoshop's, a PDF).
    auto const other = folder + G_DIR_SEPARATOR_S + "layout.psd";
    ASSERT_TRUE(g_file_set_contents(other.c_str(), "8BPS not a picture", -1, nullptr));
    ai = AN::read(records(layer(linked(other))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error, source));
    EXPECT_NE(error.find("can't show yet (layout.psd)"), std::string::npos) << error;
}

#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
TEST_F(AiNativeImportTest, PatternFillMatchesIndependentPdfAndAShiftedOneIsRefused)
{
    // A 20 x 20 tile with a 10 x 10 square in its lower left corner, started at 5, 5 of the
    // art. The page draws the same squares one by one.
    auto const tile = "0 g\n0 0 m 0 10 L 10 10 L 10 0 L 0 0 L\nf\n";
    auto native = [&](std::string const &matrix) {
        return records(layer("(Checks) 0 0 1 1 0 0 0 0 0 [" + matrix + "] p\n0 0 m 0 100 L 200 100 L 200 0 L 0 0 L\nf\n"),
                       pattern("Checks", "0 0 20 20", tile));
    };
    std::string page = "0 g ";
    for (int i = -1; i < 10; ++i) {
        for (int j = -1; j < 5; ++j) page += std::to_string(5 + 20 * i) + " " + std::to_string(5 + 20 * j) + " 10 10 re ";
    }
    page += "f";
    std::string reason;
    auto good = Extension::Internal::open_ai_native(fixture(native("1 0 0 1 5 5"), {page}), reason);
    ASSERT_TRUE(good) << reason;
    ASSERT_EQ(good->differences.size(), 1u);
    EXPECT_LT(good->differences[0].lightness, 0.005);
    // The tile started elsewhere, or drawn upside down (the square in its upper left corner).
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(native("1 0 0 1 15 5"), {page}), reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(native("1 0 0 1 5 15"), {page}), reason));
    // Scaled and turned a quarter: 40 x 40 tiles, the square now in the tile's lower right.
    std::string turned = "0 g ";
    for (int i = -1; i < 6; ++i) {
        for (int j = -1; j < 4; ++j) turned += std::to_string(20 + 40 * i) + " " + std::to_string(40 * j) + " 20 20 re ";
    }
    turned += "f";
    EXPECT_TRUE(Extension::Internal::open_ai_native(fixture(native("0 2 -2 0 40 0"), {turned}), reason)) << reason;
}

TEST_F(AiNativeImportTest, AreaTypeMatchesIndependentPdf)
{
    // Courier New's advance is 0.6 em: the page sets the two lines itself, where the frame
    // broke the paragraph.
    auto *font = FontFactory::get().parsePostscriptName("CourierNewPSMT", false);
    if (!font) GTEST_SKIP() << "CourierNewPSMT isn't installed";
    pango_font_description_free(font);
    auto src = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                       text_setup(area_text_document("(Hello world\\r)", 12, {{0, 22, 6}, {0, 46, 6}}, "CourierNewPSMT",
                                                     "/1 24 /53 << /0 << /0 0 /1 [1 0] >> >>")));
    std::string reason;
    auto good = Extension::Internal::open_ai_native(
        fixture(src, {"0 g BT /F1 24 Tf 10 38 Td (Hello ) Tj ET BT /F1 24 Tf 10 14 Td (world) Tj ET"},
                "/MediaBox [0 0 200 100]", "CourierNewPSMT"), reason);
    ASSERT_TRUE(good) << reason;
    // The second line where a renderer that didn't break the paragraph would put it.
    EXPECT_FALSE(Extension::Internal::open_ai_native(
        fixture(src, {"0 g BT /F1 24 Tf 10 38 Td (Hello ) Tj ET BT /F1 24 Tf 96.4 38 Td (world) Tj ET"},
                "/MediaBox [0 0 200 100]", "CourierNewPSMT"), reason));
}

TEST_F(AiNativeImportTest, PointTextRightAndCentreMatchIndependentPdfAndReopen)
{
    // Courier New's advance is 0.6 em: these independent PDF starts are exact.
    auto *font = FontFactory::get().parsePostscriptName("CourierNewPSMT", false);
    if (!font) GTEST_SKIP() << "CourierNewPSMT isn't installed";
    pango_font_description_free(font);
    for (unsigned align : {1u, 2u}) {
        auto src = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                           text_setup(aligned_text_document(align, "CourierNewPSMT")));
        auto pdf = "0 g BT /F1 32 Tf " + std::string(align == 2 ? "51.2" : "22.4") + " 60 Td (HHH) Tj ET";
        std::string reason;
        auto good = Extension::Internal::open_ai_native(fixture(src, {pdf}, "/MediaBox [0 0 200 100]", "CourierNewPSMT"), reason);
        ASSERT_TRUE(good) << reason;
        auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(good->document->getReprDoc()).raw());
        ASSERT_TRUE(reopened);
        auto texts = elements(reopened->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
        auto spans = elements(texts[0], "svg:tspan"); ASSERT_EQ(spans.size(), 1u);
        EXPECT_EQ(attr(spans[0], "style"), align == 2 ? "text-anchor:middle;" : "text-anchor:end;");
    }
}

TEST_F(AiNativeImportTest, PointTextVisiblePdfGlyphsAndDisplacedTextRefused)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // The PDF uses its own text operator/font lookup, independent of ATE and SVG.
    // Both baselines are at (10, 60) in the 200 x 100 point artboard.
    auto src = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                       text_setup(text_document("(Hello\\r)", 6, font,
                                                "/1 32 /53 << /0 << /0 0 /1 [1 0] >> >>")));
    auto built = make(src); ASSERT_TRUE(built);
    auto const *diagnostics = g_getenv("PROOF_AI_TEXT_DIAG");
    if (diagnostics) {
        g_mkdir_with_parents(diagnostics, 0700);
        auto svg = sp_repr_save_buf(built->document->getReprDoc());
        g_file_set_contents((std::string(diagnostics) + "/native.svg").c_str(), svg.c_str(), svg.bytes(), nullptr);
    }
    std::string reason;
    auto good = Extension::Internal::open_ai_native(
        fixture(src, {"0 g BT /F1 32 Tf 10 60 Td (Hello) Tj ET"},
                "/MediaBox [0 0 200 100]", font), reason, Extension::Internal::AiKeepDiffering::No,
            diagnostics ? diagnostics : "");
    ASSERT_TRUE(good) << reason << " (font " << font << ")";
    auto texts = elements(good->document->getReprRoot(), "svg:text"); ASSERT_EQ(texts.size(), 1u);
    auto *item = dynamic_cast<SPItem *>(good->document->getObjectByRepr(texts[0])); ASSERT_TRUE(item);
    auto bounds = item->documentVisualBounds(); ASSERT_TRUE(bounds);
    EXPECT_GT(bounds->width(), 50); EXPECT_GT(bounds->height(), 20);
    EXPECT_GT(bounds->left(), 10); EXPECT_LT(bounds->top(), 40);
    auto wrong = Extension::Internal::open_ai_native(
        fixture(src, {"0 g BT /F1 32 Tf 100 30 Td (Hello) Tj ET"},
                "/MediaBox [0 0 200 100]", font), reason);
    EXPECT_FALSE(wrong);
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
    auto scaled_src = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
        text_setup(text_document("(Hello\\r)", 6, font,
                   "/1 32 /6 0.8 /7 1.2 /53 << /0 << /0 0 /1 [1 0] >> >>")));
    auto scaled = Extension::Internal::open_ai_native(
        fixture(scaled_src, {"q 0.8 0 0 1.2 10 60 cm 0 g BT /F1 32 Tf 0 0 Td (Hello) Tj ET Q"},
                "/MediaBox [0 0 200 100]", font), reason);
    ASSERT_TRUE(scaled) << reason;
}

TEST_F(AiNativeImportTest, VectorOpacityMasksMatchIndependentPdfArtwork)
{
    auto const half = std::string("10 20 m 10 60 L 50 60 L 50 20 L f\n");
    for (auto const &paint : {"1 g\n", "0.5 g\n", "0 0 0 0.5 k\n", "0 g\n"}) {
        auto const src = records(layer("0 0 0 0 0 0 0 Xa\n" + rect + vector_mask(paint + half)), {}, false);
        auto const level = std::string(paint) == "1 g\n" ? "0" : std::string(paint) == "0 g\n" ? "1" : "0.5";
        std::string reason;
        auto good = Extension::Internal::open_ai_native(fixture(src, {std::string(level) + " g 10 20 40 40 re f"}), reason);
        ASSERT_TRUE(good) << paint << reason;
        // A missing mask would show the full black rectangle and must still be rejected.
        EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(src, {"0 g 10 20 80 40 re f"}), reason));
    }
    auto const empty = records(layer("0 g\n" + rect + vector_mask("")), {}, false);
    std::string reason;
    EXPECT_TRUE(Extension::Internal::open_ai_native(fixture(empty, {""}), reason)) << reason;
    // Raster placement has its own transform. The mask remains in art coordinates,
    // so just the left half of a scaled black image survives. Enough source pixels
    // keep the image's edge interpolation negligible against the flat PDF reference.
    std::string image = "%AI5_BeginRaster\n() 1 XG\n/DeviceRGB XN\n[0.5 0 0 0.5 10 60] 0 0 80 80 80 80 8 3 0 0 1 0 0\n"
                        "%%BeginData: 19204\nXI\n";
    image += std::string(80 * 80 * 3, '\0') + "\n%%EndData\nXH\n%AI5_EndRaster\n";
    EXPECT_TRUE(Extension::Internal::open_ai_native(
        fixture(records(layer(image), {}, false), {"0 g 10 20 40 40 re f"}), reason)) << reason;
    auto const masked_image = records(layer(image + vector_mask("1 g\n10 20 m 10 60 L 30 60 L 30 20 L f\n")), {}, false);
    EXPECT_TRUE(Extension::Internal::open_ai_native(fixture(masked_image, {"0 g 10 20 20 40 re f"}), reason)) << reason;
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(masked_image, {"0 g 10 20 40 40 re f"}), reason));
}

TEST_F(AiNativeImportTest, UnclippedOpacityMasksCompositeOnWhiteAndKeepNestedCoverage)
{
    auto const half = std::string("10 20 m 10 60 L 50 60 L 50 20 L f\n");
    std::vector<std::pair<std::string, std::string>> cases = {
        {vector_mask("", 0), "0 g 10 20 80 40 re f"},
        {vector_mask("0 0.5 0 0 0 Xy\n0 g\n" + half, 0), "0.5 g 10 20 40 40 re f 0 g 50 20 40 40 re f"},
        {vector_mask("0 0.5 0 0 0 Xy\n0 g\n" + rect + vector_mask("1 g\n" + half), 0),
         "0.25 g 10 20 40 40 re f 0 g 50 20 40 40 re f"},
        // The inner artwork inherits half opacity above. Reset it explicitly here:
        // opaque white coverage then leaves only the outer artwork's half opacity.
        {vector_mask("0 0.5 0 0 0 Xy\n0 g\n" + rect + vector_mask("0 1 0 0 0 Xy\n1 g\n" + half), 0),
         "0.5 g 10 20 40 40 re f 0 g 50 20 40 40 re f"},
        {vector_mask("1 G 4 w\n30 30 m 70 30 L 70 50 L 30 50 L h S\n"), "0 G 4 w 30 30 40 20 re S"},
        {vector_mask("0.5 g\n" + rect + "u\n1 1 0 0 0 Xy\n0.5 g\n" + half + "U\n", 0),
         "0.75 g 10 20 40 40 re f 0.5 g 50 20 40 40 re f"}
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto const source = records(layer("0 g\n" + rect + cases[i].first), {}, false);
        std::string reason;
        auto good = Extension::Internal::open_ai_native(fixture(source, {cases[i].second}), reason);
        ASSERT_TRUE(good) << i << ": " << reason;
        EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(source, {"1 g 10 20 80 40 re f"}), reason));
        if (auto const *dir = g_getenv("PROOF_AI_MASK_DIAG")) {
            g_mkdir_with_parents(dir, 0700);
            auto const svg = sp_repr_save_buf(good->document->getReprDoc());
            g_file_set_contents((std::string(dir) + "/mask-" + std::to_string(i) + ".svg").c_str(), svg.c_str(), svg.bytes(), nullptr);
        }
    }
}

TEST_F(AiNativeImportTest, OpacityMaskGradientsMatchIndependentPdfShading)
{
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0 0 0 1 1 50 100 Bs\n"
                              "0 0 0 0 1 50 0 Bs\nBD\n%AI5_EndGradient\n";
    std::string const half = "10 20 m 50 20 L 50 60 L 10 60 L h\n";
    std::string const mask = half + "Bb\n0 (G) 10 20 0 40 1 0 0 1 0 0 Bg\nf\n0 BB\n";
    std::string const shading = "<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [10 20 50 20] "
        "/Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> /Extend [true true] >>";
    for (int clip : {0, 1}) {
        auto const source = records(layer("0 g\n" + rect + vector_mask(mask, clip)), setup, false);
        auto const page = std::string(clip ? "" : "0 g 50 20 40 40 re f ") +
            "q 10 20 40 40 re W n /S1 sh Q";
        std::string reason;
        auto good = Extension::Internal::open_ai_native(
            fixture(source, {page}, "/MediaBox [0 0 200 100]", {}, {}, shading), reason);
        ASSERT_TRUE(good) << clip << ": " << reason;
        EXPECT_LT(good->differences[0].lightness, 0.005);
        if (auto const *dir = g_getenv("PROOF_AI_MASK_DIAG")) {
            g_mkdir_with_parents(dir, 0700);
            auto const svg = sp_repr_save_buf(good->document->getReprDoc());
            g_file_set_contents((std::string(dir) + "/gradient-" + std::to_string(clip) + ".svg").c_str(), svg.c_str(), svg.bytes(), nullptr);
        }
    }
}

TEST_F(AiNativeImportTest, UnclippedOpacityMaskBackdropIncludesOffboardArtworkAndStrokes)
{
    // Nothing in this target lies on its artboard; mask extent comes from the target,
    // not a page rectangle or only the narrow path carrying its stroke.
    auto b = make(records(layer("0 G 20 w\n250 30 m 300 30 L S\n" + vector_mask("", 0)), {}, false));
    ASSERT_TRUE(b);
    auto masks = elements(b->document->getDefs()->getRepr(), "svg:mask"); ASSERT_EQ(masks.size(), 1u);
    auto backgrounds = elements(masks[0], "svg:rect"); ASSERT_EQ(backgrounds.size(), 1u);
    auto number = [&](char const *key) { return g_ascii_strtod(backgrounds[0]->attribute(key), nullptr); };
    EXPECT_LT(number("x"), 250);
    EXPECT_GT(number("x") + number("width"), 300);
    EXPECT_LT(number("y"), 60);
    EXPECT_GT(number("y") + number("height"), 80);
}

TEST_F(AiNativeImportTest, TypeInAFontThatIsNotInstalledIsLeftOutOfTheComparison)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // The page sets "Hello" at 32 points with a font the page has; the records name one
    // nothing has, and say where the run was set: from the baseline at 10, 60 (40 from the
    // top), 30 points up and 8 down, 80 across.
    std::string const missing = "PROOF-Missing-Unique-Font-734";
    auto text = [&](std::string const &box) {
        return text_setup(with_glyph_run(text_document("(Hello\\r)", 6, missing, "/1 32 /53 << /0 << /0 0 /1 [1 0] >> >>"), box));
    };
    std::string const where = "8191.5 8161.5 8271.5 8199.5";
    std::string const page = "0 g BT /F1 32 Tf 10 60 Td (Hello) Tj ET";
    std::string reason;
    auto src = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text(where));
    auto good = Extension::Internal::open_ai_native(fixture(src, {page}, "/MediaBox [0 0 200 100]", font), reason);
    ASSERT_TRUE(good) << reason;
    EXPECT_EQ(good->missing_fonts, std::vector<std::string>{missing});
    ASSERT_FALSE(good->notes.empty());
    bool noted = false, share = false;
    for (auto const &note : good->notes) {
        noted = noted || note.find("1 font isn't installed (" + missing + ")") != std::string::npos;
        share = share || note.find("wasn't compared with the page") != std::string::npos;
    }
    EXPECT_TRUE(noted);
    // A fifth of the artboard isn't much of it.
    EXPECT_FALSE(share);
    auto texts = elements(good->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_EQ(attr(texts[0], "proof:font-missing"), missing);

    // Only the type is left out. Art beside it that the page doesn't have still refuses,
    auto extra = records(layer("0 g\n120 10 m 120 50 L 190 50 L 190 10 L 120 10 L\nf\n/AI11Text :\n0 /StoryIndex ,\n;\n"), text(where));
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(extra, {page}, "/MediaBox [0 0 200 100]", font), reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
    // and so does the page's type set somewhere the records don't say.
    auto moved = "0 g BT /F1 32 Tf 100 10 Td (Hello) Tj ET";
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(src, {moved}, "/MediaBox [0 0 200 100]", font), reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;

    // Type over most of an artboard: it opens, and says how much of it went unchecked.
    auto big = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text("8191.5 8141.5 8371.5 8211.5"));
    auto most = Extension::Internal::open_ai_native(fixture(big, {page}, "/MediaBox [0 0 200 100]", font), reason);
    ASSERT_TRUE(most) << reason;
    share = false;
    for (auto const &note : most->notes) share = share || note.find("wasn't compared with the page") != std::string::npos;
    EXPECT_TRUE(share);
}

TEST_F(AiNativeImportTest, ALinkedPictureThatChangedSinceTheSaveIsLeftOutOfTheComparison)
{
    // The file is blue now. The document keeps the picture it had when saved, red, and the
    // page shows that red picture in its box: the link changed since, as a photograph
    // updated in Photoshop after the .ai was last saved.
    auto const folder = temporary_folder();
    ASSERT_FALSE(folder.empty());
    auto const file = folder + G_DIR_SEPARATOR_S + "photo.png";
    write_picture(file); // 4 x 2, #336699
    std::string red, blue;
    for (int i = 0; i < 8; ++i) {
        red += argb(255, 255, 0, 0);
        blue += argb(255, 0x33, 0x66, 0x99);
    }
    std::string const old_page = "1 0 0 rg 10 60 40 20 re f";
    std::string reason;
    auto changed = records(layer(linked(file, {}, kept_preview(4, 2, red))));
    auto opened = Extension::Internal::open_ai_native(fixture(changed, {old_page}), reason);
    ASSERT_TRUE(opened) << reason;
    bool noted = false;
    for (auto const &note : opened->notes) {
        noted = noted || note.find("1 linked picture looks different from what the document kept of it when it was "
                                   "saved (photo.png)") != std::string::npos;
    }
    EXPECT_TRUE(noted);
    EXPECT_TRUE(opened->missing_links.empty());
    // It is in the document as it is now, linked to its file.
    auto const images = elements(opened->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(attr(images[0], "sodipodi:absref"), file);

    // A link that didn't change is weighed as before: the page's red against the file's blue.
    auto same = records(layer(linked(file, {}, kept_preview(4, 2, blue))));
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(same, {old_page}), reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
    // Only its box is left out: art beside it that the page lacks still refuses.
    auto extra = records(layer("0 g\n120 10 m 120 50 L 190 50 L 190 10 L 120 10 L\nf\n" +
                               linked(file, {}, kept_preview(4, 2, red))));
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(extra, {old_page}), reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
    // A kept picture of another shape than the file is a change too.
    std::string wide;
    for (int i = 0; i < 8; ++i) wide += argb(255, 0x33, 0x66, 0x99);
    auto reshaped = records(layer(linked(file, {}, kept_preview(8, 1, wide))));
    EXPECT_TRUE(Extension::Internal::open_ai_native(fixture(reshaped, {old_page}), reason)) << reason;
}

TEST_F(AiNativeImportTest, ALinkedPictureThatIsGoneIsLeftOutOfTheComparison)
{
    // The page has the picture, mid grey, in its box. The records link a file that is gone,
    // and keep a small red picture of it.
    auto const gone = std::string("Q:\\Jobs\\gone.png");
    std::string red;
    for (int i = 0; i < 8; ++i) red += argb(255, 255, 0, 0);
    auto const picture = linked(gone, {}, kept_preview(4, 2, red));
    std::string const page = "0.5 g 10 60 40 20 re f";
    std::string reason;
    auto src = records(layer(picture));
    auto good = Extension::Internal::open_ai_native(fixture(src, {page}), reason);
    ASSERT_TRUE(good) << reason;
    EXPECT_EQ(good->missing_links, std::vector<std::string>{"gone.png"});
    bool noted = false, share = false;
    for (auto const &note : good->notes) {
        noted = noted || note.find("1 linked picture can't be found (gone.png)") != std::string::npos;
        share = share || note.find("wasn't compared with the page") != std::string::npos;
    }
    EXPECT_TRUE(noted);
    // A twenty-fifth of the artboard isn't much of it.
    EXPECT_FALSE(share);
    ASSERT_EQ(good->differences.size(), 1u);
    EXPECT_LT(good->differences[0].lightness, 0.001);

    // Only its box is left out. Art beside it that the page doesn't have still refuses,
    auto extra = records(layer("0 g\n120 10 m 120 50 L 190 50 L 190 10 L 120 10 L\nf\n" + picture));
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(extra, {page}), reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
    // and so does a page that has its picture somewhere else.
    EXPECT_FALSE(Extension::Internal::open_ai_native(fixture(src, {"0.5 g 110 60 40 20 re f"}), reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;
}

namespace {

/// A CMYK profile of this machine other than PROOF's working one, with its bytes; nothing
/// when there is no such profile.
std::shared_ptr<Colors::CMS::Profile> another_cmyk_profile(std::string &bytes)
{
    auto const working = Colors::DocumentColors::workingProfile(Colors::Space::Type::CMYK);
    if (!working) return nullptr;
    for (auto const &profile : Colors::DocumentColors::profiles(Colors::Space::Type::CMYK)) {
        if (!profile || profile->getPath().empty() || profile->getName() == working->getName()) continue;
        gchar *data = nullptr;
        gsize size = 0;
        if (!g_file_get_contents(profile->getPath().c_str(), &data, &size, nullptr)) continue;
        bytes.assign(data, size);
        g_free(data);
        return profile;
    }
    return nullptr;
}

} // namespace

TEST_F(AiNativeImportTest, TheDocumentHasItsOwnColourProfileWhenItsFileCarriesOne)
{
    std::string bytes;
    auto const other = another_cmyk_profile(bytes);
    if (!other) GTEST_SKIP() << "this machine has no second CMYK profile";
    auto const working = Colors::DocumentColors::workingProfile(Colors::Space::Type::CMYK);
    std::string error;
    auto ai = AN::read(records(layer("0 0 0 1 k\n" + rect)), error);
    ASSERT_TRUE(ai) << error;

    // A file that carries none is in the working space of whoever opens it.
    auto plain = AN::build(*ai, error);
    ASSERT_TRUE(plain) << error;
    EXPECT_TRUE(plain->profile.empty());
    auto space = Colors::DocumentColors::assignedSpace(plain->document.get());
    ASSERT_TRUE(space);
    ASSERT_TRUE(space->getProfile());
    EXPECT_EQ(space->getProfile()->getName(), working->getName());
    auto paths = elements(plain->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    auto const working_black = attr(paths[0], "style");

    // One that carries its own is shown by it: the document has that profile, and its
    // black is what that profile makes of 100K.
    AN::Source source;
    source.profile = bytes;
    auto own = AN::build(*ai, error, source);
    ASSERT_TRUE(own) << error;
    EXPECT_EQ(own->profile, other->getName());
    space = Colors::DocumentColors::assignedSpace(own->document.get());
    ASSERT_TRUE(space);
    ASSERT_TRUE(space->getProfile());
    EXPECT_EQ(space->getProfile()->getName(), other->getName());
    paths = elements(own->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    auto const own_black = attr(paths[0], "style");
    EXPECT_NE(own_black.find("icc-color(" + space->getName()), std::string::npos) << own_black;
    EXPECT_NE(own_black, working_black);

    // What isn't a profile, or is one of another colour model, is passed over.
    source.profile = "not a profile";
    auto bad = AN::build(*ai, error, source);
    ASSERT_TRUE(bad) << error;
    EXPECT_TRUE(bad->profile.empty());
    ai = AN::read(records(layer("0 0 0 Xa\n" + rect), {}, false), error);
    ASSERT_TRUE(ai) << error;
    source.profile = bytes;
    auto rgb = AN::build(*ai, error, source);
    ASSERT_TRUE(rgb) << error;
    EXPECT_TRUE(rgb->profile.empty());

#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
    // From a file: the profile is the colour space of the page's process colours.
    std::string reason;
    auto src = records(layer("0 0 0 1 k\n" + rect));
    std::string const page = "/CS0 cs 0 0 0 1 sc 10 20 80 40 re f";
    auto opened = Extension::Internal::open_ai_native(
        fixture(src, {page}, "/MediaBox [0 0 200 100] /TrimBox [0 0 200 100]", {}, bytes), reason);
    ASSERT_TRUE(opened) << reason;
    EXPECT_EQ(opened->profile, other->getName());
    space = Colors::DocumentColors::assignedSpace(opened->document.get());
    ASSERT_TRUE(space);
    EXPECT_EQ(space->getProfile()->getName(), other->getName());
    // And one whose page names none.
    opened = Extension::Internal::open_ai_native(fixture(src, {"0 0 0 1 k 10 20 80 40 re f"}), reason);
    ASSERT_TRUE(opened) << reason;
    EXPECT_TRUE(opened->profile.empty());
#endif
}

TEST_F(AiNativeImportTest, MatchingPdfAcceptedAndStaleRecordsRefused)
{
    std::string reason;
    auto src = records(layer("0 g\n" + rect));
    auto path = fixture(src, {"0 g 10 20 80 40 re f"});
    auto good = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(good) << reason;
    ASSERT_EQ(good->differences.size(), 1u);
    EXPECT_LT(good->differences[0].lightness, 0.001);
    auto bad = fixture(src, {"0 g 110 20 80 40 re f"});
    EXPECT_FALSE(Extension::Internal::open_ai_native(bad, reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos);
    auto diagnostic = Extension::Internal::open_ai_native(bad, reason, Extension::Internal::AiKeepDiffering::All);
    ASSERT_TRUE(diagnostic);
    EXPECT_GT(diagnostic->differences[0].lightness, 0.1);
}

TEST_F(AiNativeImportTest, PaintedClippingPathSurvives)
{
    auto src = records(layer("q\n0.5 g\n10 20 m 90 20 L 90 60 L 10 60 L h W f\n"
                             "0 g\n20 30 m 30 30 L 30 40 L 20 40 L h f\nQ\n"), {}, false);
    auto b = make(src);
    ASSERT_TRUE(b);
    auto paths = elements(elements(b->document->getReprRoot(), "svg:g")[0], "svg:path");
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_NE(attr(paths[0], "style").find("fill:#808080"), std::string::npos);
    std::string reason;
    auto path = fixture(src, {"0.5 g 10 20 80 40 re f 0 g 20 30 10 10 re f"});
    EXPECT_TRUE(Extension::Internal::open_ai_native(path, reason)) << reason;
}

TEST_F(AiNativeImportTest, EveryArtboardMustMatch)
{
    std::string reason;
    auto src = records(layer("0 g\n" + rect + "230 20 m 230 60 L 310 60 L 310 20 L h f\n"), boards(2));
    auto matching = fixture(src, {"0 g 10 20 80 40 re f", "0 g 10 20 80 40 re f"});
    auto good = Extension::Internal::open_ai_native(matching, reason);
    ASSERT_TRUE(good) << reason;
    EXPECT_EQ(good->differences.size(), 2u);
    auto differing = fixture(src, {"0 g 10 20 80 40 re f", "0 g 110 20 80 40 re f"});
    EXPECT_FALSE(Extension::Internal::open_ai_native(differing, reason));
    EXPECT_NE(reason.find("artboard 2"), std::string::npos);
    auto missing = fixture(src, {"0 g 10 20 80 40 re f"});
    EXPECT_FALSE(Extension::Internal::open_ai_native(missing, reason));
}

TEST_F(AiNativeImportTest, ArtThatDrawsDifferentlyCanBeOfferedWithHowFar)
{
    using Extension::Internal::AiKeepDiffering;
    std::string reason;
    auto src = records(layer("0 g\n" + rect + "230 20 m 230 60 L 310 60 L 310 20 L h f\n"), boards(2));
    // Both artboards differ. To be offered, the art comes back once the first one does, with
    // how far it differs; the second isn't drawn.
    auto differing = fixture(src, {"0 g 110 20 80 40 re f", "0 g 110 20 80 40 re f"});
    EXPECT_FALSE(Extension::Internal::open_ai_native(differing, reason));
    auto offered = Extension::Internal::open_ai_native(differing, reason, AiKeepDiffering::First);
    ASSERT_TRUE(offered) << reason;
    EXPECT_NE(reason.find("draws differently"), std::string::npos);
    ASSERT_TRUE(offered->document);
    ASSERT_TRUE(offered->differing);
    EXPECT_EQ(offered->differing->artboard, 0u);
    EXPECT_EQ(offered->differing->artboards, 2u);
    EXPECT_GT(offered->differing->share, 0.1);
    EXPECT_EQ(offered->differences.size(), 1u);
    auto all = Extension::Internal::open_ai_native(differing, reason, AiKeepDiffering::All);
    ASSERT_TRUE(all) << reason;
    EXPECT_EQ(all->differences.size(), 2u);
    ASSERT_TRUE(all->differing);
    // Art that matches comes back as before, nothing differing.
    auto matching = fixture(src, {"0 g 10 20 80 40 re f", "0 g 10 20 80 40 re f"});
    auto good = Extension::Internal::open_ai_native(matching, reason, AiKeepDiffering::First);
    ASSERT_TRUE(good) << reason;
    EXPECT_TRUE(reason.empty());
    EXPECT_FALSE(good->differing);
    // A page that can't be weighed against is no offer: nothing says how far it differs.
    auto wrong_size = fixture(records(layer("0 g\n" + rect)), {"0 g 10 20 80 40 re f"}, "/MediaBox [0 0 300 100]");
    EXPECT_FALSE(Extension::Internal::open_ai_native(wrong_size, reason, AiKeepDiffering::First));
    EXPECT_NE(reason.find("size"), std::string::npos);
}

TEST_F(AiNativeImportTest, PopplersSettingsOutliveTheCheckAgainstThePage)
{
    // Poppler's glib side, which draws the page, makes Poppler's settings for its first document
    // and drops them when its last one closes. The page import that follows a refusal needs them:
    // without, PROOF crashed in the import dialog, reading a font.
    std::string reason;
    auto path = fixture(records(layer("0 g\n" + rect)), {"0 g 110 20 80 40 re f"});
    EXPECT_FALSE(Extension::Internal::open_ai_native(path, reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos);
    EXPECT_TRUE(globalParams);
    path = fixture(records(layer("0 g\n" + rect)), {"0 g 10 20 80 40 re f"});
    EXPECT_TRUE(Extension::Internal::open_ai_native(path, reason)) << reason;
    EXPECT_TRUE(globalParams);
}

TEST_F(AiNativeImportTest, BleedTrimAndNonPrintingLayers)
{
    std::string reason;
    auto src = records(layer("0 g\n" + rect) + layer("0 g\n110 20 m 190 20 L 190 60 L 110 60 L h f\n",
                                                   "1 1 1 0 0 0 0 0 79 128 255 0 50 0 Lb"));
    auto path = fixture(src, {"0 g 10 20 80 40 re f"},
                        "/MediaBox [-9 -9 209 109] /TrimBox [0 0 200 100] /BleedBox [-9 -9 209 109]");
    auto good = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(good) << reason;
    auto pages = good->document->getPageManager().getPages();
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(attr(pages[0]->getRepr(), "inkscape:bleed"), "9 9 9 9");
    auto groups = elements(good->document->getReprRoot(), "svg:g");
    EXPECT_EQ(attr(groups.back(), "style"), ""); // comparison must restore the visible layer
}

TEST_F(AiNativeImportTest, RotatedAndWrongSizePagesRefused)
{
    std::string reason;
    auto src = records(layer("0 g\n" + rect));
    auto path = fixture(src, {"0 g 10 20 80 40 re f"}, "/MediaBox [0 0 200 100] /Rotate 90");
    EXPECT_FALSE(Extension::Internal::open_ai_native(path, reason));
    EXPECT_NE(reason.find("turned"), std::string::npos);
    path = fixture(src, {"0 g 10 20 80 40 re f"}, "/MediaBox [0 0 300 100]");
    EXPECT_FALSE(Extension::Internal::open_ai_native(path, reason));
    EXPECT_NE(reason.find("size"), std::string::npos);
}

TEST_F(AiNativeImportTest, IndependentThresholdsCheckedOnEveryPage)
{
    // Page 1 has tolerable colour-only and lightness differences whose sum is
    // greater than page 2's. Page 2 still fails the independent lightness limit.
    auto src = records(layer("1 0 0 Xa\n10 10 m 20 10 L 20 30 L 10 30 L h f\n"
                             "0 g\n50 10 m 58 10 L 58 30 L 50 30 L h f\n"
                             "230 10 m 242 10 L 242 30 L 230 30 L h f\n"), boards(2), false);
    auto path = fixture(src, {"0 0.51 0 rg 10 10 10 20 re f", ""});
    std::string reason;
    auto d = Extension::Internal::open_ai_native(path, reason, Extension::Internal::AiKeepDiffering::All);
    ASSERT_TRUE(d) << reason;
    ASSERT_EQ(d->differences.size(), 2u);
    EXPECT_LT(d->differences[0].lightness, Extension::Internal::AI_NATIVE_MAX_LIGHTNESS);
    EXPECT_LT(d->differences[0].colour, Extension::Internal::AI_NATIVE_MAX_COLOUR);
    EXPECT_GT(d->differences[1].lightness, Extension::Internal::AI_NATIVE_MAX_LIGHTNESS);
    EXPECT_FALSE(Extension::Internal::open_ai_native(path, reason));
    EXPECT_NE(reason.find("artboard 2"), std::string::npos);
}

namespace {
std::string overprint_resources()
{
    auto state = [](bool op) {
        return std::string("<< /Type /ExtGState /BM /Normal /CA 1 /ca 1 /OPM 1 /SMask /None /op ") +
               (op ? "true /OP true" : "false /OP false") + " >>";
    };
    return " /ExtGState << /Base " + state(false) + " /Ink " + state(true) + " >>"
           " /Shading << /Spot << /ShadingType 2 /Coords [10 20 90 20] /Extend [true true]"
           " /ColorSpace [/Separation /TestInk /DeviceRGB << /FunctionType 2 /Domain [0 1]"
           " /C0 [1 1 1] /C1 [0.2 0.8 0.4] /N 1 >>]"
           " /Function << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [1] /N 1 >> >>"
           " /Process << /ShadingType 2 /Coords [10 20 90 20] /Extend [true true]"
           " /ColorSpace /DeviceRGB /Function << /FunctionType 2 /Domain [0 1]"
           " /C0 [0.5 0.5 0.5] /C1 [0.5 0.5 0.5] /N 1 >> >> >>";
}
std::string const overprint_boxes = "/MediaBox [0 0 200 100] /Group << /S /Transparency /CS /DeviceRGB /I false /K false >>";
std::string const overprint_pair = "/MultiChannelPaint << >> BDC q 10 20 80 40 re W n "
                                    "q /Base gs /Spot sh Q q /Ink gs /Process sh Q Q EMC";
}

TEST_F(AiNativeImportTest, RgbOverprintPageChecksItsCombinedColourWithoutChangingTheSource)
{
    auto const native = records(layer("0.1 0.4 0.2 Xa\n" + rect), {}, false);
    auto const path = fixture(native, {overprint_pair}, overprint_boxes, {}, {}, {}, overprint_resources());
    auto bytes = [](std::string const &path) {
        std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    auto const before = bytes(path);
    std::string reason;
    auto result = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(result) << reason;
    EXPECT_LT(result->differences[0].colour, 0.001);
    EXPECT_LT(result->differences[0].lightness, 0.001);
    EXPECT_EQ(bytes(path), before);
    // Wrong native art must still fail against the corrected, independent PDF.
    auto const wrong = fixture(records(layer("1 0 0 Xa\n" + rect), {}, false), {overprint_pair},
                               overprint_boxes, {}, {}, {}, overprint_resources());
    EXPECT_FALSE(Extension::Internal::open_ai_native(wrong, reason));
    EXPECT_NE(reason.find("draws differently"), std::string::npos) << reason;

    // Hairlines force the finer reference to be USED, not just drawn then discarded.
    // A thread of its own must use the preview bytes, not reopen the uncorrected file.
    std::string lines, page_lines;
    for (int y = 10; y <= 90; y += 4) {
        auto const at = std::to_string(y);
        lines += "10 " + at + " m 190 " + at + " L S\n";
        page_lines += " 10 " + at + " m 190 " + at + " l S";
    }
    auto const thin = fixture(records(layer("0.1 0.4 0.2 Xa\n" + rect + "0 G 0.12 w\n" + lines), {}, false),
                              {overprint_pair + " q /Base gs 0 G 0.12 w" + page_lines + " Q"},
                              overprint_boxes, {}, {}, {}, overprint_resources());
    auto alone = Extension::Internal::open_ai_native(thin, reason, Extension::Internal::AiKeepDiffering::All);
    ASSERT_TRUE(alone) << reason;
    g_setenv("PROOF_AI_FINER_HEAD_START", "0", true);
    auto cleanup = scope_exit([] { g_unsetenv("PROOF_AI_FINER_HEAD_START"); });
    auto beside = Extension::Internal::open_ai_native(thin, reason, Extension::Internal::AiKeepDiffering::All);
    ASSERT_TRUE(beside) << reason;
    EXPECT_FALSE(beside->differing);
    EXPECT_DOUBLE_EQ(alone->differences[0].colour, beside->differences[0].colour);
    EXPECT_DOUBLE_EQ(alone->differences[0].lightness, beside->differences[0].lightness);
}

TEST_F(AiNativeImportTest, RgbOverprintPreviewOnlyChangesExclusivelyMarkedPairs)
{
    auto preview = [&](std::string const &content, std::string const &boxes = overprint_boxes,
                       std::string const &resources = overprint_resources()) {
        auto const path = fixture({}, {content}, boxes, {}, {}, {}, resources);
        auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
        return Extension::Internal::rgb_overprint_preview(*pdf);
    };
    EXPECT_TRUE(preview(overprint_pair));
    EXPECT_FALSE(preview("q /Base gs /Spot sh /Ink gs /Process sh Q"));
    EXPECT_FALSE(preview(overprint_pair, "/MediaBox [0 0 200 100]"));
    EXPECT_FALSE(preview(overprint_pair + " q /Ink gs /Process sh Q")); // Shared with unmarked art.
    EXPECT_FALSE(preview(overprint_pair + " /Image Do")); // Inherited resources are outside the subset.
    EXPECT_FALSE(preview("/MultiChannelPaint BMC q /Base gs /Spot sh /Ink gs /Process sh /Process sh Q EMC"));
    EXPECT_FALSE(preview("/MultiChannelPaint BMC q /Base gs /Spot sh /Ink gs 0 g 10 20 80 40 re f Q EMC"));
    EXPECT_FALSE(preview("/MultiChannelPaint BMC q /Ink gs /Process sh /Base gs /Spot sh Q EMC"));
    auto resources = overprint_resources();
    auto at = resources.find("/OPM 1", resources.find("/Ink"));
    ASSERT_NE(at, resources.npos);
    resources.replace(at, 6, "/OPM 0");
    EXPECT_FALSE(preview(overprint_pair, overprint_boxes, resources));
    EXPECT_FALSE(preview(overprint_pair + " EMC")); // Unbalanced marked content cannot change a state.
}

TEST_F(AiNativeImportTest, RgbOverprintOnAPageWithAnImageKeepsItsPixelsAndRejectsInheritedForms)
{
    std::string const image = "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
                              "/BitsPerComponent 8 /ColorSpace /DeviceRGB /Length 3 >>\nstream\n" +
                              std::string("\x33\x66\xcc", 3) + "\nendstream";
    std::string const picture = " q /Base gs 8 0 0 8 120 20 cm /Image Do Q";
    auto const native = records(layer("0.1 0.4 0.2 Xa\n" + rect + "0.2 0.4 0.8 Xa\n" + "120 20 m 120 28 L 128 28 L 128 20 L h f\n"), {}, false);
    auto const path = fixture(native, {overprint_pair + picture}, overprint_boxes, {}, {}, {}, overprint_resources(), image);
    std::string reason;
    auto accepted = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(accepted) << reason;
    EXPECT_LT(accepted->differences[0].colour, 0.001);
    EXPECT_LT(accepted->differences[0].lightness, 0.001);
    auto preview = [&](std::string const &content, std::string const &object) {
        auto const file = fixture({}, {content}, overprint_boxes, {}, {}, {}, overprint_resources(), object);
        auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(file.c_str());
        return Extension::Internal::rgb_overprint_preview(*pdf);
    };
    EXPECT_TRUE(preview(picture + overprint_pair, image)); // Picture before the first pair too.
    EXPECT_FALSE(preview(overprint_pair + " q /Ink gs /Image Do Q", image)); // The changed state also paints pixels.
    EXPECT_FALSE(preview("/MultiChannelPaint BMC q /Base gs /Spot sh /Ink gs /Process sh /Image Do Q EMC", image));
    EXPECT_FALSE(preview(overprint_pair + picture,
        "<< /Type /XObject /Subtype /Form /BBox [0 0 1 1] /Length 11 >>\nstream\n/Ink gs 0 g\nendstream"));
}

TEST_F(AiNativeImportTest, RgbOverprintLeafFormsKeepTheirPaintAndLocalStates)
{
    auto form = [](std::string const &body, std::string const &resources = "") {
        return "<< /Type /XObject /Subtype /Form /BBox [0 0 1 1] " + resources +
               " /Length " + std::to_string(body.size()) + " >>\nstream\n" + body + "\nendstream";
    };
    std::string const body = "0.2 0.4 0.8 rg 0 0 1 1 re f";
    std::string const picture = " q /Base gs 1 0 0 rg 120 20 8 8 re f 8 0 0 8 120 20 cm /Image Do Q";
    std::string const native = records(layer("0.1 0.4 0.2 Xa\n" + rect +
        "0.2 0.4 0.8 Xa\n120 20 m 120 28 L 128 28 L 128 20 L h f\n"), {}, false);
    auto check = [&](std::string const &object) {
        auto const path = fixture(native, {overprint_pair + picture}, overprint_boxes,
                                  {}, {}, {}, overprint_resources(), object);
        std::string reason;
        auto opened = Extension::Internal::open_ai_native(path, reason);
        ASSERT_TRUE(opened) << reason;
        EXPECT_LT(opened->differences[0].colour, .001);
        EXPECT_LT(opened->differences[0].lightness, .001);
    };
    check(form(body)); // Inherited resources, but no state lookup.
    check(form("/Base gs " + body)); // An inherited state not changed by the preview.
    check(form("/Ink gs " + body, "/Resources << /ExtGState << /Ink "
        "<< /Type /ExtGState /BM /Normal /CA 1 /ca 1 /OPM 1 /SMask /None /op true /OP true >> >> >>"));
    // A local /Ink shadows the page's /Ink and must stay Normal over the red backdrop.
}

TEST_F(AiNativeImportTest, RgbOverprintLeafFormsRejectInheritedInkAndUninspectedDrawing)
{
    auto preview = [&](std::string const &body, std::string const &content = "") {
        std::string const object = "<< /Type /XObject /Subtype /Form /BBox [0 0 1 1] /Length " +
                                  std::to_string(body.size()) + " >>\nstream\n" + body + "\nendstream";
        auto const file = fixture({}, {content.empty() ? overprint_pair + " q /Base gs /Image Do Q" : content},
                                  overprint_boxes, {}, {}, {}, overprint_resources(), object);
        auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(file.c_str());
        return Extension::Internal::rgb_overprint_preview(*pdf);
    };
    EXPECT_FALSE(preview("/Ink gs 0 g 0 0 1 1 re f")); // Inherited candidate state is shared.
    EXPECT_FALSE(preview("/Image Do")); // A self-referencing nested Form never recurses.
    EXPECT_FALSE(preview("BI /W 1 /H 1 ID x EI"));
    EXPECT_FALSE(preview("/P scn 0 0 1 1 re f")); // A pattern could execute uninspected content.
    EXPECT_FALSE(preview("BT /Font 10 Tf (x) Tj ET")); // Including Type 3 glyphs.
    EXPECT_FALSE(preview("q 0 g")); // Unbalanced save.
    EXPECT_FALSE(preview("Q"));
    EXPECT_FALSE(preview("0 g 0 0 1 1 re f", overprint_pair + " q /Ink gs /Image Do Q"));
    EXPECT_FALSE(preview("0 g 0 0 1 1 re f",
        "/MultiChannelPaint BMC q /Base gs /Spot sh /Ink gs /Process sh /Image Do Q EMC"));
    std::string calls;
    for (unsigned i = 0; i < 65; ++i) calls += " q /Base gs /Image Do Q";
    EXPECT_FALSE(preview("0 g 0 0 1 1 re f", overprint_pair + calls));
}

TEST_F(AiNativeImportTest, DiagnosticCachesReuseAndRecoverFromDamage)
{
    auto const path = fixture(records(layer("0 g\n" + rect)), {"0 g 10 20 80 40 re f"});
    auto const folder = path.substr(0, path.find_last_of("/\\"));
    Extension::Internal::AiNativeCache cache;
    cache.records_directory = folder + "/records";
    cache.reference_directory = folder + "/references";
    std::string reason;
    auto first = Extension::Internal::open_ai_native(path, reason, Extension::Internal::AiKeepDiffering::No, {}, &cache);
    ASSERT_TRUE(first) << reason;
    EXPECT_EQ(cache.records_reused, 0u);
    EXPECT_EQ(cache.references_reused, 0u);
    auto second = Extension::Internal::open_ai_native(path, reason, Extension::Internal::AiKeepDiffering::No, {}, &cache);
    ASSERT_TRUE(second) << reason;
    EXPECT_EQ(cache.records_reused, 1u);
    EXPECT_EQ(cache.references_reused, 1u);
    ASSERT_EQ(first->differences.size(), second->differences.size());
    EXPECT_DOUBLE_EQ(first->differences[0].lightness, second->differences[0].lightness);
    EXPECT_DOUBLE_EQ(first->differences[0].colour, second->differences[0].colour);

    // A damaged reference is redrawn; a damaged record cache is decoded again.
    auto const png = cache.reference_directory + "/page-1.png";
    ASSERT_TRUE(g_file_set_contents(png.c_str(), "damaged", 7, nullptr));
    ASSERT_TRUE(Extension::Internal::open_ai_native(path, reason, Extension::Internal::AiKeepDiffering::No, {}, &cache)) << reason;
    EXPECT_EQ(cache.records_reused, 1u);
    EXPECT_EQ(cache.references_reused, 0u);
    auto const text = cache.records_directory + "/records.txt";
    ASSERT_TRUE(g_file_set_contents(text.c_str(), "damaged", 7, nullptr));
    ASSERT_TRUE(Extension::Internal::open_ai_native(path, reason, Extension::Internal::AiKeepDiffering::No, {}, &cache)) << reason;
    EXPECT_EQ(cache.records_reused, 0u);
    EXPECT_EQ(cache.references_reused, 1u);

    // Default imports still decode and render independently of all test caches.
    EXPECT_TRUE(Extension::Internal::open_ai_native(path, reason)) << reason;
}

TEST_F(AiNativeImportTest, PagesSlowToDrawAreKeptForTheNextOpening)
{
    auto const path = fixture(records(layer("0 g\n" + rect)), {"0 g 10 20 80 40 re f"});
    auto const folder = path.substr(0, path.find_last_of("/\\"));
    auto const store = folder + "/page-store";
    // Every page counts as slow here, and the store is this test's own.
    struct Environment
    {
        Environment(std::string const &store)
        {
            g_setenv("PROOF_AI_SLOW_PAGE_SECONDS", "0", true);
            g_setenv("PROOF_AI_PAGE_STORE", store.c_str(), true);
        }
        ~Environment()
        {
            g_unsetenv("PROOF_AI_SLOW_PAGE_SECONDS");
            g_unsetenv("PROOF_AI_PAGE_STORE");
        }
    } environment(store);
    auto const folders = [&] {
        std::vector<std::string> found;
        if (GDir *dir = g_dir_open(store.c_str(), 0, nullptr)) {
            while (auto const *name = g_dir_read_name(dir)) found.push_back(store + "/" + name);
            g_dir_close(dir);
        }
        return found;
    };

    std::string reason;
    auto first = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(first) << reason;
    EXPECT_EQ(first->pages_reused, 0u);
    // One folder for the file: its content's checksum, the page and how long it took.
    auto const kept = folders();
    ASSERT_EQ(kept.size(), 1u);
    for (auto const *name : {"/source", "/page-1.png", "/page-1.seconds"}) {
        EXPECT_TRUE(g_file_test((kept[0] + name).c_str(), G_FILE_TEST_EXISTS)) << name;
    }
    auto second = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(second) << reason;
    EXPECT_EQ(second->pages_reused, 1u);
    ASSERT_EQ(first->differences.size(), second->differences.size());
    EXPECT_DOUBLE_EQ(first->differences[0].lightness, second->differences[0].lightness);

    // A folder whose content checksum isn't this file's is drawn again, and kept anew.
    auto const source = kept[0] + "/source";
    ASSERT_TRUE(g_file_set_contents(source.c_str(), std::string(64, '0').c_str(), 64, nullptr));
    auto third = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(third) << reason;
    EXPECT_EQ(third->pages_reused, 0u);
    auto fourth = Extension::Internal::open_ai_native(path, reason);
    ASSERT_TRUE(fourth) << reason;
    EXPECT_EQ(fourth->pages_reused, 1u);

    // A page that draws quickly isn't kept by an ordinary opening.
    g_setenv("PROOF_AI_SLOW_PAGE_SECONDS", "1000", true);
    auto const quick = fixture(records(layer("0 g\n" + rect)), {"0 g 10 20 80 40 re f % other bytes"});
    EXPECT_TRUE(Extension::Internal::open_ai_native(quick, reason)) << reason;
    EXPECT_EQ(folders().size(), 1u);
}

TEST_F(AiNativeImportTest, AFinerDrawingBesideTheFirstGivesTheSameResult)
{
    // Hairlines: Poppler draws them a pixel wide at the first size, so the finer weighing runs.
    std::string lines, page;
    for (int y = 10; y <= 90; y += 4) {
        auto const at = std::to_string(y);
        lines += "10 " + at + " m 190 " + at + " L S\n";
        page += " 10 " + at + " m 190 " + at + " l S";
    }
    auto const path = fixture(records(layer("0 G 0.12 w\n" + lines)), {"0 G 0.12 w" + page});
    auto open = [&](char const *head_start) {
        if (head_start) g_setenv("PROOF_AI_FINER_HEAD_START", head_start, true);
        std::string reason;
        auto result = Extension::Internal::open_ai_native(path, reason, Extension::Internal::AiKeepDiffering::All);
        g_unsetenv("PROOF_AI_FINER_HEAD_START");
        return result;
    };
    auto const alone = open(nullptr); // Quick: the finer drawing comes after the first, if at all.
    auto const beside = open("0");   // Started at once, on a thread of its own.
    ASSERT_TRUE(alone);
    ASSERT_TRUE(beside);
    ASSERT_EQ(alone->differences.size(), 1u);
    ASSERT_EQ(beside->differences.size(), 1u);
    EXPECT_DOUBLE_EQ(alone->differences[0].lightness, beside->differences[0].lightness);
    EXPECT_DOUBLE_EQ(alone->differences[0].colour, beside->differences[0].colour);
    // The finer drawing taken from beside is the one used: kept, when every page counts as slow.
    auto const store = path.substr(0, path.find_last_of("/\\")) + "/page-store";
    g_setenv("PROOF_AI_SLOW_PAGE_SECONDS", "0", true);
    g_setenv("PROOF_AI_PAGE_STORE", store.c_str(), true);
    auto const kept = open("0");
    g_unsetenv("PROOF_AI_SLOW_PAGE_SECONDS");
    g_unsetenv("PROOF_AI_PAGE_STORE");
    ASSERT_TRUE(kept);
    bool finer = false;
    if (GDir *dir = g_dir_open(store.c_str(), 0, nullptr)) {
        while (auto const *name = g_dir_read_name(dir)) {
            finer = finer || g_file_test((store + "/" + name + "/page-1-finer.png").c_str(), G_FILE_TEST_EXISTS);
        }
        g_dir_close(dir);
    }
    EXPECT_TRUE(finer);
    // A page that needs no finer drawing waits for the one begun beside it, and is the same too.
    auto const plain = fixture(records(layer("0 g\n" + rect)), {"0 g 10 20 80 40 re f"});
    g_setenv("PROOF_AI_FINER_HEAD_START", "0", true);
    std::string reason;
    auto const quick = Extension::Internal::open_ai_native(plain, reason);
    g_unsetenv("PROOF_AI_FINER_HEAD_START");
    EXPECT_TRUE(quick) << reason;
}

TEST_F(AiNativeImportTest, Corpus)
{
    auto const *roots = g_getenv("PROOF_AI_CORPUS");
    auto const *file = g_getenv("PROOF_AI_FILE");
    if (!roots && !file) GTEST_SKIP() << "set PROOF_AI_CORPUS to approved folders, or PROOF_AI_FILE to an approved file";
    // CMYK paint in an RGB document shows as the application shows it (inkscape-application.cpp),
    // through the working CMYK profile; the formula again afterwards, for the other tests.
    Colors::DocumentColors::showDeviceCMYK();
    struct Formula
    {
        ~Formula()
        {
            // The library's space, not this program's copy of the inline Manager::get().
            auto const space = Colors::Color(Type::CMYK, {0, 0, 0, 0}).getSpace();
            if (auto device = std::dynamic_pointer_cast<Colors::Space::DeviceCMYK>(space)) {
                device->setProfile({}, Colors::RenderingIntent::UNKNOWN);
            }
        }
    } formula;
    auto from_utf8 = [](std::string const &s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
    auto utf8 = [](std::filesystem::path const &p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); };
    std::vector<std::filesystem::path> files;
    if (file) files.push_back(from_utf8(file));
    if (roots) {
        gchar **parts = g_strsplit(roots, ";", -1);
        for (gchar **p = parts; *p; ++p) {
            if (!**p) continue;
            for (auto const &e : std::filesystem::recursive_directory_iterator(from_utf8(*p))) {
                if (e.is_regular_file() && g_ascii_strcasecmp(utf8(e.path().extension()).c_str(), ".ai") == 0 &&
                    utf8(e.path().filename()).rfind("._", 0) != 0) files.push_back(e.path());
            }
        }
        g_strfreev(parts);
    }
    ASSERT_FALSE(files.empty());
    for (auto const &path : files) {
        std::string reason;
        auto const start = std::chrono::steady_clock::now();
        auto const *pictures = g_getenv("PROOF_AI_PICTURES");
        Extension::Internal::AiNativeCache cache;
        if (auto const *dir = g_getenv("PROOF_AI_RECORDS_CACHE")) cache.records_directory = dir;
        if (auto const *dir = g_getenv("PROOF_AI_REFERENCE_CACHE")) cache.reference_directory = dir;
        auto result = Extension::Internal::open_ai_native(utf8(path), reason, Extension::Internal::AiKeepDiffering::All,
                                                          pictures ? pictures : "", &cache);
        if (result) {
            if (auto const *dump = g_getenv("PROOF_AI_DUMP")) {
                sp_repr_save_file(result->document->getReprDoc(), dump, SP_SVG_NS_URI);
            }
        }
        double light = 0, colour = 0;
        if (result) for (auto d : result->differences) {
            light = std::max(light, d.lightness);
            colour = std::max(colour, d.colour);
        }
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        bool const accepted = result && light <= Extension::Internal::AI_NATIVE_MAX_LIGHTNESS &&
                                      colour <= Extension::Internal::AI_NATIVE_MAX_COLOUR;
        std::printf("AI_RESULT\t%s\t%.6f\t%.6f\t%lld\t%s\t%s\n", accepted ? "NATIVE" : "PAGE", light, colour,
                    static_cast<long long>(ms), utf8(path).c_str(), reason.c_str());
        std::printf("AI_CACHE\t%u\t%u\n", cache.records_reused, cache.references_reused);
        if (result) for (auto const &note : result->notes) std::printf("AI_NOTE\t%s\n", note.c_str());
        if (result) for (auto const &[part, seconds] : result->timings) std::printf("AI_TIME\t%s\t%.3f\n", part.c_str(), seconds);
        // What both writers would make of the document as read (tools/ai-writer-census.py): what each
        // refuses, or the size of its records and whether the reader takes them back. Nothing is saved.
        if (result && g_getenv("PROOF_AI_WRITE")) {
            // Optional in-memory inventory: SVG spelling alone cannot distinguish RGB
            // stops from the same numbers assigned to the document's ICC RGB profile.
            // Only the opted-in corpus diagnostic prints these; reports remain local.
            if (g_getenv("PROOF_AI_GRADIENT_INVENTORY")) {
                auto const assigned = Colors::DocumentColors::assignedSpace(result->document.get());
                auto const document_profile = assigned ? assigned->getProfile() : nullptr;
                std::set<SPGradient const *> seen;
                auto inventory = [&](auto &&self, SPObject const *parent) -> void {
                    for (auto const &object : parent->children) {
                        if (!is<SPItem>(&object)) continue; // Only scene artwork, not patterns/masks in defs.
                        if (auto shape = cast<SPShape>(&object)) {
                            if (!shape->curve() || shape->curve()->empty()) continue;
                            for (bool const stroke : {false, true}) {
                                if (stroke && shape->style->stroke_width.computed <= 0) continue;
                                auto const &paint = stroke ? static_cast<SPIPaint const &>(shape->style->stroke)
                                                           : shape->style->fill;
                                auto const *gradient = cast<SPGradient>(paint.isPaintserver() && paint.href
                                                                           ? paint.href->getObject() : nullptr);
                                if (!gradient || gradient->isSolid() || is<SPMeshGradient>(gradient) ||
                                    !seen.insert(gradient).second) continue;
                                auto const *vector = gradient->getGradientVector();
                                for (std::size_t i = 0; i < vector->stops.size(); ++i) {
                                    auto const &color = vector->stops[i].color;
                                    auto const space = color ? color->getSpace() : nullptr;
                                    auto const profile = space ? space->getProfile() : nullptr;
                                    std::printf("AI_GRADIENT\t%zu\t%s\t%zu\t%d\t%d\t%s\t%d\t%d\t%s\n",
                                                seen.size(), stroke ? "stroke" : "fill", i,
                                                space ? static_cast<int>(space->getType()) : -1,
                                                space ? static_cast<int>(space->getComponentType()) : -1,
                                                profile ? profile->getChecksum().c_str() : "",
                                                space && space->hasValidCmsProfile(),
                                                profile && document_profile && *profile == *document_profile,
                                                color ? color->toString().c_str() : "");
                                }
                            }
                        }
                        self(self, &object);
                    }
                };
                inventory(inventory, result->document->getRoot());
            }
            for (auto const version : {AN::RecordsVersion::Illustrator7, AN::RecordsVersion::Modern}) {
                auto const written = AN::write_records(result->document.get(), version);
                std::string refused, error;
                for (auto const &why : written.refused) refused += (refused.empty() ? "" : "|") + why;
                bool const reads = written.refused.empty() && AN::read(written.records, error).has_value();
                std::printf("AI_WRITE\t%s\t%zu\t%s\t%s\n", version == AN::RecordsVersion::Modern ? "modern" : "legacy7",
                            written.records.size(), written.refused.empty() ? (reads ? "reads" : "unread") : "refused",
                            written.refused.empty() ? error.c_str() : refused.c_str());
#if defined(WITH_CAPYPDF)
                // Opt-in qualification: exercise the complete PDF/native save and the ordinary
                // native reopen gate, for every page. Reuse the already imported source above.
                if (auto const *saved = g_getenv("PROOF_AI_SAVE"); saved && accepted && reads && version == AN::RecordsVersion::Modern) {
                    try {
                        if (!Extension::db.get("org.inkscape.output.pdf.capypdf")) Extension::Internal::PdfOutput::init();
                        if (!Extension::db.get("org.inkscape.output.ai.native")) Extension::Internal::AiNativeOutput::init();
                        auto *output = dynamic_cast<Extension::Output *>(Extension::db.get("org.inkscape.output.ai.native"));
                        if (!output) throw std::runtime_error("native output unavailable");
                        output->set_gui(false);
                        output->save(result->document.get(), saved);
                        std::string why;
                        auto reopened = Extension::Internal::open_ai_native(saved, why, Extension::Internal::AiKeepDiffering::All);
                        double l = 0, c = 0;
                        if (reopened) for (auto const &d : reopened->differences) {
                            l = std::max(l, d.lightness); c = std::max(c, d.colour);
                        }
                        bool const native = reopened && l <= Extension::Internal::AI_NATIVE_MAX_LIGHTNESS &&
                                                         c <= Extension::Internal::AI_NATIVE_MAX_COLOUR;
                        bool const resources = reopened && reopened->missing_links.size() <= result->missing_links.size() &&
                            std::all_of(reopened->missing_fonts.begin(), reopened->missing_fonts.end(), [&](auto const &name) {
                                return std::find(result->missing_fonts.begin(), result->missing_fonts.end(), name) != result->missing_fonts.end();
                            });
                        if (reopened && !resources) why = "the saved document introduces missing fonts or links";
                        auto const pages = result->document->getPageManager().getPageCount();
                        auto const returned_pages = reopened ? reopened->document->getPageManager().getPageCount() : 0;
                        std::printf("AI_SAVE\t%s\t%u\t%u\t%.6f\t%.6f\t%s\n",
                                    native && resources && pages == returned_pages ? "NATIVE" : "PAGE", pages, returned_pages, l, c, why.c_str());
                    } catch (std::exception const &e) {
                        std::printf("AI_SAVE\tERROR\t0\t0\t0\t0\t%s\n", e.what());
                    }
                }
#endif
            }
        }
        std::fflush(stdout);
    }
}
#endif

// ---- the writer: PROOF's records, read back ----------------------------------------------

namespace {
std::unique_ptr<SPDocument> svg_document(std::string const &body,
                                         std::string const &size = R"(width="200" height="100" viewBox="0 0 200 100")")
{
    std::string const svg = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" )"
                            R"(xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" xmlns:proof="urn:x-proof:document:1" )" +
                            size + ">" + body + "</svg>";
    return SPDocument::createNewDocFromMem(svg);
}

Geom::OptRect shape_box(SPDocument *doc, XML::Node *repr)
{
    doc->ensureUpToDate();
    auto *item = dynamic_cast<SPItem *>(doc->getObjectByRepr(repr));
    return item ? item->documentGeometricBounds() : Geom::OptRect();
}

/// Compare every anchor and handle, including the seam. Use the writer's previous
/// transform-then-normalize order so an arc's existing approximation is unchanged.
void expect_writer_geometry(SPDocument *source, XML::Node *original, SPDocument *imported, XML::Node *result)
{
    source->ensureUpToDate();
    imported->ensureUpToDate();
    auto const *a = dynamic_cast<SPShape *>(source->getObjectByRepr(original));
    auto const *b = dynamic_cast<SPShape *>(imported->getObjectByRepr(result));
    ASSERT_TRUE(a && b && a->curve() && b->curve());
    Geom::Affine const to_art = Geom::Scale(0.75, -0.75);
    auto const was = pathv_to_linear_and_cubic_beziers(*a->curve() * a->i2doc_affine() * to_art) * to_art.inverse();
    auto const now = pathv_to_linear_and_cubic_beziers(*b->curve() * b->i2doc_affine());
    ASSERT_EQ(was.size(), now.size());
    for (std::size_t p = 0; p < was.size(); ++p) {
        SCOPED_TRACE(p);
        auto count = was[p].size();
        // A primitive's microscopic closing line already rounded to zero in the
        // previous writer, and is removed by the reader's ordinary close().
        if (count == now[p].size() + 1 && was[p].back().isLineSegment() &&
            Geom::distance(was[p].back().initialPoint(), was[p].back().finalPoint()) < 3e-4) --count;
        ASSERT_EQ(count, now[p].size());
        EXPECT_EQ(was[p].closed(), now[p].closed());
        for (std::size_t s = 0; s < count; ++s) {
            SCOPED_TRACE(s);
            auto const *ac = dynamic_cast<Geom::CubicBezier const *>(&was[p][s]);
            auto const *bc = dynamic_cast<Geom::CubicBezier const *>(&now[p][s]);
            ASSERT_EQ(ac != nullptr, bc != nullptr);
            auto point = [](Geom::Point const &x, Geom::Point const &y) {
                EXPECT_NEAR(x.x(), y.x(), 3e-4);
                EXPECT_NEAR(x.y(), y.y(), 3e-4);
            };
            if (ac && bc) {
                for (unsigned k = 0; k < 4; ++k) point((*ac)[k], (*bc)[k]);
            } else {
                point(was[p][s].initialPoint(), now[p][s].initialPoint());
                point(was[p][s].finalPoint(), now[p][s].finalPoint());
            }
        }
    }
}
} // namespace

TEST_F(AiNativeImportTest, PathOperatorCasesKeepDestinationNodeTypes)
{
    // Independently authored records exercise every destination flag, not just the
    // C/c and L/l forms the writer emits. No flag changes the geometry it reads.
    std::string const art =
        "10 10 m 20 10 l 30 10 L 35 10 35 20 40 20 c 45 20 45 10 50 10 C "
        "55 10 60 20 v 65 20 70 10 V 75 10 80 20 y 85 20 90 10 Y S\n"
        "10 30 m 10 10 50 10 50 30 C 50 50 10 50 10 30 c f\n"
        "70 30 m 70 10 110 10 110 30 c f\n"
        "120 10 m 130 10 L S\n";
    std::string error;
    auto read = AN::read(records(layer(art)), error);
    ASSERT_TRUE(read) << error;
    auto const &nodes = read->layers[0].children;
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_EQ(nodes[0].nodetypes, "cscscscsc");
    EXPECT_EQ(nodes[1].nodetypes, "scs"); // explicit cubic destination sets m's seam type
    EXPECT_EQ(nodes[2].nodetypes, "csc"); // an implicit closing line keeps m's corner
    EXPECT_EQ(nodes[3].nodetypes, "cc");  // no types leak into the next paint
    auto const &v = dynamic_cast<Geom::CubicBezier const &>(nodes[0].path[0][4]);
    EXPECT_EQ(v[1], Geom::Point(50, 10));
    EXPECT_EQ(v[2], Geom::Point(55, 10));
    auto const &y = dynamic_cast<Geom::CubicBezier const &>(nodes[0].path[0][6]);
    EXPECT_EQ(y[1], Geom::Point(75, 10));
    EXPECT_EQ(y[2], Geom::Point(80, 20));
    auto b = make(records(layer(art)));
    ASSERT_TRUE(b);
    auto paths = elements(b->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 4u);
    for (std::size_t i = 0; i < paths.size(); ++i) EXPECT_EQ(attr(paths[i], "sodipodi:nodetypes"), nodes[i].nodetypes);
}

TEST_F(AiNativeImportTest, WrittenRecordsPreserveAuthoredSmoothAndCornerPoints)
{
    auto doc = svg_document(
        R"(<path d="M10 30 C15 10 20 10 30 20 C40 30 40 40 50 40 C60 40 70 10 80 10" sodipodi:nodetypes="cscs"/>)"
        R"(<path d="M10 60 C20 50 30 50 40 60 C50 70 60 70 70 60 C80 50 90 50 100 60" sodipodi:nodetypes="czac"/>)");
    ASSERT_TRUE(doc);
    auto const originals = elements(doc->getReprRoot(), "svg:path");
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        SCOPED_TRACE(static_cast<int>(version));
        auto const written = AN::write_records(doc.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        std::string error;
        auto read = AN::read(written.records, error);
        ASSERT_TRUE(read) << error;
        auto const &nodes = read->layers[0].children;
        ASSERT_EQ(nodes.size(), 2u);
        EXPECT_EQ(nodes[0].nodetypes, "cscs"); // the aligned middle handles still belong to a corner
        EXPECT_EQ(nodes[1].nodetypes, "cssc"); // AI has one smooth type for s, z and a
        auto b = make(written.records);
        ASSERT_TRUE(b);
        auto const paths = elements(b->document->getReprRoot(), "svg:path");
        ASSERT_EQ(paths.size(), originals.size());
        for (std::size_t i = 0; i < paths.size(); ++i) {
            EXPECT_EQ(attr(paths[i], "sodipodi:nodetypes"), nodes[i].nodetypes);
            expect_writer_geometry(doc.get(), originals[i], b->document.get(), paths[i]);
        }
        // The builder's types survive SVG serialization/reopen, and another .ai save.
        auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
        ASSERT_TRUE(reopened);
        auto const again = AN::write_records(reopened.get(), version);
        ASSERT_TRUE(again.refused.empty()) << again.refused.front();
        auto reread = AN::read(again.records, error);
        ASSERT_TRUE(reread) << error;
        ASSERT_EQ(reread->layers[0].children.size(), 2u);
        EXPECT_EQ(reread->layers[0].children[0].nodetypes, "cscs");
        EXPECT_EQ(reread->layers[0].children[1].nodetypes, "cssc");
    }
}

TEST_F(AiNativeImportTest, WrittenRecordsPreserveClosedSeamsAndTransformedCompounds)
{
    auto doc = svg_document(
        R"x(<g transform="matrix(1.5 .25 -.5 .75 8 4)"><path d=")x"
        R"(M10 30 C10 10 50 10 50 30 C50 50 10 50 10 30 Z )"
        R"(M70 30 C70 10 110 10 110 30 C110 50 70 50 70 30 Z )"
        R"(M120 30 C110 30 160 50 160 30 Z" sodipodi:nodetypes="ccssscccs"/></g>)");
    ASSERT_TRUE(doc);
    auto const original = elements(doc->getReprRoot(), "svg:path")[0];
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        SCOPED_TRACE(static_cast<int>(version));
        auto const written = AN::write_records(doc.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto b = make(written.records);
        ASSERT_TRUE(b);
        auto const paths = elements(b->document->getReprRoot(), "svg:path");
        ASSERT_EQ(paths.size(), 1u);
        // The duplicate at each subpath's end wins over its conflicting first type.
        // The third seam is carried by an explicit closing line, not a closing cubic.
        EXPECT_EQ(attr(paths[0], "sodipodi:nodetypes"), "scscscscs");
        expect_writer_geometry(doc.get(), original, b->document.get(), paths[0]);
    }
}

TEST_F(AiNativeImportTest, WrittenRecordsInferSmoothPointsOnlyWhenTypesAreUnspecified)
{
    auto doc = svg_document(
        R"(<path d="M10 30 C10 10 40 10 40 30 C40 50 70 50 70 30"/>)"
        R"(<path d="M10 30 C10 10 40 10 40 30 C40 50 70 50 70 30" sodipodi:nodetypes="ccc"/>)"
        R"(<path d="M10 30 C10 10 40 10 40 30 C40 50 70 50 70 30" sodipodi:nodetypes="cbc"/>)"
        R"(<path d="M10 30 C10 10 40 10 40 30 C40 50 70 50 70 30" sodipodi:nodetypes="c"/>)");
    ASSERT_TRUE(doc);
    auto const originals = elements(doc->getReprRoot(), "svg:path");
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        auto const written = AN::write_records(doc.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto b = make(written.records);
        ASSERT_TRUE(b);
        auto const paths = elements(b->document->getReprRoot(), "svg:path");
        ASSERT_EQ(paths.size(), 4u);
        EXPECT_EQ(attr(paths[0], "sodipodi:nodetypes"), "csc");
        EXPECT_EQ(attr(paths[1], "sodipodi:nodetypes"), "ccc");
        EXPECT_EQ(attr(paths[2], "sodipodi:nodetypes"), "csc");
        EXPECT_EQ(attr(paths[3], "sodipodi:nodetypes"), "csc");
        for (std::size_t i = 0; i < paths.size(); ++i) expect_writer_geometry(doc.get(), originals[i], b->document.get(), paths[i]);
    }
}

TEST_F(AiNativeImportTest, WrittenRecordsKeepPrimitiveSmoothJoinsWithoutMovingTheirHandles)
{
    auto doc = svg_document(
        R"(<ellipse cx="40" cy="40" rx="25" ry="15"/>)"
        R"(<rect x="80" y="10" width="40" height="60" rx="10" ry="15"/>)"
        R"(<rect x="140" y="10" width="40" height="60"/>)");
    ASSERT_TRUE(doc);
    std::vector<XML::Node *> originals{elements(doc->getReprRoot(), "svg:ellipse")[0],
                                      elements(doc->getReprRoot(), "svg:rect")[0],
                                      elements(doc->getReprRoot(), "svg:rect")[1]};
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        auto const written = AN::write_records(doc.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto b = make(written.records);
        ASSERT_TRUE(b);
        auto const paths = elements(b->document->getReprRoot(), "svg:path");
        ASSERT_EQ(paths.size(), originals.size());
        for (std::size_t i = 0; i < paths.size(); ++i) {
            auto const types = attr(paths[i], "sodipodi:nodetypes");
            ASSERT_FALSE(types.empty());
            EXPECT_EQ(types, std::string(types.size(), i == 2 ? 'c' : 's')) << i;
            expect_writer_geometry(doc.get(), originals[i], b->document.get(), paths[i]);
        }
    }
}

TEST_F(AiNativeImportTest, WrittenRecordsMapArcEndpointTypesAfterSubdivisionChanges)
{
    auto doc = svg_document(
        R"(<path id="arc" d="M10 30 A20 20 0 0 1 50 30 L70 30 M10 60 C10 50 30 50 30 60 C30 70 50 70 50 60"/>)");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto *shape = dynamic_cast<SPShape *>(doc->getObjectById("arc"));
    ASSERT_TRUE(shape && shape->curve());
    auto const local = pathv_to_linear_and_cubic_beziers(*shape->curve());
    ASSERT_EQ(local.size(), 2u);
    // The SBasis approximation subdivides only when its absolute tail error
    // exceeds 0.1. A moderate enlargement can still use the same two cubics;
    // choose a bounded enlargement that actually crosses that threshold.
    Geom::PathVector art;
    for (int scale : {64, 256, 1024, 4096}) {
        shape->setAttribute("transform", "scale(" + std::to_string(scale) + ")");
        doc->ensureUpToDate();
        art = pathv_to_linear_and_cubic_beziers(*shape->curve() * shape->i2doc_affine() * Geom::Scale(0.75, -0.75));
        if (art.size() == local.size() && art[0].size() > local[0].size()) break;
    }
    ASSERT_EQ(art.size(), 2u);
    ASSERT_GT(art[0].size(), local[0].size());
    std::string types(local[0].size() + 1, 's');
    types.front() = 'c';
    types[types.size() - 2] = 'c'; // authored corner at the arc/line boundary
    types += "csc";
    shape->setAttribute("sodipodi:nodetypes", types);
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        auto const written = AN::write_records(doc.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto b = make(written.records);
        ASSERT_TRUE(b);
        auto const paths = elements(b->document->getReprRoot(), "svg:path");
        ASSERT_EQ(paths.size(), 1u);
        auto const result = attr(paths[0], "sodipodi:nodetypes");
        std::string expected(art[0].size() + 1, 's');
        expected.front() = 'c';
        expected[expected.size() - 2] = 'c';
        expected += "csc";
        EXPECT_EQ(result, expected);
        expect_writer_geometry(doc.get(), shape->getRepr(), b->document.get(), paths[0]);
    }
}

TEST_F(AiNativeImportTest, WrittenRecordsReadBackAsTheDocument)
{
    auto doc = svg_document(
        R"(<g inkscape:groupmode="layer" inkscape:label="Art" inkscape:highlight-color="#ff4f4f">)"
        R"(<rect x="10" y="10" width="40" height="30" style="fill:device-cmyk(0.1 0.2 0.3 0.4);stroke:#000000;stroke-width:2"/>)"
        R"(<path d="M 60 10 H 100 V 50 H 60 Z M 70 20 V 40 H 90 V 20 Z" style="fill:#ff0000;fill-rule:evenodd"/>)"
        R"(<g><rect x="110" y="10" width="10" height="10" style="fill:#00ff00"/>)"
        R"(<rect x="125" y="10" width="10" height="10" style="fill:#0000ff" proof:overprint="fill"/></g>)"
        R"(</g>)"
        R"(<g inkscape:groupmode="layer" inkscape:label="Hidden" style="display:none" sodipodi:insensitive="true">)"
        R"(<rect x="10" y="60" width="20" height="20" style="fill:#000000"/></g>)");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_NE(written.records.find("%AI3_Cropmarks: 0 -75 150 0"), std::string::npos) << written.records.substr(0, 1200);
    auto b = make(written.records);
    ASSERT_TRUE(b);
    auto *imported = b->document.get();
    auto *root = imported->getReprRoot();

    std::vector<XML::Node *> layers;
    for (auto *g : elements(root, "svg:g")) {
        if (attr(g, "inkscape:groupmode") == "layer") layers.push_back(g);
    }
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(attr(layers[0], "inkscape:label"), "Art");
    EXPECT_EQ(attr(layers[0], "inkscape:highlight-color"), "#ff4f4f");
    EXPECT_EQ(attr(layers[1], "inkscape:label"), "Hidden");
    EXPECT_EQ(attr(layers[1], "style"), "display:none");
    EXPECT_EQ(attr(layers[1], "sodipodi:insensitive"), "true");

    auto paths = elements(root, "svg:path");
    ASSERT_EQ(paths.size(), 5u) << written.records.substr(written.records.find("%%EndSetup")) << "\n----\n"
                                << sp_repr_save_buf(imported->getReprDoc()).raw();
    // Where each lies, in the document's pixels, as the original's shapes.
    std::vector<XML::Node *> originals;
    for (auto *n : elements(doc->getReprRoot(), "svg:rect")) originals.push_back(n);
    originals.insert(originals.begin() + 1, elements(doc->getReprRoot(), "svg:path")[0]);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto const was = shape_box(doc.get(), originals[i]), now = shape_box(imported, paths[i]);
        ASSERT_TRUE(was && now) << i;
        EXPECT_NEAR(now->left(), was->left(), 1e-3) << i;
        EXPECT_NEAR(now->top(), was->top(), 1e-3) << i;
        EXPECT_NEAR(now->right(), was->right(), 1e-3) << i;
        EXPECT_NEAR(now->bottom(), was->bottom(), 1e-3) << i;
    }
    imported->ensureUpToDate();
    auto *cmyk = imported->getObjectByRepr(paths[0]);
    auto const fill = cmyk->style->fill.getColor();
    ASSERT_EQ(fill.getSpace()->getComponentType(), Type::CMYK);
    for (unsigned i = 0; i < 4; ++i) EXPECT_NEAR(fill[i], 0.1 * (i + 1), 1e-4);
    EXPECT_NEAR(cmyk->style->stroke_width.computed, 2 * 0.75, 1e-6); // in the import's points
    EXPECT_NE(attr(paths[1], "style").find("evenodd"), std::string::npos);
    EXPECT_EQ(sp_svg_read_pathv(attr(paths[1], "d").c_str()).size(), 2u);
    EXPECT_EQ(attr(paths[3], "proof:overprint"), "fill");
}

TEST_F(AiNativeImportTest, WrittenRecordsKeepSpotInksClipsAndArtboards)
{
    auto doc = svg_document(
        R"(<defs><clipPath id="c"><rect x="10" y="10" width="30" height="30"/></clipPath></defs>)"
        R"(<g inkscape:groupmode="layer" inkscape:label="Ink">)"
        R"(<rect id="spot" x="50" y="10" width="40" height="40" style="fill:#000000"/>)"
        R"x(<g clip-path="url(#c)"><rect x="0" y="0" width="60" height="60" style="fill:device-cmyk(1 0 0 0)"/></g>)x"
        R"(</g>)"
        R"(<sodipodi:namedview><inkscape:page x="0" y="0" width="100" height="100" inkscape:label="Front"/>)"
        R"(<inkscape:page x="120" y="0" width="80" height="100" inkscape:label="Back"/></sodipodi:namedview>)");
    ASSERT_TRUE(doc);
    auto const id = SpotInk::ensure(doc.get(), {"PROOF Orange", Colors::Color(Type::CMYK, {0, 0.6, 1, 0}), 0.3});
    doc->getObjectById("spot")->setAttribute("style", "fill:url(#" + id + ")");
    doc->ensureUpToDate();

    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_NE(written.records.find("%%CMYKCustomColor: 0 0.6 1 0 (PROOF Orange)"), std::string::npos);
    EXPECT_NE(written.records.find("0 0.6 1 0 (PROOF Orange) 0.7 0 Xx"), std::string::npos);
    std::string error;
    auto ai = AN::read(written.records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->artboards.size(), 2u);
    EXPECT_EQ(ai->artboards[0].name, "Front");
    EXPECT_EQ(ai->artboards[1].name, "Back");

    auto b = make(written.records);
    ASSERT_TRUE(b);
    auto *imported = b->document.get();
    imported->ensureUpToDate();
    std::optional<SpotInk::Ink> ink;
    for (auto *g : elements(imported->getReprRoot(), "svg:linearGradient")) {
        if (auto read = SpotInk::read(g); read && read->name == "PROOF Orange") ink = read;
    }
    ASSERT_TRUE(ink);
    EXPECT_NEAR(ink->tint, 0.3, 1e-4);
    bool clipped = false;
    for (auto *g : elements(imported->getReprRoot(), "svg:g")) clipped = clipped || !attr(g, "clip-path").empty();
    EXPECT_TRUE(clipped);
}

TEST_F(AiNativeImportTest, WrittenCompoundClipsKeepRulesHolesTransformsAndOpenContours)
{
    for (auto const rule : {"nonzero", "evenodd"}) {
        SCOPED_TRACE(rule);
        auto doc = svg_document(
            std::string(R"(<defs><clipPath id="c" style="clip-rule:)") + rule + R"x("><path transform="translate(5 7)" )x"
            R"(d="M0 0 H80 V80 H0 Z M20 20 V60 H60 V20 Z M100 0 H120 V20 H100" )"
            R"(sodipodi:nodetypes="ccccczzzzzcccc"/></clipPath></defs>)"
            R"x(<g transform="translate(10 3) scale(1.5)"><rect width="150" height="90" fill="#00ffff" clip-path="url(#c)"/></g>)x");
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
            SCOPED_TRACE(static_cast<int>(version));
            auto const written = AN::write_records(doc.get(), version);
            ASSERT_TRUE(written.refused.empty()) << written.refused.front();
            std::string error;
            auto ai = AN::read(written.records, error);
            ASSERT_TRUE(ai) << error;
            ASSERT_EQ(ai->layers.size(), 1u);
            auto const &clip_group = ai->layers[0].children.at(0).children.at(0);
            ASSERT_TRUE(clip_group.clipped);
            ASSERT_EQ(clip_group.children.size(), 2u);
            auto const &clip = clip_group.children[0];
            EXPECT_EQ(clip.kind, AN::Node::Kind::Compound);
            EXPECT_EQ(clip.evenodd, std::string_view(rule) == "evenodd");
            EXPECT_FALSE(clip.fill);
            EXPECT_FALSE(clip.stroke);
            ASSERT_EQ(clip.children.size(), 3u);
            for (auto const &part : clip.children) {
                auto const &pv = part.path;
                ASSERT_EQ(pv.size(), 1u);
                EXPECT_TRUE(pv[0].closed());
                EXPECT_NEAR(Geom::distance(pv[0].initialPoint(), pv[0].finalPoint()), 0, 1e-6);
            }
            auto const first = clip.children[0].path[0].initialPoint();
            EXPECT_NEAR(first.x(), 13.125, 1e-4); // (5 * 1.5 + 10) * .75
            EXPECT_NEAR(first.y(), version == AN::RecordsVersion::Modern ? -10.125 : 64.875, 1e-4);
            EXPECT_FALSE(clip_group.children[1].evenodd); // clip-rule never becomes fill-rule
            auto imported = make(written.records);
            ASSERT_TRUE(imported);
            auto clips = elements(imported->document->getReprRoot(), "svg:clipPath");
            ASSERT_EQ(clips.size(), 1u);
            auto paths = elements(clips[0], "svg:path");
            ASSERT_EQ(paths.size(), 1u);
            EXPECT_EQ(sp_svg_read_pathv(attr(paths[0], "d").c_str()).size(), 3u);
            EXPECT_EQ(attr(paths[0], "style").find("clip-rule:evenodd") != std::string::npos,
                      std::string_view(rule) == "evenodd");
        }
    }
}

TEST_F(AiNativeImportTest, WrittenClipRulesStayIndependentAcrossNestedClipsAndArtwork)
{
    auto doc = svg_document(
        R"(<defs><clipPath id="outer"><path d="M0 0 H90 V90 H0 Z M10 10 H80 V80 H10 Z" clip-rule="evenodd"/></clipPath>)"
        R"(<clipPath id="inner"><path d="M0 0 H50 V50 H0 Z M10 10 V40 H40 V10 Z" clip-rule="nonzero"/></clipPath></defs>)"
        R"x(<g clip-path="url(#outer)"><g clip-path="url(#inner)"><path fill-rule="evenodd" d="M0 0 H100 V100 H0 Z M20 20 H30 V30 H20 Z"/></g></g>)x"
        R"(<path fill-rule="nonzero" d="M100 0 H120 V20 H100 Z"/>)");
    ASSERT_TRUE(doc);
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        auto const written = AN::write_records(doc.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        std::string error;
        auto ai = AN::read(written.records, error);
        ASSERT_TRUE(ai) << error;
        auto const &outer = ai->layers.at(0).children.at(0);
        ASSERT_TRUE(outer.clipped);
        EXPECT_TRUE(outer.children.at(0).evenodd);
        auto const &inner = outer.children.at(1);
        ASSERT_TRUE(inner.clipped);
        EXPECT_FALSE(inner.children.at(0).evenodd);
        EXPECT_TRUE(inner.children.at(1).evenodd);
        EXPECT_FALSE(ai->layers[0].children.at(1).evenodd);
    }
}

TEST_F(AiNativeImportTest, WriterRefusesClippingOnTheClipGeometryInsteadOfDroppingIt)
{
    auto doc = svg_document(
        R"(<defs><clipPath id="inner"><rect width="10" height="10"/></clipPath>)"
        R"x(<clipPath id="outer"><path d="M0 0 H80 V80 H0 Z M20 20 V60 H60 V20 Z" clip-path="url(#inner)"/></clipPath></defs>)x"
        R"x(<rect width="100" height="100" clip-path="url(#outer)"/>)x");
    ASSERT_TRUE(doc);
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        auto const written = AN::write_records(doc.get(), version);
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), "clipping on a clipping path's geometry"),
                  written.refused.end());
    }
}

TEST_F(AiNativeImportTest, WrittenPatternsKeepTheirTileArtAndEveryPlacement)
{
    // One tile in three placements (two wrappers, a fill and a stroke), offset by x and y as PROOF's
    // pattern library offsets some, its art transformed, compound, clipped, stroked and transparent.
    auto doc = svg_document(
        R"(<defs><clipPath id="c"><rect width="12" height="10"/></clipPath>)"
        R"(<pattern id="tile" inkscape:label="Dots" patternUnits="userSpaceOnUse" x="3" y="4" width="20" height="10">)"
        R"x(<rect width="20" height="10" style="fill:device-cmyk(0 0.2 0 0)"/>)x"
        R"x(<g transform="translate(2 1)" style="opacity:0.5"><path d="M0 0 H8 V8 H0 Z M2 2 V6 H6 V2 Z" style="fill:#0000ff"/></g>)x"
        R"x(<g clip-path="url(#c)"><circle cx="14" cy="5" r="4" style="fill:none;stroke:#ff0000;stroke-width:1.5"/></g>)x"
        R"(</pattern>)"
        R"x(<pattern id="turned" href="#tile" patternTransform="rotate(30) scale(2)"/>)x"
        R"x(<pattern id="moved" href="#tile" patternTransform="translate(5 6)"/></defs>)x"
        R"(<g inkscape:groupmode="layer" inkscape:label="Art">)"
        R"x(<rect id="a" x="10" y="10" width="80" height="60" transform="translate(10 20)" style="fill:url(#turned)"/>)x"
        R"x(<ellipse id="b" cx="150" cy="50" rx="30" ry="20" style="fill:none;stroke:url(#moved);stroke-width:8"/>)x"
        R"x(<rect id="c2" x="100" y="80" width="40" height="15" style="fill:url(#moved);stroke:#000000"/></g>)x");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &r = written.records;
    // One definition for the one tile, in the setup, its art hidden.
    auto const begin = r.find("%AI3_BeginPattern: (Dots)\n(Dots) 0 0 20 10\n%_0 A\n");
    ASSERT_NE(begin, std::string::npos) << r.substr(0, 4000);
    EXPECT_EQ(r.find("%AI3_BeginPattern:", begin + 1), std::string::npos);
    EXPECT_LT(begin, r.find("%%EndSetup"));
    auto const end = r.find("E\n%AI3_EndPattern\n", begin);
    ASSERT_NE(end, std::string::npos);
    std::istringstream definition(r.substr(begin, end - begin));
    std::string line;
    std::getline(definition, line);
    std::getline(definition, line);
    for (std::size_t n = 0; std::getline(definition, line); ++n) EXPECT_EQ(line.rfind("%_", 0), 0u) << n << ": " << line;

    std::string error;
    auto ai = AN::read(r, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->patterns.size(), 1u);
    EXPECT_EQ(ai->patterns[0].name, "Dots");
    EXPECT_TRUE(ai->patterns[0].unread.empty()) << ai->patterns[0].unread;
    EXPECT_EQ(ai->patterns[0].tile, Geom::Rect(0, 0, 20, 10));
    ASSERT_EQ(ai->patterns[0].art.size(), 3u);

    // Each placement maps the tile onto the page as the document does: the tile's top-left corner
    // and its two sides, in the document's pixels.
    auto b = make(r);
    ASSERT_TRUE(b);
    auto *imported = b->document.get();
    imported->ensureUpToDate();
    auto placement = [](SPItem *item, bool fill) {
        auto const &paint = fill ? static_cast<SPIPaint const &>(item->style->fill) : item->style->stroke;
        auto const *p = cast<SPPattern>(paint.isPaintserver() && paint.href ? paint.href->getObject() : nullptr);
        EXPECT_TRUE(p);
        if (!p) return Geom::Affine();
        return Geom::Affine(Geom::Translate(p->x(), p->y())) * p->getTransform() * item->i2doc_affine();
    };
    std::vector<std::pair<SPItem *, bool>> before{{cast<SPItem>(doc->getObjectById("a")), true},
                                                  {cast<SPItem>(doc->getObjectById("b")), false},
                                                  {cast<SPItem>(doc->getObjectById("c2")), true}};
    std::vector<std::pair<SPItem *, bool>> after;
    for (auto *g : elements(imported->getReprRoot(), "svg:g")) {
        if (attr(g, "inkscape:groupmode") != "layer") continue;
        for (auto *n : elements(g, "svg:path")) {
            auto *item = cast<SPItem>(imported->getObjectByRepr(n));
            after.emplace_back(item, item->style->fill.isPaintserver());
        }
    }
    ASSERT_EQ(after.size(), 3u) << sp_repr_save_buf(imported->getReprDoc()).raw();
    for (std::size_t i = 0; i < 3; ++i) {
        SCOPED_TRACE(i);
        auto const was = placement(before[i].first, before[i].second), now = placement(after[i].first, after[i].second);
        for (auto const &q : {Geom::Point(0, 0), Geom::Point(20, 0), Geom::Point(0, 10)}) {
            EXPECT_NEAR((q * now).x(), (q * was).x(), 1e-4);
            EXPECT_NEAR((q * now).y(), (q * was).y(), 1e-4);
        }
    }
    // The tile itself: as large, its art where it was in it.
    auto tiles = elements(imported->getReprRoot(), "svg:pattern");
    auto const tile = std::find_if(tiles.begin(), tiles.end(), [](auto *n) { return !attr(n, "width").empty(); });
    ASSERT_NE(tile, tiles.end());
    auto *imported_tile = cast<SPPattern>(imported->getObjectByRepr(*tile));
    EXPECT_NEAR(imported_tile->width(), 20, 1e-6);
    EXPECT_NEAR(imported_tile->height(), 10, 1e-6);
    auto const art = elements(*tile, "svg:path");
    ASSERT_EQ(art.size(), 3u);
    auto bounds = [](SPDocument *d, XML::Node *n) {
        auto *item = cast<SPItem>(d->getObjectByRepr(n));
        return item->documentGeometricBounds();
    };
    auto const compound = bounds(imported, art[1]);
    ASSERT_TRUE(compound);
    EXPECT_NEAR(compound->left(), 2, 1e-4);
    EXPECT_NEAR(compound->top(), 1, 1e-4);
    EXPECT_NEAR(compound->right(), 10, 1e-4);
    EXPECT_EQ(sp_svg_read_pathv(attr(art[1], "d").c_str()).size(), 2u);
    bool clipped = false, transparent = false;
    for (auto *g : elements(*tile, "svg:g")) {
        clipped = clipped || !attr(g, "clip-path").empty();
        transparent = transparent || attr(g, "style").find("opacity:0.5") != std::string::npos;
    }
    EXPECT_TRUE(clipped);
    EXPECT_TRUE(transparent);

    // Written again from what was read: the same one definition, no larger.
    auto const again = AN::write_records(imported);
    ASSERT_TRUE(again.refused.empty()) << again.refused.front();
    auto const second = again.records.find("%AI3_BeginPattern:");
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(again.records.find("%AI3_BeginPattern:", second + 1), std::string::npos);
    auto const definition_lines = [](std::string const &s, std::size_t from) {
        auto const to = s.find("%AI3_EndPattern", from);
        return std::count(s.begin() + from, s.begin() + to, '\n');
    };
    EXPECT_EQ(definition_lines(again.records, second), definition_lines(r, begin));
}

TEST_F(AiNativeImportTest, PatternsOutsideTheWrittenSubsetAreRefusedByName)
{
    auto const tile = [](std::string const &attrs, std::string const &art) {
        return R"(<defs><linearGradient id="g"><stop stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>)"
               R"(<pattern id="inner" patternUnits="userSpaceOnUse" width="4" height="4"><rect width="2" height="2"/></pattern>)"
               R"(<pattern id="p" )" + attrs + ">" + art + R"x(</pattern></defs><rect width="50" height="50" fill="url(#p)"/>)x";
    };
    auto const square = std::string(R"(<rect width="5" height="5"/>)");
    std::vector<std::pair<std::string, std::string>> const cases{
        {tile(R"(width="0.2" height="0.2")", square), "patterns sized to their object"},
        {tile(R"(patternUnits="userSpaceOnUse" patternContentUnits="objectBoundingBox" width="10" height="10")", square),
         "patterns sized to their object"},
        {tile(R"(patternUnits="userSpaceOnUse" width="10" height="10" viewBox="0 0 5 5")", square), "patterns with a view box"},
        {tile(R"(patternUnits="userSpaceOnUse" width="10" height="10")", R"x(<rect width="5" height="5" fill="url(#inner)"/>)x"),
         "patterns inside patterns"},
        {tile(R"(patternUnits="userSpaceOnUse" width="10" height="10")", R"x(<rect width="5" height="5" fill="url(#g)"/>)x"),
         "gradients in patterns"},
        {tile(R"(patternUnits="userSpaceOnUse" width="10" height="10")",
              R"(<image width="5" height="5" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg=="/>)"),
         "pictures in patterns"},
        {tile(R"(patternUnits="userSpaceOnUse" width="10" height="10")", R"(<text x="1" y="5">A</text>)"), "type in patterns"},
        {tile(R"(patternUnits="userSpaceOnUse" width="10" height="10")", ""), "empty patterns"},
    };
    for (auto const &[body, reason] : cases) {
        SCOPED_TRACE(reason);
        auto doc = svg_document(body);
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end())
            << (written.refused.empty() ? std::string("nothing refused") : written.refused.front());
    }
}

namespace {

/// The document's page as PROOF draws it at 96 dpi: premultiplied ARGB pixels, row after row.
std::vector<std::uint32_t> page_pixels(SPDocument *doc)
{
    doc->ensureUpToDate();
    std::vector<std::uint32_t> out;
    std::unique_ptr<Inkscape::Pixbuf> pixbuf(
        sp_generate_internal_bitmap(doc, Geom::Rect(Geom::Point(0, 0), doc->getDimensions()), 96));
    if (!pixbuf) return out;
    auto *surface = pixbuf->getSurfaceRaw();
    cairo_surface_flush(surface);
    int const width = cairo_image_surface_get_width(surface), height = cairo_image_surface_get_height(surface);
    auto const stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < height; ++y) {
        auto const *row = reinterpret_cast<std::uint32_t const *>(cairo_image_surface_get_data(surface) + y * stride);
        out.insert(out.end(), row, row + width);
    }
    return out;
}

/// The largest difference of any channel at any pixel between two drawings; 256 when their sizes differ.
int largest_difference(std::vector<std::uint32_t> const &a, std::vector<std::uint32_t> const &b)
{
    if (a.size() != b.size() || a.empty()) return 256;
    int most = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (int shift : {0, 8, 16, 24}) {
            most = std::max(most, std::abs(int((a[i] >> shift) & 0xff) - int((b[i] >> shift) & 0xff)));
        }
    }
    return most;
}

std::size_t occurrences(std::string const &text, std::string const &what)
{
    std::size_t n = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

/// A closed square path in Illustrator's records, from its lower-left corner.
std::string box(int x, int y, int w, int h)
{
    return std::format("{} {} m\n{} {} L\n{} {} L\n{} {} L\n{} {} L\n", x, y, x, y + h, x + w, y + h, x + w, y, x, y);
}

} // namespace

TEST_F(AiNativeImportTest, PerceptualGradientRendersOklabAndRetainsEditableStops)
{
    // Red to blue has a purple Oklab midpoint with substantial green. Classic's
    // midpoint has none. Independent expected values, not a second call to the sampler.
    std::string const source = R"svg(<svg xmlns="http://www.w3.org/2000/svg"
        xmlns:proof="urn:x-proof:document:1" width="200" height="100">
        <defs><linearGradient id="g" proof:gradient-method="oklab">
        <stop stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/>
        </linearGradient></defs><rect width="80" height="80" fill="url(#g)"/></svg>)svg";
    auto doc = SPDocument::createNewDocFromMem(source);
    ASSERT_TRUE(doc);
    auto *gradient = cast<SPGradient>(doc->getObjectById("g"));
    ASSERT_TRUE(gradient);
    auto samples = PerceptualGradient::rendering_vector(gradient);
    ASSERT_EQ(samples.stops.size(), 129u);
    auto rgb = samples.stops[64].color->converted(Type::RGB);
    ASSERT_TRUE(rgb);
    EXPECT_NEAR((*rgb)[0] * 255, 140.36, 2);
    EXPECT_NEAR((*rgb)[1] * 255, 83.03, 2);
    EXPECT_NEAR((*rgb)[2] * 255, 162.31, 2);
    EXPECT_EQ(elements(gradient->getRepr(), "svg:stop").size(), 2u);
    auto svg = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(svg);
    auto *saved = cast<SPGradient>(svg->getObjectById("g"));
    EXPECT_TRUE(PerceptualGradient::enabled(saved));
    EXPECT_EQ(elements(saved->getRepr(), "svg:stop").size(), 2u);

    auto written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_NE(written.records.find(" 0 2 1 0 Bd"), std::string::npos);
    EXPECT_NE(written.records.find("%AI17_Alternate_Content"), std::string::npos);
    std::string error;
    auto native = AN::read(written.records, error);
    ASSERT_TRUE(native) << error;
    ASSERT_EQ(native->gradients.size(), 1u);
    EXPECT_EQ(native->gradients[0].options, (std::vector<double>{1, 0}));
    EXPECT_EQ(native->gradients[0].stops.size(), 2u);
    auto reopened = make(written.records);
    ASSERT_TRUE(reopened);
    EXPECT_LE(largest_difference(page_pixels(doc.get()), page_pixels(reopened->document.get())), 2);
    auto legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_FALSE(legacy.refused.empty());
    gradient->getRepr()->setAttribute("proof:gradient-method", "future-method");
    EXPECT_FALSE(AN::write_records(doc.get()).refused.empty());
}

TEST_F(AiNativeImportTest, PerceptualMidpointsStayOnOriginalStopsAfterEdits)
{
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 1 0 Bd\n[\n"
        "0 0 1 0 0 0 1 2 50 100 Bs\n0 1 1 0 1 0 0 2 80 0 Bs\nBD\n%AI5_EndGradient\n";
    auto b = make(records(layer("0 g\n10 20 m 90 20 L 90 60 L 10 60 L h\n"
        "Bb\n0 (G) 10 20 0 80 1 0 0 1 0 0 Bg\nf\n0 BB\n"), setup, false));
    ASSERT_TRUE(b);
    auto vectors = elements(b->document->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_FALSE(vectors.empty());
    auto *gradient = cast<SPGradient>(b->document->getObjectByRepr(vectors.front()));
    ASSERT_TRUE(gradient);
    EXPECT_TRUE(PerceptualGradient::enabled(gradient));
    auto stops = elements(vectors.front(), "svg:stop");
    ASSERT_EQ(stops.size(), 2u);
    EXPECT_EQ(attr(stops.front(), "proof:gradient-midpoint"), "0.8");
    auto before = PerceptualGradient::rendering_vector(gradient);
    stops.front()->setAttribute("style", "stop-color:#00ff00");
    b->document->ensureUpToDate();
    auto after = PerceptualGradient::rendering_vector(gradient);
    EXPECT_NE(before.stops[64].color, after.stops[64].color);
    auto written = AN::write_records(b->document.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    std::string error;
    auto native = AN::read(written.records, error);
    ASSERT_TRUE(native) << error;
    EXPECT_NEAR(native->gradients.at(0).stops.at(0).midpoint, .8, 1e-6);
}

TEST_F(AiNativeImportTest, PerceptualCmykKeepsEditableInksWhileReseparatingTheRendering)
{
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 1 0 Bd\n[\n"
        "0 0 1 0 1 50 100 Bs\n1 0 0 0 1 50 0 Bs\nBD\n%AI5_EndGradient\n";
    auto b = make(records(layer("0 g\n10 20 m 90 20 L 90 60 L 10 60 L h\n"
        "Bb\n0 (G) 10 20 0 80 1 0 0 1 0 0 Bg\nf\n0 BB\n"), setup, true));
    ASSERT_TRUE(b);
    auto vectors = elements(b->document->getDefs()->getRepr(), "svg:linearGradient");
    ASSERT_FALSE(vectors.empty());
    auto const *gradient = cast<SPGradient>(b->document->getObjectByRepr(vectors.front()));
    auto samples = PerceptualGradient::rendering_vector(gradient);
    ASSERT_GT(samples.stops.size(), 2u);
    EXPECT_EQ(samples.stops.front().color->getSpace()->getComponentType(), Type::CMYK);
    auto const *editable = gradient->getGradientVector();
    EXPECT_EQ(editable->stops.front().color->getValues(), (std::vector<double>{1, 0, 0, 0, 1}));
    EXPECT_EQ(editable->stops.back().color->getValues(), (std::vector<double>{0, 0, 1, 0, 1}));
    // The preview/PDF endpoint is deliberately a re-separation, as Illustrator
    // renders it; the editable CMYK channels above never undergo that conversion.
    auto expected = editable->stops.front().color->converted(Type::RGB);
    ASSERT_TRUE(expected);
    for (unsigned c = 0; c < 3; ++c) expected->set(c, std::clamp((*expected)[c], 0.0, 1.0));
    ASSERT_TRUE(expected->convert(editable->stops.front().color->getSpace()));
    for (unsigned c = 0; c < 4; ++c) EXPECT_NEAR((*samples.stops.front().color)[c], (*expected)[c], .002);
    auto written = AN::write_records(b->document.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    std::string error;
    auto native = AN::read(written.records, error);
    ASSERT_TRUE(native) << error;
    auto const &stops = native->gradients.at(0).stops;
    ASSERT_EQ(stops.size(), 2u);
    EXPECT_EQ(stops.front().color, AN::Color::cmyk(1, 0, 0, 0));
    EXPECT_EQ(stops.back().color, AN::Color::cmyk(0, 0, 1, 0));
}

TEST_F(AiNativeImportTest, WrittenOpacityMasksReadBackAndDrawAsTheyDid)
{
    // Masks as Illustrator writes them, in a CMYK document: Clip on and off, greys and a K-only
    // gradient, a group with transparency of its own after its mask, a mask in a mask's art, a
    // clipping group, a stroke in a mask, and a translucent multiplying object.
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0 0 0 1 1 50 100 Bs\n0 0 0 0 1 50 0 Bs\nBD\n"
                              "%AI5_EndGradient\n";
    std::string const art =
        "0 0 0 1 k\n" + box(10, 55, 30, 30) + "f\n" +
        vector_mask("0.5 g\n" + box(10, 55, 15, 30) + "f\n1 g\n" + box(25, 55, 15, 30) + "f\n") +
        "0 0 0 1 k\n" + box(50, 55, 30, 30) + "f\n" + vector_mask("0 g\n" + box(50, 55, 30, 15) + "f\n", 0) +
        "u\n0 0 0 1 k\n" + box(90, 55, 30, 30) + "f\n1 0 0 0 k\n" + box(95, 60, 20, 20) + "f\nU\n" +
        vector_mask(box(90, 55, 30, 30) + "Bb\n0 (G) 90 55 0 30 1 0 0 1 0 0 Bg\nf\n0 BB\n") +
        "0 0.6 0 0 0 Xy\n0 0 Xd\n6 () XW\n" +
        "0 1 0 0 0 Xy\n0 0 0 1 k\n" + box(130, 55, 30, 30) + "f\n" +
        vector_mask("1 g\n" + box(130, 55, 30, 30) + "f\n" + vector_mask("0.5 g\n" + box(130, 55, 20, 30) + "f\n")) +
        "q\n" + box(170, 55, 25, 30) + "h\nW\nn\n0 0 0 1 k\n" + box(165, 50, 35, 40) + "f\nQ\n" +
        vector_mask("0.25 g\n" + box(165, 50, 35, 40) + "f\n") +
        "0 1 0 0 0 Xy\n0 0 0 1 k\n" + box(90, 10, 30, 30) + "f\n" + vector_mask("1 G 6 w\n" + box(95, 15, 20, 20) + "S\n") +
        "1 0.5 0 0 0 Xy\n0 0 1 0 k\n" + box(10, 10, 30, 30) + "f\n" + vector_mask("0.5 g\n" + box(10, 10, 30, 30) + "f\n");
    auto const source = records(layer(art), setup);
    std::string error;
    auto const ai = AN::read(source, error);
    ASSERT_TRUE(ai) << error;
    auto made = make(source);
    ASSERT_TRUE(made);
    auto *before = made->document.get();
    ASSERT_EQ(elements(before->getDefs()->getRepr(), "svg:mask").size(), 8u);

    auto const written = AN::write_records(before);
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &r = written.records;
    EXPECT_EQ(occurrences(r, "%_/Mask :\n"), 8u);
    EXPECT_EQ(occurrences(r, "%_0 /Bool (Clipping) ,\n"), 1u);
    EXPECT_EQ(occurrences(r, "%_1 /Bool (Clipping) ,\n"), 7u);
    EXPECT_EQ(occurrences(r, "%_1 /Bool (Linked) ,\n"), 8u);
    EXPECT_EQ(r.find("%_%_"), std::string::npos);
    // A mask's greys are Gray, its gradient's stops too: not CMYK, though the document is.
    EXPECT_NE(r.find("%_0.5 g\n"), std::string::npos) << r;
    EXPECT_NE(r.find(" 0 50 0 %_Bs\n"), std::string::npos);
    // The mask straight after the object it masks: the import's wrapper isn't written.
    EXPECT_NE(r.find("f\n%_/ArtDictionary :\n%_/Mask :\n"), std::string::npos);

    auto const again = AN::read(r, error);
    ASSERT_TRUE(again) << error;
    auto const &was = ai->layers.at(0).children, &now = again->layers.at(0).children;
    ASSERT_EQ(now.size(), was.size());
    for (std::size_t i = 0; i < was.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(now[i].kind, was[i].kind);
        EXPECT_EQ(now[i].clipped, was[i].clipped);
        ASSERT_EQ(bool(now[i].opacity_mask), bool(was[i].opacity_mask));
        EXPECT_EQ(now[i].opacity_mask_clipped, was[i].opacity_mask_clipped);
        EXPECT_NEAR(now[i].transparency.opacity, was[i].transparency.opacity, 1e-6);
        EXPECT_EQ(now[i].transparency.blend, was[i].transparency.blend);
    }
    EXPECT_NEAR(was[2].transparency.opacity, 0.6, 1e-6);
    // A mask's art is in a group of its own, as Illustrator writes it; in it, the mask in the mask.
    ASSERT_EQ(now[3].opacity_mask->size(), 1u);
    ASSERT_EQ(now[3].opacity_mask->front().children.size(), 1u);
    EXPECT_TRUE(now[3].opacity_mask->front().children[0].opacity_mask);

    auto reopened = make(r);
    ASSERT_TRUE(reopened);
    auto *after = reopened->document.get();
    EXPECT_LE(largest_difference(page_pixels(before), page_pixels(after)), 2);
    // Written again from what was read: the same records, but for the artboard's new identity.
    auto const second = AN::write_records(after);
    ASSERT_TRUE(second.refused.empty()) << second.refused.front();
    EXPECT_EQ(occurrences(second.records, "%_/Mask :\n"), 8u);
    EXPECT_EQ(std::count(second.records.begin(), second.records.end(), '\n'), std::count(r.begin(), r.end(), '\n'));

    // Moved off the white the import put under it, the masked object shows only where that white
    // is: that is what PROOF draws, and the mask is written with Clip on and the white as its art.
    auto *unclipped = elements(before->getDefs()->getRepr(), "svg:mask")[1];
    ASSERT_EQ(attr(unclipped, "proof:ai-mask-clipping"), "false");
    XML::Node *wrapper = nullptr;
    for (auto *g : elements(before->getReprRoot(), "svg:g")) {
        if (attr(g, "mask") == "url(#" + attr(unclipped, "id") + ")") wrapper = g;
    }
    ASSERT_TRUE(wrapper && wrapper->firstChild());
    wrapper->firstChild()->setAttribute("transform", "translate(30 20)");
    auto const moved = AN::write_records(before);
    ASSERT_TRUE(moved.refused.empty()) << moved.refused.front();
    EXPECT_EQ(occurrences(moved.records, "%_0 /Bool (Clipping) ,\n"), 0u);
    EXPECT_EQ(occurrences(moved.records, "%_1 /Bool (Clipping) ,\n"), 8u);
    auto moved_back = make(moved.records);
    ASSERT_TRUE(moved_back);
    EXPECT_LE(largest_difference(page_pixels(before), page_pixels(moved_back->document.get())), 2);
}

TEST_F(AiNativeImportTest, MasksMadeInProofAreWrittenWithClipOn)
{
    // Masks as PROOF makes them: in the masked object's user space, turned with it, with no
    // backdrop of their own; a grey, a gradient, and a mask on a group in a group.
    auto doc = svg_document(
        R"(<defs><mask id="grey" maskUnits="userSpaceOnUse"><rect x="10" y="10" width="20" height="30" fill="#808080"/></mask>)"
        R"(<linearGradient id="fade" x1="50" y1="0" x2="90" y2="0" gradientUnits="userSpaceOnUse"><stop stop-color="#000000"/>)"
        R"(<stop offset="1" stop-color="#ffffff"/></linearGradient>)"
        R"x(<mask id="ramp"><rect x="50" y="10" width="40" height="40" fill="url(#fade)"/></mask>)x"
        R"(<mask id="round" maskUnits="userSpaceOnUse"><circle r="15" fill="#ffffff"/></mask></defs>)"
        R"(<g inkscape:groupmode="layer" inkscape:label="Art">)"
        R"x(<rect x="5" y="5" width="40" height="40" fill="#ff0000" mask="url(#grey)"/>)x"
        R"x(<rect x="50" y="10" width="40" height="40" fill="#0000ff" mask="url(#ramp)"/>)x"
        R"x(<g transform="translate(140 50) rotate(20)" mask="url(#round)"><rect x="-20" y="-20" width="40" height="40" fill="#00aa00"/>)x"
        R"(<rect x="-5" y="-5" width="30" height="10" fill="#000000"/></g></g>)");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_EQ(occurrences(written.records, "%_/Mask :\n"), 3u);
    EXPECT_EQ(occurrences(written.records, "%_1 /Bool (Clipping) ,\n"), 3u);
    EXPECT_NE(written.records.find("%_0.502 g\n"), std::string::npos);
    // In an RGB document the mask's gradient has neutral RGB stops: Illustrator draws Gray ones
    // through its Gray working space there.
    EXPECT_NE(written.records.find(" 0 0 0 2 50 0 %_Bs\n"), std::string::npos) << written.records;
    EXPECT_NE(written.records.find(" 1 1 1 2 50 100 %_Bs\n"), std::string::npos);
    auto reopened = make(written.records);
    ASSERT_TRUE(reopened);
    EXPECT_LE(largest_difference(page_pixels(doc.get()), page_pixels(reopened->document.get())), 3);

    // A hidden object's mask isn't: its art is shown in the mask, once the object is.
    auto hidden = svg_document(R"(<defs><mask id="m" maskUnits="userSpaceOnUse"><rect width="20" height="20" fill="#ffffff"/></mask></defs>)"
                               R"x(<rect width="30" height="30" style="display:none" mask="url(#m)"/>)x");
    ASSERT_TRUE(hidden);
    auto const shown = AN::write_records(hidden.get());
    ASSERT_TRUE(shown.refused.empty()) << shown.refused.front();
    EXPECT_NE(shown.records.find("%_X=\n%_0 Xw\n%_u\n"), std::string::npos) << shown.records;
    std::string error;
    auto const read = AN::read(shown.records, error);
    ASSERT_TRUE(read) << error;
    auto const &object = read->layers.at(0).children.at(0);
    EXPECT_FALSE(object.visible);
    ASSERT_TRUE(object.opacity_mask && !object.opacity_mask->empty());
    EXPECT_TRUE(object.opacity_mask->front().visible);
    EXPECT_TRUE(object.opacity_mask->front().children.at(0).visible);
}

TEST_F(AiNativeImportTest, MasksOutsideTheWrittenSubsetAreRefusedByName)
{
    auto const masked = [](std::string const &art, std::string const &mask_attrs = R"(maskUnits="userSpaceOnUse")") {
        return R"(<defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="20" y2="0"><stop stop-color="#ff0000"/>)"
               R"(<stop offset="1" stop-color="#0000ff"/></linearGradient>)"
               R"(<pattern id="p" patternUnits="userSpaceOnUse" width="4" height="4"><rect width="2" height="2"/></pattern>)"
               R"(<mask id="m" )" + mask_attrs + ">" + art + R"x(</mask></defs><rect width="50" height="50" mask="url(#m)"/>)x";
    };
    std::vector<std::pair<std::string, std::string>> const cases{
        {masked(R"(<rect width="20" height="20" fill="#ff0000"/>)"), "colours other than grey in opacity masks"},
        {masked(R"x(<rect width="20" height="20" fill="url(#g)"/>)x"), "colours other than grey in opacity masks"},
        {masked(R"x(<rect width="20" height="20" fill="url(#p)"/>)x"), "patterns in opacity masks"},
        {masked(R"(<image width="5" height="5" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg=="/>)"),
         "pictures in opacity masks"},
        {masked(R"(<rect width="20" height="20" fill="#808080" style="mix-blend-mode:screen"/>)"),
         "blend modes other than Multiply in opacity masks"},
        {masked(R"(<text x="1" y="5">A</text>)"), "type in opacity masks"},
        {masked(R"(<rect width="0.5" height="0.5" fill="#ffffff"/>)", R"(maskContentUnits="objectBoundingBox")"),
         "opacity masks sized to their object"},
        {R"(<defs><mask id="m"><rect width="20" height="20" fill="#ffffff"/></mask></defs>)"
         R"x(<rect id="r" width="20" height="20"/><use href="#r" x="30" mask="url(#m)"/>)x", "opacity masks on clones"},
        {R"(<defs><mask id="m"><rect width="20" height="20" fill="#ffffff"/></mask>)"
         R"x(<clipPath id="c"><rect width="10" height="10" mask="url(#m)"/></clipPath></defs><rect width="20" height="20" clip-path="url(#c)"/>)x",
         "opacity masks on clipping paths"},
        {R"(<defs><mask id="m"><rect width="20" height="20" fill="#ffffff"/></mask>)"
         R"x(<pattern id="p" patternUnits="userSpaceOnUse" width="10" height="10"><rect width="5" height="5" mask="url(#m)"/></pattern>)x"
         R"x(</defs><rect width="50" height="50" fill="url(#p)"/>)x", "opacity masks in patterns"},
    };
    for (auto const &[body, reason] : cases) {
        SCOPED_TRACE(reason);
        auto doc = svg_document(body);
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end())
            << (written.refused.empty() ? std::string("nothing refused") : written.refused.front());
    }
    // Illustrator 7 has no masks.
    auto doc = svg_document(masked(R"(<rect width="20" height="20" fill="#ffffff"/>)"));
    ASSERT_TRUE(doc);
    auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(legacy.records.empty());
    EXPECT_NE(std::find(legacy.refused.begin(), legacy.refused.end(), "opacity masks (Illustrator 7)"), legacy.refused.end());
    EXPECT_TRUE(AN::write_records(doc.get()).refused.empty());
}

TEST_F(AiNativeImportTest, WrittenRecordsKeepInksDefinedInLabAndRgb)
{
    auto doc = svg_document(R"(<g inkscape:groupmode="layer" inkscape:label="Ink">)"
                            R"(<rect id="lab" x="10" y="10" width="40" height="40"/>)"
                            R"(<rect id="tint" x="60" y="10" width="40" height="40"/>)"
                            R"(<rect id="rgb" x="110" y="10" width="40" height="40"/>)"
                            R"(<rect id="cmyk" x="160" y="10" width="30" height="40"/></g>)");
    ASSERT_TRUE(doc);
    // Lab 50 60 30, kept as Inkscape keeps Lab: L/100, a and b scaled from [-128, 127].
    auto const lab = Colors::Color(Type::LAB, {0.5, (60.0 + 128) / 255, (30.0 + 128) / 255});
    auto const full = SpotInk::ensure(doc.get(), {"Book Red", lab, 1.0});
    auto const quarter = SpotInk::ensure(doc.get(), {"Book Red", lab, 0.25});
    auto const rgb = SpotInk::ensure(doc.get(), {"Screen Green", Colors::Color(Type::RGB, {0, 0.6, 0.2}), 0.5});
    auto const cmyk = SpotInk::ensure(doc.get(), {"Press Orange", Colors::Color(Type::CMYK, {0, 0.6, 1, 0}), 1.0});
    doc->getObjectById("lab")->setAttribute("style", "fill:url(#" + full + ")");
    doc->getObjectById("tint")->setAttribute("style", "fill:none;stroke:url(#" + quarter + ");stroke-width:4");
    doc->getObjectById("rgb")->setAttribute("style", "fill:url(#" + rgb + ")");
    doc->getObjectById("cmyk")->setAttribute("style", "fill:url(#" + cmyk + ")");
    doc->ensureUpToDate();

    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &records = written.records;
    // Every name, then CMYK inks by their CMYK and RGB and Lab inks by their RGB, that list last;
    // all after the format, as Illustrator writes them. Before it, a `%%+` line left Illustrator
    // 29.2.1 misreading every radial gradient's Bm.
    EXPECT_NE(records.find("%AI3_ColorUsage: Color\n%%DocumentCustomColors: (Book Red)\n%%+ (Press Orange)\n"
                           "%%+ (Screen Green)\n"),
              std::string::npos);
    EXPECT_LT(records.find("%AI5_FileFormat 14.0\n"), records.find("%%DocumentCustomColors:"));
    auto const cmyk_list = records.find("%%CMYKCustomColor: 0 0.6 1 0 (Press Orange)\n%%RGBCustomColor: ");
    ASSERT_NE(cmyk_list, std::string::npos);
    auto const rgb_list = records.find("%%RGBCustomColor: ");
    auto const book = records.find(" (Book Red)\n%%+ 0 0.6 0.2 (Screen Green)\n", rgb_list);
    ASSERT_NE(book, std::string::npos);
    EXPECT_EQ(records.find('\n', rgb_list), book + std::string(" (Book Red)").size());
    // x knows only CMYK: a Lab or RGB ink is Xx with its type even at full strength.
    auto const line_of = [&](std::string const &end) {
        auto const at = records.find(end);
        if (at == std::string::npos) return std::string();
        auto const start = records.rfind('\n', at) + 1;
        return records.substr(start, at + end.size() - start);
    };
    auto const lab_full = line_of(" 50 60 30 (Book Red) 0 2 Xx\n");
    auto const lab_quarter = line_of(" 50 60 30 (Book Red) 0.75 2 XX\n");
    ASSERT_FALSE(lab_full.empty());
    ASSERT_FALSE(lab_quarter.empty());
    EXPECT_NE(records.find(" 0 0.6 0.2 (Screen Green) 0.5 1 Xx\n"), std::string::npos);
    EXPECT_NE(records.find("\n0 0.6 1 0 (Press Orange) 0 x\n"), std::string::npos);
    // Beside the Lab, the CMYK of the colour at that tint, as Illustrator writes it.
    auto const cmyk_of = [](std::string const &line) { return line.substr(0, line.find(" 50 60 30")); };
    EXPECT_NE(cmyk_of(lab_full), cmyk_of(lab_quarter));
    // By the working rendering intent, as the writer converts it.
    auto const tinted = SpotInk::display_color(lab, 0.25);
    auto shown = tinted.getValues();
    ASSERT_TRUE(tinted.getSpace()->convert(shown, Colors::Manager::get().find(Type::CMYK),
                                           Colors::DocumentColors::workingIntent()));
    auto const four = [](double v) {
        auto s = std::format("{:.4f}", v);
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s.pop_back();
        return s == "-0" ? std::string("0") : s;
    };
    EXPECT_EQ(cmyk_of(lab_quarter), four(std::clamp(shown[0], 0.0, 1.0)) + " " + four(std::clamp(shown[1], 0.0, 1.0)) +
                                        " " + four(std::clamp(shown[2], 0.0, 1.0)) + " " +
                                        four(std::clamp(shown[3], 0.0, 1.0)));

    std::string error;
    auto ai = AN::read(records, error);
    ASSERT_TRUE(ai) << error;
    std::map<std::string, AN::Ink> inks;
    for (auto const &ink : ai->inks) inks.emplace(ink.name, ink);
    ASSERT_EQ(inks.size(), 3u);
    EXPECT_EQ(inks.at("Book Red").color, AN::Color::lab(50, 60, 30));
    EXPECT_EQ(inks.at("Screen Green").color, AN::Color::rgb(0, 0.6, 0.2));
    EXPECT_EQ(inks.at("Press Orange").color, AN::Color::cmyk(0, 0.6, 1, 0));

    auto b = make(records);
    ASSERT_TRUE(b);
    auto *imported = b->document.get();
    imported->ensureUpToDate();
    std::map<std::string, std::vector<SpotInk::Ink>> swatches;
    for (auto *g : elements(imported->getReprRoot(), "svg:linearGradient")) {
        if (auto read = SpotInk::read(g)) swatches[read->name].push_back(*read);
    }
    ASSERT_EQ(swatches["Book Red"].size(), 2u);
    for (auto const &ink : swatches["Book Red"]) {
        ASSERT_TRUE(ink.alternate.getSpace());
        EXPECT_EQ(ink.alternate.getSpace()->getType(), Type::LAB);
        EXPECT_NEAR(ink.alternate[0], lab[0], 1e-4);
        EXPECT_NEAR(ink.alternate[1], lab[1], 1e-4);
        EXPECT_NEAR(ink.alternate[2], lab[2], 1e-4);
    }
    std::set<double> tints;
    for (auto const &ink : swatches["Book Red"]) tints.insert(std::round(ink.tint * 1e4) / 1e4);
    EXPECT_EQ(tints, (std::set<double>{0.25, 1.0}));
    // The import gives each ink its full-strength swatch beside the tints used.
    tints.clear();
    for (auto const &ink : swatches["Screen Green"]) {
        EXPECT_EQ(ink.alternate.getSpace()->getType(), Type::RGB);
        EXPECT_NEAR(ink.alternate[1], 0.6, 1e-4);
        tints.insert(std::round(ink.tint * 1e4) / 1e4);
    }
    EXPECT_EQ(tints, (std::set<double>{0.5, 1.0}));

    // Illustrator 7 had no Lab inks; its RGB form is not qualified.
    auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(legacy.records.empty());
    EXPECT_EQ(legacy.refused, std::vector<std::string>{"spot inks defined in RGB or Lab (Illustrator 7)"});

    // One name, one definition: the same name in CMYK too is refused.
    auto const clash = SpotInk::ensure(doc.get(), {"Book Red", Colors::Color(Type::CMYK, {0, 1, 0.6, 0}), 1.0});
    doc->getObjectById("cmyk")->setAttribute("style", "fill:url(#" + clash + ")");
    doc->ensureUpToDate();
    auto const twice = AN::write_records(doc.get());
    EXPECT_TRUE(twice.records.empty());
    EXPECT_EQ(twice.refused, std::vector<std::string>{"one ink name with two definitions"});
}

TEST_F(AiNativeImportTest, WrittenRegistrationKeepsItsPlateMeaningTintsAndOverprint)
{
    auto doc = svg_document(R"(<rect id="zero" x="10" y="10" width="20" height="20"/>)"
                            R"(<rect id="tint" x="40" y="10" width="20" height="20"/>)"
                            R"(<rect id="full" x="70" y="10" width="20" height="20"/>)");
    ASSERT_TRUE(doc);
    std::array<double, 3> const tints{0.0, 0.35, 1.0};
    std::array<char const *, 3> const names{"zero", "tint", "full"};
    for (unsigned i = 0; i < tints.size(); ++i) {
        auto const id = SpotInk::ensure(doc.get(), {"All", Colors::Color(Type::CMYK, {1, 1, 1, 1}), tints[i]});
        auto *item = doc->getObjectById(names[i]);
        ASSERT_TRUE(item);
        item->setAttribute("style", "fill:url(#" + id + ");stroke:url(#" + id + ");stroke-width:2");
        item->setAttribute("proof:overprint", "fill stroke");
    }
    doc->ensureUpToDate();
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_NE(written.records.find("%%CMYKProcessColor: 1 1 1 1 ([Registration])\n"), std::string::npos);
    EXPECT_EQ(written.records.find("%%DocumentCustomColors:"), std::string::npos);
    EXPECT_EQ(written.records.find("%%CMYKCustomColor:"), std::string::npos);
    std::string error;
    auto ai = AN::read(written.records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->layers.size(), 1u);
    auto const &children = ai->layers[0].children;
    ASSERT_EQ(children.size(), tints.size());
    for (unsigned i = 0; i < children.size(); ++i) {
        SCOPED_TRACE(i);
        auto const &node = children[i];
        ASSERT_TRUE(node.fill);
        ASSERT_TRUE(node.stroke);
        for (auto const *paint : {&*node.fill, &*node.stroke}) {
            ASSERT_GE(paint->ink, 0);
            ASSERT_LT(static_cast<unsigned>(paint->ink), ai->inks.size());
            EXPECT_EQ(ai->inks[paint->ink].kind, AN::Ink::Kind::Registration);
            EXPECT_EQ(ai->inks[paint->ink].name, "[Registration]");
            EXPECT_NEAR(paint->tint, tints[i], 1e-6);
        }
        EXPECT_TRUE(node.overprint_fill);
        EXPECT_TRUE(node.overprint_stroke);
    }
    // Actual Illustrator 7 Registration encoding remains unqualified.
    auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(legacy.records.empty());
    EXPECT_EQ(legacy.refused, std::vector<std::string>{"Registration ink (Illustrator 7)"});
}

TEST_F(AiNativeImportTest, RegistrationGradientReferencesRemainRefused)
{
    auto doc = svg_document(
        R"(<defs xmlns:xlink="http://www.w3.org/1999/xlink"><linearGradient id="base" inkscape:swatch="solid" proof:ink="All" )"
        R"x(proof:ink-alternate="device-cmyk(1 1 1 1)" proof:ink-tint="0.35">)x"
        R"x(<stop offset="0" style="stop-color:device-cmyk(0.35 0.35 0.35 0.35)"/></linearGradient>)x"
        R"(<linearGradient id="reference" inkscape:swatch="solid" xlink:href="#base"/></defs>)"
        R"x(<rect width="20" height="20" fill="url(#reference)"/>)x");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    // This is a gradient reference, rather than a self-contained single-stop
    // swatch. Keep the existing refusal until that form is qualified as paint.
    EXPECT_TRUE(written.records.empty());
    EXPECT_EQ(written.refused, std::vector<std::string>{"spot ink swatches used as gradients"});
    // Partial local metadata must not turn the reference into process paint.
    doc->getObjectById("reference")->setAttribute("proof:ink", "All");
    auto const refused = AN::write_records(doc.get());
    EXPECT_TRUE(refused.records.empty());
    EXPECT_EQ(refused.refused, std::vector<std::string>{"spot ink swatches used as gradients"});
}

TEST_F(AiNativeImportTest, TheWriterRefusesAmbiguousAndMalformedRegistration)
{
    struct Case { std::string attributes, reason; };
    std::vector<Case> const cases{
        {R"x(proof:ink="[Registration]" proof:ink-alternate="device-cmyk(1 1 1 1)")x", "ambiguous Registration ink names"},
        {R"x(proof:ink="All" proof:ink-alternate="device-cmyk(0.75 0.68 0.67 0.9)")x", "noncanonical Registration ink definitions"},
        {R"(proof:ink="All" proof:ink-alternate="#010101")", "noncanonical Registration ink definitions"},
        {R"x(proof:ink="All" proof:ink-alternate="lab(0% 0 0)")x", "noncanonical Registration ink definitions"},
        {R"(proof:ink="All" proof:ink-alternate="not-a-colour")", "malformed Registration ink definitions"},
        {R"(proof:ink="All")", "malformed Registration ink definitions"},
    };
    for (auto const &test : cases) {
        SCOPED_TRACE(test.attributes);
        auto doc = svg_document("<defs><linearGradient id='ink' inkscape:swatch='solid' " + test.attributes +
            R"(><stop offset="0" stop-color="#000000"/></linearGradient></defs>)"
            R"x(<rect width="20" height="20" fill="url(#ink)"/>)x");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_EQ(written.refused, std::vector<std::string>{test.reason});
    }
    for (auto const tint : {"", "nonsense", "0.35 trailing", "-0.1", "1.1", "nan", "inf"}) {
        SCOPED_TRACE(tint);
        auto doc = svg_document(
            R"(<defs><linearGradient id="ink" inkscape:swatch="solid" proof:ink="All" )"
            R"x(proof:ink-alternate="device-cmyk(1 1 1 1)" proof:ink-tint=")x" + std::string(tint) +
            R"("><stop offset="0" stop-color="#000000"/></linearGradient></defs>)"
            R"x(<rect width="20" height="20" fill="url(#ink)"/>)x");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_EQ(written.refused, std::vector<std::string>{"noncanonical Registration ink definitions"});
    }
    // RGB black is Registration in a CMYK document too, as Illustrator 29 keeps an RGB
    // document's when the document becomes CMYK: %%RGBProcessColor and Xz still.
    auto cmyk = svg_document(
        R"(<defs><linearGradient id="ink" inkscape:swatch="solid" proof:ink="All" proof:ink-alternate="#000000">)"
        R"x(<stop offset="0" stop-color="#000000"/></linearGradient></defs><rect width="20" height="20" fill="url(#ink)"/>)x",
        R"(width="200" height="100" viewBox="0 0 200 100" proof:color-mode="CMYK")");
    ASSERT_TRUE(cmyk);
    auto const in_cmyk = AN::write_records(cmyk.get());
    ASSERT_TRUE(in_cmyk.refused.empty()) << in_cmyk.refused.front();
    EXPECT_NE(in_cmyk.records.find("%AI9_ColorModel: 2\n"), std::string::npos);
    EXPECT_NE(in_cmyk.records.find("%%RGBProcessColor: 0 0 0 ([Registration])\n"), std::string::npos);
    EXPECT_NE(in_cmyk.records.find(" 0 0 0 ([Registration]) 0 1 Xz\n"), std::string::npos);
    // One document, one Registration: not defined both ways.
    auto both = svg_document(
        R"x(<defs><linearGradient id="c" inkscape:swatch="solid" proof:ink="All" proof:ink-alternate="device-cmyk(1 1 1 1)">)x"
        R"(<stop offset="0" stop-color="#000000"/></linearGradient>)"
        R"(<linearGradient id="r" inkscape:swatch="solid" proof:ink="All" proof:ink-alternate="#000000">)"
        R"(<stop offset="0" stop-color="#000000"/></linearGradient></defs>)"
        R"x(<rect width="20" height="20" fill="url(#c)"/><rect x="30" width="20" height="20" fill="url(#r)"/>)x");
    ASSERT_TRUE(both);
    EXPECT_EQ(AN::write_records(both.get()).refused, std::vector<std::string>{"Registration defined in both CMYK and RGB"});
}

TEST_F(AiNativeImportTest, AnRgbDocumentsRegistrationIsWrittenAsIllustratorWritesItThere)
{
    // As the owner's Illustrator CS5 and CS6 RGB documents carry it: RGB black, its CMYK
    // first, `c m y k 0 0 0 ([Registration]) tint 1 Xz` (XZ for a stroke), 0 at full strength.
    std::string const registration = "0.75 0.68 0.67 0.9 0 0 0 ([Registration]) ";
    auto const art = registration + "0 1 Xz\n" + box(10, 10, 30, 30) + "f\n" +
                     registration + "0.4 1 XZ\n2 w\n" + box(50, 10, 30, 30) + "S\n" +
                     registration + "0.65 1 Xz\n" + box(90, 10, 30, 30) + "f\n";
    auto made = make(records(layer(art), {}, false));
    ASSERT_TRUE(made);
    auto *doc = made->document.get();
    std::vector<SpotInk::Ink> inks;
    for (auto *swatch : elements(doc->getDefs()->getRepr(), "svg:linearGradient")) {
        if (auto ink = SpotInk::read(swatch)) inks.push_back(*ink);
    }
    ASSERT_EQ(inks.size(), 3u);
    for (auto const &ink : inks) {
        EXPECT_EQ(ink.name, "All");
        ASSERT_TRUE(ink.alternate.getSpace());
        EXPECT_EQ(ink.alternate.getSpace()->getComponentType(), Type::RGB);
        EXPECT_EQ(ink.alternate.toString(false), "#000000");
    }

    auto const written = AN::write_records(doc);
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &r = written.records;
    EXPECT_EQ(occurrences(r, "%%RGBProcessColor: 0 0 0 ([Registration])\n"), 1u);
    EXPECT_EQ(r.find("%%CMYKProcessColor:"), std::string::npos);
    EXPECT_EQ(r.find("%%DocumentCustomColors:"), std::string::npos);
    EXPECT_EQ(r.find(" Xs\n"), std::string::npos);
    EXPECT_EQ(occurrences(r, " 0 0 0 ([Registration]) 0 1 Xz\n"), 1u) << r;
    EXPECT_EQ(occurrences(r, " 0 0 0 ([Registration]) 0.4 1 XZ\n"), 1u);
    EXPECT_EQ(occurrences(r, " 0 0 0 ([Registration]) 0.65 1 Xz\n"), 1u);

    std::string error;
    auto ai = AN::read(r, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->layers.size(), 1u);
    auto const &children = ai->layers[0].children;
    ASSERT_EQ(children.size(), 3u);
    std::array<std::optional<AN::Paint> const *, 3> const paints{&children[0].fill, &children[1].stroke, &children[2].fill};
    std::array<double, 3> const tints{1.0, 0.6, 0.35};
    for (unsigned i = 0; i < paints.size(); ++i) {
        SCOPED_TRACE(i);
        ASSERT_TRUE(*paints[i]);
        auto const &paint = **paints[i];
        ASSERT_GE(paint.ink, 0);
        auto const &ink = ai->inks[paint.ink];
        EXPECT_EQ(ink.kind, AN::Ink::Kind::Registration);
        EXPECT_EQ(ink.name, "[Registration]");
        EXPECT_EQ(ink.color.model, AN::Color::Model::Rgb);
        EXPECT_EQ(ink.color.v[0] + ink.color.v[1] + ink.color.v[2], 0.0);
        EXPECT_NEAR(paint.tint, tints[i], 1e-6);
    }

    // Reopened and written again, the same.
    auto again = make(r);
    ASSERT_TRUE(again);
    auto const rewritten = AN::write_records(again->document.get());
    ASSERT_TRUE(rewritten.refused.empty()) << rewritten.refused.front();
    for (auto const *line : {"%%RGBProcessColor: 0 0 0 ([Registration])\n", " 0 0 0 ([Registration]) 0 1 Xz\n",
                             " 0 0 0 ([Registration]) 0.4 1 XZ\n", " 0 0 0 ([Registration]) 0.65 1 Xz\n"}) {
        EXPECT_EQ(occurrences(rewritten.records, line), 1u) << line;
    }
}

TEST_F(AiNativeImportTest, OrdinaryInkNamesNearRegistrationStayOrdinarySpots)
{
    for (auto const name : {"all", "ALL", "All ", "Ordinary"}) {
        SCOPED_TRACE(name);
        auto doc = svg_document(R"(<rect id="art" width="20" height="20"/>)");
        ASSERT_TRUE(doc);
        auto const id = SpotInk::ensure(doc.get(), {name, Colors::Color(Type::CMYK, {1, 1, 1, 1}), 0.35});
        doc->getObjectById(id)->setAttribute("inkscape:label", "[Registration]");
        doc->getObjectById("art")->setAttribute("style", "fill:url(#" + id + ")");
        for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
            auto const written = AN::write_records(doc.get(), version);
            ASSERT_TRUE(written.refused.empty()) << written.refused.front();
            EXPECT_EQ(written.records.find("%%CMYKProcessColor:"), std::string::npos);
            std::string error;
            auto ai = AN::read(written.records, error);
            ASSERT_TRUE(ai) << error;
            ASSERT_EQ(ai->layers.size(), 1u);
            ASSERT_EQ(ai->layers[0].children.size(), 1u);
            auto const &fill = ai->layers[0].children[0].fill;
            ASSERT_TRUE(fill);
            ASSERT_GE(fill->ink, 0);
            ASSERT_LT(static_cast<unsigned>(fill->ink), ai->inks.size());
            EXPECT_EQ(ai->inks[fill->ink].kind, AN::Ink::Kind::Spot);
            EXPECT_EQ(ai->inks[fill->ink].name, name);
            EXPECT_NEAR(fill->tint, 0.35, 1e-6);
        }
    }
}

TEST_F(AiNativeImportTest, LegacyWrittenRecordsKeepTheSingleArtboardGeometryAndPaint)
{
    auto doc = svg_document(
        R"(<g inkscape:groupmode="layer" inkscape:label="Art" inkscape:highlight-color="#4fff4f">)"
        R"(<rect x="10" y="10" width="40" height="30" style="fill:device-cmyk(0.1 0.2 0.3 0.4);stroke:#000000;stroke-width:2"/>)"
        R"(<path d="M 60 10 H 100 V 50 H 60 Z M 70 20 V 40 H 90 V 20 Z" style="fill:#ff0000;fill-rule:evenodd"/>)"
        R"(<g><path d="M 110 10 C 120 0 130 30 140 10" style="fill:none;stroke:device-cmyk(1 0 0 0);stroke-dasharray:8 4;stroke-linecap:round"/></g>)"
        R"(</g>)"
        R"(<g inkscape:groupmode="layer" inkscape:label="Hidden" style="display:none" sodipodi:insensitive="true">)"
        R"(<rect x="10" y="60" width="20" height="20" fill="#000000"/></g>)");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_EQ(written.records.find("%!PS-Adobe-3.0\n"), 0u);
    EXPECT_NE(written.records.find("%AI5_FileFormat 3\n"), std::string::npos);
    EXPECT_NE(written.records.find("%AI3_Cropmarks: 0 0 150 75\n"), std::string::npos);
    EXPECT_NE(written.records.find("%AI3_TemplateBox: 75 37 75 37\n"), std::string::npos);
    EXPECT_NE(written.records.find("1 1 1 1 0 0 2 79 255 79 Lb\n"), std::string::npos);
    auto artwork = written.records;
    auto const advisory = artwork.find("%AI9_ColorModel: 1\n");
    ASSERT_NE(advisory, std::string::npos);
    artwork.erase(advisory, std::string_view("%AI9_ColorModel: 1\n").size());
    for (auto const marker : {"AI9_", "AI24_", " Xw", "annotatepage", "AIPrivateData"}) {
        EXPECT_EQ(artwork.find(marker), std::string::npos) << marker;
    }
    auto b = make(written.records);
    ASSERT_TRUE(b);
    auto *imported = b->document.get();
    auto *root = imported->getReprRoot();
    auto layers = elements(root, "svg:g");
    ASSERT_GE(layers.size(), 3u);
    EXPECT_EQ(attr(layers[0], "inkscape:label"), "Art");
    EXPECT_EQ(attr(layers[0], "inkscape:highlight-color"), "#4fff4f");
    auto hidden = std::find_if(layers.begin(), layers.end(), [](auto *g) { return attr(g, "inkscape:label") == "Hidden"; });
    ASSERT_NE(hidden, layers.end());
    EXPECT_EQ(attr(*hidden, "style"), "display:none");
    EXPECT_EQ(attr(*hidden, "sodipodi:insensitive"), "true");
    auto paths = elements(root, "svg:path");
    ASSERT_EQ(paths.size(), 4u);
    std::vector<XML::Node *> originals{elements(doc->getReprRoot(), "svg:rect")[0],
                                      elements(doc->getReprRoot(), "svg:path")[0],
                                      elements(doc->getReprRoot(), "svg:path")[1],
                                      elements(doc->getReprRoot(), "svg:rect")[1]};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto const was = shape_box(doc.get(), originals[i]), now = shape_box(imported, paths[i]);
        ASSERT_TRUE(was && now) << i;
        EXPECT_NEAR(now->left(), was->left(), 1e-3) << i;
        EXPECT_NEAR(now->top(), was->top(), 1e-3) << i;
        EXPECT_NEAR(now->right(), was->right(), 1e-3) << i;
        EXPECT_NEAR(now->bottom(), was->bottom(), 1e-3) << i;
    }
    auto const fill = imported->getObjectByRepr(paths[0])->style->fill.getColor();
    ASSERT_EQ(fill.getSpace()->getComponentType(), Type::CMYK);
    for (unsigned i = 0; i < 4; ++i) EXPECT_NEAR(fill[i], 0.1 * (i + 1), 1e-4);
    EXPECT_NE(attr(paths[1], "style").find("evenodd"), std::string::npos);
    EXPECT_EQ(sp_svg_read_pathv(attr(paths[1], "d").c_str()).size(), 2u);
    EXPECT_NE(attr(paths[2], "style").find("stroke-dasharray"), std::string::npos);
}

TEST_F(AiNativeImportTest, LegacyWrittenRecordsKeepCmykSpotTintsAndClipping)
{
    auto doc = svg_document(
        R"(<defs><clipPath id="c"><rect x="10" y="10" width="30" height="30"/></clipPath></defs>)"
        R"(<g inkscape:groupmode="layer" inkscape:label="Ink">)"
        R"(<rect id="spot" x="50" y="10" width="40" height="40" fill="#000000" proof:overprint="fill stroke"/>)"
        R"x(<g clip-path="url(#c)"><rect x="0" y="0" width="60" height="60" fill="#00ffff"/></g>)x"
        R"(</g>)");
    ASSERT_TRUE(doc);
    auto const id = SpotInk::ensure(doc.get(), {"PROOF Orange", Colors::Color(Type::CMYK, {0, 0.6, 1, 0}), 0.3});
    doc->getObjectById("spot")->setAttribute("style", "fill:url(#" + id + ");stroke:url(#" + id + ");stroke-width:2");
    auto const written = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_NE(written.records.find("0 0.6 1 0 (PROOF Orange) 0.7 x\n"), std::string::npos);
    EXPECT_NE(written.records.find("0 0.6 1 0 (PROOF Orange) 0.7 X\n"), std::string::npos);
    EXPECT_EQ(written.records.find(" Xx\n"), std::string::npos);
    auto b = make(written.records);
    ASSERT_TRUE(b);
    bool tint = false, clip = false, overprint = false;
    for (auto *g : elements(b->document->getReprRoot(), "svg:linearGradient")) {
        if (auto ink = SpotInk::read(g); ink && ink->name == "PROOF Orange") {
            tint = std::abs(ink->tint - 0.3) < 1e-4;
        }
    }
    for (auto *g : elements(b->document->getReprRoot(), "svg:g")) clip = clip || !attr(g, "clip-path").empty();
    for (auto *p : elements(b->document->getReprRoot(), "svg:path")) overprint = overprint || attr(p, "proof:overprint") == "fill stroke";
    EXPECT_TRUE(tint);
    EXPECT_TRUE(clip);
    EXPECT_TRUE(overprint);
}

TEST_F(AiNativeImportTest, LegacyWrittenRecordsUseTheSelectedArtboardsOrigin)
{
    auto doc = svg_document(
        R"(<rect x="130" y="30" width="20" height="10" fill="#ff0000"/>)"
        R"(<sodipodi:namedview><inkscape:page x="120" y="20" width="80" height="60" inkscape:label="Only"/></sodipodi:namedview>)");
    ASSERT_TRUE(doc);
    ASSERT_EQ(doc->getPageManager().getPages().size(), 1u);
    auto const written = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    std::string error;
    auto read = AN::read(written.records, error);
    ASSERT_TRUE(read) << error;
    ASSERT_EQ(read->artboards.size(), 1u);
    EXPECT_EQ(read->artboards[0].rect, Geom::Rect(Geom::Point(0, 0), Geom::Point(60, 45)));
    auto b = make(written.records);
    ASSERT_TRUE(b);
    auto paths = elements(b->document->getReprRoot(), "svg:path");
    ASSERT_EQ(paths.size(), 1u);
    auto box = shape_box(b->document.get(), paths[0]);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 10, 1e-3);
    EXPECT_NEAR(box->top(), 10, 1e-3);
    EXPECT_NEAR(box->width(), 20, 1e-3);
    EXPECT_NEAR(box->height(), 10, 1e-3);
}

TEST_F(AiNativeImportTest, LegacyWriterRefusesFeaturesThatNeedTheModernFormat)
{
    std::vector<std::pair<std::string, std::string>> cases{
        {R"(<sodipodi:namedview><inkscape:page x="0" y="0" width="100" height="100"/><inkscape:page x="120" y="0" width="80" height="100"/></sodipodi:namedview>)", "more than one artboard"},
        {R"(<rect width="10" height="10" style="display:none"/>)", "hidden objects"},
        {R"(<g inkscape:groupmode="layer" inkscape:label="&#20013;"><rect width="10" height="10"/></g>)", "layer names outside Windows-1252"},
        {R"(<g inkscape:groupmode="layer"><g inkscape:groupmode="layer"><rect width="10" height="10"/></g></g>)", "nested layers"},
        {R"(<g inkscape:groupmode="layer" proof:layer-dim="70"><rect width="10" height="10"/></g>)", "layer image dimming"},
        {R"(<rect width="10" height="10" style="opacity:0.5"/>)", "transparency"},
        {R"(<g style="mix-blend-mode:multiply"><rect width="10" height="10"/></g>)", "blend modes"},
        {R"(<rect width="10" height="10" style="fill:#ff0000;fill-opacity:0.5"/>)", "transparency"},
        {R"(<defs><linearGradient id="g"><stop stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff" stop-opacity="0.5"/>)"
         R"x(</linearGradient></defs><rect width="50" height="50" fill="url(#g)"/>)x", "gradient transparency"},
        {R"(<defs><pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse"><rect width="5" height="5"/></pattern></defs>)"
         R"x(<rect width="50" height="50" fill="url(#p)"/>)x", "patterns (Illustrator 7)"},
        // Gradient meshes came with Illustrator 8.
        {R"(<defs><meshgradient id="g" x="0" y="0" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>)"
         R"(<stop path="c 10,0 20,0 30,0" style="stop-color:#ff0000"/><stop path="c 0,10 0,20 0,30" style="stop-color:#00ff00"/>)"
         R"(<stop path="c -10,0 -20,0 -30,0" style="stop-color:#0000ff"/><stop path="c 0,-10 0,-20 0,-30" style="stop-color:#ffff00"/>)"
         R"x(</meshpatch></meshrow></meshgradient></defs><rect width="30" height="30" fill="url(#g)"/>)x", "mesh gradients (Illustrator 7)"},
    };
    for (auto const &[body, reason] : cases) {
        SCOPED_TRACE(reason);
        auto doc = svg_document(body);
        ASSERT_TRUE(doc);
        auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
        EXPECT_TRUE(legacy.records.empty());
        EXPECT_TRUE(std::any_of(legacy.refused.begin(), legacy.refused.end(), [&](auto const &r) { return r.find(reason) != std::string::npos; }));
        EXPECT_TRUE(AN::write_records(doc.get()).refused.empty());
    }
    auto doc = svg_document(R"(<rect id="ink" width="10" height="10"/>)");
    ASSERT_TRUE(doc);
    auto const id = SpotInk::ensure(doc.get(), {"Ink \xe4\xb8\xad", Colors::Color(Type::CMYK, {0, 0.6, 1, 0}), 0.3});
    doc->getObjectById("ink")->setAttribute("style", "fill:url(#" + id + ")");
    auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(legacy.records.empty());
    EXPECT_NE(std::find(legacy.refused.begin(), legacy.refused.end(), "ink names outside Windows-1252 (Illustrator 7)"), legacy.refused.end());
    EXPECT_TRUE(AN::write_records(doc.get()).refused.empty());
}

TEST(AiNativeWriterStrings, AllDefinedWindows1252CharactersAndStrictRefusals)
{
    std::string const utf8 =
        "\xe2\x82\xac\xe2\x80\x9a\xc6\x92\xe2\x80\x9e\xe2\x80\xa6\xe2\x80\xa0\xe2\x80\xa1"
        "\xcb\x86\xe2\x80\xb0\xc5\xa0\xe2\x80\xb9\xc5\x92\xc5\xbd\xe2\x80\x98\xe2\x80\x99"
        "\xe2\x80\x9c\xe2\x80\x9d\xe2\x80\xa2\xe2\x80\x93\xe2\x80\x94\xcb\x9c\xe2\x84\xa2"
        "\xc5\xa1\xe2\x80\xba\xc5\x93\xc5\xbe\xc5\xb8\xc2\xa0\xc2\xa1\xc2\xa2\xc2\xa3\xc2"
        "\xa4\xc2\xa5\xc2\xa6\xc2\xa7\xc2\xa8\xc2\xa9\xc2\xaa\xc2\xab\xc2\xac\xc2\xad\xc2"
        "\xae\xc2\xaf\xc2\xb0\xc2\xb1\xc2\xb2\xc2\xb3\xc2\xb4\xc2\xb5\xc2\xb6\xc2\xb7\xc2"
        "\xb8\xc2\xb9\xc2\xba\xc2\xbb\xc2\xbc\xc2\xbd\xc2\xbe\xc2\xbf\xc3\x80\xc3\x81\xc3"
        "\x82\xc3\x83\xc3\x84\xc3\x85\xc3\x86\xc3\x87\xc3\x88\xc3\x89\xc3\x8a\xc3\x8b\xc3"
        "\x8c\xc3\x8d\xc3\x8e\xc3\x8f\xc3\x90\xc3\x91\xc3\x92\xc3\x93\xc3\x94\xc3\x95\xc3"
        "\x96\xc3\x97\xc3\x98\xc3\x99\xc3\x9a\xc3\x9b\xc3\x9c\xc3\x9d\xc3\x9e\xc3\x9f\xc3"
        "\xa0\xc3\xa1\xc3\xa2\xc3\xa3\xc3\xa4\xc3\xa5\xc3\xa6\xc3\xa7\xc3\xa8\xc3\xa9\xc3"
        "\xaa\xc3\xab\xc3\xac\xc3\xad\xc3\xae\xc3\xaf\xc3\xb0\xc3\xb1\xc3\xb2\xc3\xb3\xc3"
        "\xb4\xc3\xb5\xc3\xb6\xc3\xb7\xc3\xb8\xc3\xb9\xc3\xba\xc3\xbb\xc3\xbc\xc3\xbd\xc3"
        "\xbe\xc3\xbf";
    std::string const expected =
        "\x80\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8e\x91\x92\x93\x94\x95\x96\x97"
        "\x98\x99\x9a\x9b\x9c\x9e\x9f\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac"
        "\xad\xae\xaf\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf\xc0"
        "\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf\xd0\xd1\xd2\xd3\xd4"
        "\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8"
        "\xe9\xea\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc"
        "\xfd\xfe\xff";
    EXPECT_EQ(AN::legacy_name_bytes(utf8), expected);
    EXPECT_EQ(AN::legacy_name_bytes("ASCII (A) \\ B"), "ASCII (A) \\ B");
    EXPECT_EQ(AN::legacy_name_bytes(""), "");
    for (std::string const bad : {"\xe4\xb8\xad", "\xf0\x9f\x98\x80", "\xce\xa9", "\xc3", "\xff",
                                  "\xc2\x81", "\xc2\x8d", "\xc2\x8f", "\xc2\x90", "\xc2\x9d"}) {
        EXPECT_FALSE(AN::legacy_name_bytes(bad));
    }
}

TEST_F(AiNativeImportTest, LegacyNamesKeepWindows1252AndDistinctUtf8LookingBytes)
{
    std::string const label = "Caf\xc3\xa9 \xe2\x82\xac \xc5\x92 \xc5\xb8 \xe2\x80\x93 (A) \\ B";
    std::string const ambiguous = "\xc3\x83\xc2\xa9"; // U+00C3 U+00A9, encoded as C3 A9 in Windows-1252.
    auto doc = svg_document(R"(<g inkscape:groupmode="layer" inkscape:label=")" + label +
                            R"("><rect id="first" width="10" height="10"/></g>)" +
                            R"(<g inkscape:groupmode="layer" inkscape:label=")" + ambiguous +
                            R"("><rect id="second" x="20" width="10" height="10"/></g>)");
    ASSERT_TRUE(doc);
    for (auto const &[id, name] : std::vector<std::pair<std::string, std::string>>{{"first", label}, {"second", ambiguous}}) {
        auto ink = SpotInk::ensure(doc.get(), {name, Colors::Color(Type::CMYK, {0, 0.6, 1, 0}), 0.3});
        doc->getObjectById(id)->setAttribute("style", "fill:url(#" + ink + ");stroke:url(#" + ink + ");stroke-width:1");
    }
    doc->setDocumentFilename("Title Caf\xc3\xa9 \xe2\x82\xac.svg");
    for (auto version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        SCOPED_TRACE(version == AN::RecordsVersion::Modern ? "modern" : "legacy");
        auto const written = AN::write_records(doc.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        if (version == AN::RecordsVersion::Illustrator7) {
            EXPECT_NE(written.records.find("%%Title: (Title Caf\\351 \\200.svg)\n"), std::string::npos);
            EXPECT_NE(written.records.find("(\\303\\251) Ln\n"), std::string::npos);
            EXPECT_NE(written.records.find("(Caf\\351 \\200 \\214 \\237 \\226 \\(A\\) \\\\ B) Ln\n"), std::string::npos);
        }
        std::string error;
        auto read = AN::read(written.records, error);
        ASSERT_TRUE(read) << error;
        ASSERT_EQ(read->layers.size(), 2u);
        EXPECT_EQ(read->layers[0].name, label);
        EXPECT_EQ(read->layers[1].name, ambiguous);
        ASSERT_EQ(read->inks.size(), 2u);
        EXPECT_EQ(read->inks[0].name, label);
        EXPECT_EQ(read->inks[1].name, ambiguous);
        EXPECT_NEAR(read->layers[1].children.at(0).fill->tint, .3, 1e-6);
        EXPECT_NEAR(read->layers[1].children.at(0).stroke->tint, .3, 1e-6);
        auto imported = make(written.records);
        ASSERT_TRUE(imported);
        auto const layers = elements(imported->document->getReprRoot(), "svg:g");
        EXPECT_TRUE(std::any_of(layers.begin(), layers.end(), [&](auto node) {
            auto name = node->attribute("inkscape:label");
            return name && ambiguous == name;
        }));
    }
    doc->setDocumentFilename("Title \xe4\xb8\xad.svg");
    auto const refused = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(refused.records.empty());
    EXPECT_EQ(refused.refused, std::vector<std::string>{"document titles outside Windows-1252 (Illustrator 7)"});
    EXPECT_TRUE(AN::write_records(doc.get()).refused.empty());
}

TEST_F(AiNativeImportTest, LegacySaveCopyPreservesTheWorkingDocumentAndPreviousFileOnRefusal)
{
    auto doc = svg_document(R"(<rect width="20" height="10" fill="#000000"/>)");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    doc->setModifiedSinceSave(true);
    auto const before = sp_repr_save_buf(doc->getReprDoc());
    gchar *dir = g_dir_make_tmp("proof-legacy-output-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const directory(dir);
    std::filesystem::path const folder(std::u8string(directory.begin(), directory.end()));
    g_free(dir);
    std::string const path = directory + "/previous.ai";
    std::string const new_path = directory + "/new.ai";
    auto cleanup = scope_exit([&] {
        std::error_code error;
        std::filesystem::remove(folder / "previous.ai", error);
        std::filesystem::remove(folder / "new.ai", error);
        std::filesystem::remove(folder, error);
    });
    Extension::Internal::AiLegacyOutput output;
    ASSERT_NO_THROW(output.save(nullptr, doc.get(), path.c_str()));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    gchar *contents = nullptr;
    gsize size = 0;
    ASSERT_TRUE(g_file_get_contents(path.c_str(), &contents, &size, nullptr));
    std::string const saved(contents, size);
    g_free(contents);
    EXPECT_EQ(saved.find("%!PS-Adobe-3.0\n"), 0u);
    auto refused = svg_document(R"(<text x="10" y="20" style="writing-mode:vertical-rl">Keep live type</text>)");
    ASSERT_TRUE(refused);
    EXPECT_THROW(output.save(nullptr, refused.get(), path.c_str()), std::runtime_error);
    EXPECT_THROW(output.save(nullptr, refused.get(), new_path.c_str()), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(folder / "new.ai"));
    ASSERT_TRUE(g_file_get_contents(path.c_str(), &contents, &size, nullptr));
    EXPECT_EQ(std::string(contents, size), saved);
    g_free(contents);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator()), 1);
}

TEST_F(AiNativeImportTest, WrittenRecordsRefuseUnsupportedEffectsOnLayersAndTheRoot)
{
    // A layer is written by a different route from ordinary groups. Its effects and the
    // SVG root's must be checked too, or the PDF and Illustrator artwork can disagree. The
    // modern form writes an object's transparency, but not a layer's or the document's.
    std::vector<std::pair<std::string, std::string>> effects{
        {R"(style="opacity:0.5")", "transparency"},
        {R"(style="mix-blend-mode:multiply")", "blend modes"},
        {R"x(mask="url(#mask)")x", "opacity masks"},
        {R"x(filter="url(#blur)")x", "filters"},
    };
    std::string const defs =
        R"(<defs><mask id="mask"><rect width="200" height="100" fill="white"/></mask>)"
        R"(<filter id="blur"><feGaussianBlur stdDeviation="2"/></filter></defs>)";
    std::string const rect = R"(<rect x="10" y="10" width="20" height="20" fill="#ff0000"/>)";
    for (auto const &[attrs, base] : effects) {
        for (bool layer : {false, true}) {
            auto const body = layer ? defs + R"(<g inkscape:groupmode="layer" )" + attrs + ">" + rect + "</g>"
                                    : defs + rect;
            auto doc = svg_document(body, R"(width="200" height="100" viewBox="0 0 200 100" )" +
                                             (layer ? "" : attrs));
            ASSERT_TRUE(doc);
            for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
                // Illustrator 7 has no masks anywhere; the modern form writes an object's.
                bool const placed = base != "filters" && !(base == "opacity masks" && version == AN::RecordsVersion::Illustrator7);
                auto const reason = !placed ? (base == "opacity masks" ? base + " (Illustrator 7)" : base)
                                            : base + (layer ? " on layers" : " on the whole document");
                SCOPED_TRACE(reason);
                auto const written = AN::write_records(doc.get(), version);
                EXPECT_TRUE(written.records.empty());
                EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end());
            }
        }
    }
    auto doc = svg_document(R"(<defs><clipPath id="clip"><rect width="20" height="20"/></clipPath></defs>)" + rect,
                            R"x(width="200" height="100" viewBox="0 0 200 100" clip-path="url(#clip)")x");
    ASSERT_TRUE(doc);
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        auto const written = AN::write_records(doc.get(), version);
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), "clipping on the SVG root"), written.refused.end());
    }
}

namespace {
/// A mesh gradient of 2 by 2 patches as Inkscape writes one, from 10,10, each patch 30 by 20 user
/// units: RGB corners, one at half opacity. `extra` goes on the element.
std::string svg_mesh(std::string const &extra = {})
{
    return R"(<meshgradient id="m" x="10" y="10" gradientUnits="userSpaceOnUse")" + extra + R"(><meshrow><meshpatch>)"
           R"(<stop path="l 30,0" style="stop-color:#ff0000"/><stop path="l 0,20" style="stop-color:#00ff00"/>)"
           R"(<stop path="l -30,0" style="stop-color:#0000ff;stop-opacity:0.5"/><stop path="l 0,-20" style="stop-color:#ffffff"/>)"
           R"(</meshpatch><meshpatch>)"
           R"(<stop path="l 30,0"/><stop path="l 0,20" style="stop-color:#ffff00"/><stop path="l -30,0" style="stop-color:#00ffff"/>)"
           R"(</meshpatch></meshrow><meshrow><meshpatch>)"
           R"(<stop path="l 0,20"/><stop path="l -30,0" style="stop-color:#ff00ff"/><stop path="l 0,-20" style="stop-color:#808080"/>)"
           R"(</meshpatch><meshpatch>)"
           R"(<stop path="l 0,20"/><stop path="l -30,0" style="stop-color:#000000"/>)"
           R"(</meshpatch></meshrow></meshgradient>)";
}

/// Four regular trapezoids form a square ring; a fifth repeats the first, overlapping it.
std::string svg_mesh_ring(unsigned patches, double alpha)
{
    std::array<Geom::Point, 6> const top{{{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 0}, {100, 0}}};
    std::array<Geom::Point, 6> const bottom{{{25, 25}, {75, 25}, {75, 75}, {25, 75}, {25, 25}, {75, 25}}};
    std::string mesh = R"(<meshgradient id="m" gradientUnits="userSpaceOnUse"><meshrow>)";
    auto stop = [alpha](Geom::Point d) {
        return std::format(R"(<stop path="l {},{}" stop-color="#ff0000" stop-opacity="{}"/>)", d.x(), d.y(), alpha);
    };
    for (unsigned i = 0; i < patches; ++i) {
        mesh += "<meshpatch>" + stop(top[i + 1] - top[i]) + stop(bottom[i + 1] - top[i + 1]) +
                stop(bottom[i] - bottom[i + 1]);
        if (!i) mesh += stop(top[0] - bottom[0]);
        mesh += "</meshpatch>";
    }
    return mesh + "</meshrow></meshgradient>";
}

/// The first SVG mesh gradient of a document, its array built.
SPMeshGradient *first_mesh(SPDocument *doc)
{
    doc->ensureUpToDate();
    auto gradients = elements(doc->getDefs()->getRepr(), "svg:meshgradient");
    auto *mesh = gradients.empty() ? nullptr : dynamic_cast<SPMeshGradient *>(doc->getObjectByRepr(gradients[0]));
    if (mesh) mesh->ensureArray();
    return mesh;
}

/// Every corner and handle of two meshes' patches the same within `tolerance`, and their colours.
void expect_same_mesh_patches(SPMeshGradient *a, SPDocument *doc_b, Geom::Affine const &b_to_a, double tolerance)
{
    ASSERT_TRUE(a && doc_b);
    doc_b->ensureUpToDate();
    auto const gradients = elements(doc_b->getDefs()->getRepr(), "svg:meshgradient");
    ASSERT_EQ(a->array.patch_rows() * a->array.patch_columns(), gradients.size());
    for (unsigned c = 0; c < a->array.patch_columns(); ++c) {
        for (unsigned r = 0; r < a->array.patch_rows(); ++r) {
            auto *b = dynamic_cast<SPMeshGradient *>(doc_b->getObjectByRepr(gradients[c * a->array.patch_rows() + r]));
            ASSERT_TRUE(b);
            b->ensureArray();
            ASSERT_EQ(b->array.patch_rows(), 1u);
            ASSERT_EQ(b->array.patch_columns(), 1u);
            SPMeshPatchI p(&a->array.nodes, r, c), q(&b->array.nodes, 0, 0);
            for (unsigned k = 0; k < 4; ++k) {
                for (unsigned i = 0; i < 4; ++i) {
                    EXPECT_TRUE(Geom::are_near(p.getPoint(k, i), q.getPoint(k, i) * b_to_a, tolerance))
                        << r << ' ' << c << ' ' << k << ' ' << i;
                }
                auto const x = p.getColor(k), y = q.getColor(k);
                ASSERT_TRUE(x && y);
                auto const u = x->converted(Type::RGB), v = y->converted(Type::RGB);
                ASSERT_TRUE(u && v);
                for (unsigned i = 0; i < 3; ++i) EXPECT_NEAR((*u)[i], (*v)[i], 2e-3);
                EXPECT_NEAR(x->getOpacity(), y->getOpacity(), 1e-3);
            }
        }
    }
}
} // namespace

TEST_F(AiNativeImportTest, AMeshGradientIsWrittenAsIllustratorsMeshAndReadBack)
{
    // On its own outline: editable patch objects in a group, no clipping group. The page is 200 by 100 user
    // units, 150 by 75 pt; art space has y up from the artboard's top-left corner.
    auto doc = svg_document("<defs>" + svg_mesh() + R"mesh(</defs><path d="M 10,10 H 70 V 50 H 10 Z" style="fill:url(#m)"/>)mesh");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &records = written.records;
    EXPECT_NE(records.find("0 g\n/Mesh X!\n%_2 /Version X#\n%_/Cartesian /Type X#\n%_[1 1] /Size X#\n%_/Data X#\n"
                           "%_[0 0] /P X#\n%_[0] /R X#\n%_/DeviceRGB /CS X#\n"
                           "%_[1 0 0 1 7.5 -7.5 15 -7.5 1 7.5 -12.5 1] /N X#\n"),
              std::string::npos)
        << records.substr(records.find("/Mesh"), 600);
    // Independent patches preserve SVG's colour interpolation in Illustrator.
    EXPECT_NE(records.find("%_[0 0 1 0.5 30 -22.5 22.5 -22.5 1 30 -17.5 1] /N X#\n"), std::string::npos);
    EXPECT_NE(records.find("%_/E X#\n/End X!\n\n0 0 Xd\n6 () XW\n"), std::string::npos);
    EXPECT_EQ(records.find("W\nn\n"), std::string::npos);
    EXPECT_NE(records.find("/Mesh X!"), records.rfind("/Mesh X!"));

    // Read back: four editable patches, every corner, handle and colour as it was; written again,
    // the same records.
    auto b = make(records);
    ASSERT_TRUE(b);
    EXPECT_EQ(elements(b->document->getReprRoot(), "svg:path").size(), 4u);
    // The import's user unit is the point: 0.75 of this document's.
    expect_same_mesh_patches(first_mesh(doc.get()), b->document.get(), Geom::Scale(1 / 0.75), 1e-3);
    auto const again = AN::write_records(b->document.get());
    ASSERT_TRUE(again.refused.empty()) << again.refused.front();
    auto mesh_of = [](std::string const &r) { return r.substr(r.find("/Mesh X!"), r.find("6 () XW\n", r.find("/Mesh X!")) - r.find("/Mesh X!")); };
    EXPECT_EQ(mesh_of(again.records), mesh_of(records));
}

TEST_F(AiNativeImportTest, AMeshOnAnotherOutlineIsClippedToIt)
{
    // Inkscape's mesh tool fills a shape's box and the shape clips it: the shape's outline is a
    // clipping group about the mesh, which takes the object's opacity.
    auto doc = svg_document("<defs>" + svg_mesh() + R"(</defs><ellipse cx="40" cy="30" rx="30" ry="20" style="fill:url(#m);opacity:0.5"/>)");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &records = written.records;
    auto const clip = records.find("q\n");
    auto const mesh = records.find("/Mesh X!");
    ASSERT_NE(clip, std::string::npos);
    ASSERT_NE(mesh, std::string::npos);
    EXPECT_LT(clip, mesh);
    EXPECT_LT(records.find("h\nW\nn\n", clip), mesh);
    EXPECT_NE(records.find("6 () XW\nQ\n0 0.5 0 0 0 Xy\n0 0 Xd\n6 () XW\n"), std::string::npos)
        << records.substr(mesh, 1200);
    auto b = make(records);
    ASSERT_TRUE(b);
    auto *doc_b = b->document.get();
    doc_b->ensureUpToDate();
    std::vector<XML::Node *> paths;
    for (auto *path : elements(doc_b->getReprRoot(), "svg:path")) {
        if (attr(path, "proof:ai-mesh") == "true") paths.push_back(path);
    }
    ASSERT_EQ(paths.size(), 4u);
    auto *item = dynamic_cast<SPItem *>(doc_b->getObjectByRepr(paths[0]->parent()));
    ASSERT_TRUE(item);
    EXPECT_TRUE(item->getClipObject());
    expect_same_mesh_patches(first_mesh(doc.get()), doc_b, Geom::Scale(1 / 0.75), 1e-3);
}

TEST_F(AiNativeImportTest, AnIllustratorMeshGoesBackAsItCame)
{
    // Read, written, read again: the same grid, cut where its anchors were, the same corners.
    auto anchors = [](std::size_t a, std::size_t b) {
        return a == 1 && b == 0 ? std::string("[45 0 40 0 50 0 1] /A X#\n") : std::string();
    };
    auto art = grid_mesh(2, 2, grid_colour, anchors);
    auto const first = art.find("[0.1 0 0.05 0 1 30 0 40 0 1 30 10 1]");
    ASSERT_NE(first, std::string::npos);
    art.replace(first, 36, "[0.1 0 0.05 0 1 30 0 35 0 1 30 10 1]");
    auto const second = art.find("[0.2 0 0.1 0 1 60 0 60 10 1 50 0 1]");
    ASSERT_NE(second, std::string::npos);
    art.replace(second, 35, "[0.2 0 0.1 0 1 60 0 60 10 1 55 0 1]");
    auto a = make(records(layer(art)));
    ASSERT_TRUE(a);
    auto const written = AN::write_records(a->document.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_EQ(written.records.find("%_[3 2] /Size X#\n"), std::string::npos);
    auto b = make(written.records);
    ASSERT_TRUE(b);
    // The writer's art space has its origin at the artboard's top-left corner, the records' at
    // its bottom-left: the same document units.
    expect_same_mesh_patches(first_mesh(a->document.get()), b->document.get(), Geom::identity(), 1e-3);
}

TEST_F(AiNativeImportTest, MeshBoundaryAndTensorChecksDoNotDependOnTranslation)
{
    for (auto const &translation : {"0", "10000000"}) {
        SCOPED_TRACE(translation);
        auto const transform = std::string("<g transform=\"translate(") + translation + " 0)\">";
        // A small but real outline change must keep its clip even far from the origin.
        auto doc = svg_document("<defs>" + svg_mesh() + "</defs>" + transform +
            R"mesh(<path d="M 10,10 C 20,10.005 30,10.005 40,10 H 70 V 50 H 10 Z" fill="url(#m)"/></g>)mesh");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        EXPECT_NE(written.records.find("h\nW\nn\n"), std::string::npos);

        auto mesh = svg_mesh();
        mesh.replace(mesh.find("<meshpatch>"), 11, R"(<meshpatch tensor="5,5 -5,5 -5,-5 5,-4">)");
        doc = svg_document("<defs>" + mesh + "</defs>" + transform +
            R"mesh(<path d="M 10,10 H 70 V 50 H 10 Z" fill="url(#m)"/></g>)mesh");
        ASSERT_TRUE(doc);
        auto const refused = AN::write_records(doc.get());
        EXPECT_TRUE(refused.records.empty());
        EXPECT_NE(std::find(refused.refused.begin(), refused.refused.end(), "mesh gradients with tensor points"),
                  refused.refused.end());
    }
}

TEST_F(AiNativeImportTest, MeshFormsTheWriterCannotWriteAreNamed)
{
    auto const stroke_mesh = "<defs>" + svg_mesh() + R"(</defs><path d="M 10,10 H 70 V 50 H 10 Z" style="fill:none;stroke:url(#m);stroke-width:2"/>)";
    std::vector<std::pair<std::string, std::string>> const cases{
        {"<defs>" + svg_mesh(R"( type="bicubic")") + R"mesh(</defs><path d="M 10,10 H 70 V 50 H 10 Z" style="fill:url(#m)"/>)mesh",
         "smoothed mesh gradients"},
        {"<defs>" + svg_mesh() + R"(</defs><path d="M 10,10 H 70 V 50 H 10 Z" style="fill:url(#m);stroke:#000000"/>)",
         "strokes on mesh-filled objects"},
        {stroke_mesh, "mesh gradients on strokes"},
        {"<defs>" + svg_mesh() + R"mesh(</defs><path d="M 10,10 H 70 V 50 H 10 Z" fill="url(#m)" proof:overprint="fill"/>)mesh",
         "overprint on mesh gradients"},
    };
    for (auto const &[body, reason] : cases) {
        SCOPED_TRACE(reason);
        auto doc = svg_document(body);
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end())
            << (written.refused.empty() ? std::string("nothing refused") : written.refused.front());
    }
    // A tensor point moved from where the Coons patch has it; corners in RGB and CMYK.
    for (auto const &[from, to, reason] :
         {std::tuple{std::string(R"(<meshpatch><stop path="l 30,0" style="stop-color:#ff0000"/>)"),
                     std::string(R"(<meshpatch tensor="5,5 -5,5 -5,-5 5,-4"><stop path="l 30,0" style="stop-color:#ff0000"/>)"),
                     "mesh gradients with tensor points"},
          std::tuple{std::string("stop-color:#00ffff"), std::string("stop-color:device-cmyk(1 0 0 0)"),
                     "mesh gradients of more than one colour model"}}) {
        SCOPED_TRACE(reason);
        auto mesh = svg_mesh();
        ASSERT_NE(mesh.find(from), std::string::npos);
        mesh.replace(mesh.find(from), from.size(), to);
        auto doc = svg_document("<defs>" + mesh + R"mesh(</defs><path d="M 10,10 H 70 V 50 H 10 Z" style="fill:url(#m)"/>)mesh");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end())
            << (written.refused.empty() ? std::string("nothing refused") : written.refused.front());
    }
}

TEST_F(AiNativeImportTest, MeshWriterRefusesFoldedAndDegenerateGeometry)
{
    // A translucent grid doubles back over itself. Independent patches would paint the
    // overlap repeatedly, unlike the original single mesh. Also test a curved side that
    // reverses inside a patch although all its corners still form a normal rectangle.
    for (auto const &transform : {"", "translate(10000000 10000000)", "matrix(-1 .2 .3 1 100 0)"}) {
        for (auto const &kind : {"grid fold", "curved fold", "collapsed"}) {
            SCOPED_TRACE(std::string(kind) + " " + transform);
            auto mesh = svg_mesh();
            if (std::string(kind) == "grid fold") {
                auto const start = mesh.find("</meshpatch><meshpatch>");
                auto const top = mesh.find("l 30,0", start);
                mesh.replace(top, 6, "l -30,0");
                auto const bottom = mesh.find("l -30,0", top + 8);
                mesh.replace(bottom, 7, "l 30,0");
            } else if (std::string(kind) == "curved fold") {
                mesh.replace(mesh.find("l 30,0"), 6, "C -20,10 30,10 40,10");
            } else {
                // All four patches have zero height.
                for (auto const &side : {"l 0,20", "l 0,-20"}) {
                    for (auto pos = mesh.find(side); pos != std::string::npos; pos = mesh.find(side)) {
                        mesh.replace(pos, std::strlen(side), "l 0,0");
                    }
                }
            }
            auto doc = svg_document("<defs>" + mesh + "</defs><g transform=\"" + transform +
                                    R"mesh("><rect width="100" height="100" fill="url(#m)"/></g>)mesh");
            ASSERT_TRUE(doc);
            auto const written = AN::write_records(doc.get());
            EXPECT_TRUE(written.records.empty());
            EXPECT_NE(std::find(written.refused.begin(), written.refused.end(),
                                "translucent mesh gradients outside the non-overlapping patch subset"), written.refused.end());
        }
    }
}

TEST_F(AiNativeImportTest, MeshWriterChecksTheWholeGridForOverlap)
{
    // Five individually regular trapezoids turn once round a square and repeat the first.
    // Every patch has the same orientation and no local fold; a per-patch test misses this.
    auto doc = svg_document("<defs>" + svg_mesh_ring(5, .5) + R"mesh(</defs><rect width="100" height="100" fill="url(#m)"/>)mesh");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    EXPECT_TRUE(written.records.empty());
    EXPECT_NE(std::find(written.refused.begin(), written.refused.end(),
                        "translucent mesh gradients outside the non-overlapping patch subset"), written.refused.end());
}

TEST_F(AiNativeImportTest, MeshWriterKeepsTheOpaqueSubsetWithGroupOpacity)
{
    // This valid ring cannot fit the conservative global direction cones. Its opaque corners
    // should keep the existing writing route. Object alpha applies to the group just once.
    auto doc = svg_document("<defs>" + svg_mesh_ring(4, 1) +
                            R"mesh(</defs><rect width="100" height="100" fill="url(#m)" opacity=".5"/>)mesh");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto reopened = make(written.records);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(elements(reopened->document->getDefs()->getRepr(), "svg:meshgradient").size(), 4u);
    auto const before = page_pixels(doc.get()), after = page_pixels(reopened->document.get());
    ASSERT_EQ(before.size(), 200u * 100u);
    ASSERT_EQ(after.size(), before.size());
    for (auto const &point : {Geom::Point(50, 10), Geom::Point(10, 50), Geom::Point(90, 50)}) {
        auto const i = unsigned(point.y()) * 200 + unsigned(point.x());
        EXPECT_NEAR(before[i] >> 24, 128u, 1);
        EXPECT_EQ(after[i], before[i]);
    }
    EXPECT_EQ(after[50 * 200 + 50], 0u); // The ring's hole remains transparent.
}

TEST_F(AiNativeImportTest, MeshWriterKeepsCurvedAndReflectedGeometry)
{
    std::string const mesh = R"(<meshgradient id="m" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>)"
        R"(<stop path="C30,-8 70,8 100,0" stop-color="#ff0000"/>)"
        R"(<stop path="C95,30 105,70 100,100" stop-color="#00ff00"/>)"
        R"(<stop path="C70,95 30,105 0,100" stop-color="#0000ff" stop-opacity=".5"/>)"
        R"(<stop path="C-5,70 5,30 0,0" stop-color="#ffffff"/>)"
        R"(</meshpatch></meshrow></meshgradient>)";
    for (auto const &transform : {"", "translate(10000000 10000000)", "rotate(137)", "matrix(-1 .2 .3 1 100 0)"}) {
        SCOPED_TRACE(transform);
        auto doc = svg_document("<defs>" + mesh + "</defs><g transform=\"" + transform +
                                R"mesh("><rect width="100" height="100" fill="url(#m)"/></g>)mesh");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        EXPECT_NE(written.records.find("/Mesh X!"), std::string::npos);
    }
}

/// Each character a text object sets, and where: its start, on its line's baseline, in the
/// document. Plain SVG text moves later chunks off the first chunk's line: their placement says.
std::vector<std::pair<gunichar, Geom::Point>> set_characters(SPDocument *doc, XML::Node *repr)
{
    doc->ensureUpToDate();
    std::vector<std::pair<gunichar, Geom::Point>> out;
    auto *text = cast<SPText>(doc->getObjectByRepr(repr));
    if (!text) return out;
    auto const &layout = text->layout;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        SPObject *source = nullptr;
        Glib::ustring::iterator at;
        layout.getSourceOfCharacter(it, &source, &at);
        if (!is<SPString>(source)) continue;
        auto const start = Geom::Point(layout.characterAnchorPoint(it).x(), layout.chunkPlacement(it).y());
        out.emplace_back(*at, start * text->i2doc_affine());
    }
    return out;
}

TEST_F(AiNativeImportTest, MixedLtrBidiRunOrderMatchesPangoReferenceLayout)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial, which has Hebrew and Arabic, isn't installed";
    for (auto const *contents : {"ABC אבגד 123", "ABC אבגד 123 (45)", "ABC العربية 123 (45)",
                                 "123 אבגד ABC 456", "אבגד 123 ABC 456", "ABC لا 123"}) {
        SCOPED_TRACE(contents);
        auto *context = pango_font_map_create_context(pango_cairo_font_map_get_default());
        pango_context_set_base_dir(context, PANGO_DIRECTION_LTR);
        auto *reference = pango_layout_new(context);
        pango_layout_set_auto_dir(reference, false);
        auto *font = pango_font_description_from_string("Arial");
        pango_font_description_set_absolute_size(font, 18 * PANGO_SCALE);
        pango_layout_set_font_description(reference, font);
        pango_layout_set_text(reference, contents, -1);
        std::vector<std::pair<double, gunichar>> expected, actual;
        for (auto const *at = contents; *at; at = g_utf8_next_char(at)) {
            PangoRectangle position;
            pango_layout_index_to_pos(reference, at - contents, &position);
            expected.emplace_back(position.x + position.width / 2.0, g_utf8_get_char(at));
        }
        pango_font_description_free(font);
        g_object_unref(reference);
        g_object_unref(context);
        auto doc = svg_document(std::string("<text id='t' x='20' y='40' style='font-family:Arial;"
            "font-size:18px;direction:ltr'>") + contents + "</text>");
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        auto *text = cast<SPText>(doc->getObjectById("t"));
        for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
            actual.emplace_back(text->layout.characterBoundingBox(it).midpoint().x(), text->layout.characterAt(it));
        }
        auto visual_order = [](auto positions) {
            std::stable_sort(positions.begin(), positions.end(), [](auto const &a, auto const &b) { return a.first < b.first; });
            Glib::ustring result;
            for (auto const &[position, character] : positions) result += character;
            return result.raw();
        };
        EXPECT_EQ(visual_order(actual), visual_order(expected));
    }
}

TEST_F(AiNativeImportTest, ParagraphDirectionIsIndependentOfComposer)
{
    for (unsigned direction = 0; direction < 2; ++direction) {
        for (unsigned composer = 0; composer < 2; ++composer) {
            auto source = aligned_text_document(2);
            auto const at = source.find("/5 << /0 2 >>");
            ASSERT_NE(at, std::string::npos);
            source.replace(at, 13, "/5 << /0 2 /33 " + std::to_string(direction) + " /35 " + std::to_string(composer) + " >>");
            auto const parsed = AN::read_text_document(text_setup(source), Geom::Point(100, 50));
            ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
            EXPECT_EQ(parsed->stories[0]->rtl, direction != 0);
            EXPECT_EQ(parsed->stories[0]->world_ready, composer != 0);
            auto built = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(source)));
            ASSERT_TRUE(built);
            auto *repr = elements(built->document->getReprRoot(), "svg:text").at(0);
            EXPECT_EQ(attr(repr, "style").find("direction:rtl") != std::string::npos, direction != 0);
            EXPECT_EQ(attr(repr, "proof:ai-text-composer"), composer ? "middle-eastern" : "latin");
            auto const written = AN::write_records(built->document.get());
            ASSERT_TRUE(written.refused.empty()) << written.refused.front();
            auto const reread = AN::read_text_document(written.records, {});
            ASSERT_TRUE(reread->stories.at(0)) << reread->errors.at(0);
            EXPECT_EQ(reread->stories[0]->world_ready, composer != 0);
            EXPECT_EQ(reread->stories[0]->rtl, direction != 0);
        }
    }
}

TEST_F(AiNativeImportTest, RtlPointTextLeftAnchorExcludesTheReturnsLayoutAdvance)
{
    // Illustrator's ME composer places the return before visible RTL text. Even
    // a physically left-aligned line has a negative segment offset for that
    // advance. Its editable anchor remains the frame/line anchor, at any size.
    for (auto const *offset : {"-5.00098", "-2.3", "0"}) {
        auto source = aligned_text_document(1);
        auto at = source.find("/5 << /0 1 >>");
        ASSERT_NE(at, std::string::npos);
        source.replace(at, 13, "/5 << /0 1 /33 1 /35 1 >>");
        at = source.find("[-57.6 0]");
        ASSERT_NE(at, std::string::npos);
        source.replace(at, 9, std::string("[") + offset + " 0]");
        auto parsed = AN::read_text_document(text_setup(source), Geom::Point(100, 50));
        ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
        auto const &story = *parsed->stories[0];
        EXPECT_EQ(story.lines.at(0).alignment, 0u);
        EXPECT_TRUE(Geom::are_near(story.lines[0].origin * story.to_art, Geom::Point(80, 60), 1e-9));
    }
}

TEST_F(AiNativeImportTest, WrittenBidirectionalTypeRetainsDirectionOrderAndAnchors)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial, which has Hebrew, isn't installed";
    struct Case { char const *direction; char const *anchor; char const *contents; bool unordered = false; };
    Case const cases[] = {
        {"rtl", "start", "Latin text 123"},
        {"rtl", "middle", "Latin text 123"},
        {"rtl", "end", "Latin text 123"},
        {"rtl", "start", "אבגד"},
        {"rtl", "middle", "אבגד"},
        {"rtl", "end", "אבגד"},
        {"ltr", "start", "אבגד"},
        {"ltr", "middle", "ABC אבגד 123"},
        {"rtl", "middle", "ABC אבגד 123"},
        {"rtl", "start", "العربية"},
        {"rtl", "middle", "العَرَبِيَّة"},
        {"rtl", "middle", "لا"},
        {"rtl", "end", "السلام عليكم"},
        {"rtl", "end", "ABC العربية 123"},
        {"ltr", "end", "<tspan fill='#ff0000'>אב</tspan>גד"},
        {"ltr", "start", "ABC ב 123", true},
        {"ltr", "middle", "אבגד", true},
        {"rtl", "start", "<tspan sodipodi:role='line' x='140' y='30'>אבגד</tspan>"
                         "<tspan sodipodi:role='line' x='140' y='55'>אב</tspan>"},
    };
    for (auto const &c : cases) {
        SCOPED_TRACE(::testing::Message() << c.direction << "/" << c.anchor << "/" << c.contents);
        auto doc = svg_document(std::string("<text id='t' x='140' y='30' xml:space='preserve' "
            "style='font-family:Arial;font-size:16px;direction:") + c.direction + ";text-anchor:" + c.anchor +
            (c.unordered ? ";unicode-bidi:bidi-override" : "") + "'>" + c.contents + "</text>");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto const parsed = AN::read_text_document(written.records, {});
        ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
        EXPECT_EQ(parsed->stories[0]->rtl, std::string(c.direction) == "rtl");
        EXPECT_EQ(parsed->stories[0]->unordered, c.unordered);
        EXPECT_EQ(parsed->stories[0]->world_ready, !c.unordered);
        auto again = make(written.records);
        ASSERT_TRUE(again);
        auto const before = set_characters(doc.get(), doc->getObjectById("t")->getRepr());
        auto const after = set_characters(again->document.get(), elements(again->document->getReprRoot(), "svg:text").at(0));
        ASSERT_EQ(before.size(), after.size());
        for (std::size_t i = 0; i < before.size(); ++i) {
            EXPECT_EQ(before[i].first, after[i].first);
            EXPECT_NEAR(before[i].second.x(), after[i].second.x(), 0.02) << i;
            EXPECT_NEAR(before[i].second.y(), after[i].second.y(), 0.02) << i;
        }
    }
}

TEST_F(AiNativeImportTest, WrittenRtlAreaTypeKeepsPhysicalParagraphAlignment)
{
    if (available_text_font() != "ArialMT") GTEST_SKIP() << "Arial, which has Hebrew, isn't installed";
    for (auto const *alignment : {"start", "end", "center", "left", "right"}) {
        SCOPED_TRACE(alignment);
        auto doc = svg_document(std::string("<text id='t' proof:text-frame='M20 10 H180 V80 H20 Z' "
            "style='font-family:Arial;font-size:16px;direction:rtl;text-align:") + alignment + "'>"
            "<tspan x='30' y='40' style='text-anchor:end'>אבגד</tspan></text>");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto const parsed = AN::read_text_document(written.records, {});
        ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
        auto const &story = *parsed->stories[0];
        EXPECT_TRUE(story.rtl);
        EXPECT_TRUE(story.area);
        unsigned const expected = std::string(alignment) == "center" ? 2 :
            (std::string(alignment) == "start" || std::string(alignment) == "right") ? 1 : 0;
        EXPECT_EQ(story.lines.at(0).paragraph_alignment, expected);
        auto again = make(written.records);
        ASSERT_TRUE(again);
        auto const before = set_characters(doc.get(), doc->getObjectById("t")->getRepr());
        auto const after = set_characters(again->document.get(), elements(again->document->getReprRoot(), "svg:text").at(0));
        ASSERT_EQ(before.size(), after.size());
        for (std::size_t i = 0; i < before.size(); ++i) {
            EXPECT_NEAR(before[i].second.x(), after[i].second.x(), 0.02);
            EXPECT_NEAR(before[i].second.y(), after[i].second.y(), 0.02);
        }
    }
}

TEST_F(AiNativeImportTest, AreaParagraphIndentsSurviveNativeSaveAndScaling)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP();
    auto const paragraphs = " /5 << /0 [ << /1 12 /0 << /0 << /5 << /1 -3.25 /2 12.5 /3 7.75 >> >> >> >> "
                            "<< /1 4 /0 << /0 << /5 << /1 4.5 /2 2.25 /3 9.5 >> >> >> >> ] >>";
    // The first paragraph wraps; its three margins must remain one declaration.
    auto const source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
        text_setup(area_text_document("(Hello world\\rEnd\\r)", 16, {{0, 12, 6}, {0, 26, 6}, {0, 40, 4}}, font, {}, paragraphs)));
    auto imported = make(source);
    ASSERT_TRUE(imported);
    auto const texts = elements(imported->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    auto const spans = elements(texts[0], "svg:tspan");
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(attr(spans[0], AN::PARAGRAPH_INDENTS_ATTRIBUTE), "-3.25 12.5 7.75");
    EXPECT_EQ(attr(spans[1], AN::PARAGRAPH_INDENTS_ATTRIBUTE), "-3.25 12.5 7.75");
    EXPECT_EQ(attr(spans[2], AN::PARAGRAPH_INDENTS_ATTRIBUTE), "4.5 2.25 9.5");
    for (auto const scale : {1.0, 2.0}) {
        texts[0]->setAttribute("transform", scale == 1 ? "" : "scale(2)");
        imported->document->ensureUpToDate();
        auto const written = AN::write_records(imported->document.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto const parsed = AN::read_text_document(written.records, {});
        ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
        auto const &lines = parsed->stories[0]->lines;
        ASSERT_EQ(lines.size(), 3u);
        for (unsigned i = 0; i < 3; ++i) {
            auto const expected = i < 2 ? std::array{-3.25, 12.5, 7.75} : std::array{4.5, 2.25, 9.5};
            for (unsigned k = 0; k < 3; ++k) EXPECT_NEAR(lines[i].indents[k], expected[k] * scale, 1e-4);
        }
        auto returned = make(written.records);
        ASSERT_TRUE(returned);
        auto const before = set_characters(imported->document.get(), texts[0]);
        auto const after = set_characters(returned->document.get(), elements(returned->document->getReprRoot(), "svg:text")[0]);
        ASSERT_EQ(before.size(), after.size());
        for (unsigned i = 0; i < before.size(); ++i) {
            EXPECT_NEAR(before[i].second.x(), after[i].second.x(), 0.02);
            EXPECT_NEAR(before[i].second.y(), after[i].second.y(), 0.02);
        }
    }
    // Inconsistent metadata in one wrapped paragraph must not silently choose a margin.
    spans[1]->setAttribute(AN::PARAGRAPH_INDENTS_ATTRIBUTE, "0 0 0");
    EXPECT_FALSE(AN::write_records(imported->document.get()).refused.empty());
}

TEST_F(AiNativeImportTest, MalformedParagraphIndentsAreRefused)
{
    for (auto const *value : {"1 2", "1 2 3 4", "nan 0 0", "0 0 inf", "1x 2 3", "1000001 0 0"}) {
        auto doc = svg_document(std::string("<text proof:text-frame='M0 0 H100 V100 H0 Z' proof:paragraph-indents='") +
            value + "' x='10' y='30' style='font-family:Arial;font-size:12px'>Text</text>");
        ASSERT_TRUE(doc);
        EXPECT_FALSE(AN::write_records(doc.get()).refused.empty()) << value;
    }
    auto const invalid = " /5 << /0 [ << /1 6 /0 << /0 << /5 << /1 (bad) >> >> >> >> ] >>";
    auto parsed = AN::read_text_document(text_setup(area_text_document("(Hello\\r)", 6, {{0, 12, 6}}, "ArialMT", {}, invalid)),
                                         Geom::Point(100, 50));
    EXPECT_FALSE(parsed->stories.at(0));
}

TEST_F(AiNativeImportTest, AreaTrailingSpacesKeepAlignmentAndTypedBreaks)
{
    for (auto const *alignment : {"left", "center", "right"}) {
        auto doc = svg_document(std::string("<text id='t' xml:space='preserve' proof:text-frame='M10 5 H190 V95 H10 Z' ") +
            "style='font-family:Arial;font-size:12px;text-align:" + alignment + "'>"
            "<tspan x='20' y='25' proof:line-end='wrap'>Alpha   </tspan>"
            "<tspan x='20' y='43' proof:line-end='break'>Beta   </tspan>"
            "<tspan x='20' y='61'>Gamma   </tspan></text>");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto const parsed = AN::read_text_document(written.records, {});
        ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
        auto const &lines = parsed->stories[0]->lines;
        ASSERT_EQ(lines.size(), 3u);
        EXPECT_EQ(lines[0].end, AN::TextLine::End::Wrap);
        EXPECT_EQ(lines[1].end, AN::TextLine::End::Break);
        EXPECT_EQ(lines[2].end, AN::TextLine::End::Paragraph);
        auto returned = make(written.records);
        ASSERT_TRUE(returned);
        auto const before = set_characters(doc.get(), doc->getObjectById("t")->getRepr());
        auto const after = set_characters(returned->document.get(), elements(returned->document->getReprRoot(), "svg:text")[0]);
        ASSERT_EQ(before.size(), after.size());
        for (unsigned i = 0; i < before.size(); ++i) {
            EXPECT_EQ(before[i].first, after[i].first);
            EXPECT_NEAR(before[i].second.x(), after[i].second.x(), 0.02);
            EXPECT_NEAR(before[i].second.y(), after[i].second.y(), 0.02);
        }
    }
}

TEST_F(AiNativeImportTest, AutomaticAreaHyphensDrawWithoutEnteringTheEditableStory)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP();
    std::string const paragraphs = " /5 << /0 [ << /1 12 /0 << /0 << /5 << /9 true >> >> >> >> ] >>";
    auto native = area_text_document("(firstsecond\\r)", 12, {{0, 12, 5}, {0, 26, 7}}, font, {}, paragraphs);
    auto const at = native.find("/15 << /0 5 /2 0");
    ASSERT_NE(at, std::string::npos);
    native.replace(at, std::strlen("/15 << /0 5 /2 0"), "/15 << /0 5 /1 [ << /0 2 /1 4 /2 2 >> ] /2 1");
    auto imported = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(native)));
    ASSERT_TRUE(imported);
    auto const texts = elements(imported->document->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 1u);
    auto const spans = elements(texts[0], "svg:tspan");
    auto const hyphen = std::find_if(spans.begin(), spans.end(), [](auto *span) {
        return attr(span, AN::AUTOMATIC_HYPHEN_ATTRIBUTE) == "true";
    });
    ASSERT_NE(hyphen, spans.end());
    EXPECT_STREQ((*hyphen)->firstChild()->content(), "-");
    auto const written = AN::write_records(imported->document.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const parsed = AN::read_text_document(written.records, {});
    ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
    auto const &lines = parsed->stories[0]->lines;
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_TRUE(lines[0].automatic_hyphen);
    EXPECT_FALSE(lines[1].automatic_hyphen);
    EXPECT_EQ(lines[0].runs[0].text, "first");
    EXPECT_EQ(lines[1].runs[0].text, "second");
    auto returned = make(written.records);
    ASSERT_TRUE(returned);
    auto const before = set_characters(imported->document.get(), texts[0]);
    auto const after = set_characters(returned->document.get(), elements(returned->document->getReprRoot(), "svg:text")[0]);
    ASSERT_EQ(before.size(), after.size());
    EXPECT_EQ(before.size(), 12u); // Eleven typed letters and one display-only hyphen.
    for (unsigned i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].first, after[i].first);
        EXPECT_NEAR(before[i].second.x(), after[i].second.x(), 0.02);
        EXPECT_NEAR(before[i].second.y(), after[i].second.y(), 0.02);
    }
    (*hyphen)->setAttribute("style", "fill:#ff0000");
    EXPECT_FALSE(AN::write_records(imported->document.get()).refused.empty());
    auto const where = native.find("/9 true");
    native.replace(where, 7, "/9 false");
    auto const invalid = AN::read_text_document(text_setup(native), Geom::Point(100, 50));
    EXPECT_FALSE(invalid->stories.at(0));
}

TEST_F(AiNativeImportTest, WrittenPointTypeKeepsLeadingAndTrailingEmptyLines)
{
    auto doc = svg_document("<text id='t' style='font-family:Arial;font-size:12px;text-anchor:middle'>"
        "<tspan x='100' y='10'/><tspan x='100' y='30' proof:line-end='break'>Hello</tspan>"
        "<tspan x='100' y='50'/></text>");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const parsed = AN::read_text_document(written.records, {});
    ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
    auto const &lines = parsed->stories[0]->lines;
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_TRUE(lines[0].runs.empty());
    EXPECT_TRUE(lines[2].runs.empty());
    EXPECT_EQ(lines[1].end, AN::TextLine::End::Break);
    EXPECT_EQ(lines[1].runs[0].text, "Hello");
    EXPECT_EQ(lines[0].alignment, 2u);
    auto returned = make(written.records);
    ASSERT_TRUE(returned);
    auto const before = set_characters(doc.get(), doc->getObjectById("t")->getRepr());
    auto const after = set_characters(returned->document.get(), elements(returned->document->getReprRoot(), "svg:text")[0]);
    ASSERT_EQ(before.size(), after.size());
    for (unsigned i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].first, after[i].first);
        EXPECT_NEAR(before[i].second.x(), after[i].second.x(), 0.02);
        EXPECT_NEAR(before[i].second.y(), after[i].second.y(), 0.02);
    }
}

TEST_F(AiNativeImportTest, WrittenTypeReadsBackWithEveryCharacterWhereItWas)
{
    // Point type: lines that are paragraphs about one anchor, runs of fonts, colours, tracking,
    // underline, superscript and small caps, a turned frame, a stroke without a fill.
    auto doc = svg_document(
        R"x(<text id="plain" xml:space="preserve" x="20" y="30" style="font-family:Arial;font-size:20px;fill:device-cmyk(0 1 1 0)">Hello PROOF</text>)x"
        R"(<text id="lines" xml:space="preserve" x="100" y="55" style="font-family:Arial;font-size:12px;text-anchor:middle;fill:#0000ff">)"
        R"(<tspan sodipodi:role="line" x="100" y="55">Centred line one</tspan>)"
        R"(<tspan sodipodi:role="line" x="100" y="75" style="font-family:'Times New Roman';font-weight:bold;fill:#000000;letter-spacing:1.2px">)"
        R"(Second <tspan style="text-decoration-line:underline">line</tspan></tspan></text>)"
        R"x(<text id="turned" xml:space="preserve" transform="rotate(-30 30 90)" x="30" y="90" style="font-family:Arial;font-size:14px;fill:#00a000">)x"
        R"(Turned <tspan style="font-variant-position:super">30</tspan> <tspan style="font-variant-caps:small-caps">Small</tspan></text>)"
        R"(<text id="right" xml:space="preserve" x="190" y="95" style="font-family:'Times New Roman';font-style:italic;font-size:10px;)"
        R"(text-anchor:end;fill:none;stroke:#000000;stroke-width:0.4px">Right</text>)");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &records = written.records;
    // The text documents last in the setup, and an object for each story in the body.
    auto const section = records.find("%AI11_BeginTextDocument");
    ASSERT_NE(section, std::string::npos);
    EXPECT_LT(records.find("%AI11_EndTextDocument"), records.find("%%EndSetup"));
    std::size_t objects = 0;
    for (auto at = records.find("/AI11Text :"); at != std::string::npos; at = records.find("/AI11Text :", at + 1)) ++objects;
    EXPECT_EQ(objects, 4u);

    auto b = make(records);
    ASSERT_TRUE(b);
    auto *imported = b->document.get();
    auto const texts = elements(imported->getReprRoot(), "svg:text");
    ASSERT_EQ(texts.size(), 4u) << sp_repr_save_buf(imported->getReprDoc()).raw();
    char const *const ids[] = {"plain", "lines", "turned", "right"};
    for (std::size_t i = 0; i < texts.size(); ++i) {
        SCOPED_TRACE(ids[i]);
        EXPECT_EQ(attr(texts[i], "proof:ai-story"), std::to_string(i));
        auto const was = set_characters(doc.get(), doc->getObjectById(ids[i])->getRepr());
        auto const now = set_characters(imported, texts[i]);
        ASSERT_EQ(now.size(), was.size());
        ASSERT_FALSE(was.empty());
        for (std::size_t c = 0; c < was.size(); ++c) {
            EXPECT_EQ(now[c].first, was[c].first) << c;
            EXPECT_NEAR(now[c].second.x(), was[c].second.x(), 0.02) << c;
            EXPECT_NEAR(now[c].second.y(), was[c].second.y(), 0.02) << c;
        }
    }
    // Process colour stays in its model.
    imported->ensureUpToDate();
    auto const fill = imported->getObjectByRepr(texts[0])->style->fill.getColor();
    ASSERT_EQ(fill.getSpace()->getComponentType(), Type::CMYK);
    EXPECT_NEAR(fill[0], 0.0, 1e-4);
    EXPECT_NEAR(fill[1], 1.0, 1e-4);
    EXPECT_NEAR(fill[2], 1.0, 1e-4);
    EXPECT_NEAR(fill[3], 0.0, 1e-4);
}

TEST_F(AiNativeImportTest, WrittenTypeKeepsItsFeaturesInTheStory)
{
    auto doc = svg_document(
        R"x(<g transform="scale(2)"><text xml:space="preserve" x="10" y="20" style="font-family:Arial;font-size:10px">Big</text></g>)x"
        R"(<text xml:space="preserve" x="10" y="60" style="font-family:Arial;font-size:16px;letter-spacing:1.6px;)"
        R"(font-feature-settings:'kern' 0;font-variant-ligatures:no-common-ligatures no-contextual">Ab)"
        R"(<tspan style="baseline-shift:4px">c</tspan><tspan style="text-transform:uppercase;text-decoration-line:line-through">d</tspan>)"
        R"(<tspan style="fill:none;stroke:#ff0000;stroke-width:2px">e</tspan></text>)"
        R"(<text xml:space="preserve" x="10" y="90" proof:font-missing="PROOFNoSuchFont-Bold" style="font-family:Arial;font-size:8px">Gone</text>)",
        R"(width="200" height="100" viewBox="0 0 200 100" proof:superscript="0.5 0.4")");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const texts = AN::read_text_document(written.records, {});
    ASSERT_TRUE(texts->error.empty()) << texts->error;
    ASSERT_EQ(texts->stories.size(), 3u);
    for (auto const &e : texts->errors) EXPECT_TRUE(e.empty()) << e;
    EXPECT_NEAR(texts->type.superscript_size, 0.5, 1e-9);
    EXPECT_NEAR(texts->type.superscript_shift, 0.4, 1e-9);

    // A uniform scale is in the size: 10 px, twice, in points.
    auto const &big = texts->stories[0]->lines.at(0).runs.at(0).style;
    EXPECT_EQ(big.font, "ArialMT");
    EXPECT_NEAR(big.size, 15, 1e-6);
    EXPECT_NEAR(texts->stories[0]->to_art.descrim(), 1, 1e-6);

    auto const &runs = texts->stories[1]->lines.at(0).runs;
    ASSERT_EQ(runs.size(), 4u);
    EXPECT_EQ(runs[0].text, "Ab");
    EXPECT_NEAR(runs[0].style.size, 12, 1e-6);
    EXPECT_NEAR(runs[0].style.tracking, 100, 1e-6); // 1.6 px of 16 px
    EXPECT_EQ(runs[0].style.kerning, 0);
    EXPECT_FALSE(runs[0].style.ligatures);
    EXPECT_FALSE(runs[0].style.contextual);
    EXPECT_NEAR(runs[1].style.baseline_shift, 3, 1e-6);
    EXPECT_TRUE(runs[2].style.all_caps);
    EXPECT_TRUE(runs[2].style.strikethrough);
    EXPECT_FALSE(runs[3].style.fill);
    ASSERT_TRUE(runs[3].style.stroke);
    EXPECT_NEAR(runs[3].style.stroke_width, 1.5, 1e-6);

    // Type set in a font standing in for one that isn't installed keeps that font's name.
    EXPECT_EQ(texts->stories[2]->lines.at(0).runs.at(0).style.font, "PROOFNoSuchFont-Bold");
}

TEST_F(AiNativeImportTest, WrittenTypeInInksNamesTheirArtStylesAndReadsBack)
{
    auto doc = svg_document(R"(<text id="whole" xml:space="preserve" x="10" y="20" style="font-family:Arial;font-size:16px">Spot</text>)"
                            R"(<text id="tinted" xml:space="preserve" x="10" y="45" style="font-family:Arial;font-size:16px">Lab )"
                            R"(<tspan id="stroked">ink</tspan></text>)"
                            R"x(<text id="mixed" xml:space="preserve" x="10" y="70" style="font-family:Arial;font-size:16px;fill:device-cmyk(0.1 0.2 0.3 0.4)">)x"
                            R"(Mixed <tspan id="again">spot</tspan></text>)");
    ASSERT_TRUE(doc);
    auto const lab = Colors::Color(Type::LAB, {0.4, (50.0 + 128) / 255, (-60.0 + 128) / 255});
    auto const cmyk = SpotInk::ensure(doc.get(), {"Press Orange", Colors::Color(Type::CMYK, {0, 0.6, 1, 0}), 1.0});
    auto const tinted = SpotInk::ensure(doc.get(), {"Book Violet", lab, 0.4});
    auto const rgb = SpotInk::ensure(doc.get(), {"Screen Green", Colors::Color(Type::RGB, {0, 0.6, 0.2}), 0.6});
    doc->getObjectById("whole")->setAttribute("style", "font-family:Arial;font-size:16px;fill:url(#" + cmyk + ")");
    doc->getObjectById("tinted")->setAttribute("style", "font-family:Arial;font-size:16px;fill:url(#" + tinted + ")");
    doc->getObjectById("stroked")->setAttribute("style", "stroke:url(#" + rgb + ");stroke-width:0.5px");
    doc->getObjectById("again")->setAttribute("style", "fill:url(#" + cmyk + ")");
    doc->ensureUpToDate();

    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &records = written.records;
    // One art style for each ink and tint, the line a path in it has, before the text
    // documents; the ink declared as for a path.
    auto const styles = records.find("%AI5_Begin_NonPrinting\nNp\n%AI9_BeginArtStyles\n");
    ASSERT_NE(styles, std::string::npos);
    EXPECT_LT(records.find("%%BeginSetup"), styles);
    EXPECT_LT(records.find("%AI9_EndArtStyles\n%AI5_End_NonPrinting--\n"), records.find("%AI11_BeginTextDocument"));
    std::size_t count = 0;
    for (auto at = records.find("/KnownStyle :"); at != std::string::npos; at = records.find("/KnownStyle :", at + 1)) ++count;
    EXPECT_EQ(count, 3u);
    EXPECT_NE(records.find("(Anon PROOF 1) /Name ,\n/SimpleStyle :\n0 O\n0 0.6 1 0 (Press Orange) 0 x\n0 1 0 0 0 Xy\n"),
              std::string::npos);
    EXPECT_NE(records.find(" 40 50 -60 (Book Violet) 0.6 2 Xx\n"), std::string::npos);
    // A stroke's style fills with the ink too, as Illustrator writes it.
    EXPECT_NE(records.find(" 0 0.6 0.2 (Screen Green) 0.4 1 Xx\n"), std::string::npos);
    EXPECT_EQ(records.find("(Screen Green) 0.4 1 XX"), std::string::npos);
    EXPECT_NE(records.find("%%DocumentCustomColors: (Book Violet)\n%%+ (Press Orange)\n%%+ (Screen Green)\n"), std::string::npos);

    // Read back, each character is in the ink and at the tint it was.
    auto const texts = AN::read_text_document(records, {});
    ASSERT_TRUE(texts->error.empty()) << texts->error;
    ASSERT_EQ(texts->stories.size(), 3u);
    for (auto const &e : texts->errors) EXPECT_TRUE(e.empty()) << e;
    auto const &whole = texts->stories[0]->lines.at(0).runs.at(0).style;
    ASSERT_TRUE(whole.fill_ink);
    EXPECT_EQ(whole.fill_ink->ink.name, "Press Orange");
    EXPECT_EQ(whole.fill_ink->ink.color, AN::Color::cmyk(0, 0.6, 1, 0));
    EXPECT_NEAR(whole.fill_ink->tint, 1, 1e-6);
    auto const &lines = texts->stories[1]->lines.at(0).runs;
    ASSERT_EQ(lines.size(), 2u);
    for (auto const &run : lines) {
        ASSERT_TRUE(run.style.fill_ink);
        EXPECT_EQ(run.style.fill_ink->ink.name, "Book Violet");
        EXPECT_EQ(run.style.fill_ink->ink.color, AN::Color::lab(40, 50, -60));
        EXPECT_NEAR(run.style.fill_ink->tint, 0.4, 1e-6);
    }
    EXPECT_FALSE(lines[0].style.stroke_ink);
    ASSERT_TRUE(lines[1].style.stroke_ink);
    EXPECT_EQ(lines[1].style.stroke_ink->ink.name, "Screen Green");
    EXPECT_NEAR(lines[1].style.stroke_ink->tint, 0.6, 1e-6);
    auto const &mixed = texts->stories[2]->lines.at(0).runs;
    ASSERT_EQ(mixed.size(), 2u);
    EXPECT_FALSE(mixed[0].style.fill_ink);
    ASSERT_TRUE(mixed[1].style.fill_ink);
    EXPECT_EQ(mixed[1].style.fill_ink->ink.name, "Press Orange");

    // The import paints them with the inks' swatches, which the next save writes the same way.
    auto b = make(records);
    ASSERT_TRUE(b);
    auto *imported = b->document.get();
    imported->ensureUpToDate();
    std::map<std::string, std::set<double>> used;
    std::function<void(SPObject *)> walk = [&](SPObject *o) {
        if (is<SPString>(o) && !cast<SPString>(o)->string.empty()) {
            for (SPIPaint const *paint : std::initializer_list<SPIPaint const *>{&o->parent->style->fill, &o->parent->style->stroke}) {
                if (!paint->isPaintserver() || !paint->href) continue;
                if (auto const *g = cast<SPGradient>(paint->href->getObject()); g) {
                    if (auto const ink = SpotInk::read(g->getRepr())) used[ink->name].insert(std::round(ink->tint * 1e4) / 1e4);
                }
            }
        }
        for (auto &child : o->children) walk(&child);
    };
    walk(imported->getRoot());
    EXPECT_EQ(used["Press Orange"], (std::set<double>{1.0}));
    EXPECT_EQ(used["Book Violet"], (std::set<double>{0.4}));
    EXPECT_EQ(used["Screen Green"], (std::set<double>{0.6}));
    auto const again = AN::write_records(imported);
    ASSERT_TRUE(again.refused.empty()) << again.refused.front();
    auto const styles_of = [](std::string const &r) {
        auto const from = r.find("%AI9_BeginArtStyles");
        return from == std::string::npos ? std::string() : r.substr(from, r.find("%AI9_EndArtStyles") - from);
    };
    EXPECT_EQ(styles_of(again.records), styles_of(records));

    // An ink at an opacity is refused on type as before: a character has no opacity of its own.
    doc->getObjectById("whole")->setAttribute("style", "font-family:Arial;font-size:16px;fill:url(#" + cmyk + ");fill-opacity:0.5");
    doc->ensureUpToDate();
    auto const translucent = AN::write_records(doc.get());
    EXPECT_EQ(translucent.refused, std::vector<std::string>{"translucent fills or strokes on type"});
}

/// Area type's frame on the page: its outline's bounds in the document.
Geom::OptRect frame_on_page(SPDocument *doc, XML::Node *repr)
{
    doc->ensureUpToDate();
    auto *text = cast<SPText>(doc->getObjectByRepr(repr));
    if (!text || !text->getAttribute(AN::TEXT_FRAME_ATTRIBUTE)) return {};
    return (sp_svg_read_pathv(text->getAttribute(AN::TEXT_FRAME_ATTRIBUTE)) * text->i2doc_affine()).boundsExact();
}

TEST_F(AiNativeImportTest, WrittenAreaTypeKeepsItsFrameParagraphsAndLines)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // An import's area type: two lines that are one paragraph, a line break typed in the next,
    // and a paragraph its frame doesn't show.
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(paragraphs_in_a_frame(font, "more\\r", 5))));
    ASSERT_TRUE(b);
    auto const written = AN::write_records(b->document.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    // The text object carries its frame, as Illustrator writes area type.
    EXPECT_NE(written.records.find("/Art :\nX=\n4 As\n"), std::string::npos);
    EXPECT_NE(written.records.find("n\nX+\n; /ConfiningPath ,\n2 /TextAntialiasing ,\n"), std::string::npos);
    auto const texts = AN::read_text_document(written.records, {});
    ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
    auto const &s = *texts->stories[0];
    EXPECT_TRUE(s.area);
    EXPECT_EQ(s.overflow, "more\n");
    EXPECT_EQ(s.first_baseline, std::pair(5u, 0.0));
    ASSERT_EQ(s.lines.size(), 4u);
    using End = AN::TextLine::End;
    End const ends[] = {End::Wrap, End::Paragraph, End::Break, End::Paragraph};
    char const *const words[] = {"Hello ", "world", "Next", "line"};
    for (std::size_t i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(s.lines[i].end, ends[i]);
        EXPECT_EQ(s.lines[i].paragraph_alignment, i < 2 ? 2u : 1u);
        EXPECT_EQ(s.lines[i].hyphenate, i < 2);
        ASSERT_EQ(s.lines[i].runs.size(), 1u);
        EXPECT_EQ(s.lines[i].runs[0].text, words[i]);
    }
    // Opened again, every character is where it was, and so is the frame.
    auto again = make(written.records);
    ASSERT_TRUE(again);
    auto const was = elements(b->document->getReprRoot(), "svg:text").at(0);
    auto const now = elements(again->document->getReprRoot(), "svg:text").at(0);
    auto const before = set_characters(b->document.get(), was), after = set_characters(again->document.get(), now);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t c = 0; c < before.size(); ++c) {
        EXPECT_EQ(after[c].first, before[c].first) << c;
        EXPECT_NEAR(after[c].second.x(), before[c].second.x(), 0.02) << c;
        EXPECT_NEAR(after[c].second.y(), before[c].second.y(), 0.02) << c;
    }
    auto const f0 = frame_on_page(b->document.get(), was), f1 = frame_on_page(again->document.get(), now);
    ASSERT_TRUE(f0 && f1);
    EXPECT_TRUE(Geom::are_near(f0->min(), f1->min(), 1e-3) && Geom::are_near(f0->max(), f1->max(), 1e-3)) << *f0 << " " << *f1;
    EXPECT_EQ(attr(now, AN::TEXT_OVERFLOW_ATTRIBUTE), "more\n");

    // A line break typed inside a paragraph of point type, which an import keeps, too.
    auto point = svg_document(R"(<text xml:space="preserve" style="font-family:Arial;font-size:12px">)"
                              R"(<tspan x="10" y="20" proof:line-end="break">One</tspan><tspan x="10" y="34">Two</tspan></text>)");
    ASSERT_TRUE(point);
    auto const lines = AN::write_records(point.get());
    ASSERT_TRUE(lines.refused.empty()) << lines.refused.front();
    auto const broken = AN::read_text_document(lines.records, {});
    ASSERT_TRUE(broken->stories.at(0)) << broken->errors[0];
    ASSERT_EQ(broken->stories[0]->lines.size(), 2u);
    EXPECT_EQ(broken->stories[0]->lines[0].end, End::Break);
    EXPECT_EQ(broken->stories[0]->lines[1].end, End::Paragraph);
}

/// Each character of `repr`'s text where the layout sets it, as written and opened again.
void expect_characters_where_they_were(SPDocument *was_doc, XML::Node *was, SPDocument *now_doc, XML::Node *now)
{
    auto const before = set_characters(was_doc, was), after = set_characters(now_doc, now);
    ASSERT_EQ(after.size(), before.size());
    ASSERT_FALSE(before.empty());
    for (std::size_t c = 0; c < before.size(); ++c) {
        EXPECT_EQ(after[c].first, before[c].first) << c;
        EXPECT_NEAR(after[c].second.x(), before[c].second.x(), 0.02) << c;
        EXPECT_NEAR(after[c].second.y(), before[c].second.y(), 0.02) << c;
    }
}

TEST_F(AiNativeImportTest, WrittenVariableFontInstancesRetainLoadedAxesAndShapes)
{
    if (!has_bahnschrift()) GTEST_SKIP() << "Bahnschrift isn't installed";
    for (auto const &design : {std::vector<double>{400, 100}, {600, 75}, {350.25, 80.125}}) {
        auto font = AN::find_font("Bahnschrift", design);
        ASSERT_TRUE(font);
        auto doc = svg_document("<text id='t' x='20' y='40' style=\"font-size:24px;" + font->css + "\">Variable 123</text>");
        ASSERT_TRUE(doc);
        std::string reason;
        auto const ps = AN::postscript_font(doc->getObjectById("t")->style, reason);
        ASSERT_TRUE(ps) << reason;
        EXPECT_EQ(ps->design, design);
        EXPECT_TRUE(ps->truetype);
        EXPECT_FALSE(ps->name.empty());
        auto const written = AN::write_records(doc.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto const parsed = AN::read_text_document(written.records, {});
        ASSERT_TRUE(parsed->stories.at(0)) << parsed->errors.at(0);
        EXPECT_EQ(parsed->stories[0]->lines[0].runs[0].style.design, design);
        auto again = make(written.records);
        ASSERT_TRUE(again);
        auto *text = elements(again->document->getReprRoot(), "svg:text").at(0);
        expect_characters_where_they_were(doc.get(), doc->getObjectById("t")->getRepr(), again->document.get(), text);
    }
}

TEST_F(AiNativeImportTest, VariableFontSettingsThatCannotBeRepresentedStayRefused)
{
    for (auto const *style : {"font-family:Arial;font-variation-settings:'wght' 350",
                            "font-family:Bahnschrift;font-variation-settings:'NOPE' 1"}) {
        auto doc = svg_document(std::string("<text x='10' y='30' style=\"font-size:20px;") + style + "\">Axes</text>");
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_FALSE(written.refused.empty());
        EXPECT_TRUE(written.records.empty());
    }
}

TEST_F(AiNativeImportTest, WrittenTabsKeepTheirRulers)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // An import's point type with a stop and an interval of its own, and area type with a ruler
    // in each of its two paragraphs.
    auto const point = with_paragraphs(text_document("(A\\tB\\tC\\r)", 6, font, "/11 0"), {},
        "<< /1 6 /0 << /0 << /5 << /31 40 /30 << /0 [ << /0 50 /1 0 >> ] >> >> >> >> >>");
    auto const area = area_text_document("(A\\tB\\rC\\tD\\r)", 8, {{0, 10, 4}, {0, 24, 4}}, font, {},
        "/5 << /0 [ << /1 4 /0 << /0 << /5 << /31 36 >> >> >> >> "
        "<< /1 4 /0 << /0 << /5 << /31 72 >> >> >> >> ] >>");
    for (bool const is_area : {false, true}) {
        SCOPED_TRACE(is_area ? "area" : "point");
        auto const &document = is_area ? area : point;
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(document)));
        ASSERT_TRUE(b);
        auto const written = AN::write_records(b->document.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        auto const texts = AN::read_text_document(written.records, {});
        ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
        auto const &lines = texts->stories[0]->lines;
        if (!is_area) {
            ASSERT_EQ(lines.size(), 1u);
            ASSERT_TRUE(lines[0].tabs);
            EXPECT_DOUBLE_EQ(lines[0].tabs->interval, 40);
            EXPECT_EQ(lines[0].tabs->positions, std::vector<double>{50});
            EXPECT_EQ(lines[0].runs.at(0).text, "A\tB\tC");
        } else {
            ASSERT_EQ(lines.size(), 2u);
            ASSERT_TRUE(lines[0].tabs && lines[1].tabs);
            EXPECT_DOUBLE_EQ(lines[0].tabs->interval, 36);
            EXPECT_DOUBLE_EQ(lines[1].tabs->interval, 72);
        }
        // Opened again, each tab steps to the stop it did.
        auto again = make(written.records);
        ASSERT_TRUE(again);
        expect_characters_where_they_were(b->document.get(), elements(b->document->getReprRoot(), "svg:text").at(0),
                                          again->document.get(), elements(again->document->getReprRoot(), "svg:text").at(0));
    }
}

TEST_F(AiNativeImportTest, CentredTypeWithSpacesAtItsLineEndsIsWrittenWhereItIsSet)
{
    // Illustrator aligns a line by its width with the spaces at its end and without the tracking
    // after its last letter (its own layout of PROOF's controls). An import's point type is laid
    // out so too (proof:line-alignment). Inkscape leaves both out only at the end of the text's
    // last line: its own type is written with each anchor where Illustrator then sets the line
    // where Inkscape set it.
    std::string const type = "font-family:Arial;font-size:16px;text-anchor:middle;letter-spacing:1.6px";
    auto doc = svg_document(
        R"(<text id="centred" xml:space="preserve" x="100" y="30" style="font-family:Arial;font-size:20px;text-anchor:middle">Centred </text>)"
        R"(<text id="right" xml:space="preserve" x="190" y="60" style="font-family:Arial;font-size:20px;text-anchor:end;letter-spacing:2px">Right  </text>)"
        R"(<text id="import" xml:space="preserve" proof:line-alignment="illustrator" style=")" + type + R"(">)"
        R"(<tspan x="100" y="80" style="text-anchor:middle">One </tspan><tspan x="100" y="96" style="text-anchor:middle">Two </tspan></text>)"
        R"(<text id="plain" xml:space="preserve" style=")" + type + R"(">)"
        R"(<tspan x="100" y="80">One </tspan><tspan x="100" y="96">Two </tspan></text>)");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto layout_of = [&](char const *id) -> Text::Layout const & { return cast<SPText>(doc->getObjectById(id))->layout; };
    auto last_line = [](Text::Layout const &l) {
        auto it = l.begin();
        while (l.chunkIndex(it) == 0) it.nextCharacter();
        return it;
    };
    auto start_of = [](Text::Layout const &l, Text::Layout::iterator const &it) { return l.characterAnchorPoint(it).x(); };
    // The layout. Plain text keeps the first line's tracking after its last letter, and leaves
    // the last line's space and tracking out; the import keeps the spaces and leaves the tracking out.
    auto const &plain = layout_of("plain"), &aligned = layout_of("import");
    auto space = plain.end();
    space.prevCharacter();
    double const space_width = plain.characterBoundingBox(space).width(); // with its tracking
    EXPECT_GT(space_width, 2.6);
    // The layout keeps its positions in single precision.
    EXPECT_NEAR(start_of(aligned, aligned.begin()) - start_of(plain, plain.begin()), 1.6 / 2, 1e-5);
    EXPECT_NEAR(start_of(plain, last_line(plain)) - start_of(aligned, last_line(aligned)), space_width / 2, 1e-5);
    // Plain text: no one anchor sets both its lines in Illustrator where they are.
    auto refused = AN::write_records(doc.get()).refused;
    std::string const apart = "centred or right-aligned lines that Illustrator would align apart (spaces or tracking at their ends)";
    EXPECT_NE(std::find(refused.begin(), refused.end(), apart), refused.end());
    doc->getObjectById("plain")->deleteObject();
    // Nor Inkscape's own lines with tracking: it keeps it after the first line's last letter.
    auto own = svg_document(R"(<text xml:space="preserve" style=")" + type + R"(">)"
                            R"(<tspan sodipodi:role="line" x="100" y="20">Tracked</tspan><tspan sodipodi:role="line" x="100" y="40">Tracked</tspan></text>)");
    ASSERT_TRUE(own);
    refused = AN::write_records(own.get()).refused;
    EXPECT_NE(std::find(refused.begin(), refused.end(), apart), refused.end());

    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const texts = AN::read_text_document(written.records, {});
    ASSERT_EQ(texts->stories.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(texts->stories[i]) << texts->errors[i];
        EXPECT_TRUE(texts->stories[i]->illustrator_alignment) << i;
        EXPECT_EQ(texts->stories[i]->lines.at(0).alignment, i == 1 ? 1u : 2u) << i;
    }
    auto again = make(written.records);
    ASSERT_TRUE(again);
    auto const reopened = elements(again->document->getReprRoot(), "svg:text");
    ASSERT_EQ(reopened.size(), 3u);
    char const *const ids[] = {"centred", "right", "import"};
    for (std::size_t i = 0; i < 3; ++i) {
        SCOPED_TRACE(ids[i]);
        EXPECT_EQ(attr(reopened[i], Text::LINE_ALIGNMENT_ATTRIBUTE), "illustrator");
        expect_characters_where_they_were(doc.get(), doc->getObjectById(ids[i])->getRepr(), again->document.get(), reopened[i]);
    }
}

TEST_F(AiNativeImportTest, NewPointTypeIsAlignedAsIllustratorAlignsIt)
{
    // The Text tool's point type (a click) lines up as Illustrator lines it up: two centred lines
    // with tracking, which Inkscape's own layout couldn't write, are written where they are set.
    // Type set right to left keeps Inkscape's way.
    auto doc = svg_document("");
    ASSERT_TRUE(doc);
    auto new_type = [&](char const *direction, Geom::Point at) {
        auto css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "font-family", "Arial");
        sp_repr_css_set_property(css, "font-size", "16px");
        sp_repr_css_set_property(css, "text-anchor", "middle");
        sp_repr_css_set_property(css, "text-align", "center");
        sp_repr_css_set_property(css, "letter-spacing", "1.6px");
        sp_repr_css_set_property(css, "direction", direction);
        auto text = create_text_at_position(doc->getRoot(), css, at);
        sp_repr_css_attr_unref(css);
        return text;
    };
    auto *centred = new_type("ltr", {100, 30});
    auto *rtl = new_type("rtl", {100, 80});
    EXPECT_EQ(attr(centred->getRepr(), Text::LINE_ALIGNMENT_ATTRIBUTE), "illustrator");
    EXPECT_EQ(rtl->getRepr()->attribute(Text::LINE_ALIGNMENT_ATTRIBUTE), nullptr);
    rtl->deleteObject();
    // Typed as the tool types: a word, Enter, a word with a space after it.
    doc->ensureUpToDate();
    auto it = sp_te_insert(centred, centred->layout.end(), "Tracked");
    doc->ensureUpToDate();
    it = sp_te_insert_line(centred, it);
    doc->ensureUpToDate();
    sp_te_insert(centred, it, "Two ");
    doc->ensureUpToDate();
    ASSERT_EQ(elements(centred->getRepr(), "svg:tspan").size(), 2u);
    // Laid out Inkscape's way, the same type has no one anchor for both its lines.
    centred->removeAttribute(Text::LINE_ALIGNMENT_ATTRIBUTE);
    doc->ensureUpToDate();
    auto const refused = AN::write_records(doc.get()).refused;
    EXPECT_NE(std::find(refused.begin(), refused.end(),
                        "centred or right-aligned lines that Illustrator would align apart (spaces or tracking at their ends)"),
              refused.end());
    centred->setAttribute(Text::LINE_ALIGNMENT_ATTRIBUTE, "illustrator");
    doc->ensureUpToDate();

    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const texts = AN::read_text_document(written.records, {});
    ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
    EXPECT_TRUE(texts->stories[0]->illustrator_alignment);
    auto again = make(written.records);
    ASSERT_TRUE(again);
    auto const reopened = elements(again->document->getReprRoot(), "svg:text").at(0);
    EXPECT_EQ(attr(reopened, Text::LINE_ALIGNMENT_ATTRIBUTE), "illustrator");
    expect_characters_where_they_were(doc.get(), centred->getRepr(), again->document.get(), reopened);
}

TEST_F(AiNativeImportTest, TypeInAShapeIsWrittenAsAreaType)
{
    // Inkscape's type in a shape: its paragraphs as typed, wrapped where the shape is full,
    // centred; the frame is the shape, moved by its own transform.
    auto doc = svg_document(R"x(<defs><rect id="frame" x="20" y="10" width="90" height="80" transform="translate(5 0)"/></defs>)x"
                            "<text id=\"t\" xml:space=\"preserve\" style=\"shape-inside:url(#frame);white-space:pre;font-family:Arial;"
                            "font-size:12px;text-align:center;text-anchor:middle;fill:#000000\">"
                            "Words that wrap inside the frame more than once\nSecond</text>");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const texts = AN::read_text_document(written.records, {});
    ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
    auto const &s = *texts->stories[0];
    EXPECT_TRUE(s.area);
    ASSERT_GE(s.lines.size(), 3u);
    using End = AN::TextLine::End;
    std::string joined;
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(s.lines[i].paragraph_alignment, 2u);
        EXPECT_FALSE(s.lines[i].hyphenate);
        for (auto const &run : s.lines[i].runs) joined += run.text;
        bool const ends_paragraph = i + 2 >= s.lines.size();
        EXPECT_EQ(s.lines[i].end, ends_paragraph ? End::Paragraph : End::Wrap);
        if (ends_paragraph) joined += "|";
    }
    EXPECT_EQ(joined, "Words that wrap inside the frame more than once|Second|");

    // Opened again, as an import's area type: every character where Inkscape set it, in the frame.
    auto b = make(written.records);
    ASSERT_TRUE(b);
    auto const now = elements(b->document->getReprRoot(), "svg:text").at(0);
    auto before = set_characters(doc.get(), doc->getObjectById("t")->getRepr());
    auto const after = set_characters(b->document.get(), now);
    // The line feed is a character of Inkscape's line; the import's lines end without one.
    std::erase_if(before, [](auto const &c) { return c.first == '\n'; });
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t c = 0; c < before.size(); ++c) {
        EXPECT_EQ(after[c].first, before[c].first) << c;
        EXPECT_NEAR(after[c].second.x(), before[c].second.x(), 0.02) << c;
        EXPECT_NEAR(after[c].second.y(), before[c].second.y(), 0.02) << c;
    }
    auto const frame = frame_on_page(b->document.get(), now);
    ASSERT_TRUE(frame);
    EXPECT_TRUE(Geom::are_near(frame->min(), Geom::Point(25, 10), 1e-3) && Geom::are_near(frame->max(), Geom::Point(115, 90), 1e-3))
        << *frame;
    EXPECT_NE(attr(now, "style").find("text-align:center"), std::string::npos);
    // Its first baseline is fixed where Inkscape set it below the frame's top, in points (a pixel is 0.75).
    ASSERT_TRUE(s.first_baseline);
    EXPECT_EQ(s.first_baseline->first, 0u);
    EXPECT_NEAR(s.first_baseline->second, (before.at(0).second.y() - 10) * 0.75, 1e-4);
}

TEST_F(AiNativeImportTest, ALineFeedTakesNoRoomInTheLineItEnds)
{
    // Centred type in a shape: a line that ends with a typed line feed is centred as one that
    // doesn't, on the shape's middle.
    auto doc = svg_document(R"(<defs><rect id="frame" x="0" y="0" width="100" height="80"/></defs>)"
                            "<text id=\"t\" xml:space=\"preserve\" style=\"shape-inside:url(#frame);white-space:pre;font-family:Arial;"
                            "font-size:20px;text-align:center;text-anchor:middle\">mm\nmm</text>");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const &layout = cast<SPText>(doc->getObjectById("t"))->layout;
    std::map<unsigned, Geom::Rect> lines;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        SPObject *source = nullptr;
        Glib::ustring::iterator at;
        layout.getSourceOfCharacter(it, &source, &at);
        if (*at != 'm') continue;
        auto const box = layout.characterBoundingBox(it);
        auto [found, fresh] = lines.try_emplace(layout.lineIndex(it), box);
        if (!fresh) found->second.unionWith(box);
    }
    ASSERT_EQ(lines.size(), 2u);
    for (auto const &[line, box] : lines) EXPECT_NEAR(box.midpoint().x(), 50, 0.05) << line;
}

/// Each character a text object draws, and where: the middle of its outline, in the document.
std::vector<std::pair<gunichar, Geom::Point>> drawn_characters(SPDocument *doc, XML::Node *repr)
{
    doc->ensureUpToDate();
    std::vector<std::pair<gunichar, Geom::Point>> out;
    auto *text = cast<SPText>(doc->getObjectByRepr(repr));
    if (!text) return out;
    auto const &layout = text->layout;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        SPObject *source = nullptr;
        Glib::ustring::iterator at;
        layout.getSourceOfCharacter(it, &source, &at);
        auto next = it;
        next.nextCharacter();
        auto const box = layout.convertToCurves(it, next).boundsFast();
        if (!is<SPString>(source) || !it.hasGlyph() || !box) continue;
        out.emplace_back(*at, box->midpoint() * text->i2doc_affine());
    }
    return out;
}

void expect_drawn_alike(SPDocument *a, XML::Node *x, SPDocument *b, XML::Node *y, double tolerance)
{
    auto const was = drawn_characters(a, x), now = drawn_characters(b, y);
    ASSERT_EQ(now.size(), was.size());
    ASSERT_FALSE(was.empty());
    for (std::size_t c = 0; c < was.size(); ++c) {
        EXPECT_EQ(now[c].first, was[c].first) << c;
        EXPECT_TRUE(Geom::are_near(now[c].second, was[c].second, tolerance)) << c << ": " << now[c].second << " " << was[c].second;
    }
}

TEST_F(AiNativeImportTest, WrittenPathTypeKeepsItsPathAndBrackets)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    // An import's path type, left, right and centred on a line kept as a cubic, and on a closed
    // square from halfway along its last side round past its start.
    struct Case
    {
        std::string document;
        int units;
        unsigned alignment;
        bool loop;
    };
    std::vector<Case> const cases{{path_text_document("(ABCD\\r)", 5, font), 5, 0, false},
                                  {path_text_document("(ABCD\\r)", 5, font), 5, 1, false},
                                  {path_text_document("(ABCD\\r)", 5, font), 5, 2, false},
                                  {loop_text_document("(AAAAAAAAAA\\r)", 11, font, "3.5 7.49"), 11, 0, true}};
    for (auto const &[document, units, alignment, loop] : cases) {
        SCOPED_TRACE(alignment + 10 * loop);
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                              text_setup(with_paragraphs(document, {}, std::format("<< /1 {} /0 << /0 << /5 << /0 {} >> >> >> >>",
                                                                                   units, alignment)))));
        ASSERT_TRUE(b);
        auto const written = AN::write_records(b->document.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        // The text object carries its path, as Illustrator writes path type.
        EXPECT_NE(written.records.find(loop ? "/Art :\nX=\n4 As\n" : "/Art :\nX=\n1 As\n"), std::string::npos);
        auto const texts = AN::read_text_document(written.records, {});
        ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
        auto const &s = *texts->stories[0];
        ASSERT_TRUE(s.path);
        EXPECT_EQ(s.lines.at(0).alignment, alignment);
        auto const was = elements(b->document->getReprRoot(), "svg:textPath").at(0);
        EXPECT_NEAR(s.path->start, std::stod(attr(was, "proof:path-start")), 0.002);
        EXPECT_NEAR(s.path->end, std::stod(attr(was, "proof:path-end")), 0.002);
        // Opened again, it draws each character where it was.
        auto again = make(written.records);
        ASSERT_TRUE(again);
        expect_drawn_alike(b->document.get(), elements(b->document->getReprRoot(), "svg:text").at(0), again->document.get(),
                           elements(again->document->getReprRoot(), "svg:text").at(0), 0.02);
    }
}

TEST_F(AiNativeImportTest, PathTypeSourceOptionsAndSpacingSurviveSvgAndNativeWrite)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto const options = "/11 << /4 100 /18 112.5 /6 false /22 0.5 /0 false /1 0 /2 0 /7 false >>";
    auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(path_text_document("(ABCD\\r)", 5, font, {}, options))));
    ASSERT_TRUE(b);
    EXPECT_TRUE(std::any_of(b->notes.begin(), b->notes.end(), [](auto const &note) {
        return note.find("automatic curve spacing uses a font-metric approximation") != std::string::npos;
    }));
    auto doc = SPDocument::createNewDocFromMem(sp_repr_save_buf(b->document->getReprDoc()).raw());
    ASSERT_TRUE(doc);
    auto const spans = elements(doc->getReprRoot(), "svg:textPath");
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(attr(spans[0], "proof:ai-path-options"), "/0 false /1 0 /18 112.5 /2 0 /22 0.5 /4 100 /6 false /7 false");
    EXPECT_EQ(attr(spans[0], "proof:path-spacing"), "auto");
    auto fresh = doc->getObjectByRepr(spans[0])->updateRepr(doc->getReprDoc(), nullptr, SP_OBJECT_WRITE_BUILD | SP_OBJECT_WRITE_EXT);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(attr(fresh, "proof:path-spacing"), "auto");
    EXPECT_EQ(attr(fresh, "proof:ai-path-options"), attr(spans[0], "proof:ai-path-options"));
    Inkscape::GC::release(fresh);

    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const texts = AN::read_text_document(written.records, {});
    ASSERT_TRUE(texts->stories.at(0)) << texts->errors[0];
    ASSERT_TRUE(texts->stories[0]->path);
    // Retain the live mode; Illustrator recomputes caches from the current font itself.
    EXPECT_TRUE(texts->stories[0]->path->automatic_spacing);
    EXPECT_EQ(texts->stories[0]->path->source_options, "/6 false");
    auto again = make(written.records);
    ASSERT_TRUE(again);
    expect_drawn_alike(doc.get(), elements(doc->getReprRoot(), "svg:text").at(0), again->document.get(),
                       elements(again->document->getReprRoot(), "svg:text").at(0), 0.02);
    EXPECT_FALSE(attr(spans[0], "proof:ai-path-options").empty()); // The working master still retains it.
}

TEST_F(AiNativeImportTest, InkscapesTypeOnAPathIsWrittenAsPathType)
{
    // Inkscape's type on a path: from a start offset, centred on half the path's length, ending at
    // an offset, on the other side, on a line, and centred over a closed path's start.
    auto doc = svg_document(
        R"x(<path id="curve" d="M20,100 C60,20 140,20 180,100" transform="translate(5 0)" style="fill:none;stroke:#000000"/>)x"
        R"(<path id="line" d="M20,150 H180"/>)"
        R"(<path id="loop" d="M300,60 C322,60 340,78 340,100 C340,122 322,140 300,140 C278,140 260,122 260,100 C260,78 278,60 300,60 Z"/>)"
        R"(<text id="left" style="font-family:Arial;font-size:12px"><textPath href="#curve" startOffset="10">Left on a curve</textPath></text>)"
        R"(<text id="centre" style="font-family:Arial;font-size:12px;text-anchor:middle"><textPath href="#curve" startOffset="50%">Centred</textPath></text>)"
        R"(<text id="right" style="font-family:Arial;font-size:12px;text-anchor:end"><textPath href="#curve" startOffset="150">Right</textPath></text>)"
        R"(<text id="under" style="font-family:Arial;font-size:12px"><textPath href="#curve" side="right" startOffset="20">Other side</textPath></text>)"
        R"(<text id="straight" style="font-family:Arial;font-size:12px;fill:#0000ff"><textPath href="#line" startOffset="40">Straight</textPath></text>)"
        R"(<text id="ring" style="font-family:Arial;font-size:12px;text-anchor:middle"><textPath href="#loop" startOffset="5">Round the loop</textPath></text>)"
        // From far along a rectangle's last side, round its corner: a straight last side's own time.
        R"(<path id="box" d="M60,380 H220 V460 H60 Z"/>)"
        R"(<text id="corner" style="font-family:Arial;font-size:12px"><textPath href="#box" startOffset="420">Round the last corner</textPath></text>)");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const texts = AN::read_text_document(written.records, {});
    ASSERT_EQ(texts->stories.size(), 7u);
    for (auto const &e : texts->errors) EXPECT_TRUE(e.empty()) << e;
    unsigned const alignments[] = {0, 2, 1, 0, 0, 2, 0};
    for (std::size_t i = 0; i < 7; ++i) {
        ASSERT_TRUE(texts->stories[i] && texts->stories[i]->path) << i;
        EXPECT_EQ(texts->stories[i]->lines.at(0).alignment, alignments[i]) << i;
    }
    // Lengths in points: a pixel is 0.75. The loop's brackets run on past its start.
    EXPECT_NEAR(texts->stories[0]->path->start, 7.5, 0.01);
    EXPECT_NEAR(texts->stories[4]->path->start, 30, 0.01);
    auto const &ring = *texts->stories[5]->path;
    EXPECT_GT(ring.end, ring.outline[0].length());
    EXPECT_NEAR(ring.end - ring.start, ring.outline[0].length(), 0.01);

    // Opened again, every character is drawn where Inkscape drew it.
    auto b = make(written.records);
    ASSERT_TRUE(b);
    auto const now = elements(b->document->getReprRoot(), "svg:text");
    ASSERT_EQ(now.size(), 7u);
    char const *const ids[] = {"left", "centre", "right", "under", "straight", "ring", "corner"};
    for (std::size_t i = 0; i < 7; ++i) {
        SCOPED_TRACE(ids[i]);
        expect_drawn_alike(doc.get(), doc->getObjectById(ids[i])->getRepr(), b->document.get(), now[i], 0.05);
    }
}

TEST_F(AiNativeImportTest, AutomaticPathSpacingMovesGlyphsCursorsAndSelectionTogether)
{
    auto doc = svg_document(
        R"(<path id="p" d="M20,100 C70,30 170,30 220,100"/>)"
        R"(<text id="t" style="font-family:Arial;font-size:12px"><textPath href="#p" proof:path-spacing="auto" startOffset="10">Variant abcdef</textPath></text>)");
    ASSERT_TRUE(doc); doc->ensureUpToDate();
    auto *text = cast<SPText>(doc->getObjectById("t")); ASSERT_TRUE(text);
    ASSERT_TRUE(text->layout.automaticPathSpacingSupported());
    EXPECT_GT(text->layout.automaticPathSpacingHeight(), 0);
    for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
        Geom::Point cursor; double height, rotation;
        text->layout.queryCursorShape(it, cursor, height, rotation);
        auto const nearest = text->layout.getNearestCursorPositionTo(cursor);
        EXPECT_EQ(text->layout.iteratorToCharIndex(nearest), text->layout.iteratorToCharIndex(it));
        auto after = it; after.nextCharacter();
        auto const glyph = text->layout.convertToCurves(it, after).boundsFast();
        if (!glyph) continue; // Space.
        auto const box = text->layout.characterBoundingBox(it);
        EXPECT_LT(Geom::L2(box.midpoint() - glyph->midpoint()), 12);
        EXPECT_FALSE(text->layout.createSelectionShape(it, after, Geom::identity()).empty());
    }
    auto const original = text->layout.convertToCurves();
    auto path = elements(text->getRepr(), "svg:textPath").at(0);
    path->setAttribute("proof:path-spacing", nullptr); doc->ensureUpToDate();
    auto const ordinary = text->layout.convertToCurves();
    ASSERT_FALSE(original.empty()); ASSERT_FALSE(ordinary.empty());
    EXPECT_GT(Geom::L2(original.back().finalPoint() - ordinary.back().finalPoint()), 0.2);
    // On a straight path there is no curve spacing adjustment.
    doc->getObjectById("p")->getRepr()->setAttribute("d", "M20,100 H220");
    doc->ensureUpToDate(); auto const line = text->layout.convertToCurves();
    path->setAttribute("proof:path-spacing", "auto"); doc->ensureUpToDate();
    auto const automatic_line = text->layout.convertToCurves();
    ASSERT_EQ(line.size(), automatic_line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        EXPECT_NEAR(Geom::L2(line[i].finalPoint() - automatic_line[i].finalPoint()), 0, 1e-5);
    }
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto again = make(written.records); ASSERT_TRUE(again);
    expect_drawn_alike(doc.get(), text->getRepr(), again->document.get(),
                       elements(again->document->getReprRoot(), "svg:text").at(0), 0.02);
}

TEST_F(AiNativeImportTest, AutomaticPathTypeMatchesIllustratorFontControls)
{
    // Independent Illustrator 29.2.1 layout records, generated with four fonts at
    // 12/24 pt (2026-10-23). Origins relative to the first glyph of "Variant ".
    // The shape and start offset scale with the font size; the cache's rounding does not.
    struct Sample { char const *postscript; char const *family; double size; std::array<Geom::Point, 8> points; };
    Sample const samples[] = {
        {"ArialMT", "Arial", 12, {{{0, 0}, {4.65130, -5.26355}, {9.26525, -9.94690}, {12.15203, -12.65541}, {14.05930, -14.41185}, {19.10997, -18.60829}, {24.41152, -22.51521}, {27.13976, -24.38188}}}},
        {"ArialMT", "Arial", 24, {{{0, 0}, {9.28223, -10.50504}, {18.53052, -19.89382}, {24.28223, -25.29088}, {28.09650, -28.80437}, {38.19715, -37.19861}, {48.79950, -45.01384}, {54.20789, -48.71613}}}},
        {"TimesNewRomanPSMT", "Times New Roman", 12, {{{0, 0}, {4.81946, -5.40537}, {8.48352, -9.16469}, {11.34302, -11.90033}, {13.74304, -14.09720}, {17.74780, -17.49480}, {22.44824, -21.08965}, {25.12146, -22.99486}}}},
        {"TimesNewRomanPSMT", "Times New Roman", 24, {{{0, 0}, {9.63883, -10.81073}, {16.98805, -18.35037}, {22.66426, -23.78052}, {27.48580, -28.19440}, {35.47298, -34.97125}, {44.89649, -42.17929}, {50.24280, -45.98972}}}},
        {"CourierNewPSMT", "Courier New", 12, {{{0, 0}, {4.70038, -5.34366}, {9.67841, -10.41162}, {14.92816, -15.19016}, {20.44269, -19.66519}, {26.18976, -23.80649}, {32.18274, -27.61868}, {38.38593, -31.07413}}}},
        {"CourierNewPSMT", "Courier New", 24, {{{0, 0}, {9.38024, -10.66529}, {19.33557, -20.80268}, {29.83422, -30.36118}, {40.83923, -39.29523}, {52.35583, -47.59687}, {64.31665, -55.20825}, {76.69689, -62.10922}}}},
        {"MyriadPro-Regular", "Myriad Pro", 12, {{{0, 0}, {4.10116, -4.67724}, {8.05832, -8.78094}, {10.88137, -11.50268}, {12.88169, -13.37384}, {17.18808, -17.08404}, {22.37045, -21.07067}, {25.56707, -23.33044}}}},
        {"MyriadPro-Regular", "Myriad Pro", 24, {{{0, 0}, {8.18194, -9.33246}, {16.11658, -17.56213}, {21.76282, -23.00561}, {25.76343, -26.74786}, {34.37622, -34.16833}, {44.74090, -42.14160}, {51.13415, -46.66113}}}},
    };
    unsigned tested = 0;
    for (auto const &sample : samples) {
        auto *description = FontFactory::get().parsePostscriptName(sample.postscript, false);
        if (!description) continue;
        pango_font_description_free(description);
        SCOPED_TRACE(std::string(sample.family) + " " + std::to_string(sample.size));
        auto const k = sample.size / 12;
        auto doc = SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:proof='urn:x-proof:document:1' width='500pt' height='240pt' viewBox='0 0 500 240'>"
            "<path id='p' d='M30,200 c" + std::to_string(50*k) + "," + std::to_string(-70*k) + " " + std::to_string(150*k) + "," +
            std::to_string(-70*k) + " " + std::to_string(200*k) + ",0'/>"
            "<text id='t' style='font-family:" + sample.family + ";font-size:" + std::to_string(sample.size) + "px'>"
            "<textPath href='#p' startOffset='" + std::to_string(10*k) + "' proof:path-spacing='auto'>Variant abcdef</textPath></text></svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        auto *text = cast<SPText>(doc->getObjectById("t")); ASSERT_TRUE(text);
        auto const &glyphs = text->layout.glyphs(); ASSERT_GE(glyphs.size(), sample.points.size());
        for (std::size_t i = 1; i < sample.points.size(); ++i) {
            Geom::Point const relative(glyphs[i].x - glyphs[0].x, glyphs[i].y - glyphs[0].y);
            EXPECT_LT(Geom::L2(relative - sample.points[i]), sample.size * 0.005) << i;
        }
        ++tested;
    }
    if (!tested) GTEST_SKIP() << "none of the independently measured fonts is installed";
}

TEST_F(AiNativeImportTest, AutomaticCircleSpacingClipsAfterOneTurnAndKeepsTheWholeStory)
{
    // A deliberately excessive story, starting near the closing point and wrapping once.
    auto doc = svg_document(
        R"(<defs><path id="p" d="M20,60 C20,48.9543 28.9543,40 40,40 C51.0457,40 60,48.9543 60,60 C60,71.0457 51.0457,80 40,80 C28.9543,80 20,71.0457 20,60 Z"/></defs>)"
        R"(<text id="t" style="font-family:Arial;font-size:16px"><textPath href="#p" startOffset="120" proof:path-start="120" proof:path-end="245.68" proof:path-spacing="auto">WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW</textPath></text>)");
    ASSERT_TRUE(doc); doc->ensureUpToDate();
    auto *text = cast<SPText>(doc->getObjectById("t")); ASSERT_TRUE(text);
    ASSERT_TRUE(text->layout.automaticPathSpacingSupported());
    EXPECT_TRUE(text->layout.inputTruncated());
    EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), 40u);
    unsigned visible = 0, hidden = 0;
    bool saw_hidden = false;
    for (auto const &glyph : text->layout.glyphs()) {
        if (glyph.hidden) { ++hidden; saw_hidden = true; }
        else { ++visible; EXPECT_FALSE(saw_hidden); }
        EXPECT_TRUE(std::isfinite(glyph.x)); EXPECT_TRUE(std::isfinite(glyph.y));
    }
    EXPECT_GT(visible, 1u); EXPECT_LT(visible, 40u); EXPECT_GT(hidden, 0u);
    // Clicks and selection still follow the visible text after it crossed the seam.
    auto it = text->layout.begin(); it.nextCharacter();
    Geom::Point cursor; double height, rotation;
    text->layout.queryCursorShape(it, cursor, height, rotation);
    EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.getNearestCursorPositionTo(cursor)), 1u);
    EXPECT_EQ(text->layout.createSelectionShape(text->layout.begin(), text->layout.end(), Geom::identity()).size(), 4 * visible);
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto again = make(written.records); ASSERT_TRUE(again);
    auto *reopened = cast<SPText>(again->document->getObjectByRepr(elements(again->document->getReprRoot(), "svg:text").at(0)));
    ASSERT_TRUE(reopened); again->document->ensureUpToDate();
    EXPECT_EQ(reopened->layout.iteratorToCharIndex(reopened->layout.end()), 40u);
    EXPECT_EQ(std::count_if(reopened->layout.glyphs().begin(), reopened->layout.glyphs().end(),
                            [](auto const &g) { return !g.hidden; }), visible);
}

TEST_F(AiNativeImportTest, AutomaticPathSpacingRefusesSharpTightAndCollapsedNativeOutput)
{
    for (auto const *geometry : {"M20,50 H100 V100 H20 Z",
         "M20,50 C20,48.89543 20.89543,48 22,48 C23.10457,48 24,48.89543 24,50 C24,51.10457 23.10457,52 22,52 C20.89543,52 20,51.10457 20,50 Z",
         "M20,60 C20,48.9543 28.9543,40 40,40 C51.0457,40 60,48.9543 60,60 C60,71.0457 51.0457,80 40,80 C28.9543,80 20,71.0457 20,60 Z"}) {
        // The inner side of a radius-2 loop collapses before the reference-height offset.
        auto doc = svg_document(std::string("<path id='p' d='") + geometry + "'/>"
            "<text id='t' style='font-family:Arial;font-size:16px'><textPath href='#p' side='right' proof:path-spacing='auto'>ABCDEFGHIJK</textPath></text>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        auto *text = cast<SPText>(doc->getObjectById("t")); ASSERT_TRUE(text);
        EXPECT_FALSE(text->layout.automaticPathSpacingSupported());
        for (auto const &g : text->layout.glyphs()) { EXPECT_TRUE(std::isfinite(g.x)); EXPECT_TRUE(std::isfinite(g.y)); }
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(std::any_of(written.refused.begin(), written.refused.end(), [](auto const &why) {
            return why.find("sharp, very tight or collapsed path") != std::string::npos;
        }));
    }
}

TEST_F(AiNativeImportTest, AutomaticPathTypeWordOverflowKeepsPrefixAtEveryAlignment)
{
    std::string const content = "Round the entire circle and then some " + std::string(40, 'W');
    for (auto const &[alignment, offset] : {std::pair{"start", "0"}, std::pair{"middle", "125.68"}, std::pair{"end", "251.36"}}) {
        SCOPED_TRACE(alignment);
        auto doc = svg_document(
            std::string(R"(<defs><path id="p" d="M20,60 C20,37.90861 37.90861,20 60,20 C82.09139,20 100,37.90861 100,60 C100,82.09139 82.09139,100 60,100 C37.90861,100 20,82.09139 20,60 Z"/></defs>)") +
            "<text id='t' style='font-family:Arial;font-size:16px;text-anchor:" + alignment + "'><textPath href='#p' startOffset='" + offset +
            "' proof:path-start='0' proof:path-end='251.36' proof:path-spacing='auto'>" + content + "</textPath></text>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        auto *text = cast<SPText>(doc->getObjectById("t")); ASSERT_TRUE(text);
        EXPECT_TRUE(text->layout.inputTruncated());
        EXPECT_EQ(text->layout.iteratorToCharIndex(text->layout.end()), content.size());
        // The word "some" does not fit. It must not become a partial word or replace the
        // beginning with the story's tail when the visible prefix is centred/right aligned.
        for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
            auto const index = text->layout.iteratorToCharIndex(it);
            if (content[index] == ' ') continue;
            EXPECT_EQ(it.hasGlyph(), index < 33) << index;
        }
    }
}

TEST_F(AiNativeImportTest, AutomaticPathTypeSharpImportRemainsFailClosed)
{
    auto const font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto data = loop_text_document("(ABCD\\r)", 5, font, "0 4");
    auto const at = data.find("/0 2 /6"); ASSERT_NE(at, std::string::npos);
    data.insert(at + 4, " /11 << /6 false >>");
    std::string error;
    auto ai = AN::read(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(data)), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("sharp, very tight or collapsed path"), std::string::npos) << error;
}

TEST_F(AiNativeImportTest, TypeOutsideTheWrittenSubsetIsRefusedByName)
{
    std::vector<std::pair<std::string, std::string>> const cases{
        {R"(<text x="10" y="20" style="writing-mode:vertical-rl">Hi</text>)", "vertical type"},
        {R"(<text x="10" y="20" style="inline-size:100px">Hi</text>)", "auto-wrapped type (inline-size)"},
        {R"(<defs><rect id="a" width="50" height="50"/><rect id="b" x="60" width="50" height="50"/></defs>)"
         R"x(<text style="shape-inside:url(#a) url(#b)">Hi</text>)x", "type flowed into several shapes"},
        {R"(<defs><rect id="a" width="100" height="50"/><rect id="b" width="10" height="10"/></defs>)"
         R"x(<text style="shape-inside:url(#a);shape-subtract:url(#b)">Hi</text>)x", "type flowed around other shapes (shape-subtract)"},
        {R"(<defs><rect id="a" width="100" height="50"/></defs>)"
         R"x(<text style="shape-inside:url(#a);shape-padding:4px">Hi</text>)x", "type flowed with padding inside its shape (shape-padding)"},
        {R"(<defs><rect id="a" width="100" height="50"/></defs>)"
         R"x(<text style="shape-inside:url(#a);text-align:justify">Hi there</text>)x", "justified type flowed into a shape"},
        {R"(<defs><rect id="a" width="30" height="14"/></defs>)"
         R"x(<text style="shape-inside:url(#a);font-size:12px">Hi there and more words than fit</text>)x",
         "type that overflows the shape it flows into"},
        {R"x(<text transform="skewX(20)" proof:text-frame="M0 0 H100 V50 H0 Z" x="10" y="20">Hi</text>)x", "area type stretched or skewed"},
        {R"(<text proof:text-frame="M0 0 H10 V10 H0 Z M20 0 H30 V10 H20 Z" x="2" y="8">Hi</text>)",
         "area type in a frame that isn't one outline"},
        {R"(<text proof:text-frame="M0 0 H100 L50 50 Z" x="10" y="20" style="text-align:justify">Hi</text>)",
         "justified type in a frame that isn't a rectangle"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p">Hi</textPath><textPath href="#p" startOffset="50">Hi</textPath></text>)",
         "type on several paths"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text x="10" y="80">Off <textPath href="#p">Hi</textPath></text>)",
         "type partly on a path"},
        {R"(<defs><path id="p" d="M10 50 L30 50"/></defs><text style="font-size:12px"><textPath href="#p">Too long for it</textPath></text>)",
         "type that runs off its path"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p" startOffset="-20">Hi</textPath></text>)",
         "type that starts off its path"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p">H<tspan style="baseline-shift:3px">i</tspan></textPath></text>)",
         "type on a path raised, lowered or shifted"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p" style="font-variant-position:super">Hi</textPath></text>)",
         "type on a path raised, lowered or shifted"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p" style="text-decoration-line:underline">Hi</textPath></text>)",
         "type on a path underlined or struck through"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p">H<tspan x="40">i</tspan></textPath></text>)",
         "characters on a path placed by x or y"},
        {R"x(<defs><path id="p" d="M10 50 L190 50"/></defs><text transform="scale(2 1)"><textPath href="#p">Hi</textPath></text>)x",
         "type on a path stretched or skewed"},
        {R"(<defs><path id="p" d="M10 50 L90 50 M110 50 L190 50"/></defs><text><textPath href="#p">Hi</textPath></text>)",
         "type on a path of several pieces, or none"},
        {R"(<text><textPath href="#nothing">Hi</textPath></text>)", "type on a path that isn't there"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p" proof:path-start="0" proof:path-end="bad">Hi</textPath></text>)",
         "type on a path with brackets that can't be written"},
        {R"(<defs><path id="p" d="M10 50 L190 50"/></defs><text><textPath href="#p" startOffset="30" proof:path-start="0" proof:path-end="100">Hi</textPath></text>)",
         "type on a path moved off its brackets"},
        {R"(<text x="10" y="20" dx="0 3">Hi</text>)", "characters moved or turned one by one (dx, dy, rotate)"},
        {R"(<text x="10" y="20" rotate="0 15">Hi</text>)", "characters moved or turned one by one (dx, dy, rotate)"},
        {R"(<text x="10" y="20" textLength="80">Hi</text>)", "type stretched to a length (textLength)"},
        {R"(<text x="10" y="20" style="direction:rtl">Hi<tspan direction="ltr">there</tspan></text>)",
         "type with mixed character directions or reading-order overrides"},
        {R"(<text x="10" y="20" style="unicode-bidi:isolate">Hi</text>)", "type with unsupported reading-order overrides"},
        {R"(<text x="10" y="20" style="direction:rtl;unicode-bidi:bidi-override">Hi</text>)",
         "type with unsupported reading-order overrides"},
        {R"(<text x="10" y="20" proof:ai-text-composer="unknown">Hi</text>)", "type with an unknown paragraph composer"},
        {"<text x=\"10\" y=\"20\" proof:ai-text-composer=\"middle-eastern\" proof:tab-origin=\"10\" "
         "proof:tab-interval=\"36\" xml:space=\"preserve\">A\tB</text>", "tabs with a Middle Eastern composer"},
        {"<text x=\"10\" y=\"20\" style=\"letter-spacing:1px\">\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d</text>",
         "bidirectional type with tracking or trailing spaces"},
        {"<text x=\"10\" y=\"20\" xml:space=\"preserve\">A\tB</text>", "tabs in type without tab stops"},
        {"<defs><rect id=\"a\" width=\"200\" height=\"50\"/></defs><text xml:space=\"preserve\" proof:tab-origin=\"0\" "
         "proof:tab-interval=\"36\" style=\"shape-inside:url(#a)\">A\tB</text>", "tabs in type flowed into a shape"},
        {"<defs><path id=\"p\" d=\"M10 50 L190 50\"/></defs><text xml:space=\"preserve\" proof:tab-origin=\"0\" "
         "proof:tab-interval=\"36\"><textPath href=\"#p\">A\tB</textPath></text>", "tabs on a path"},
        {"<text x=\"100\" y=\"20\" xml:space=\"preserve\" proof:tab-origin=\"100\" proof:tab-interval=\"36\" "
         "style=\"text-anchor:middle\">A\tB</text>", "tabs in type that isn't left-aligned"},
        {"<text x=\"10\" y=\"20\" xml:space=\"preserve\" proof:tab-origin=\"0\" proof:tab-interval=\"36\">A\tB</text>",
         "tab stops measured from elsewhere than the type's anchor"},
        {"<text x=\"10\" y=\"20\" xml:space=\"preserve\"><tspan proof:tab-origin=\"10\" proof:tab-interval=\"36\">A\t</tspan>"
         "<tspan proof:tab-origin=\"10\" proof:tab-interval=\"20\">B\tC</tspan></text>", "lines of type with two tab rulers"},
        {"<text proof:text-frame=\"M0 0 H200 V100 H0 Z\" xml:space=\"preserve\"><tspan x=\"0\" y=\"20\" proof:line-end=\"wrap\" "
         "proof:tab-origin=\"0\" proof:tab-interval=\"36\">A\t</tspan><tspan x=\"0\" y=\"40\" proof:tab-origin=\"0\" "
         "proof:tab-interval=\"20\">B\tC</tspan></text>", "paragraphs of type with two tab rulers"},
        {R"(<text><tspan x="10" y="20">A</tspan><tspan x="50" y="40">B</tspan></text>)", "lines of type that don't share one anchor"},
        // Inkscape leaves the space at the end of a text's last line out of its alignment, and
        // Illustrator doesn't: that line is centred on another anchor than the one before.
        {R"(<text xml:space="preserve" style="font-family:Arial;font-size:12px;text-anchor:middle">)"
         R"(<tspan sodipodi:role="line" x="100" y="20">A</tspan><tspan sodipodi:role="line" x="100" y="40">B </tspan></text>)",
         "centred or right-aligned lines that Illustrator would align apart (spaces or tracking at their ends)"},
        {R"(<text><tspan x="10" y="40">A</tspan><tspan x="10" y="20">B</tspan></text>)",
         "lines of type that don't each lie below the one before"},
        {R"(<text x="10" y="20">A<tspan x="50">B</tspan></text>)", "lines of type set in pieces"},
        {R"(<text x="10" y="20" style="font-family:PROOFNoSuchFamilyAtAll">Hi</text>)",
         "type in a font that isn't installed (PROOFNoSuchFamilyAtAll)"},
        {R"(<defs><linearGradient id="g"><stop stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs>)"
         R"x(<text x="10" y="20" style="fill:url(#g)">Hi</text>)x", "gradients on type"},
        {R"(<defs><pattern id="p" width="10" height="10" patternUnits="userSpaceOnUse"><rect width="5" height="5"/></pattern></defs>)"
         R"x(<text x="10" y="20" style="fill:url(#p)">Hi</text>)x", "patterns on type"},
        {R"(<text x="10" y="20" style="fill:#000000;fill-opacity:0.5">Hi</text>)", "translucent fills or strokes on type"},
        {R"(<text x="10" y="20" style="stroke:#000000;stroke-dasharray:2 1">Hi</text>)", "dashed strokes on type"},
        {R"(<text x="10" y="20" style="stroke:#000000;stroke-linejoin:round">Hi</text>)",
         "stroke caps or joins on type other than butt and miter"},
        {R"(<text x="10" y="20" style="stroke:#000000;paint-order:stroke">Hi</text>)", "strokes painted under type's fill"},
        {R"(<text x="10" y="20" style="text-transform:lowercase">Hi</text>)", "type set in lower case or capitalised"},
        {R"(<text x="10" y="20" style="font-variant-caps:all-small-caps">Hi</text>)", "capitals other than all caps and small caps"},
        {R"(<text x="10" y="20" style="font-variant-ligatures:discretionary-ligatures">Hi</text>)",
         "discretionary or historical ligatures"},
        {R"(<text x="10" y="20" style="font-variant-numeric:oldstyle-nums">Hi</text>)",
         "OpenType figure, alternate or East Asian forms on type"},
        {R"(<text x="10" y="20" style="font-feature-settings:'ss01'">Hi</text>)", "OpenType feature settings on type"},
        {R"(<text x="10" y="20" style="word-spacing:3px">Hi</text>)", "word spacing on type"},
        {R"(<text x="10" y="20">H<tspan style="baseline-shift:super">i</tspan></text>)",
         "superscript or subscript made by a baseline shift"},
        {R"(<text x="10" y="20" style="text-decoration-line:overline">Hi</text>)", "overlines on type"},
        {R"(<text x="10" y="20">H<tspan style="opacity:0.5">i</tspan></text>)", "transparency on part of a text"},
        {R"(<text x="10" y="20" style="font-size:2000px">Hi</text>)", "type sizes Illustrator can't set (under 0.1 or over 1296 pt)"},
        {R"(<defs><mask id="m"><rect width="20" height="20" fill="#ffffff"/></mask></defs>)"
         R"x(<text x="10" y="20" mask="url(#m)">Hi</text>)x", "opacity masks on type"},
        {R"(<flowRoot><flowRegion><rect width="100" height="50"/></flowRegion><flowPara>Hi</flowPara></flowRoot>)",
         "flowed type (SVG 1.2)"},
    };
    for (auto const &[body, reason] : cases) {
        SCOPED_TRACE(reason);
        auto doc = svg_document(body);
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end())
            << (written.refused.empty() ? std::string("nothing refused") : written.refused.front());
    }
}

TEST_F(AiNativeImportTest, BothWritersNameEveryFormOutsideTheirSubset)
{
    // The refusals no other test reaches, in both forms: the list the interchange document
    // publishes ("What the Illustrator 7 copy keeps and refuses").
    std::vector<std::pair<std::string, std::string>> const cases{
        {R"(<defs><marker id="m"><path d="M0 0 L5 5"/></marker></defs>)"
         R"x(<path d="M10 10 L50 50" style="stroke:#000000;marker-end:url(#m)"/>)x", "markers"},
        {R"(<path d="M10 10 L50 50" style="stroke:#000000" vector-effect="non-scaling-stroke"/>)", "non-scaling strokes"},
        {R"(<path d="M10 10 L50 50 L10 50 Z" style="fill:#ff0000;stroke:#000000;paint-order:stroke"/>)",
         "strokes painted under their fill"},
        {R"x(<path d="M10 10 L50 50" style="stroke:#000000" transform="skewX(20)"/>)x", "strokes on stretched or skewed objects"},
        {R"(<defs><clipPath id="c"><rect width="10" height="10"/><rect x="20" width="10" height="10"/></clipPath></defs>)"
         R"x(<g clip-path="url(#c)"><rect width="50" height="50"/></g>)x", "clipping paths other than one path"},
        {R"(<defs><linearGradient id="g" spreadMethod="reflect" x2="0.5"><stop stop-color="#ff0000"/>)"
         R"x(<stop offset="1" stop-color="#0000ff"/></linearGradient></defs><rect width="50" height="50" fill="url(#g)"/>)x",
         "repeating or reflected gradients"},
    };
    for (auto const &[body, reason] : cases) {
        SCOPED_TRACE(reason);
        auto doc = svg_document(body);
        ASSERT_TRUE(doc);
        for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
            auto const written = AN::write_records(doc.get(), version);
            EXPECT_TRUE(written.records.empty());
            EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end())
                << (written.refused.empty() ? std::string("nothing refused") : written.refused.front());
        }
    }
    // Type: the modern form writes ordinary point type; Illustrator 7's legacy type isn't written.
    auto type = svg_document(R"(<text x="10" y="20">Hi</text>)");
    ASSERT_TRUE(type);
    EXPECT_TRUE(AN::write_records(type.get()).refused.empty());
    auto const legacy = AN::write_records(type.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_NE(std::find(legacy.refused.begin(), legacy.refused.end(), "type (Illustrator 7)"), legacy.refused.end());
    // Transparency: Illustrator 7 has none, and the modern form refuses only a fill and a
    // stroke each with an opacity of its own, which takes an appearance.
    auto both = svg_document(R"(<rect width="50" height="50" style="fill:#ff0000;fill-opacity:0.5;stroke:#000000"/>)");
    ASSERT_TRUE(both);
    for (auto const &[version, reason] : {std::pair{AN::RecordsVersion::Modern, "separate fill and stroke transparency"},
                                          std::pair{AN::RecordsVersion::Illustrator7, "transparency"}}) {
        SCOPED_TRACE(reason);
        auto const written = AN::write_records(both.get(), version);
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), reason), written.refused.end());
    }
}

TEST_F(AiNativeImportTest, TheWriterRefusesWhatItCannotWriteByName)
{
    auto doc = svg_document(R"(<g inkscape:groupmode="layer"><text x="10" y="20" style="writing-mode:vertical-rl">Hi</text>)"
                            R"(<rect x="10" y="30" width="10" height="10" style="fill:#ff0000;stroke:#000000;stroke-opacity:0.5"/></g>)");
    ASSERT_TRUE(doc);
    auto const written = AN::write_records(doc.get());
    EXPECT_TRUE(written.records.empty());
    EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), "vertical type"), written.refused.end());
    EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), "separate fill and stroke transparency"),
              written.refused.end());

    // A document in millimetres: its artboard in points.
    auto a4 = svg_document(R"(<rect x="10" y="10" width="20" height="20" style="fill:#000000"/>)",
                           R"(width="210mm" height="297mm" viewBox="0 0 210 297")");
    auto const page = AN::write_records(a4.get());
    ASSERT_TRUE(page.refused.empty());
    EXPECT_NE(page.records.find("%AI3_Cropmarks: 0 -841.8898 595.2756 0"), std::string::npos) << page.records.substr(0, 900);
}

namespace {

/// A PNG of `w` by `h` straight RGBA pixels, as a data: address.
std::string png_uri(int w, int h, std::vector<std::uint8_t> const &rgba)
{
    auto *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    auto const stride = gdk_pixbuf_get_rowstride(pb);
    for (int y = 0; y < h; ++y) std::copy_n(rgba.data() + y * w * 4, w * 4, gdk_pixbuf_get_pixels(pb) + y * stride);
    gchar *buffer = nullptr;
    gsize size = 0;
    gdk_pixbuf_save_to_buffer(pb, &buffer, &size, "png", nullptr, nullptr);
    g_object_unref(pb);
    gchar *base64 = g_base64_encode(reinterpret_cast<guchar const *>(buffer), size);
    std::string out = std::string("data:image/png;base64,") + base64;
    g_free(base64);
    g_free(buffer);
    return out;
}

void expect_same_box(SPDocument *a, XML::Node *x, SPDocument *b, XML::Node *y)
{
    auto const was = shape_box(a, x), now = shape_box(b, y);
    ASSERT_TRUE(was && now);
    EXPECT_NEAR(now->left(), was->left(), 1e-3);
    EXPECT_NEAR(now->top(), was->top(), 1e-3);
    EXPECT_NEAR(now->right(), was->right(), 1e-3);
    EXPECT_NEAR(now->bottom(), was->bottom(), 1e-3);
}

} // namespace

TEST_F(AiNativeImportTest, WrittenPicturesKeepTheirCmykSamples)
{
    // An Illustrator CMYK picture, read, written and read again: its inks as they were.
    std::string const art = "%AI5_BeginRaster\n() 1 XG\n/DeviceCMYK XN\n[2 0 0 -2 50 40] 0 0 2 1 2 1 8 4 0 0 1 0 0\n"
                            "%%BeginData: 12\nXI\n\x10\x20\x30\x40\x50\x60\x70\x80\n%%EndData\nXH\n%AI5_EndRaster\n";
    auto b = make(records(layer(art)));
    ASSERT_TRUE(b);
    auto const written = AN::write_records(b->document.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    // As Illustrator writes it: the space, 8 bits CMYK without alpha, kind 2, the count of "XI",
    // its line end and the samples.
    EXPECT_NE(written.records.find("%AI5_File:\n%AI5_BeginRaster\n() 0 XG\n/DeviceCMYK XN\n"), std::string::npos);
    EXPECT_NE(written.records.find(" 2 1 0 Xh\n"), std::string::npos);
    EXPECT_NE(written.records.find(" 0 0 2 1 2 1 8 4 0 2 1 0 4 4 0 0\n%%BeginData: 11\nXI\n"
                                   "\x10\x20\x30\x40\x50\x60\x70\x80\n%%EndData\nXH\n%AI5_EndRaster\nN\n"),
              std::string::npos);
    std::string error;
    auto ai = AN::read(written.records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->images.size(), 1u);
    EXPECT_EQ(ai->images[0].channels, 4);
    EXPECT_EQ(ai->images[0].samples, std::string("\x10\x20\x30\x40\x50\x60\x70\x80", 8));
    EXPECT_TRUE(ai->images[0].alpha.empty());
    auto again = make(written.records);
    ASSERT_TRUE(again);
    auto const first = elements(b->document->getReprRoot(), "svg:image");
    auto const second = elements(again->document->getReprRoot(), "svg:image");
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    expect_same_box(b->document.get(), first[0], again->document.get(), second[0]);
    EXPECT_EQ(attr(second[0], "proof:samples"), attr(first[0], "proof:samples"));
}

TEST_F(AiNativeImportTest, WrittenPicturesKeepTheirGreySamples)
{
    // A grey picture prints on the black plate alone. Shown as RGB, it once came back RGB from
    // every save, and went to all four plates (the Illustrator 7 closeout, 2026-10-21).
    std::string const art = "%AI5_BeginRaster\n() 1 XG\n/DeviceGray XN\n[2 0 0 -2 50 40] 0 0 3 1 3 1 8 1 0 0 1 0 0\n"
                            "%%BeginData: 7\nXI\n\x10\x80\xf0\n%%EndData\nXH\n%AI5_EndRaster\n";
    auto b = make(records(layer(art)));
    ASSERT_TRUE(b);
    auto const first = elements(b->document->getReprRoot(), "svg:image");
    ASSERT_EQ(first.size(), 1u);
    EXPECT_FALSE(attr(first[0], "proof:samples").empty());
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        SCOPED_TRACE(static_cast<int>(version));
        auto const written = AN::write_records(b->document.get(), version);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        EXPECT_NE(written.records.find(version == AN::RecordsVersion::Modern ? "/DeviceGray XN\n" : "] 0 0 3 1 3 1 8 1 0 0 0 0\n"),
                  std::string::npos);
        std::string error;
        auto ai = AN::read(written.records, error);
        ASSERT_TRUE(ai) << error;
        ASSERT_EQ(ai->images.size(), 1u);
        EXPECT_EQ(ai->images[0].channels, 1);
        EXPECT_EQ(ai->images[0].samples, std::string("\x10\x80\xf0", 3));
        auto again = make(written.records);
        ASSERT_TRUE(again);
        auto const second = elements(again->document->getReprRoot(), "svg:image");
        ASSERT_EQ(second.size(), 1u);
        expect_same_box(b->document.get(), first[0], again->document.get(), second[0]);
        EXPECT_EQ(attr(second[0], "proof:samples"), attr(first[0], "proof:samples"));
    }
}

TEST_F(AiNativeImportTest, WrittenPicturesKeepTheirRgbAndTransparency)
{
    std::vector<std::uint8_t> const clear{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 255,
                                          10, 20, 30, 255, 40, 50, 60, 0, 70, 80, 90, 255};
    std::vector<std::uint8_t> opaque(clear);
    for (std::size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
    // Stretched and turned; and one fitted into a square box, as preserveAspectRatio's default.
    auto doc = svg_document(R"(<g inkscape:groupmode="layer" inkscape:label="Pictures">)"
                            R"(<image x="10" y="20" width="30" height="20" preserveAspectRatio="none" )"
                            R"x(transform="rotate(15 25 30)" href=")x" + png_uri(3, 2, clear) + R"("/>)"
                            R"(<image x="100" y="20" width="30" height="30" href=")" + png_uri(3, 2, opaque) + R"("/>)"
                            R"(</g>)");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_NE(written.records.find(" 0 0 3 2 3 2 8 3 1 17 1 0 4 4 0 0\n%%BeginData: 21\nXI\n"), std::string::npos);
    EXPECT_NE(written.records.find(" 0 0 3 2 3 2 8 3 0 1 1 0 4 4 0 0\n%%BeginData: 21\nXI\n"), std::string::npos);
    std::string error;
    auto ai = AN::read(written.records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->images.size(), 2u);
    auto const &img = ai->images[0];
    ASSERT_EQ(img.width, 3);
    ASSERT_EQ(img.height, 2);
    ASSERT_EQ(img.channels, 3);
    ASSERT_EQ(img.alpha.size(), 6u);
    for (int i = 0; i < 6; ++i) {
        auto const a = clear[i * 4 + 3];
        EXPECT_EQ(static_cast<std::uint8_t>(img.alpha[i]), a) << i;
        if (a == 0) continue; // A clear pixel has no colour to keep.
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(static_cast<std::uint8_t>(img.samples[i * 3 + c]), clear[i * 4 + c], a == 255 ? 0 : 2) << i;
        }
    }
    EXPECT_TRUE(ai->images[1].alpha.empty());
    auto again = make(written.records);
    ASSERT_TRUE(again);
    auto const before = elements(doc->getReprRoot(), "svg:image");
    auto const after = elements(again->document->getReprRoot(), "svg:image");
    ASSERT_EQ(after.size(), 2u);
    expect_same_box(doc.get(), before[0], again->document.get(), after[0]);
    // The fitted one lies in the middle of its box, 30 by 20: the picture, not its box, is written.
    auto const fitted = shape_box(again->document.get(), after[1]);
    ASSERT_TRUE(fitted);
    EXPECT_NEAR(fitted->top(), 25, 1e-3);
    EXPECT_NEAR(fitted->height(), 20, 1e-3);
    EXPECT_NEAR(fitted->left(), 100, 1e-3);
    EXPECT_NEAR(fitted->width(), 30, 1e-3);
}

TEST_F(AiNativeImportTest, WrittenTransparencyReadsBackOnEachObjectAndGroup)
{
    std::vector<std::uint8_t> const pixels{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255};
    auto doc = svg_document(
        R"(<defs><clipPath id="c"><rect x="50" y="50" width="15" height="15"/></clipPath></defs>)"
        R"(<g inkscape:groupmode="layer" inkscape:label="Art">)"
        // 0: an object's own blend mode and opacity.
        R"(<rect x="10" y="10" width="20" height="20" style="fill:#ff0000;opacity:0.5;mix-blend-mode:multiply"/>)"
        // 1: a group's, with isolated blending and knockout, and its members' own.
        R"(<g style="opacity:0.6;mix-blend-mode:screen;isolation:isolate" proof:knockout="on">)"
        R"(<rect x="40" y="10" width="20" height="20" style="fill:#00ff00;opacity:0.3"/>)"
        R"(<rect x="50" y="20" width="20" height="20" style="fill:#0000ff"/></g>)"
        // 2: after a group, an object as transparent as the group's first member.
        R"(<rect x="80" y="10" width="20" height="20" style="fill:#000000;opacity:0.3"/>)"
        // 3, 4: a plain group whose last member is transparent, then an object straight after it.
        R"(<g><rect x="110" y="10" width="20" height="20" style="fill:#ff00ff;opacity:0.4"/></g>)"
        R"(<rect x="140" y="10" width="20" height="20" style="fill:#ffff00;opacity:0.7"/>)"
        // 5: a compound path.
        R"(<path d="M 10 50 H 40 V 80 H 10 Z M 20 60 V 70 H 30 V 60 Z" style="fill:#00ffff;opacity:0.7;mix-blend-mode:overlay"/>)"
        // 6: a clipped object: its clipping group has its transparency.
        R"x(<rect x="50" y="50" width="30" height="30" clip-path="url(#c)" style="fill:#ff0000;opacity:0.5;mix-blend-mode:darken"/>)x"
        // 7, 8: a paint's own opacity, when it is the only paint, with the object's.
        R"(<rect x="90" y="50" width="20" height="20" style="fill:#ff0000;fill-opacity:0.25"/>)"
        R"(<path d="M 120 50 L 150 80" style="fill:none;stroke:#000000;stroke-width:2;stroke-opacity:0.4;opacity:0.5"/>)"
        // 9: knockout neither on nor off, and the knockout shape.
        R"(<rect x="10" y="90" width="20" height="20" style="fill:#000000" proof:knockout="neutral" proof:knockout-shape="true"/>)"
        // 10, 11: a clone with transparency of its own, of an opaque object.
        R"(<rect id="source" x="40" y="90" width="20" height="20" style="fill:#00ff00"/>)"
        R"x(<use href="#source" transform="translate(30 0)" style="opacity:0.5;mix-blend-mode:luminosity"/>)x"
        // 12: nested groups, each with its own.
        R"(<g style="opacity:0.8"><g style="opacity:0.5"><rect x="100" y="90" width="20" height="20" style="fill:#0000ff"/></g></g>)"
        // 13: a picture; 14, 15: a cropped one, then an object straight after its clipping group.
        R"(<image x="130" y="90" width="20" height="20" preserveAspectRatio="none" style="opacity:0.5;mix-blend-mode:hue" href=")" +
        png_uri(2, 2, pixels) + R"("/>)"
        R"(<image x="10" y="120" width="30" height="10" preserveAspectRatio="xMidYMid slice" style="opacity:0.4" href=")" +
        png_uri(2, 2, pixels) + R"("/>)"
        R"(<rect x="50" y="120" width="10" height="10" style="fill:#000000;opacity:0.9"/>)"
        R"(</g>)",
        R"(width="200" height="150" viewBox="0 0 200 150" )");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    auto const &r = written.records;
    // As Illustrator writes them: an object's before it, a group's after its end with Xd and
    // a style marker, and a group with none of its own ended by `9 () XW`.
    EXPECT_NE(r.find("1 0.5 0 0 0 Xy\n"), std::string::npos);
    EXPECT_NE(r.find("U\n2 0.6 1 1 0 Xy\n0 0 Xd\n6 () XW\n"), std::string::npos);
    EXPECT_NE(r.find("U\n9 () XW\n"), std::string::npos);
    EXPECT_NE(r.find("Q\n8 0.5 0 0 0 Xy\n0 0 Xd\n6 () XW\n"), std::string::npos);
    EXPECT_NE(r.find("Q\n9 () XW\n"), std::string::npos);
    EXPECT_EQ(AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7).records, "");

    std::string error;
    auto ai = AN::read(r, error);
    ASSERT_TRUE(ai) << error;
    auto const &art = ai->layers.at(0).children;
    ASSERT_EQ(art.size(), 16u) << r.substr(r.find("%%EndSetup"));
    auto expect = [](AN::Node const &n, int blend, double opacity, bool isolate = false, int knockout = 0,
                     bool shape = false) {
        EXPECT_EQ(n.transparency.blend, blend);
        EXPECT_NEAR(n.transparency.opacity, opacity, 1e-4);
        EXPECT_EQ(n.transparency.isolate, isolate);
        EXPECT_EQ(n.transparency.knockout, knockout);
        EXPECT_EQ(n.transparency.knockout_shape, shape);
    };
    using K = AN::Node::Kind;
    {
        SCOPED_TRACE("object");
        EXPECT_EQ(art[0].kind, K::Path);
        expect(art[0], 1, 0.5);
    }
    {
        SCOPED_TRACE("group");
        ASSERT_EQ(art[1].kind, K::Group);
        expect(art[1], 2, 0.6, true, 1);
        ASSERT_EQ(art[1].children.size(), 2u);
        expect(art[1].children[0], 0, 0.3);
        expect(art[1].children[1], 0, 1);
        expect(art[2], 0, 0.3);
    }
    {
        SCOPED_TRACE("plain group, then an object");
        ASSERT_EQ(art[3].kind, K::Group);
        expect(art[3], 0, 1);
        expect(art[3].children.at(0), 0, 0.4);
        expect(art[4], 0, 0.7);
    }
    {
        SCOPED_TRACE("compound path");
        EXPECT_EQ(art[5].kind, K::Compound);
        expect(art[5], 3, 0.7);
    }
    {
        SCOPED_TRACE("clipped object");
        ASSERT_EQ(art[6].kind, K::Group);
        EXPECT_TRUE(art[6].clipped);
        expect(art[6], 8, 0.5);
        expect(art[6].children.back(), 0, 1);
    }
    {
        SCOPED_TRACE("paint opacity");
        expect(art[7], 0, 0.25);
        expect(art[8], 0, 0.2);
        expect(art[9], 0, 1, false, 2, true);
    }
    {
        SCOPED_TRACE("clone");
        expect(art[10], 0, 1);
        ASSERT_EQ(art[11].kind, K::Group);
        expect(art[11], 15, 0.5);
        expect(art[11].children.at(0), 0, 1);
    }
    {
        SCOPED_TRACE("nested groups");
        ASSERT_EQ(art[12].kind, K::Group);
        expect(art[12], 0, 0.8);
        ASSERT_EQ(art[12].children.at(0).kind, K::Group);
        expect(art[12].children.at(0), 0, 0.5);
        expect(art[12].children.at(0).children.at(0), 0, 1);
    }
    {
        SCOPED_TRACE("pictures");
        EXPECT_EQ(art[13].kind, K::Image);
        expect(art[13], 12, 0.5);
        ASSERT_EQ(art[14].kind, K::Group);
        EXPECT_TRUE(art[14].clipped);
        expect(art[14], 0, 1);
        EXPECT_EQ(art[14].children.back().kind, K::Image);
        expect(art[14].children.back(), 0, 0.4);
        expect(art[15], 0, 0.9);
    }

    // And in the document the import makes: CSS as the import writes it.
    auto again = make(r);
    ASSERT_TRUE(again);
    bool group = false, knockout = false;
    for (auto *g : elements(again->document->getReprRoot(), "svg:g")) {
        auto const style = attr(g, "style");
        if (style.find("mix-blend-mode:screen") == std::string::npos) continue;
        group = style.find("opacity:0.6") != std::string::npos && style.find("isolation:isolate") != std::string::npos;
        knockout = attr(g, "proof:knockout") == "on";
    }
    EXPECT_TRUE(group);
    EXPECT_TRUE(knockout);
}

TEST_F(AiNativeImportTest, PicturesTheWriterCannotWriteAreRefusedByName)
{
    std::vector<std::uint8_t> const pixel{10, 20, 30, 255};
    struct Case { std::string image, reason; };
    std::vector<Case> const cases{
        {R"(<image x="0" y="0" width="10" height="10" href="data:image/svg+xml;base64,PHN2Zy8+"/>)", "SVG pictures"},
    };
    for (auto const &test : cases) {
        SCOPED_TRACE(test.reason);
        auto doc = svg_document(test.image);
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), test.reason), written.refused.end());
    }
    auto embedded = svg_document(R"(<image x="0" y="0" width="10" height="10" href=")" + png_uri(1, 1, pixel) + R"("/>)");
    ASSERT_TRUE(embedded);
    EXPECT_TRUE(AN::write_records(embedded.get()).refused.empty());
    auto const legacy = AN::write_records(embedded.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(legacy.refused.empty());
}

TEST_F(AiNativeImportTest, LegacyPicturesKeepOpaquePixelsTransformsAndFitting)
{
    std::vector<std::uint8_t> const pixels{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
                                          10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255};
    auto const address = png_uri(3, 2, pixels);
    auto doc = svg_document(R"x(<image x="10" y="20" width="30" height="20" preserveAspectRatio="none"
        transform="rotate(15 25 30)" href=")x" + address + R"x("/>
        <image x="100" y="20" width="30" height="30" href=")x" + address +
        R"x("/><rect x="10" y="70" width="30" height="10" fill="#2060c0"/>)x");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const written = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_TRUE(std::all_of(written.records.begin(), written.records.end(), [](unsigned char c) { return c < 128; }));
    EXPECT_NE(written.records.find("%AI9_ColorModel: 1\n"), std::string::npos);
    EXPECT_NE(written.records.find(" 0 0 3 2 3 2 8 3 0 0 0 0\n%%BeginData: 41 ASCII Bytes\nXI\n%"),
              std::string::npos);
    EXPECT_NE(written.records.find("%AI5_EndRaster\nN\n"), std::string::npos);
    std::string error;
    auto ai = AN::read(written.records, error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(ai->cmyk);
    ASSERT_EQ(ai->images.size(), 2u);
    for (auto const &image : ai->images) {
        EXPECT_EQ(image.width, 3);
        EXPECT_EQ(image.height, 2);
        EXPECT_EQ(image.channels, 3);
        EXPECT_TRUE(image.alpha.empty());
        for (unsigned i = 0; i < 6; ++i) for (unsigned c = 0; c < 3; ++c) {
            EXPECT_EQ(static_cast<std::uint8_t>(image.samples[i * 3 + c]), pixels[i * 4 + c]);
        }
    }
    auto again = make(written.records);
    ASSERT_TRUE(again);
    auto const first = elements(doc->getReprRoot(), "svg:image");
    auto const second = elements(again->document->getReprRoot(), "svg:image");
    ASSERT_EQ(second.size(), 2u);
    expect_same_box(doc.get(), first[0], again->document.get(), second[0]);
    auto const fitted = shape_box(again->document.get(), second[1]);
    ASSERT_TRUE(fitted);
    EXPECT_NEAR(fitted->top(), 25, 1e-3);
    EXPECT_NEAR(fitted->height(), 20, 1e-3);
    EXPECT_EQ(elements(again->document->getReprRoot(), "svg:path").size(), 1u);
    auto clear = pixels;
    clear[3] = 128;
    auto transparent = svg_document(R"x(<image width="30" height="20" href=")x" + png_uri(3, 2, clear) + R"x("/>)x");
    ASSERT_TRUE(transparent);
    transparent->ensureUpToDate();
    auto const refused = AN::write_records(transparent.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(refused.records.empty());
    EXPECT_EQ(refused.refused, std::vector<std::string>{"picture transparency (Illustrator 7)"});
}

TEST_F(AiNativeImportTest, LegacyPicturesKeepGrayAndCmykSamples)
{
    for (bool cmyk : {false, true}) {
        SCOPED_TRACE(cmyk);
        std::string const samples = cmyk ? std::string("\x10\x20\x30\x40\x50\x60\x70\x80", 8)
                                         : std::string("\x30\xa0", 2);
        auto const folder = temporary_folder();
        ASSERT_FALSE(folder.empty());
        auto const path = folder + G_DIR_SEPARATOR_S + "sample.tif";
        auto *tiff = TIFFOpen(path.c_str(), "w");
        ASSERT_NE(tiff, nullptr);
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 2);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 1);
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, cmyk ? 4 : 1);
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, cmyk ? PHOTOMETRIC_SEPARATED : PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        std::vector<unsigned char> row(samples.begin(), samples.end());
        int const result = TIFFWriteScanline(tiff, row.data(), 0);
        TIFFClose(tiff);
        ASSERT_GE(result, 0);
        gchar *data = nullptr;
        gsize size = 0;
        ASSERT_TRUE(g_file_get_contents(path.c_str(), &data, &size, nullptr));
        gchar *base64 = g_base64_encode(reinterpret_cast<guchar const *>(data), size);
        std::string const address = "data:image/tiff;base64," + std::string(base64);
        g_free(base64);
        g_free(data);
        g_remove(path.c_str());
        g_rmdir(folder.c_str());
        auto before = svg_document(R"x(<image x="10" y="20" width="30" height="15" href=")x" + address + R"x("/>)x");
        ASSERT_TRUE(before);
        before->getReprRoot()->setAttribute("proof:color-mode", cmyk ? "CMYK" : "RGB");
        before->ensureUpToDate();
        auto const written = AN::write_records(before.get(), AN::RecordsVersion::Illustrator7);
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        std::string error;
        auto ai = AN::read(written.records, error);
        ASSERT_TRUE(ai) << error;
        EXPECT_EQ(ai->cmyk, cmyk);
        ASSERT_EQ(ai->images.size(), 1u);
        EXPECT_EQ(ai->images[0].channels, cmyk ? 4 : 1);
        EXPECT_EQ(ai->images[0].samples, samples);
        auto after = make(written.records);
        ASSERT_TRUE(after);
        auto const a = elements(before->getReprRoot(), "svg:image");
        auto const b = elements(after->document->getReprRoot(), "svg:image");
        ASSERT_EQ(a.size(), 1u);
        ASSERT_EQ(b.size(), 1u);
        expect_same_box(before.get(), a[0], after->document.get(), b[0]);
    }
}

TEST_F(AiNativeImportTest, WrittenLinksKeepTheirFileAndTransformedPixelBounds)
{
    gchar *dir = g_dir_make_tmp("proof-written-links-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    std::string const file = std::string(dir) + G_DIR_SEPARATOR_S + "sample (linked).png";
    write_picture(file);
    gchar *uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);
    ASSERT_NE(uri, nullptr);
    auto doc = svg_document(
        R"x(<g transform="translate(5 7) rotate(15)"><image id="linked" x="20" y="30" width="80" height="20"
            preserveAspectRatio="none" href=")x" + std::string(uri) + R"x("/></g>
        <image x="120" y="90" width="40" height="40" href=")x" + std::string(uri) + R"x("/>
        <rect x="10" y="160" width="20" height="20" fill="#2060c0"/>)x");
    g_free(uri);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_EQ(written.records.find("%AI5_BeginRaster"), std::string::npos);
    EXPECT_NE(written.records.find("%AI5_BeginPlace\n%%FileType: 1\n"), std::string::npos);
    EXPECT_NE(written.records.find("sample \\(linked\\).png"), std::string::npos);
    std::string error;
    auto ai = AN::read(written.records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->placed.size(), 2u);
    EXPECT_TRUE(ai->images.empty());
    for (auto const &link : ai->placed) {
        EXPECT_EQ(link.path, file);
        EXPECT_DOUBLE_EQ(link.box.width(), 4);
        EXPECT_DOUBLE_EQ(link.box.height(), 2);
    }
    auto again = make(written.records);
    ASSERT_TRUE(again);
    auto const before = elements(doc->getReprRoot(), "svg:image");
    auto const after = elements(again->document->getReprRoot(), "svg:image");
    ASSERT_EQ(after.size(), 2u);
    expect_same_box(doc.get(), before[0], again->document.get(), after[0]);
    auto const fitted = shape_box(again->document.get(), after[1]);
    ASSERT_TRUE(fitted);
    EXPECT_NEAR(fitted->left(), 120, 1e-3);
    EXPECT_NEAR(fitted->top(), 100, 1e-3);
    EXPECT_NEAR(fitted->width(), 40, 1e-3);
    EXPECT_NEAR(fitted->height(), 20, 1e-3);
    EXPECT_EQ(elements(again->document->getReprRoot(), "svg:path").size(), 1u);
    auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_EQ(legacy.refused, std::vector<std::string>{"linked pictures (Illustrator 7)"});
    g_remove(file.c_str());
    g_rmdir(dir);
    g_free(dir);
}

TEST_F(AiNativeImportTest, WrittenLinksRefuseFilesThatDisappear)
{
    gchar *dir = g_dir_make_tmp("proof-written-missing-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    std::string const file = std::string(dir) + G_DIR_SEPARATOR_S + "sample.png";
    write_picture(file);
    gchar *uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);
    ASSERT_NE(uri, nullptr);
    auto doc = svg_document(R"x(<image x="10" y="20" width="40" height="20" href=")x" +
                            std::string(uri) + R"x("/>)x");
    g_free(uri);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    ASSERT_TRUE(AN::write_records(doc.get()).refused.empty());
    ASSERT_EQ(g_remove(file.c_str()), 0);
    auto const written = AN::write_records(doc.get());
    EXPECT_TRUE(written.records.empty());
    EXPECT_EQ(written.refused, std::vector<std::string>{"missing or unreadable linked pictures"});
    g_rmdir(dir);
    g_free(dir);
}

TEST_F(AiNativeImportTest, AMissingLinkIsWrittenAsIllustratorKeepsIt)
{
    // Illustrator 29.2.1 saves a document whose linked picture is gone with its place record as it
    // was: the file's address, the box, and the small picture it keeps of it. An import's is
    // written so too; Illustrator shows that picture in its place, or fits the file into the box
    // when it finds it, however PROOF moved or scaled it (its layout of PROOF's control).
    auto const rows = argb(255, 255, 0, 0) + argb(255, 0, 255, 0) + argb(255, 0, 0, 255) + argb(128, 255, 255, 255) +
                      argb(255, 0x33, 0x66, 0x99) + argb(255, 0x33, 0x66, 0x99) + argb(255, 0x33, 0x66, 0x99) +
                      argb(0, 0, 0, 0);
    auto const gone = std::string("Q:\\Jobs\\gone.png");
    auto b = make(records(layer("0 g\n" + rect + linked(gone, {}, kept_preview(4, 2, rows)))));
    ASSERT_TRUE(b);
    auto const image = elements(b->document->getReprRoot(), "svg:image").at(0);
    auto const where = drawn(b->document.get(), image);
    ASSERT_TRUE(where);
    auto const written = AN::write_records(b->document.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    EXPECT_NE(written.records.find("%%IncludeFile:" + gone + "\n%AI17_Begin_Content_if_version_gt:24 5\n\n"
                                   "%AI26_BeginPlacedObjectPreview\n4 2 16 32 17 " + rows + "\n%AI26_EndPlacedObjectPreview\n"
                                   "%AI17_End_Versioned_Content\n~\n%AI5_EndPlace\n"),
              std::string::npos);

    // Opened again: the same link, missing, its picture kept and shown where it was.
    auto reopened = [&](AN::Built const &from) {
        auto const again = AN::write_records(from.document.get());
        EXPECT_TRUE(again.refused.empty());
        return make(again.records);
    };
    auto again = reopened(*b);
    ASSERT_TRUE(again);
    ASSERT_EQ(again->missing_links.size(), 1u);
    EXPECT_TRUE(again->missing_links[0].preview);
    auto const back = elements(again->document->getReprRoot(), "svg:image").at(0);
    EXPECT_EQ(attr(back, "sodipodi:absref"), gone);
    EXPECT_EQ(attr(back, SPImage::LINK_PREVIEW_ATTRIBUTE), attr(image, SPImage::LINK_PREVIEW_ATTRIBUTE));
    auto const now = drawn(again->document.get(), back);
    ASSERT_TRUE(now);
    EXPECT_TRUE(Geom::are_near(now->min(), where->min(), 1e-6)) << now->min();
    EXPECT_TRUE(Geom::are_near(now->max(), where->max(), 1e-6)) << now->max();

    // Moved and scaled in PROOF (the picture's own box changes, as SPImage::set_transform makes it):
    // written in its new box.
    auto *item = cast<SPItem>(b->document->getObjectByRepr(image));
    ASSERT_TRUE(item);
    item->set_i2d_affine(item->i2dt_affine() * Geom::Scale(0.5) * Geom::Translate(30, 5));
    item->doWriteTransform(item->transform);
    auto const scaled = drawn(b->document.get(), image);
    ASSERT_TRUE(scaled);
    EXPECT_NEAR(scaled->width(), where->width() / 2, 1e-6);
    again = reopened(*b);
    ASSERT_TRUE(again);
    auto const moved = drawn(again->document.get(), elements(again->document->getReprRoot(), "svg:image").at(0));
    ASSERT_TRUE(moved);
    EXPECT_TRUE(Geom::are_near(moved->min(), scaled->min(), 1e-6)) << moved->min() << " " << scaled->min();
    EXPECT_TRUE(Geom::are_near(moved->max(), scaled->max(), 1e-6)) << moved->max() << " " << scaled->max();

    // Not without the picture Illustrator kept, nor filling the box some other way than whole.
    auto plain = make(records(layer(linked(gone))));
    ASSERT_TRUE(plain);
    EXPECT_EQ(AN::write_records(plain->document.get()).refused, std::vector<std::string>{"missing or unreadable linked pictures"});
    image->setAttribute("preserveAspectRatio", "xMidYMid");
    EXPECT_EQ(AN::write_records(b->document.get()).refused, std::vector<std::string>{"missing linked pictures that don't fill their box"});
    image->setAttribute("preserveAspectRatio", "none");
    image->setAttribute(SPImage::LINK_PREVIEW_ATTRIBUTE, "data:image/png;base64,AAAA");
    EXPECT_EQ(AN::write_records(b->document.get()).refused, std::vector<std::string>{"missing linked pictures whose preview can't be read"});
}

TEST_F(AiNativeImportTest, WrittenLinksRefuseAChangedFileUntilItIsReloaded)
{
    gchar *dir = g_dir_make_tmp("proof-written-changed-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    std::string const file = std::string(dir) + G_DIR_SEPARATOR_S + "sample.png";
    write_picture(file);
    gchar *uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);
    ASSERT_NE(uri, nullptr);
    auto doc = svg_document(R"x(<image x="10" y="20" width="40" height="20" href=")x" +
                            std::string(uri) + R"x("/>)x");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const path = std::filesystem::path(std::u8string(file.begin(), file.end()));
    auto const loaded = std::filesystem::last_write_time(path);
    write_picture(file, 2, 4); // The file now has a different aspect ratio from the loaded pixels.
    std::filesystem::last_write_time(path, loaded + std::chrono::seconds(10));
    auto const written = AN::write_records(doc.get());
    EXPECT_TRUE(written.records.empty());
    EXPECT_EQ(written.refused, std::vector<std::string>{"linked pictures changed since they were loaded"});
    auto reloaded = svg_document(R"x(<image width="40" height="20" href=")x" + std::string(uri) + R"x("/>)x");
    ASSERT_TRUE(reloaded);
    reloaded->ensureUpToDate();
    EXPECT_TRUE(AN::write_records(reloaded.get()).refused.empty());
    g_free(uri);
    g_remove(file.c_str());
    g_rmdir(dir);
    g_free(dir);
}

// Records for checking in Illustrator: written to PROOF_AI_WRITER_OUT when it is set.
TEST_F(AiNativeImportTest, WriterRecordsForIllustrator)
{
    auto const *out = g_getenv("PROOF_AI_WRITER_OUT");
    if (!out) GTEST_SKIP() << "PROOF_AI_WRITER_OUT is not set";
    auto doc = svg_document(
        R"x(<defs><clipPath id="c"><circle cx="60" cy="160" r="35"/></clipPath></defs>)x"
        R"x(<g inkscape:groupmode="layer" inkscape:label="Process" inkscape:highlight-color="#4fff4f">)x"
        R"x(<rect x="20" y="20" width="80" height="60" style="fill:device-cmyk(0 0.9 0.2 0);stroke:device-cmyk(0 0 0 1);stroke-width:3"/>)x"
        R"x(<path d="M 120 20 H 200 V 80 H 120 Z M 140 35 V 65 H 180 V 35 Z" style="fill:#2060c0;fill-rule:evenodd"/>)x"
        R"x(<path d="M 220 20 C 260 0 300 60 340 30" style="fill:none;stroke:device-cmyk(1 0 0 0);stroke-width:4;stroke-dasharray:12 6;stroke-linecap:round" proof:overprint="stroke"/>)x"
        R"x(</g>)x"
        R"x(<g inkscape:groupmode="layer" inkscape:label="Ink">)x"
        R"x(<rect id="full" x="220" y="60" width="50" height="40" style="fill:#000000"/>)x"
        R"x(<rect id="tint" x="290" y="60" width="50" height="40" style="fill:#000000"/>)x"
        R"x(<g clip-path="url(#c)"><rect x="20" y="120" width="80" height="80" style="fill:device-cmyk(0 0 1 0)"/>)x"
        R"x(<rect x="60" y="120" width="40" height="80" style="fill:device-cmyk(0 0 0 0.6)"/></g>)x"
        R"x(</g>)x"
        R"x(<g inkscape:groupmode="layer" inkscape:label="Скрытый" style="display:none"><rect x="150" y="120" width="40" height="40" style="fill:#ff0000"/></g>)x"
        R"x(<g inkscape:groupmode="layer" inkscape:label="Locked" sodipodi:insensitive="true"><rect x="220" y="140" width="40" height="40" style="fill:device-cmyk(0.5 0 0.5 0)"/></g>)x"
        R"x(<sodipodi:namedview><inkscape:page x="0" y="0" width="360" height="220" inkscape:label="Front"/>)x"
        R"x(<inkscape:page x="400" y="0" width="200" height="220" inkscape:label="Обратная"/></sodipodi:namedview>)x",
        R"x(width="360" height="220" viewBox="0 0 360 220" proof:color-mode="CMYK")x");
    ASSERT_TRUE(doc);
    auto const ink = SpotInk::Ink{"PROOF Orange", Colors::Color(Type::CMYK, {0, 0.6, 1, 0}), 1.0};
    doc->getObjectById("full")->setAttribute("style", "fill:url(#" + SpotInk::ensure(doc.get(), ink) + ")");
    auto tint = ink;
    tint.tint = 0.3;
    doc->getObjectById("tint")->setAttribute("style", "fill:url(#" + SpotInk::ensure(doc.get(), tint) + ")");
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    std::ofstream(std::filesystem::path(out) / "writer-w1.records", std::ios::binary) << written.records;
}

#if defined(WITH_POPPLER) && defined(WITH_CAPYPDF)
namespace {
Extension::Output *native_output_for_test()
{
    if (!Extension::db.get("org.inkscape.output.pdf.capypdf")) Extension::Internal::PdfOutput::init();
    if (!Extension::db.get("org.inkscape.output.ai.native")) Extension::Internal::AiNativeOutput::init();
    return dynamic_cast<Extension::Output *>(Extension::db.get("org.inkscape.output.ai.native"));
}
}

TEST_F(AiNativeImportTest, NativeSaveKeepsIndividuallyHiddenArtworkOutOfItsPdf)
{
    if (!Extension::Internal::PdfOutput::supports_pdf17()) GTEST_SKIP();
    auto *output = native_output_for_test();
    ASSERT_TRUE(output);
    output->set_gui(false);
    auto const picture = png_uri(1, 1, {255, 0, 0, 255});
    auto doc = svg_document("<rect x='10' y='10' width='20' height='20' fill='#008000'/>"
        "<rect width='200' height='100' style='display:none;fill:#0000ff'/>"
        "<image width='200' height='100' href='" + picture + "' style='display:none'/>"
        "<text x='5' y='70' style='font-family:Arial;font-size:48px;display:none'>Hidden</text>");
    ASSERT_TRUE(doc);
    auto const before = sp_repr_save_buf(doc->getReprDoc());
    auto const records = AN::write_records(doc.get());
    ASSERT_TRUE(records.refused.empty()) << records.refused.front();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    gchar *dir = g_dir_make_tmp("proof-hidden-pdf-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const folder = dir, path = folder + "/hidden.ai";
    g_free(dir);
    auto cleanup = scope_exit([&] { g_unlink(path.c_str()); g_rmdir(folder.c_str()); });
    output->save(doc.get(), path.c_str());
    std::string reason;
    auto const opened = Extension::Internal::open_ai_native(path, reason, Extension::Internal::AiKeepDiffering::All);
    ASSERT_TRUE(opened) << reason;
    ASSERT_EQ(opened->differences.size(), 1u);
    EXPECT_LT(opened->differences[0].lightness, 0.002);
    EXPECT_LT(opened->differences[0].colour, 0.002);
    auto const images = elements(opened->document->getReprRoot(), "svg:image");
    ASSERT_EQ(images.size(), 1u); // Hidden artwork remains editable in the native model.
    EXPECT_NE(attr(images[0], "style").find("display:none"), std::string::npos);
    EXPECT_EQ(elements(opened->document->getReprRoot(), "svg:text").size(), 1u);
}

TEST_F(AiNativeImportTest, NativeSaveAsRefusesWithoutChangingTheDocumentOrPreviousFile)
{
    if (!Extension::Internal::PdfOutput::supports_pdf17()) GTEST_SKIP() << "bundled PDF backend required";
    auto *native = native_output_for_test();
    ASSERT_TRUE(native);
    EXPECT_FALSE(native->savecopy_only());
    native->set_gui(false);
    auto doc = svg_document(R"(<text x="10" y="20" style="writing-mode:vertical-rl">Keep live type</text>)");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    doc->setModifiedSinceSave(true);
    auto const before = sp_repr_save_buf(doc->getReprDoc());
    gchar *dir = g_dir_make_tmp("proof-native-refusal-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const path = std::string(dir) + "/previous.ai";
    g_free(dir);
    ASSERT_TRUE(g_file_set_contents(path.c_str(), "previous file", -1, nullptr));
    try {
        native->save(doc.get(), path.c_str());
        FAIL() << "live type must be refused";
    } catch (std::runtime_error const &e) {
        EXPECT_NE(std::string(e.what()).find("type"), std::string::npos);
    }
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    gchar *contents = nullptr;
    ASSERT_TRUE(g_file_get_contents(path.c_str(), &contents, nullptr, nullptr));
    EXPECT_STREQ(contents, "previous file");
    g_free(contents);
}

TEST_F(AiNativeImportTest, NativeSaveDoesNotMutateTheDocumentOrInheritPdfOptions)
{
    if (!Extension::Internal::PdfOutput::supports_pdf17()) GTEST_SKIP() << "bundled PDF backend required";
    auto *native = native_output_for_test();
    auto *pdf = dynamic_cast<Extension::Output *>(Extension::db.get("org.inkscape.output.pdf.capypdf"));
    ASSERT_TRUE(native && pdf);
    native->set_gui(false);
    auto doc = svg_document(R"(<rect x="10" y="10" width="30" height="20" fill="#ff0000"/>)");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    doc->setModifiedSinceSave(true);
    auto const before = sp_repr_save_buf(doc->getReprDoc());
    auto const pages = doc->getPageManager().getPageCount();
    gchar *dir = g_dir_make_tmp("proof-native-save-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const path = std::string(dir) + "/saved.ai";
    g_free(dir);
    std::string const previous = pdf->get_param_optiongroup("textToPath");
    pdf->set_param_optiongroup("textToPath", "LaTeX");
    EXPECT_NO_THROW(native->save(doc.get(), path.c_str()));
    EXPECT_TRUE(pdf->get_param_optiongroup_is("textToPath", "LaTeX"));
    pdf->set_param_optiongroup("textToPath", previous.c_str());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(doc->getPageManager().getPageCount(), pages);
    unsigned files = 0;
    for (auto const &entry : std::filesystem::directory_iterator(std::filesystem::path(path).parent_path())) {
        if (entry.is_regular_file()) ++files;
    }
    EXPECT_EQ(files, 1u) << "no LaTeX sidecar or incomplete temporary file";
    std::string error;
    auto records = Extension::Internal::read_ai_native_records(path, {}, 64 << 20, error);
    ASSERT_TRUE(records) << error;
    EXPECT_NE(records->text.find("%%Creator: PROOF"), std::string::npos);
}

TEST_F(AiNativeImportTest, PrivateDataGoesOnEveryPageAndReadsBack)
{
    // Two pages, as a PDF export leaves them, with nothing of Illustrator's.
    std::map<int, std::string> objects;
    objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    objects[2] = "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>";
    objects[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R /LastModified (D:20000101000000Z) >>";
    objects[4] = "<< /Length 24 >>\nstream\n0 0 0 1 k 0 0 10 10 re f\nendstream";
    objects[5] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 100] /Contents 4 0 R >>";
    std::ostringstream pdf;
    pdf << "%PDF-1.7\n";
    std::vector<std::size_t> offsets(objects.size() + 1);
    for (auto const &[n, body] : objects) {
        offsets[n] = std::size_t(pdf.tellp());
        pdf << n << " 0 obj\n" << body << "\nendobj\n";
    }
    auto const xref = pdf.tellp();
    pdf << "xref\n0 " << offsets.size() << "\n0000000000 65535 f \n";
    for (std::size_t n = 1; n < offsets.size(); ++n) pdf << std::setw(10) << std::setfill('0') << offsets[n] << " 00000 n \n";
    pdf << "trailer\n<< /Size " << offsets.size() << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    gchar *dir = g_dir_make_tmp("proof-native-output-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    std::string const path = std::string(dir) + "/written.ai";
    g_free(dir);
    auto const bytes = pdf.str();
    ASSERT_TRUE(g_file_set_contents(path.c_str(), bytes.data(), bytes.size(), nullptr));

    // Records that don't compress below 64 KiB, so they take several blocks.
    std::string const header = "%!PS-Adobe-3.0 \r\n%%Creator: PROOF\r\n%AI5_FileFormat 14.0\r\n%%EndComments\r\n";
    std::string records = header;
    std::uint32_t state = 12345;
    for (int line = 0; line < 5000; ++line) {
        records += "%_";
        for (int i = 0; i < 60; ++i) {
            state = state * 1664525u + 1013904223u;
            records += "0123456789abcdef"[state >> 28];
        }
        records += "\r\n";
    }
    std::string error;
    ASSERT_TRUE(AN::add_private_data(path, records, error)) << error;

    auto const back = Extension::Internal::read_ai_native_records(path, {}, 64 << 20, error);
    ASSERT_TRUE(back) << error;
    EXPECT_EQ(back->encoding, "AI24 Zstandard");
    EXPECT_TRUE(back->text == records);

    if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    auto doc = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
    ASSERT_TRUE(doc && doc->isOk());
    ASSERT_EQ(doc->getNumPages(), 2);
    Ref holder{-1, -1};
    for (int i = 1; i <= 2; ++i) {
        auto *page = doc->getPage(i);
        ASSERT_TRUE(page);
        // The page keeps what it had.
        EXPECT_NEAR(page->getMediaBox().x2, i == 1 ? 200 : 300, 1e-9);
        Object const object = doc->getXRef()->fetch(page->getRef());
        Object const modified = object.dictLookup("LastModified");
        ASSERT_TRUE(modified.isString());
        EXPECT_NE(std::string(modified.getString()), "D:20000101000000Z");
        Object const piece = object.dictLookup("PieceInfo");
        ASSERT_TRUE(piece.isDict());
        Object const &named = piece.getDict()->lookupNF("Illustrator");
        ASSERT_TRUE(named.isRef());
        if (i == 1) holder = named.getRef();
        EXPECT_EQ(named.getRef(), holder) << "every page names the one copy";
    }
    Object const illustrator = doc->getXRef()->fetch(holder);
    Object const priv = illustrator.dictLookup("Private");
    ASSERT_TRUE(priv.isDict());
    Object const blocks = priv.dictLookup("NumBlock");
    ASSERT_TRUE(blocks.isInt());
    EXPECT_GE(blocks.getInt(), 2);
    EXPECT_TRUE(priv.dictLookup("AIPrivateData" + std::to_string(blocks.getInt())).isStream());
    EXPECT_TRUE(priv.dictLookup("AIPrivateData" + std::to_string(blocks.getInt() + 1)).isNull());
    EXPECT_EQ(priv.dictLookup("ContainerVersion").getInt(), 12);
    Object meta = priv.dictLookup("AIMetaData");
    ASSERT_TRUE(meta.isStream());
    std::string meta_text;
    meta.getStream()->fillString(meta_text);
    EXPECT_EQ(meta_text, header);

    // Records without a header aren't Illustrator's; nothing is added.
    GStatBuf before;
    ASSERT_EQ(g_stat(path.c_str(), &before), 0);
    EXPECT_FALSE(AN::add_private_data(path, "%_no header\r\n", error));
    EXPECT_EQ(error, "the records have no header");
    GStatBuf after;
    ASSERT_EQ(g_stat(path.c_str(), &after), 0);
    EXPECT_EQ(before.st_size, after.st_size);
}
#endif
