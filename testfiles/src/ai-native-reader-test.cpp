// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: Illustrator native records, the scanner and the reader.
 *
 * The fixtures are hand-written records in Illustrator's format (see
 * doc/PROOF-ai-native-records.md); nothing here comes from files another app wrote.
 *
 * With PROOF_AI_CORPUS set to folders of .ai files (separated by ';'), the corpus case
 * reads every file and reports how many read natively. Use only files the user approved.
 */
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>
#include <2geom/bezier-curve.h>

#include "extension/internal/ai/ai-native-lexer.h"
#include "extension/internal/ai/ai-native-reader.h"
#ifdef WITH_POPPLER
#include "extension/internal/pdfinput/ai-private-data.h"
#endif

using namespace Inkscape::Extension::Internal::AiNative;
using namespace std::literals;

namespace {

std::string utf8(std::filesystem::path const &p)
{
    auto const s = p.u8string();
    return {s.begin(), s.end()};
}

std::filesystem::path from_utf8(std::string const &s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::vector<Token> all_tokens(std::string_view src)
{
    Lexer lex(src);
    std::vector<Token> out;
    while (auto t = lex.next()) {
        out.push_back(*t);
        if (out.size() > 10000) break;
    }
    return out;
}

/// Records of a w x h page: header comments, `setup`, `body` (layers), the trailer.
std::string records(std::string const &body, std::string const &setup = {}, double w = 200, double h = 100)
{
    auto const W = std::to_string(w), H = std::to_string(h);
    return "%!PS-Adobe-3.0\n%%Creator: PROOF tests\n%%BoundingBox: 0 0 " + W + " " + H +
           "\n%%HiResBoundingBox: 0 0 " + W + " " + H + "\n%AI5_FileFormat 14.0\n%AI3_Cropmarks: 0 0 " + W + " " + H +
           "\n%AI9_ColorModel: 2\n%%EndComments\n%%BeginProlog\n/junk { 1 2 3 } def\n%%EndProlog\n%%BeginSetup\n" + setup +
           "%%EndSetup\n" + body + "%%PageTrailer\ngsave annotatepage grestore showpage\n%%Trailer\n%%EOF\n";
}

/// A layer named `name` (shown, unlocked, printing, light blue) holding `art`.
std::string layer(std::string const &name, std::string const &art, std::string const &lb = "1 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb")
{
    return "%AI5_BeginLayer\n" + lb + "\n(" + name + ") Ln\n0 A\n0 Xw\n" + art + "LB\n%AI5_EndLayer--\n";
}

std::string rect(double x0, double y0, double x1, double y1)
{
    auto s = [](double v) { return std::to_string(v); };
    return s(x0) + " " + s(y0) + " m\n" + s(x0) + " " + s(y1) + " L\n" + s(x1) + " " + s(y1) + " L\n" + s(x1) + " " +
           s(y0) + " L\n" + s(x0) + " " + s(y0) + " L\n";
}

Document read_ok(std::string const &recs)
{
    std::string error;
    auto doc = read(recs, error);
    EXPECT_TRUE(doc) << error;
    return doc ? std::move(*doc) : Document{};
}

std::string read_error(std::string const &recs)
{
    std::string error;
    auto doc = read(recs, error);
    EXPECT_FALSE(doc);
    return error;
}

} // namespace

// ---- the scanner -------------------------------------------------------------------------

TEST(AiNativeLexer, HiddenLinesAndComments)
{
    auto toks = all_tokens("%AI5_BeginLayer\r\n1 (a\\)b) Ln\r%_/X : ;\n0 %_note 7\n<4142>\n");
    ASSERT_EQ(toks.size(), 9u);
    EXPECT_EQ(toks[0].kind, Token::Kind::Comment);
    EXPECT_EQ(toks[0].view, "AI5_BeginLayer");
    EXPECT_EQ(toks[1].number, 1.0);
    EXPECT_EQ(toks[2].string, "a)b");
    EXPECT_EQ(toks[3].view, "Ln");
    EXPECT_EQ(toks[4].kind, Token::Kind::Name);
    EXPECT_TRUE(toks[4].hidden);
    EXPECT_EQ(toks[5].view, ":");
    EXPECT_TRUE(toks[6].hidden);
    EXPECT_FALSE(toks[7].hidden); // a %_ later in a line is a comment
    EXPECT_EQ(toks[8].string, "AB");
}

TEST(AiNativeLexer, GradientOperatorsEndingALineAreRead)
{
    // A gradient's ramp count and stops end their lines with an operator after "%_".
    auto toks = all_tokens("1 %_Br\n0 0 0 1 1 1 6 50 100 %_BS \r\n0 0 50 100 %_Bs\n3 %_Bsx\n4 %_BS 5\n");
    ASSERT_EQ(toks.size(), 19u);
    EXPECT_EQ(toks[1].kind, Token::Kind::Word);
    EXPECT_EQ(toks[1].view, "Br");
    EXPECT_TRUE(toks[1].hidden);
    EXPECT_FALSE(toks[2].hidden);
    EXPECT_EQ(toks[11].view, "BS");
    EXPECT_TRUE(toks[11].hidden);
    EXPECT_EQ(toks[16].view, "Bs");
    // Anything else after "%_" in a line stays a comment.
    EXPECT_EQ(toks[17].number, 3.0);
    EXPECT_EQ(toks[18].number, 4.0);
}

TEST(AiNativeLexer, ImageDataIsOneToken)
{
    // The count starts after the comment's \r: "\nXI\n" and 3 samples; an alpha channel
    // after them isn't counted.
    auto toks = all_tokens("[1 0 0 1 0 0] 0 0 1 1\r\n%%BeginData: 7\r\nXI\n(%)A%%EndData\r\nXH\r\n");
    auto it = std::find_if(toks.begin(), toks.end(), [](auto const &t) { return t.kind == Token::Kind::Data; });
    ASSERT_NE(it, toks.end());
    EXPECT_EQ(it->view, "(%)A");
    EXPECT_EQ(toks.back().view, "XH");
    // A thumbnail's hex lines are comments.
    toks = all_tokens("%%BeginData: 9 Hex Bytes\r\n%00FF\r\n%%EndData\r\n1\r\n");
    EXPECT_EQ(toks.back().number, 1.0);
}

TEST(AiNativeLexer, PlacedPreviewsAreSkipped)
{
    auto toks = all_tokens("%AI26_BeginPlacedObjectPreview\r\n2 1 (\x01\xff\r\n%AI26_EndPlacedObjectPreview\r\nN\r\n"s);
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks.back().view, "N");
}

