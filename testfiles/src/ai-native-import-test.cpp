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
#include "extension/internal/ai/ai-native-import.h"
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
                    std::string const &boxes = "/MediaBox [0 0 200 100] /TrimBox [0 0 200 100]")
{
    std::map<int, std::string> objects;
    objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    std::string kids;
    auto stream = [](std::string const &s) { return "<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "\nendstream"; };
    objects[3] = "<< /AIPrivateData1 4 0 R /NumBlock 1 /ContainerVersion 11 /CreatorVersion 16 /RoundtripVersion 16 >>";
    objects[4] = stream(native);
    for (std::size_t i = 0; i < contents.size(); ++i) {
        int const p = 5 + int(i) * 2;
        kids += std::to_string(p) + " 0 R ";
        objects[p] = "<< /Type /Page /Parent 2 0 R " + boxes + " /Resources << >> /Contents " + std::to_string(p + 1) +
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
    EXPECT_NE(error.find("type that shows"), std::string::npos);
    ai->layers[0].visible = false;
    auto b = AN::build(*ai, error);
    ASSERT_TRUE(b) << error;
    ASSERT_EQ(b->notes.size(), 1u);
    EXPECT_NE(b->notes[0].find("text object"), std::string::npos);
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
        auto result = Extension::Internal::open_ai_native(utf8(path), reason, true, pictures ? pictures : "");
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
        std::fflush(stdout);
    }
}
#endif
