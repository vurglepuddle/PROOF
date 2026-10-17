// SPDX-License-Identifier: GPL-2.0-or-later
// Hand-written native records and PDF pages: no production artwork in these tests.
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <GlobalParams.h>
// Windows' headers, which Poppler's bring in, make these words nothing.
#undef near
#undef far
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <gtest/gtest.h>
#include <tiffio.h>
#include <zlib.h>
#include <2geom/affine.h>
#include <2geom/pathvector.h>

#include "colors/cms/profile.h"
#include "colors/color.h"
#include "display/drawing-item.h"
#include "display/drawing.h"
#include "colors/document-colors.h"
#include "colors/spaces/cms.h"
#include "desktop-style.h"
#include "display/cairo-utils.h"
#include "display/control/font-highlight.h"
#include "doc-per-case-test.h"
#include "document.h"
#include "file.h"
#include "extension/internal/ai/ai-native-fonts.h"
#include "extension/internal/ai/ai-native-import.h"
#include "extension/internal/ai/ai-native-text.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
#include "extension/internal/pdfinput/ai-native-open.h"
#endif
#include "object/sp-defs.h"
#include "object/sp-image.h"
#include "psd-fixture.h"
#include "object/sp-item.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "page-manager.h"
#include "spot-ink.h"
#include "style.h"
#include "svg/svg.h"
#include "type-settings.h"
#include "util/units.h"
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
                    std::string const &font = {}, std::string const &profile = {}, std::string const &shading = {})
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
    resources = "<<" + resources + " >>";
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
}