TEST(AiNativeLexer, StringsAndNumbers)
{
    auto toks = all_tokens("(a(b)c\\101\\\nd) -3.5 .5 +2 1e-3 1x2 nan 1e999");
    EXPECT_EQ(toks[0].string, "a(b)cAd");
    EXPECT_EQ(toks[1].number, -3.5);
    EXPECT_EQ(toks[2].number, 0.5);
    EXPECT_EQ(toks[3].number, 2.0);
    EXPECT_DOUBLE_EQ(toks[4].number, 0.001);
    EXPECT_EQ(toks[5].kind, Token::Kind::Word); // 1x2
    EXPECT_EQ(toks[6].kind, Token::Kind::Word); // nan
    EXPECT_EQ(toks[7].kind, Token::Kind::Word); // out of range
}

TEST(AiNativeLexer, HostileInputEnds)
{
    for (auto src : {"(unterminated \\"sv, "<12"sv, "%%BeginData: 99999999999\r\nXI"sv, "%%BeginData: 5\rXI\r"sv, "/"sv,
                     "%%BeginData: 5\r"sv, "%_"sv, "\0\0\0"sv}) {
        EXPECT_LT(all_tokens(src).size(), 10u) << src;
    }
}

TEST(AiNativeLexer, SkipPast)
{
    Lexer lex("/Binary : /ASCII85Decode ,\r\n%_(abc[%\r\n%x~>\r\n%_; 1");
    for (int i = 0; i < 4; ++i) lex.next();
    EXPECT_EQ(lex.skip_past("~>"), "\r\n%_(abc[%\r\n%x~>");
    EXPECT_EQ(lex.next()->view, ";");
    EXPECT_EQ(lex.next()->number, 1.0);
}

// ---- the reader: structure ---------------------------------------------------------------

TEST(AiNativeReader, LayersAndOptions)
{
    auto doc = read_ok(records(layer("Back", "0 g\n" + rect(10, 10, 50, 50) + "f\n") +
                               layer("Notes", "", "0 1 0 0 1 0 0 3 0 0 0 0 70 0 Lb")));
    ASSERT_EQ(doc.layers.size(), 2u);
    auto const &back = doc.layers[0];
    EXPECT_EQ(back.name, "Back");
    EXPECT_TRUE(back.visible);
    EXPECT_FALSE(back.locked);
    EXPECT_TRUE(back.layer.printable);
    EXPECT_EQ(back.layer.color_index, 0);
    ASSERT_EQ(back.children.size(), 1u);
    auto const &notes = doc.layers[1];
    EXPECT_FALSE(notes.visible);
    EXPECT_TRUE(notes.locked);
    EXPECT_FALSE(notes.layer.printable);
    EXPECT_EQ(notes.layer.dim, 70);
    EXPECT_EQ(notes.layer.color_index, 3);
    EXPECT_TRUE(doc.cmyk); // %AI9_ColorModel: 2
}

TEST(AiNativeReader, PathsAndPainting)
{
    auto doc = read_ok(records(layer("L", "0 g\n2.5 w 1 J 2 j 4 M [3 2] 1 d\n10 10 m\n20 10 l\n30 20 25 30 20 30 c\n15 30 10 20 v\n10 15 10 12 y\nb\n")));
    auto const &p = doc.layers[0].children.at(0);
    EXPECT_EQ(p.kind, Node::Kind::Path);
    ASSERT_EQ(p.path.size(), 1u);
    EXPECT_TRUE(p.path[0].closed()); // lower-case painting closes
    EXPECT_EQ(p.path[0].size_open(), 4u);
    ASSERT_TRUE(p.fill && p.stroke);
    EXPECT_EQ(p.stroke_style.width, 2.5);
    EXPECT_EQ(p.stroke_style.cap, 1);
    EXPECT_EQ(p.stroke_style.join, 2);
    EXPECT_EQ(p.stroke_style.miter, 4.0);
    EXPECT_EQ(p.stroke_style.dash, (std::vector<double>{3, 2}));
    EXPECT_EQ(p.stroke_style.dash_offset, 1.0);
    // v: the first control point is the current point; y: the second is the end.
    auto const &v = dynamic_cast<Geom::CubicBezier const &>(p.path[0][2]);
    EXPECT_EQ(v[1], Geom::Point(20, 30));
    auto const &y = dynamic_cast<Geom::CubicBezier const &>(p.path[0][3]);
    EXPECT_EQ(y[2], Geom::Point(10, 12));
}

