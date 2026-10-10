// SPDX-License-Identifier: GPL-2.0-or-later
// Hand-written native records and PDF pages: no production artwork in these tests.
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <glib.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include "colors/color.h"
#include "colors/document-colors.h"
#include "colors/spaces/cms.h"
#include "doc-per-case-test.h"
#include "document.h"
#include "file.h"
#include "extension/internal/ai/ai-native-import.h"
#include "extension/internal/ai/ai-native-text.h"
#include "libnrtype/font-factory.h"
#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
#include "extension/internal/pdfinput/ai-native-open.h"
#endif
#include "object/sp-defs.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "spot-ink.h"
#include "style.h"
#include "svg/svg.h"
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
                    std::string const &font = {})
{
    std::map<int, std::string> objects;
    objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    std::string kids;
    auto stream = [](std::string const &s) { return "<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "\nendstream"; };
    objects[3] = "<< /AIPrivateData1 4 0 R /NumBlock 1 /ContainerVersion 11 /CreatorVersion 16 /RoundtripVersion 16 >>";
    objects[4] = stream(native);
    std::string resources = "<< >>";
    if (!font.empty()) {
        int const id = 5 + int(contents.size()) * 2;
        objects[id] = "<< /Type /Font /Subtype /Type1 /BaseFont /" + font + " /Encoding /WinAnsiEncoding >>";
        resources = "<< /Font << /F1 " + std::to_string(id) + " 0 R >> >>";
    }
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
}

TEST_F(AiNativeImportTest, PointTextRefusesBrokenUnicodeFramesStylesAndEncodings)
{
    for (auto const &doc : {
             text_document("(\\376\\377\\000A\\330\\075\\336\\000\\000\\015)", 2),
             text_document("(Hi\\r)", 3, "ArialMT", "/6 0"),
             text_document("(Hi\\r)", 3, "ArialMT", "/9 2"),
             text_document("(Hi\\r)", 1, "ArialMT", {}, "/2 [1 0 0 1 -90 -10]",
                           "<< /1 2 /0 << /0 << /6 << /6 1.2 >> >> >> >>"),
             text_document("(Hi\\r)", 3, "ArialMT", "/2 true"),
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

TEST_F(AiNativeImportTest, PointTextMissingExactFontUsesFallback)
{
    auto source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
                          text_setup(text_document("(Hello\\r)", 6, "PROOF-Missing-Unique-Font-734")));
    std::string error; auto ai = AN::read(source, error); ASSERT_TRUE(ai);
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("exact font"), std::string::npos) << error;
}

TEST_F(AiNativeImportTest, PointTextMissingGlyphRefusesImplicitFontSubstitution)
{
    auto font = available_text_font();
    if (font.empty()) GTEST_SKIP() << "none of the test fonts is installed";
    auto source = records(layer("/AI11Text :\n0 /StoryIndex ,\n;\n"),
        text_setup(text_document("(\\376\\377\\333\\377\\337\\375\\000\\015)", 3, font)));
    std::string error; auto ai = AN::read(source, error); ASSERT_TRUE(ai);
    EXPECT_FALSE(AN::build(*ai, error));
    EXPECT_NE(error.find("doesn't contain every text character"), std::string::npos) << error;
    ai->layers[0].visible = false;
    auto hidden = AN::build(*ai, error); ASSERT_TRUE(hidden) << error;
    EXPECT_TRUE(elements(hidden->document->getReprRoot(), "svg:text").empty());
    ASSERT_EQ(hidden->notes.size(), 1u);
    EXPECT_NE(hidden->notes[0].find("left out"), std::string::npos);
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
    auto bad = AN::read_text_document(text_setup(aligned_text_document(3)), Geom::Point(100, 50));
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

#if defined(WITH_POPPLER) && defined(HAVE_POPPLER_CAIRO)
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
                "/MediaBox [0 0 200 100]", font), reason, false, diagnostics ? diagnostics : "");
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
    auto diagnostic = Extension::Internal::open_ai_native(bad, reason, true);
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
    auto d = Extension::Internal::open_ai_native(path, reason, true);
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
    auto first = Extension::Internal::open_ai_native(path, reason, false, {}, &cache);
    ASSERT_TRUE(first) << reason;
    EXPECT_EQ(cache.records_reused, 0u);
    EXPECT_EQ(cache.references_reused, 0u);
    auto second = Extension::Internal::open_ai_native(path, reason, false, {}, &cache);
    ASSERT_TRUE(second) << reason;
    EXPECT_EQ(cache.records_reused, 1u);
    EXPECT_EQ(cache.references_reused, 1u);
    ASSERT_EQ(first->differences.size(), second->differences.size());
    EXPECT_DOUBLE_EQ(first->differences[0].lightness, second->differences[0].lightness);
    EXPECT_DOUBLE_EQ(first->differences[0].colour, second->differences[0].colour);

    // A damaged reference is redrawn; a damaged record cache is decoded again.
    auto const png = cache.reference_directory + "/page-1.png";
    ASSERT_TRUE(g_file_set_contents(png.c_str(), "damaged", 7, nullptr));
    ASSERT_TRUE(Extension::Internal::open_ai_native(path, reason, false, {}, &cache)) << reason;
    EXPECT_EQ(cache.records_reused, 1u);
    EXPECT_EQ(cache.references_reused, 0u);
    auto const text = cache.records_directory + "/records.txt";
    ASSERT_TRUE(g_file_set_contents(text.c_str(), "damaged", 7, nullptr));
    ASSERT_TRUE(Extension::Internal::open_ai_native(path, reason, false, {}, &cache)) << reason;
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
        auto result = Extension::Internal::open_ai_native(utf8(path), reason, true, pictures ? pictures : "", &cache);
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
        std::fflush(stdout);
    }
}
#endif