TEST_F(AiNativeImportTest, MeshKeepsEditableCornersAndHandlesAfterReopen)
{
    auto b = make(records(layer(mesh_art("DeviceRGB", {"1 0 0", "0 1 0", "0 0 1", "1 1 1"})), {}, false));
    ASSERT_TRUE(b);
    auto check = [&](SPDocument *doc) {
        doc->ensureUpToDate();
        auto gradients = elements(doc->getDefs()->getRepr(), "svg:meshgradient");
        ASSERT_EQ(gradients.size(), 1u);
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
    for (auto options : {"/11 << /0 true >>", "/11 << /4 12 >>", "/11 << /18 12 >>", "/11 << /22 0.05 >>",
                         "/11 << /6 true >>", "/11 << /7 true >>", "/11 0", "/11 << /4 false >>", "/11 << /123 0 >>"}) {
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
        "<< /0 << /5 << /35 1 /32 << /1 18 /8 100 /16 1 /18 false "
        "/53 << /0 << /0 0 /1 [1 0] >> >> >> >> >> >> "
        "<< /0 << /6 0 /5 << /32 << /1 24 >> >> >> >> "
        "<< /0 << /6 1 /5 << /35 0 /32 << /8 150 >> >> >> >>";
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
    auto justified = with_paragraphs(area_text_document("(Hi\\r)", 3, {{0, 10, 3}}),
        "<< /0 << /5 << /0 3 >> >> >>", "<< /1 3 /0 << /0 0 >> >>");
    auto refused = AN::read_text_document(text_setup(justified), Geom::Point(100, 50));
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
    // A space at the line's end is part of what Illustrator centred, and isn't of what is
    // centred here: the line is set from where its glyphs start instead.
    auto spaced = aligned_text_document(2);
    spaced.replace(spaced.find("(HHH\\r)"), 7, "(HH \\r)");
    auto texts = AN::read_text_document(text_setup(spaced), Geom::Point(100, 50));
    ASSERT_TRUE(texts->stories[0]) << texts->errors[0];
    EXPECT_EQ(texts->stories[0]->lines.at(0).alignment, 0u);
    EXPECT_TRUE(Geom::are_near(texts->stories[0]->lines[0].origin * texts->stories[0]->to_art, Geom::Point(51.2, 60), 1e-9));
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
    // Justified lines aren't set the way Illustrator spaces them.
    auto const justified = " /5 << /0 [ << /1 6 /0 << /0 << /5 << /0 3 >> >> >> >> ] >>";
    texts = AN::read_text_document(text_setup(area_text_document("(Hello\\r)", 6, {{0, 12, 6}}, "ArialMT", {}, justified)),
                                   Geom::Point(100, 50));
    EXPECT_FALSE(texts->stories[0]);

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
        auto at = data.find(" /6 << /0 [");
        data.insert(at, " /5 << /0 [ << /1 5 /0 << /0 << /5 << /0 " + std::to_string(alignment) + direction + " >> >> >> >> ] >>");
        // What Illustrator's composer for these scripts leaves with the segment.
        std::string const segment = "<< /99 /S >>";
        at = data.find(segment);
        if (ordered) data.replace(at, segment.size(), "<< /99 /S /15 << /0 5 /2 5 /5 true /7 << /0 5 >> >> >>");
        return data;
    };
    auto read = AN::read_text_document(text_setup(hebrew(" /35 1")), Geom::Point(100, 50));
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
    auto rtl = build(" /35 1", &text_style, &span_style);
    EXPECT_NE(text_style.find("direction:rtl;"), std::string::npos) << text_style;
    EXPECT_EQ(span_style, "text-anchor:end;");
    auto ltr = build(" /35 0", &text_style, &span_style);
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
    auto bad = AN::read_text_document(text_setup(hebrew(" /35 2")), Geom::Point(100, 50));
    ASSERT_EQ(bad->stories.size(), 1u);
    EXPECT_FALSE(bad->stories[0]);
    // Where Illustrator didn't put such letters in order it shows them backwards, set from
    // the left as they were typed. The story says so, and they are set that way here.
    auto backwards = AN::read_text_document(text_setup(hebrew(" /35 0", 0, false)), Geom::Point(100, 50));
    ASSERT_EQ(backwards->stories.size(), 1u);
    ASSERT_TRUE(backwards->stories[0]) << backwards->errors.at(0);
    EXPECT_TRUE(backwards->stories[0]->unordered);
    EXPECT_FALSE(backwards->stories[0]->rtl);
    EXPECT_FALSE(read->stories[0]->unordered);
    EXPECT_FALSE(AN::same_drawing(*backwards->stories[0], *read->stories[0]));
    // Nothing says how a paragraph read from the right sets them unordered.
    auto odd = AN::read_text_document(text_setup(hebrew(" /35 1", 0, false)), Geom::Point(100, 50));
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
    auto const as_typed = places(hebrew(" /35 0", 0, false), &text_style, &notes);
    EXPECT_NE(text_style.find("direction:ltr;unicode-bidi:bidi-override;"), std::string::npos) << text_style;
    ASSERT_EQ(as_typed.size(), 4u);
    // The first letter typed is the leftmost, and so on.
    EXPECT_TRUE(std::is_sorted(as_typed.begin(), as_typed.end())) << as_typed[0] << " " << as_typed[3];
    EXPECT_TRUE(std::any_of(notes.begin(), notes.end(), [](std::string const &n) { return n.find("show backwards") != std::string::npos; }));
    // In order, it is the rightmost; and nothing is said.
    auto const in_order = places(hebrew(" /35 0"), &text_style, &notes);
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
        auto const reference = build(" /35 0", &text_style, &span_style);
        auto b = make(records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"), text_setup(hebrew(" /35 0", alignment))));
        ASSERT_TRUE(b) << alignment;
        auto texts = elements(b->document->getReprRoot(), "svg:text");
        ASSERT_EQ(texts.size(), 1u);
        EXPECT_EQ(attr(elements(texts[0], "svg:tspan").at(0), "style"), std::string("text-anchor:") + anchor + ";") << alignment;
        auto const drawn_here = drawn(b->document.get(), texts[0]);
        ASSERT_TRUE(reference && drawn_here);
        EXPECT_NEAR(drawn_here->left(), reference->left(), 0.01) << alignment;
    }
    // Justified area type is another matter: its lines are stretched to its frame.
    auto justified_point = AN::read_text_document(text_setup(hebrew(" /35 0", 4)), Geom::Point(100, 50));
    ASSERT_TRUE(justified_point->stories.at(0)) << justified_point->errors.at(0);
    auto unknown = AN::read_text_document(text_setup(hebrew(" /35 0", 7)), Geom::Point(100, 50));
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

TEST_F(AiNativeImportTest, ALinkedPdfPageShowsItsFirstPageCroppedToItsCropBox)
{
    // The collaborator's linked .ai and .pdf files (2026-10-17): Illustrator places a page and
    // keeps it as it is. PROOF shows the first page's crop box, drawn by Poppler.
    auto const placed = fixture(records(layer("0 g\n" + rect)), {"0 0 1 0 k 0 0 200 100 re f"}); // 200 by 100
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

    // Another page than the first, or another box than its crop box: refused, saying why.
    ai = AN::read(records(layer(page(placed, 200, 100, 2))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("page 2"), std::string::npos) << error;
    ai = AN::read(records(layer(page(placed, 150, 100, 1))), error);
    ASSERT_TRUE(ai) << error;
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("crop box"), std::string::npos) << error;
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

TEST_F(AiNativeImportTest, Corpus)
{
    auto const *roots = g_getenv("PROOF_AI_CORPUS");
    auto const *file = g_getenv("PROOF_AI_FILE");
    if (!roots && !file) GTEST_SKIP() << "set PROOF_AI_CORPUS to approved folders, or PROOF_AI_FILE to an approved file";
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
        std::fflush(stdout);
    }
}
#endif