TEST(AiNativeReader, GroupsCompoundsAndClipping)
{
    std::string art = "u\n0 g\n" + rect(0, 0, 10, 10) + "f\n" + rect(20, 0, 30, 10) + "f\nU\n";
    art += "*u\n1 XR\n" + rect(0, 0, 100, 100) + "f\n" + rect(10, 10, 90, 90) + "f\n*U\n";
    art += "q\n" + rect(0, 0, 50, 50) + "h W n\n" + rect(0, 0, 80, 80) + "f\nQ\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 3u);
    EXPECT_EQ(kids[0].kind, Node::Kind::Group);
    EXPECT_EQ(kids[0].children.size(), 2u);
    EXPECT_EQ(kids[1].kind, Node::Kind::Compound);
    EXPECT_TRUE(kids[1].evenodd);
    EXPECT_TRUE(kids[1].fill);
    EXPECT_EQ(kids[2].kind, Node::Kind::Group);
    EXPECT_TRUE(kids[2].clipped);
    ASSERT_EQ(kids[2].children.size(), 2u);
    EXPECT_TRUE(kids[2].children[0].clipping);
    EXPECT_FALSE(kids[2].children[0].fill);
}

TEST(AiNativeReader, NamesStateAndTransparency)
{
    std::string art = "1 Xw 1 A 0 g\n" + rect(0, 0, 10, 10) + "f\n";
    art += "%_/ArtDictionary :\n%_/XMLUID : (My_Rect__x23_2) ; (AI10_ArtUID) ,\n%_(0.5) /String (BBAccumRotation) ,\n%_;\n";
    art += "0 Xw 0 A\nu\n" + rect(0, 0, 5, 5) + "f\nU\n2 0.5 1 1 0 Xy\n";
    art += "/ArtDictionary :\n(Shapes) /UnicodeString (AIArtName) ,\n;\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_FALSE(kids[0].visible);
    EXPECT_TRUE(kids[0].locked);
    EXPECT_EQ(kids[0].name, "My Rect #2");
    EXPECT_EQ(kids[0].box_rotation, 0.5);
    EXPECT_EQ(kids[1].name, "Shapes");
    EXPECT_EQ(kids[1].transparency.blend, 2); // Screen
    EXPECT_EQ(kids[1].transparency.opacity, 0.5);
    EXPECT_TRUE(kids[1].transparency.isolate);
    EXPECT_EQ(kids[1].transparency.knockout, 1);
}

TEST(AiNativeReader, ArtboardsAndSpace)
{
    std::string setup = "%AI9_BeginDocumentData\n%_/Document :\n%_/Dictionary :\n%_0 /Int (CropAreaActive) ,\n%_/Array :\n"
                        "%_/Dictionary :\n%_100 500 /RealPointRelToROrigin (PositionPoint1) ,\n"
                        "%_400 300 /RealPointRelToROrigin (PositionPoint2) ,\n%_(Cover) /UnicodeString (Name) ,\n%_; ,\n"
                        "%_/Dictionary :\n%_500 500 /RealPointRelToROrigin (PositionPoint1) ,\n"
                        "%_700 400 /RealPointRelToROrigin (PositionPoint2) ,\n%_; ,\n"
                        "%_; (ArtboardArray) ,\n%_; /Recorded ,\n%_;\n%AI9_EndDocumentData\n";
    auto doc = read_ok(records(layer("L", "0 g\n" + rect(100, 300, 110, 310) + "f\n"), setup));
    ASSERT_EQ(doc.artboards.size(), 2u);
    EXPECT_EQ(doc.artboards[0].name, "Cover");
    EXPECT_EQ(doc.artboards[1].name, "Artboard 2");
    EXPECT_EQ(doc.artboards[0].rect, Geom::Rect(Geom::Point(100, 300), Geom::Point(400, 500)));
    // The first artboard's top-left corner is the document's origin, y down.
    EXPECT_EQ(Geom::Point(100, 500) * doc.to_doc, Geom::Point(0, 0));
    EXPECT_EQ(Geom::Point(400, 300) * doc.to_doc, Geom::Point(300, 200));
}

TEST(AiNativeReader, CropmarksWithoutArtboards)
{
    auto doc = read_ok(records(layer("L", "0 g\n" + rect(0, 0, 10, 10) + "f\n")));
    ASSERT_EQ(doc.artboards.size(), 1u);
    EXPECT_EQ(Geom::Point(0, 100) * doc.to_doc, Geom::Point(0, 0));
}

// ---- the reader: colour ------------------------------------------------------------------

TEST(AiNativeReader, ProcessColours)
{
    std::string art = "0.25 g 0.1 0.2 0.3 0.4 K\n" + rect(0, 0, 1, 1) + "B\n";
    art += "0.1 0.2 0.3 0 1 0.5 0 Xa\n" + rect(0, 0, 1, 1) + "f\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &a = doc.layers[0].children[0];
    EXPECT_EQ(a.fill->color, Color::gray(0.25));
    EXPECT_EQ(a.stroke->color, Color::cmyk(0.1, 0.2, 0.3, 0.4));
    EXPECT_EQ(doc.layers[0].children[1].fill->color, Color::rgb(1, 0.5, 0));
}

TEST(AiNativeReader, SpotInksKeepLabAndTint)
{
    // Xx type 2: c m y k L a b (name) tint 2. A tint operand of 0.65 is a 35% tint.
    std::string art = "0.0182 0.0956 0.8644 0 89.0196 0 76 (PANTONE 114 C) 0.65 2 Xx\n" + rect(0, 0, 1, 1) + "f\n";
    art += "0 0.5 1 0 (MySpot) 0 x\n" + rect(0, 0, 1, 1) + "f\n";
    art += "0.2 0.3 0.4 0 0.8 0.6 0.4 (RgbSpot) 0 1 XX\n" + rect(0, 0, 1, 1) + "S\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    auto const &a = *kids[0].fill;
    ASSERT_GE(a.ink, 0);
    auto const &ink = doc.inks[a.ink];
    EXPECT_EQ(ink.name, "PANTONE 114 C");
    EXPECT_EQ(ink.kind, Ink::Kind::Spot);
    EXPECT_EQ(ink.color, Color::lab(89.0196, 0, 76));
    EXPECT_NEAR(ink.cmyk[2], 0.8644, 1e-9);
    EXPECT_NEAR(a.tint, 0.35, 1e-9);
    auto const &b = doc.inks[kids[1].fill->ink];
    EXPECT_EQ(b.name, "MySpot");
    EXPECT_EQ(b.color, Color::cmyk(0, 0.5, 1, 0));
    EXPECT_EQ(kids[1].fill->tint, 1.0);
    auto const &c = doc.inks[kids[2].stroke->ink];
    EXPECT_EQ(c.color, Color::rgb(0.8, 0.6, 0.4));
}

TEST(AiNativeReader, GlobalProcessColours)
{
    // Xk: the same layout as Xx; type 0 is CMYK (no extra values), type 1 RGB.
    std::string art = "0.7 0.6 0.1 0 0.38 0.38 0.63 (R=97 G=97 B=160) 0 1 Xk\n" + rect(0, 0, 1, 1) + "f\n";
    art += "0.1 0.9 0.8 0 (Brand Red) 0.5 0 XK\n" + rect(0, 0, 1, 1) + "S\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    auto const &a = doc.inks[kids[0].fill->ink];
    EXPECT_EQ(a.kind, Ink::Kind::Process);
    EXPECT_EQ(a.color, Color::rgb(0.38, 0.38, 0.63));
    auto const &b = doc.inks[kids[1].stroke->ink];
    EXPECT_EQ(b.kind, Ink::Kind::Process);
    EXPECT_EQ(b.color, Color::cmyk(0.1, 0.9, 0.8, 0));
    EXPECT_EQ(kids[1].stroke->tint, 0.5);
}

TEST(AiNativeReader, Registration)
{
    std::string art = "1 1 1 1 ([Registration]) 0 Xs\n" + rect(0, 0, 1, 1) + "f\n";
    art += "0.75 0.68 0.67 0.9 0 0 0 ([Registration]) 0 1 XZ\n" + rect(0, 0, 1, 1) + "S\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    EXPECT_EQ(doc.inks[kids[0].fill->ink].kind, Ink::Kind::Registration);
    EXPECT_EQ(doc.inks[kids[1].stroke->ink].kind, Ink::Kind::Registration);
}

TEST(AiNativeReader, OverprintAndFillRule)
{
    auto doc = read_ok(records(layer("L", "1 O 1 R 1 XR 0 g 0 G\n" + rect(0, 0, 1, 1) + "B\n0 O 0 R\n" + rect(0, 0, 1, 1) + "B\n")));
    auto const &kids = doc.layers[0].children;
    EXPECT_TRUE(kids[0].overprint_fill);
    EXPECT_TRUE(kids[0].overprint_stroke);
    EXPECT_TRUE(kids[0].evenodd);
    EXPECT_FALSE(kids[1].overprint_fill);
}

TEST(AiNativeReader, PaletteSettlesInks)
{
    // A gradient names a global process colour in CMYK form only; the palette says what it is.
    std::string setup = "%AI5_BeginPalette\n0 0 Pb\n0.1 0.9 0.8 0 (Brand Red) 0 0 Xk\n(Brand Red) Pc\n"
                        "0.0182 0.0956 0.8644 0 89 0 76 (PANTONE 114 C) 0 2 Xx\n(PANTONE 114 C) Pc\nPB\n%AI5_EndPalette\n"
                        "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0.1 0.9 0.8 0 (Brand Red) 0 3 50 0 Bs\n"
                        "0.0182 0.0956 0.8644 0 (PANTONE 114 C) 0.5 3 50 100 Bs\nBD\n%AI5_EndGradient\n";
    // Real files: the path, Bb (no operands), Bg, then the paint operator, then BB.
    std::string art = "0 g\n" + rect(0, 0, 100, 10) + "Bb\n0 (G) 0 5 0 100 1 0 0 1 0 0 Bg\nf\n0 BB\n";
    auto doc = read_ok(records(layer("L", art), setup));
    ASSERT_EQ(doc.inks.size(), 2u);
    ASSERT_EQ(doc.gradients.size(), 1u);
    auto const &g = doc.gradients[0];
    ASSERT_EQ(g.stops.size(), 2u);
    EXPECT_EQ(doc.inks[g.stops[0].ink].kind, Ink::Kind::Process);
    EXPECT_EQ(doc.inks[g.stops[1].ink].color, Color::lab(89, 0, 76)); // the palette's Lab ink
    EXPECT_NEAR(g.stops[1].tint, 0.5, 1e-9);
    auto const &fill = *doc.layers[0].children.at(0).fill;
    EXPECT_EQ(fill.kind, Paint::Kind::Gradient);
}

TEST(AiNativeReader, StopsOfGlobalSwatchesAsIllustratorWritesThem)
{
    // Each stop twice: a process stand-in ending "%_BS", then the exact stop on a "%_" line.
    // Gradients come before the Swatches panel, so a stop meets a swatch's name first.
    std::string setup =
        "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n<\n000102\n>\n<\n030405\n>\n1 %_Br\n[\n"
        "0.25 1 1 0.45 1 1 6 50 75.4601 %_BS\n%_0.25 1 1 0.45 (Logo Red) 0 0 5 1 6 50 75.4601 Bs\n"
        "0 1 1 0 1 1 6 68 0 %_BS\n%_0 1 1 0 1 1 6 68 0 Bs\n"
        "0.02 0.1 0.86 0 1 1 6 50 100 %_BS\n%_0.02 0.1 0.86 0 89 0 76 (PANTONE 114 C) 0.5 2 5 1 6 50 100 Bs\n"
        "BD\n%AI5_EndGradient\n"
        "%AI5_BeginPalette\n0 0 Pb\n0.25 1 1 0.45 (Logo Red) 0 0 Xk\n(Logo Red) Pc\n"
        "0.02 0.1 0.86 0 89 0 76 (PANTONE 114 C) 0 2 Xx\n(PANTONE 114 C) Pc\nPB\n%AI5_EndPalette\n";
    std::string art = "0 g\n" + rect(0, 0, 100, 10) + "Bb\n0 (G) 0 5 0 100 1 0 0 1 0 0 Bg\nf\n0 BB\n";
    auto doc = read_ok(records(layer("L", art), setup));
    ASSERT_EQ(doc.gradients.size(), 1u);
    auto const &g = doc.gradients[0];
    ASSERT_EQ(g.stops.size(), 3u);
    // Ramp order: the plain red at 0, the swatch at 75.46, the spot at 100.
    EXPECT_EQ(g.stops[0].ink, -1);
    EXPECT_EQ(g.stops[0].color, Color::cmyk(0, 1, 1, 0));
    EXPECT_NEAR(g.stops[0].midpoint, 0.68, 1e-9);
    ASSERT_GE(g.stops[1].ink, 0);
    EXPECT_EQ(g.stops[1].color, Color::cmyk(0.25, 1, 1, 0.45));
    EXPECT_EQ(doc.inks[g.stops[1].ink].name, "Logo Red");
    EXPECT_EQ(doc.inks[g.stops[1].ink].kind, Ink::Kind::Process);
    EXPECT_EQ(doc.inks[g.stops[1].ink].color, Color::cmyk(0.25, 1, 1, 0.45));
    ASSERT_GE(g.stops[2].ink, 0);
    EXPECT_EQ(doc.inks[g.stops[2].ink].kind, Ink::Kind::Spot);
    EXPECT_EQ(doc.inks[g.stops[2].ink].color, Color::lab(89, 0, 76));
    EXPECT_NEAR(g.stops[2].tint, 0.5, 1e-9);
    // One ink a name: the stop's first sight of it became the swatch.
    EXPECT_EQ(doc.inks.size(), 2u);
}

TEST(AiNativeReader, StopStandInsCountOnlyWithoutTheExactStop)
{
    // A stand-in with no exact stop after it is the stop; the old form ends its line "%_Bs".
    std::string setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0 %_Br\n[\n"
                        "0 0 0 1 1 1 6 50 100 %_BS\n1 0 0 0 1 1 6 50 0 %_BS\nBD\n%AI5_EndGradient\n"
                        "%AI5_BeginGradient: (Old)\n(Old) 0 2 Bd\n[\n0 %_Br\n[\n"
                        "0 0 50 100 %_Bs\n0.3 0.02 0.07 0 1 50 0 %_Bs\nBD\n%AI5_EndGradient\n";
    std::string art = "0 g\n" + rect(0, 0, 100, 10) + "Bb\n0 (G) 0 5 0 100 1 0 0 1 0 0 Bg\nf\n0 BB\n";
    auto doc = read_ok(records(layer("L", art), setup));
    ASSERT_EQ(doc.gradients.size(), 2u);
    ASSERT_EQ(doc.gradients[0].stops.size(), 2u);
    EXPECT_EQ(doc.gradients[0].stops[0].color, Color::cmyk(1, 0, 0, 0));
    EXPECT_EQ(doc.gradients[0].stops[1].color, Color::cmyk(0, 0, 0, 1));
    ASSERT_EQ(doc.gradients[1].stops.size(), 2u);
    EXPECT_EQ(doc.gradients[1].stops[0].color, Color::cmyk(0.3, 0.02, 0.07, 0));
    EXPECT_EQ(doc.gradients[1].stops[1].color, Color::gray(0));
}

TEST(AiNativeReader, Gradients)
{
    std::string setup = "%AI5_BeginGradient: (G)\n(G) 1 2 Bd\n[\n0 0 0 0 1 0 0 2 0.5 6 30 100 Bs\n0.81 0 1 6 50 0 Bs\nBD\n%AI5_EndGradient\n";
    std::string art = "0 g\n" + rect(0, 0, 100, 100) + "Bb\n1 (G) 0 0 0 1 1 0 0 1 0 0 Bg\n50 0 0 -50 100 100 Bm\n2 3 0 0 Bh\nf\n0 BB\n";
    auto doc = read_ok(records(layer("L", art), setup));
    ASSERT_EQ(doc.gradients.size(), 1u);
    auto const &g = doc.gradients[0];
    EXPECT_TRUE(g.radial);
    ASSERT_EQ(g.stops.size(), 2u);
    EXPECT_EQ(g.stops[0].offset, 0.0);
    EXPECT_EQ(g.stops[0].color, Color::gray(0.81));
    EXPECT_EQ(g.stops[1].opacity, 0.5);
    EXPECT_EQ(g.stops[1].color, Color::rgb(1, 0, 0));
    EXPECT_EQ(g.stops[1].midpoint, 0.3);
    auto const &p = *doc.layers[0].children[0].fill;
    EXPECT_EQ(p.kind, Paint::Kind::Gradient);
    EXPECT_EQ(p.gradient.gradient, 0);
    EXPECT_EQ(p.gradient.bm, Geom::Affine(50, 0, 0, -50, 100, 100));
    EXPECT_EQ(p.gradient.hilight, Geom::Point(2, 3));
    EXPECT_TRUE(p.gradient.bm_given);
    EXPECT_FALSE(p.gradient.xm);
}

TEST(AiNativeReader, GradientMatricesOfSeveralRamps)
{
    // Xm for the whole ramp, then a Bm for each ramp between two stops, with Bc caps.
    std::string setup = "%AI5_BeginGradient: (G)\n(G) 0 3 Bd\n[\n0 0 50 100 Bs\n0.5 0 50 40 Bs\n1 0 50 0 Bs\nBD\n%AI5_EndGradient\n";
    std::string art = "0 g\n" + rect(0, 0, 100, 100) +
                      "Bb\n3 -4 -30 0.5 Bh\n1 (G) 0.1 0 -54 0.5 1 0 0 1 0 0 1 Bg\n100 0 0 -100 0 100 Xm\n"
                      "900 0 0 -100 -900 100 Bc\n40 0 0 -100 0 100 Bm\n60 0 0 -100 40 100 Bm\n900 0 0 -100 100 100 Bc\nf\n0 BB\n";
    auto doc = read_ok(records(layer("L", art), setup));
    auto const &p = doc.layers[0].children.at(0).fill->gradient;
    ASSERT_TRUE(p.xm);
    EXPECT_EQ(*p.xm, Geom::Affine(100, 0, 0, -100, 0, 100));
    EXPECT_TRUE(p.bm_given);
    EXPECT_EQ(p.bm, Geom::Affine(40, 0, 0, -100, 0, 100));
    EXPECT_EQ(p.hilight, Geom::Point(3, -4));
    EXPECT_EQ(p.hilight_angle, -30.0);
    EXPECT_EQ(p.hilight_length, 0.5);
}

// ---- the reader: images, drawn looks, text -----------------------------------------------

TEST(AiNativeReader, CmykImageKeepsItsSamples)
{
    std::string art = "%AI5_BeginRaster\n() 1 XG\n/DeviceCMYK XN\n[ 1 0 0 1 10 20 ] 0 0 2 1 2 1 8 4 0 0 1 0 0\n"
                      "%%BeginData: 12\nXI\n\x10\x20\x30\x40\x50\x60\x70\x80\n%%EndData\nXH\n%AI5_EndRaster\n";
    auto doc = read_ok(records(layer("L", art)));
    ASSERT_EQ(doc.images.size(), 1u);
    auto const &img = doc.images[0];
    EXPECT_EQ(img.channels, 4);
    EXPECT_EQ(img.width, 2);
    EXPECT_EQ(img.height, 1);
    EXPECT_EQ(img.samples, "\x10\x20\x30\x40\x50\x60\x70\x80"s);
    EXPECT_EQ(img.matrix, Geom::Affine(1, 0, 0, 1, 10, 20));
}

TEST(AiNativeReader, DrawnLookReplacesItsObject)
{
    // The drawn look (a group) comes first; the object itself follows on %_ lines.
    std::string art = "u\n0 g\n" + rect(0, 0, 10, 10) + "f\n0 G\n" + rect(0, 0, 10, 10) + "S\nU\n";
    art += "%_0 g\n%_" + rect(0, 0, 10, 10) + "%_f\n%_/ArtDictionary :\n%_(Badge) /UnicodeString (AIArtName) ,\n%_;\n";
    art += "1 (Style 1) XW\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_TRUE(kids[0].drawn_look);
    EXPECT_EQ(kids[0].name, "Badge");
    EXPECT_EQ(kids[0].children.size(), 2u);
    EXPECT_EQ(doc.drawn_looks, 1u);
    EXPECT_EQ(doc.commented_kept, 0u);
}

TEST(AiNativeReader, TextSlots)
{
    std::string art = "/AI11Text :\n0 /FreeUndo ,\n3 /StoryIndex ,\n;\n%_/AI11Text :\n%_3 /StoryIndex ,\n%_;\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 1u); // the commented copy of story 3 goes
    EXPECT_EQ(kids[0].kind, Node::Kind::Text);
    EXPECT_EQ(kids[0].story, 3u);
}

TEST(AiNativeReader, TextAppearanceUsesItsCanonicalStoryInItsOriginalLayer)
{
    // Illustrator's FreeUndo=1 appearance can refer to a different story. The
    // following commented FreeUndo=0 object is the editable source of truth.
    auto text = [](unsigned proxy, unsigned canonical) {
        return "u\n/AI11Text :\n1 /FreeUndo ,\n" + std::to_string(proxy) +
               " /StoryIndex ,\n;\nU\n6 () XW\n%_/AI11Text :\n%_0 /FreeUndo ,\n%_" +
               std::to_string(canonical) + " /StoryIndex ,\n%_;\n1 (Text appearance) XW\n";
    };
    auto doc = read_ok(records(layer("Labels", text(4, 56)) + layer("Trees", text(5, 0))));
    ASSERT_EQ(doc.layers.size(), 2u);
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    ASSERT_EQ(doc.layers[1].children.size(), 1u);
    EXPECT_EQ(doc.layers[0].children[0].kind, Node::Kind::Text);
    EXPECT_EQ(doc.layers[0].children[0].story, 56u);
    EXPECT_EQ(doc.layers[1].children[0].kind, Node::Kind::Text);
    EXPECT_EQ(doc.layers[1].children[0].story, 0u);
}

TEST(AiNativeReader, GroupNamesAfterAppearanceMarkers)
{
    auto art = "u\n0 g\n" + rect(0, 0, 10, 10) +
               "f\nU\n9 () XW\n%_/ArtDictionary :\n%_(Named group) /UnicodeString (AIArtName) ,\n%_;\n";
    auto doc = read_ok(records(layer("L", art)));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    EXPECT_EQ(doc.layers[0].children[0].name, "Named group");
    EXPECT_TRUE(doc.layers[0].children[0].children[0].name.empty());
}

TEST(AiNativeReader, TextAppearanceKeepsNamedGroupsAndAppliesOpacityOnce)
{
    auto const proxy = std::string("u\n/AI11Text :\n1 /FreeUndo ,\n4 /StoryIndex ,\n;\nU\n");
    auto const canonical = std::string("%_/AI11Text :\n%_0 /FreeUndo ,\n%_56 /StoryIndex ,\n%_;\n1 (Text appearance) XW\n");
    // A name after the marker is the wrapper's: it stays, around the canonical story.
    auto named = read_ok(records(layer("L", proxy + "6 () XW\n%_/ArtDictionary :\n%_(Named text group) /UnicodeString "
                                                    "(AIArtName) ,\n%_;\n" + canonical)));
    ASSERT_EQ(named.layers[0].children.size(), 1u);
    auto const &group = named.layers[0].children[0];
    EXPECT_EQ(group.kind, Node::Kind::Group);
    EXPECT_EQ(group.name, "Named text group");
    ASSERT_EQ(group.children.size(), 1u);
    EXPECT_EQ(group.children[0].story, 56u);
    EXPECT_EQ(group.children[0].transparency.opacity, 1.0);
    // Transparency straight after the wrapper closes is the wrapper's, once.
    auto faded = read_ok(records(layer("L", proxy + "0 0.5 0 0 0 Xy\n6 () XW\n" + canonical)));
    ASSERT_EQ(faded.layers[0].children.size(), 1u);
    EXPECT_EQ(faded.layers[0].children[0].kind, Node::Kind::Group);
    EXPECT_EQ(faded.layers[0].children[0].transparency.opacity, 0.5);
    ASSERT_EQ(faded.layers[0].children[0].children.size(), 1u);
    EXPECT_EQ(faded.layers[0].children[0].children[0].story, 56u);
    EXPECT_EQ(faded.layers[0].children[0].children[0].transparency.opacity, 1.0);
    // After the marker it is neither's: a state left for the paths that follow. (Real files
    // leave such states standing across text objects without resetting them: type doesn't
    // take its transparency from Xy.) The plain wrapper gives way to the canonical story.
    auto later = read_ok(records(layer("L", proxy + "6 () XW\n1 0.2 0 0 0 Xy\n" + canonical)));
    ASSERT_EQ(later.layers[0].children.size(), 1u);
    EXPECT_EQ(later.layers[0].children[0].kind, Node::Kind::Text);
    EXPECT_EQ(later.layers[0].children[0].story, 56u);
    EXPECT_EQ(later.layers[0].children[0].transparency.opacity, 1.0);
}

TEST(AiNativeReader, TransparencyAfterAStyleMarkerIsTheNextObjects)
{
    // As real files write it: a group closes, its style marker follows, and then the state
    // of the next object. That 15% multiply is the small path's, not the whole group's.
    std::string art = "0 g\nu\n" + rect(0, 0, 50, 50) + "f\nU\n0 0 Xd\n6 () XW\n1 0.15 0 0 0 Xy\n" + rect(0, 0, 5, 5) + "f\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(kids[0].kind, Node::Kind::Group);
    EXPECT_EQ(kids[0].transparency.opacity, 1.0);
    EXPECT_EQ(kids[0].transparency.blend, 0);
    EXPECT_EQ(kids[1].transparency.opacity, 0.15);
    EXPECT_EQ(kids[1].transparency.blend, 1);
    // Without a marker in between it is the group's, as before.
    art = "0 g\nu\n" + rect(0, 0, 50, 50) + "f\nU\n1 0.15 0 0 0 Xy\n";
    doc = read_ok(records(layer("L", art)));
    EXPECT_EQ(doc.layers[0].children.at(0).transparency.opacity, 0.15);
}

TEST(AiNativeReader, IgnoredCensusOperators)
{
    // Ar (output resolution), An (CS5), XD brush metadata.
    std::string art = "300 Ar\n0 g\n" + rect(0, 0, 1, 1) + "f\n1 An\nu\n" + rect(0, 0, 1, 1) + "N\n(007F647F) Xt 0 XD\nU\n";
    auto doc = read_ok(records(layer("L", art)));
    EXPECT_EQ(doc.layers[0].children.size(), 2u);
}

// ---- failing closed ----------------------------------------------------------------------

TEST(AiNativeReader, UnknownOperatorOnAShownLayer)
{
    auto const e = read_error(records(layer("L", "0 g\n" + rect(0, 0, 1, 1) + "f\n7 Qz\n")));
    EXPECT_NE(e.find("`Qz`"), std::string::npos) << e;
}

TEST(AiNativeReader, HiddenLayersLeaveOutWhatTheyCantRead)
{
    auto doc = read_ok(records(layer("Old", "0 g\n" + rect(0, 0, 1, 1) + "f\n7 Qz\n", "0 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb") +
                               layer("New", "0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    EXPECT_EQ(doc.layers.size(), 2u);
    ASSERT_EQ(doc.left_out.size(), 1u);
    EXPECT_EQ(doc.left_out[0], "`Qz`");
}

TEST(AiNativeReader, UnsupportedFeaturesFailClosed)
{
    EXPECT_NE(read_error(records(layer("L", "(P) 0 0 1 1 0 0 0 1 1 0 0 p\n" + rect(0, 0, 1, 1) + "f\n"))).find("pattern fills"), std::string::npos);
    EXPECT_NE(read_error(records(layer("L", "%AI5_BeginPlace\n%AI5_EndPlace\n"))).find("placed files"), std::string::npos);
    EXPECT_NE(read_error(records(layer("L", "/SymbolInstance :\n(S) /String (SymbolName) ,\n;\n"))).find("symbols"), std::string::npos);
    EXPECT_NE(read_error(records(layer("L", "0 g\n/Mesh X!\n%_2 /Version X#\n/End X!\n"))).find("`X!`"), std::string::npos);
    // An opacity mask: its art is in the masked object's dictionary.
    std::string mask = "u\n0 g\n" + rect(0, 0, 9, 9) + "f\nU\n%_/ArtDictionary :\n%_/Mask :\n%_1 /Bool (Clipping) ,\n"
                       "%_0 /Bool (Inverted) ,\n%_0 /Bool (Disabled) ,\n%_X=\n%_0 g\n%_" + rect(0, 0, 5, 5) + "%_f\n%_;\n%_;\n";
    EXPECT_NE(read_error(records(layer("L", mask))).find("opacity masks"), std::string::npos);
    // A disabled mask changes nothing.
    auto disabled = mask;
    disabled.replace(disabled.find("%_0 /Bool (Disabled)"), 20, "%_1 /Bool (Disabled)");
    read_ok(records(layer("L", disabled)));
}

TEST(AiNativeReader, BrokenStructure)
{
    EXPECT_FALSE(read_error(records(layer("L", "0 g\n" + rect(0, 0, 1, 1) + "f\nQ\n"))).empty());
    EXPECT_FALSE(read_error("%AI5_BeginLayer\n1 1 1 1 0 0 0 0 0 0 0 0 50 0 Lb\n(L) Ln\n0 g\n10 10 m 20 20 l f\n").empty());
    EXPECT_FALSE(read_error(records("")).empty()); // no layers
    EXPECT_FALSE(read_error("%AI5_BeginPattern\nnever ends").empty());
}

TEST(AiNativeReader, LimitsHold)
{
    Limits tight;
    tight.max_depth = 4;
    std::string deep;
    for (int i = 0; i < 10; ++i) deep += "u\n";
    deep += "0 g\n" + rect(0, 0, 1, 1) + "f\n";
    for (int i = 0; i < 10; ++i) deep += "U\n";
    std::string error;
    EXPECT_FALSE(read(records(layer("L", deep)), error, tight));
    EXPECT_NE(error.find("nested"), std::string::npos);

    Limits few;
    few.max_points = 3;
    EXPECT_FALSE(read(records(layer("L", "0 g\n" + rect(0, 0, 1, 1) + "f\n")), error, few));
}

TEST(AiNativeReader, XmlNames)
{
    EXPECT_EQ(xml_name("Layer_1"), "Layer 1");
    EXPECT_EQ(xml_name("My_Rect__x23_2"), "My Rect #2");
    EXPECT_EQ(xml_name("_x31_abc__x28_paren_x29_"), "1abc (paren)");
    EXPECT_EQ(xml_name("S_00000092429200007574234970000005974767392385616558_"), "S");
    EXPECT_EQ(xml_name("a_x_"), "a x ");
}

TEST(AiNativeReader, LatinText)
{
    EXPECT_EQ(text_of("caf\xe9"), "caf\xc3\xa9");
    EXPECT_EQ(text_of("\xfe\xff\x00\x41\x00\xe9"s), "A\xc3\xa9");
}

// ---- the corpus (opt-in) -----------------------------------------------------------------

#ifdef WITH_POPPLER
TEST(AiNativeReader, Corpus)
{
    auto const *roots = g_getenv("PROOF_AI_CORPUS");
    if (!roots) GTEST_SKIP() << "set PROOF_AI_CORPUS to folders of approved .ai files";
    std::map<std::string, int> outcome;
    std::size_t files = 0, readable = 0;
    gchar **parts = g_strsplit(roots, ";", -1);
    for (gchar **p = parts; *p; ++p) {
        if (!**p) continue;
        for (auto const &e : std::filesystem::recursive_directory_iterator(from_utf8(*p))) {
            auto const ext = utf8(e.path().extension());
            if (!e.is_regular_file() || g_ascii_strcasecmp(ext.c_str(), ".ai") != 0) continue;
            auto const name = utf8(e.path().filename());
            if (name.rfind("._", 0) == 0) continue;
            ++files;
            std::string error;
            auto recs = Inkscape::Extension::Internal::read_ai_native_records(utf8(e.path()), {}, 256u << 20, error);
            if (!recs) {
                ++outcome["no records: " + error.substr(0, 60)];
                continue;
            }
            auto doc = read(recs->text, error);
            if (doc) {
                ++readable;
                ++outcome["readable"];
            } else {
                ++outcome[error.substr(0, 90)];
            }
            std::printf("%s\t%s\t%zu\t%s\n", doc ? "READ" : "PAGE", recs->encoding.c_str(), recs->text.size(),
                        name.c_str());
        }
    }
    g_strfreev(parts);
    for (auto const &[what, n] : outcome) std::printf("%5d  %s\n", n, what.c_str());
    std::printf("%zu of %zu files read natively\n", readable, files);
    EXPECT_GT(files, 0u);
}
#endif
