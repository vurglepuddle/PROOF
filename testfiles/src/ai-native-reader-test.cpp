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

/// `art` as Illustrator's own copy of an object: every line of it on a %_ line.
std::string hidden(std::string const &art)
{
    std::string out;
    std::size_t at = 0;
    while (at < art.size()) {
        auto const end = art.find('\n', at);
        auto const next = end == std::string::npos ? art.size() : end + 1;
        out += "%_" + art.substr(at, next - at);
        at = next;
    }
    return out;
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

TEST(AiNativeLexer, MarkersAndImageDataOnHiddenLines)
{
    // An object written on %_ lines has its markers there, and its samples after a hidden
    // %%BeginData; the samples themselves are never prefixed.
    auto toks = all_tokens("%_%AI5_BeginRaster\n%_[1 0 0 1 0 0] 0 0 1 1\r\n%_%%BeginData: 7\r\nXI\n{(}A%%EndData\r\n"
                           "%_XH\r\n%_%AI5_EndRaster\n0 %_%AI5_NotAMarker\nXz\n");
    ASSERT_GE(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, Token::Kind::Comment);
    EXPECT_EQ(toks[0].view, "AI5_BeginRaster");
    EXPECT_TRUE(toks[0].hidden);
    auto it = std::find_if(toks.begin(), toks.end(), [](auto const &t) { return t.kind == Token::Kind::Data; });
    ASSERT_NE(it, toks.end());
    EXPECT_EQ(it->view, "{(}A");
    EXPECT_TRUE(it->hidden);
    ASSERT_NE(it + 2, toks.end());
    EXPECT_EQ((it + 1)->view, "XH");
    EXPECT_EQ((it + 2)->view, "AI5_EndRaster");
    EXPECT_TRUE((it + 2)->hidden);
    // Later in a line it is still a comment that says nothing.
    EXPECT_EQ(toks.back().view, "Xz");
    EXPECT_EQ((toks.end() - 2)->number, 0.0);
    // A comment of a plain line isn't hidden.
    EXPECT_FALSE(all_tokens("%AI5_BeginLayer\n")[0].hidden);
}

TEST(AiNativeLexer, PlacedPreviewsAreSkipped)
{
    auto toks = all_tokens("%AI26_BeginPlacedObjectPreview\r\n2 1 (\x01\xff\r\n%AI26_EndPlacedObjectPreview\r\nN\r\n"s);
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks.back().view, "N");
}

TEST(AiNativeLexer, APlacedPreviewIsPassedOverByItsCount)
{
    // Eight pixels in one row of 32 bytes, which may be any bytes: here the marker that ends
    // a preview. The section comes with its marker's comment.
    auto const rows = "%AI26_EndPlacedObjectPreview\r\nAB"s;
    ASSERT_EQ(rows.size(), 32u);
    auto const src = "%AI26_BeginPlacedObjectPreview\r\n8 1 32 32 17 " + rows + "\r\n%AI26_EndPlacedObjectPreview\r\nN\r\n";
    auto toks = all_tokens(src);
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, Token::Kind::Comment);
    EXPECT_EQ(toks[0].data, "8 1 32 32 17 " + rows + "\r\n");
    EXPECT_EQ(toks.back().view, "N");
    auto const head = preview_head(toks[0].data);
    ASSERT_TRUE(head);
    EXPECT_EQ(head->width, 8u);
    EXPECT_EQ(head->height, 1u);
    EXPECT_EQ(head->row_bytes, 32u);
    EXPECT_EQ(head->bits, 32u);
    EXPECT_EQ(head->kind, 17u);
    EXPECT_EQ(head->size, 13u);
    EXPECT_FALSE(preview_head("8 1 32 32"));
    EXPECT_FALSE(preview_head("8 1 x 32 17 "));
    EXPECT_FALSE(preview_head("8 1 12345678901 32 17 "));
    // A count that the end marker doesn't follow isn't gone by: what comes after is still read.
    toks = all_tokens("%AI26_BeginPlacedObjectPreview\n8 9 32 32 17 "s + rows.substr(30) + "\n%AI26_EndPlacedObjectPreview\nN\n" +
                      std::string(400, ' '));
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

TEST(AiNativeReader, LegacyLayerColoursUseTheTenOperandLayout)
{
    // Adobe's Illustrator 7 specification, section 9.2; no modern reserved fields.
    auto doc = read_ok(records(layer("Legacy green", "", "0 0 0 0 1 0 2 79 255 79 Lb") +
                               layer("Custom", "", "1 1 1 1 0 0 -1 12 34 56 Lb")));
    ASSERT_EQ(doc.layers.size(), 2u);
    auto const &old = doc.layers[0];
    EXPECT_FALSE(old.visible);
    EXPECT_TRUE(old.locked);
    EXPECT_FALSE(old.layer.preview);
    EXPECT_FALSE(old.layer.printable);
    EXPECT_EQ(old.layer.dim, 50);
    EXPECT_EQ(old.layer.color_index, 2);
    EXPECT_EQ(old.layer.color_rgb, (std::array<int, 3>{79, 255, 79}));
    EXPECT_EQ(doc.layers[1].layer.color_index, -1);
    EXPECT_EQ(doc.layers[1].layer.color_rgb, (std::array<int, 3>{12, 34, 56}));
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

TEST(AiNativeReader, AClippingPathInAGroupOfItsOwnClipsTheGroupItEnds)
{
    // What is clipped, then the clipping path, painted, in a group of its own: as Illustrator
    // writes a page-sized background that also clips the art over it.
    std::string const art = "q\nu\n0 1 0 0 k\n" + rect(-20, -20, 60, 60) + "f\nU\n0 0 Xd\n6 () XW\n"
                            "u\n1 0 0 0 k\n" + rect(0, 0, 40, 40) + "h\nW\nf\nU\n0 0 Xd\n6 () XW\nQ\n9 () XW\n";
    auto doc = read_ok(records(layer("L", art)));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    auto const &group = doc.layers[0].children[0];
    EXPECT_TRUE(group.clipped);
    ASSERT_EQ(group.children.size(), 2u);
    // The clipping path comes first, with its paint: it lies under what it clips.
    EXPECT_EQ(group.children[0].kind, Node::Kind::Path);
    EXPECT_TRUE(group.children[0].clipping);
    ASSERT_TRUE(group.children[0].fill);
    EXPECT_EQ(group.children[0].fill->color, Color::cmyk(1, 0, 0, 0));
    EXPECT_EQ(group.children[1].kind, Node::Kind::Group);
    EXPECT_FALSE(group.children[1].clipped);

    // A clipping path with a fill and a stroke is written three times in that group: to be
    // filled, to clip, to be stroked. It is one path, with both.
    std::string const thrice = "q\nu\n0 1 0 0 k\n" + rect(-20, -20, 60, 60) + "f\nU\n9 () XW\nu\n0 0 0 1 k\n1 XR\n" +
                               rect(0, 0, 40, 40) + "h\nW\nf\n" + rect(0, 0, 40, 40) + "h\nW\nn\n0 R\n0 0 0 1 K\n1.5 w\n" +
                               rect(0, 0, 40, 40) + "h\nW\ns\nU\n9 () XW\nQ\n9 () XW\n";
    doc = read_ok(records(layer("L", thrice)));
    auto const &framed = doc.layers[0].children[0];
    EXPECT_TRUE(framed.clipped);
    ASSERT_EQ(framed.children.size(), 2u);
    EXPECT_EQ(framed.children[0].kind, Node::Kind::Path);
    EXPECT_TRUE(framed.children[0].clipping);
    EXPECT_EQ(framed.children[0].path.size(), 1u);
    ASSERT_TRUE(framed.children[0].fill);
    EXPECT_EQ(framed.children[0].fill->color, Color::cmyk(0, 0, 0, 1));
    ASSERT_TRUE(framed.children[0].stroke);
    EXPECT_DOUBLE_EQ(framed.children[0].stroke_style.width, 1.5);

    // Two clipping paths that aren't the same path clip together: the second clips a group
    // of its own inside the first one's, and what shows is where both let it through.
    doc = read_ok(records(layer("L", "q\n0 g\n" + rect(0, 0, 100, 100) + "f\n" + rect(0, 0, 60, 60) + "h\nW\nn\n" +
                                         rect(40, 40, 100, 100) + "h\nW\nn\nQ\n9 () XW\n")));
    auto const &both = doc.layers[0].children[0];
    EXPECT_TRUE(both.clipped);
    ASSERT_EQ(both.children.size(), 2u);
    EXPECT_TRUE(both.children[0].clipping);
    EXPECT_EQ(both.children[0].path.boundsFast()->max(), Geom::Point(60, 60));
    auto const &second = both.children[1];
    EXPECT_EQ(second.kind, Node::Kind::Group);
    EXPECT_TRUE(second.clipped);
    EXPECT_TRUE(second.clip_chain);
    ASSERT_EQ(second.children.size(), 2u);
    EXPECT_TRUE(second.children[0].clipping);
    EXPECT_EQ(second.children[0].path.boundsFast()->min(), Geom::Point(40, 40));
    EXPECT_TRUE(second.children[1].fill);
    // The same two in a group of their own at the end of the clip group clip all of it.
    doc = read_ok(records(layer("L", "q\n0 g\n" + rect(0, 0, 100, 100) + "f\nu\n" + rect(0, 0, 60, 60) + "h\nW\nn\n" +
                                         rect(40, 40, 100, 100) + "h\nW\nn\nU\n9 () XW\nQ\n9 () XW\n")));
    auto const &wrapped = doc.layers[0].children[0];
    EXPECT_TRUE(wrapped.clipped);
    ASSERT_EQ(wrapped.children.size(), 2u);
    EXPECT_TRUE(wrapped.children[0].clipping);
    ASSERT_EQ(wrapped.children[1].children.size(), 2u);
    EXPECT_TRUE(wrapped.children[1].clip_chain);
    EXPECT_TRUE(wrapped.children[1].children[0].clipping);
    EXPECT_TRUE(wrapped.children[1].children[1].fill);

    // A clipping group with a clipping path of its own keeps such a group as it is.
    doc = read_ok(records(layer("L", "q\n" + rect(0, 0, 50, 50) + "h\nW\nn\nu\n1 0 0 0 k\n" + rect(0, 0, 40, 40) +
                                         "h\nW\nf\nU\n0 g\n" + rect(0, 0, 5, 5) + "f\nQ\n")));
    auto const &kept = doc.layers[0].children[0];
    EXPECT_TRUE(kept.clipped);
    ASSERT_EQ(kept.children.size(), 3u);
    EXPECT_FALSE(kept.children[0].fill);
    EXPECT_EQ(kept.children[1].kind, Node::Kind::Group);
    EXPECT_TRUE(kept.children[1].clipped);
}

TEST(AiNativeReader, ACompoundPathTakesItsPaintFromPathsGroupedInsideIt)
{
    // A compound path with a style of its own: its paths are in a group inside it, with the
    // style's marker after the group. The paint is the paths', not the group's.
    std::string art = "*u\nu\n1 D\n0.2 g\n1 0.4 0 0 0 Xy\n" + rect(0, 0, 100, 100) + "f\n0 D\n" + rect(10, 10, 90, 90) +
                      "f\nU\n9 () XW\n*U\n0 g\n0 1 0 0 0 Xy\n" + rect(0, 0, 5, 5) + "f\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 2u);
    auto const &compound = kids[0];
    EXPECT_EQ(compound.kind, Node::Kind::Compound);
    ASSERT_TRUE(compound.fill);
    EXPECT_FALSE(compound.stroke);
    EXPECT_EQ(compound.transparency.blend, 1);
    EXPECT_EQ(compound.transparency.opacity, 0.4);
    // Both paths are still its outline.
    ASSERT_EQ(compound.children.size(), 1u);
    EXPECT_EQ(compound.children[0].kind, Node::Kind::Group);
    EXPECT_EQ(compound.children[0].children.size(), 2u);
    EXPECT_EQ(kids[1].kind, Node::Kind::Path);
    EXPECT_TRUE(kids[1].transparency.is_default());
}

TEST(AiNativeReader, ACompoundPathIsPaintedAsItsLastPath)
{
    // A gradient is written around the last path of a compound path alone. The paths
    // before it are painted black, for readers that know no gradients.
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0 0 0 0 1 0 0 2 1 6 50 100 Bs\n0.81 0 1 6 50 0 Bs\nBD\n%AI5_EndGradient\n";
    std::string const art = "*u\n1 D\n0 g\n" + rect(0, 0, 100, 100) + "f\n0 D\n0 g\n" + rect(10, 10, 90, 90) +
                            "Bb\n1 (G) 0 0 0 1 1 0 0 1 0 0 Bg\n100 0 0 100 0 0 Xm\nf\n0 BB\n*U\n";
    auto doc = read_ok(records(layer("L", art), setup));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    auto const &compound = doc.layers[0].children[0];
    EXPECT_EQ(compound.kind, Node::Kind::Compound);
    ASSERT_TRUE(compound.fill);
    EXPECT_EQ(compound.fill->kind, Paint::Kind::Gradient);
    EXPECT_EQ(compound.children.size(), 2u);
    // Paths that aren't painted at all don't take the paint away: the last painted one counts.
    doc = read_ok(records(layer("L", "*u\n0.5 g\n" + rect(0, 0, 100, 100) + "f\n" + rect(10, 10, 90, 90) + "n\n*U\n")));
    ASSERT_TRUE(doc.layers[0].children[0].fill);
    EXPECT_EQ(doc.layers[0].children[0].fill->color, Color::gray(0.5));
}

TEST(AiNativeReader, TransparencyAfterAContainerEnds)
{
    // A compound path has the transparency of its paths. What is set after it ends is the
    // next object's, and doesn't make the compound path opaque.
    std::string art = "*u\n0 g\n3 0.2 0 0 0 Xy\n" + rect(0, 0, 100, 100) + "f\n" + rect(10, 10, 90, 90) +
                      "f\n*U\n0 1 0 0 0 Xy\n" + rect(0, 0, 5, 5) + "f\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const *kids = &doc.layers[0].children;
    ASSERT_EQ(kids->size(), 2u);
    EXPECT_EQ((*kids)[0].transparency.blend, 3); // Overlay
    EXPECT_DOUBLE_EQ((*kids)[0].transparency.opacity, 0.2);
    EXPECT_TRUE((*kids)[1].transparency.is_default());

    // A group is written with its own state after it closes, ended by `Xd` and `6 () XW`.
    // Where that is the state of the last thing in it, no `Xy` says so again.
    art = "1 0.5 0 0 0 Xy\nu\n0 g\n" + rect(0, 0, 10, 10) + "f\nU\n1 w 4 M\n0 0 Xd\n6 () XW\n";
    // The group of a look has no state of its own (`9 () XW`), and one that sets its own has that.
    art += "u\n" + rect(0, 0, 10, 10) + "f\nU\n9 () XW\n";
    art += "u\n" + rect(0, 0, 10, 10) + "f\nU\n0 1 0 0 0 Xy\n0 0 Xd\n6 () XW\n";
    doc = read_ok(records(layer("L", art)));
    kids = &doc.layers[0].children;
    ASSERT_EQ(kids->size(), 3u);
    EXPECT_EQ((*kids)[0].transparency.blend, 1); // Multiply
    EXPECT_DOUBLE_EQ((*kids)[0].transparency.opacity, 0.5);
    EXPECT_TRUE((*kids)[1].transparency.is_default());
    EXPECT_TRUE((*kids)[2].transparency.is_default());
    // What is in the last group was written while the state was still the first group's.
    EXPECT_DOUBLE_EQ((*kids)[2].children[0].transparency.opacity, 0.5);
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

TEST(AiNativeReader, AnArtboardThatIsOnlyGuessedAtLiesWhereThePageIs)
{
    // No artboard list and no crop marks: a size and a centre. The centre is a fraction off
    // where the art was drawn for, as in real files.
    auto const written = [](std::string const &marks) {
        return "%!PS-Adobe-3.0\n%%Creator: PROOF tests\n%%BoundingBox: 91 175 898 517\n"
               "%%HiResBoundingBox: 91.1187 175.9609 897.0176 517\n%AI5_FileFormat 14.0\n%AI5_ArtSize: 920.9541 436.478\n"
               "%AI3_TemplateBox: 496.1143 341.9014 496.1143 341.9014\n" + marks + "%AI9_ColorModel: 2\n%%EndComments\n"
               "%%BeginProlog\n%%EndProlog\n%%BeginSetup\n%%EndSetup\n" +
               layer("L", "0 g\n" + rect(100, 200, 110, 210) + "f\n") + "%%PageTrailer\n%%Trailer\n%%EOF\n";
    };
    std::string error;
    auto doc = read(written({}), error);
    ASSERT_TRUE(doc) << error;
    EXPECT_TRUE(doc->board_guessed);
    ASSERT_EQ(doc->artboards.size(), 1u);
    auto const guessed = doc->artboards[0].rect;
    EXPECT_NEAR(guessed.width(), 920.9541, 1e-6);
    // The page is as large, and has the art from 55.504, 52.038 of its corner.
    Geom::Rect const page(Geom::Point(0, 0), Geom::Point(920.954, 436.478));
    Geom::Rect const art(Geom::Point(55.504, 52.038), Geom::Point(861.403, 393.077));
    auto moved = *doc;
    EXPECT_TRUE(place_board_by_page(moved, page, art));
    auto const placed = moved.artboards[0].rect;
    EXPECT_NEAR(placed.left(), 91.1187 - 55.504, 1e-6);
    EXPECT_NEAR(placed.top(), 175.9609 - 52.038, 1e-6);
    EXPECT_NEAR(placed.width(), guessed.width(), 1e-9);
    EXPECT_NEAR(placed.height(), guessed.height(), 1e-9);
    // Under a point from the guess, and the art follows: the page's top left corner is 0,0.
    EXPECT_LT(std::abs(placed.left() - guessed.left()), 1.0);
    EXPECT_TRUE(Geom::are_near(Geom::Point(placed.left(), placed.bottom()) * moved.to_doc, Geom::Point(0, 0), 1e-9));

    // Bounds of another height are other art, or cut by the page: that way stays as guessed.
    moved = *doc;
    Geom::Rect const cut(Geom::Point(55.504, 0), Geom::Point(861.403, 300));
    EXPECT_TRUE(place_board_by_page(moved, page, cut));
    EXPECT_NEAR(moved.artboards[0].rect.left(), 91.1187 - 55.504, 1e-6);
    EXPECT_NEAR(moved.artboards[0].rect.top(), guessed.top(), 1e-9);
    // A page of another size isn't this artboard's.
    moved = *doc;
    EXPECT_FALSE(place_board_by_page(moved, Geom::Rect(Geom::Point(0, 0), Geom::Point(612, 792)), art));
    EXPECT_EQ(moved.artboards[0].rect, guessed);

    // Crop marks say where the artboard is: nothing to correct.
    auto marked = read(written("%AI3_Cropmarks: 30 120 950.9541 556.478\n"), error);
    ASSERT_TRUE(marked) << error;
    EXPECT_FALSE(marked->board_guessed);
    auto const before = marked->artboards[0].rect;
    EXPECT_FALSE(place_board_by_page(*marked, Geom::Rect(Geom::Point(0, 0), Geom::Point(920.9541, 436.478)), art));
    EXPECT_EQ(marked->artboards[0].rect, before);
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

TEST(AiNativeReader, CoincidentStopsKeepTheLinearAndRadialPaintOrder)
{
    for (bool const radial : {false, true}) {
        auto const stops = radial
            ? "1 0 50 0 Bs\n0.25 0 50 50 Bs\n0.75 0 50 50 Bs\n0 0 50 100 Bs\n"
            : "0 0 50 100 Bs\n0.75 0 50 50 Bs\n0.25 0 50 50 Bs\n1 0 50 0 Bs\n";
        auto const setup = std::string("%AI5_BeginGradient: (G)\n(G) ") + (radial ? "1" : "0") +
                           " 4 Bd\n[\n" + stops + "BD\n%AI5_EndGradient\n";
        auto doc = read_ok(records(layer("L", "0 g\n" + rect(0, 0, 10, 10) + "f\n"), setup));
        ASSERT_EQ(doc.gradients.size(), 1u);
        auto const &ramp = doc.gradients[0].stops;
        ASSERT_EQ(ramp.size(), 4u);
        EXPECT_EQ(ramp[0].color, Color::gray(1));
        EXPECT_EQ(ramp[1].color, Color::gray(.25));
        EXPECT_EQ(ramp[2].color, Color::gray(.75));
        EXPECT_EQ(ramp[3].color, Color::gray(0));
        EXPECT_EQ(ramp[1].offset, .5);
        EXPECT_EQ(ramp[2].offset, .5);
    }
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

TEST(AiNativeReader, AGradientNotedWithAnObjectsStatePaintsNothing)
{
    // A group that keeps a note of a gradient: an instance with no path in it, between the
    // group's `Xd` and its marker. The path after the group is painted as it says itself.
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n0 0 0 0 1 0 0 2 1 6 50 100 Bs\n0.81 0 1 6 50 0 Bs\nBD\n%AI5_EndGradient\n";
    std::string const art = "u\n0 g\n" + rect(0, 0, 10, 10) + "f\nU\n0 1 Xd\nBb\n2 (G) -65 0 -75 108 1 0 0 1 0 0 1 Bg\n0 BB\n6 () XW\n" +
                            rect(20, 0, 30, 10) + "f\n0.5 g\n" + rect(40, 0, 50, 10) + "f\n";
    auto doc = read_ok(records(layer("L", art), setup));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 3u);
    ASSERT_TRUE(kids[1].fill);
    EXPECT_EQ(kids[1].fill->kind, Paint::Kind::Solid);
    EXPECT_EQ(kids[1].fill->color, Color::gray(0));
    EXPECT_EQ(kids[2].fill->color, Color::gray(0.5));
    EXPECT_TRUE(doc.warnings.empty());
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

TEST(AiNativeReader, UncountedRasterIsRefusedInsteadOfSilentlyDropped)
{
    std::string const header = "%AI5_BeginRaster\n[1 0 0 1 10 20] 0 0 1 1 1 1 8 3 0 0 0 0";
    auto const error = read_error(records(layer("L", header + " XI\n%FF0080\n%AI5_EndRaster\nN\n")));
    EXPECT_NE(error.find("raster data without byte-count brackets"), std::string::npos);
    auto const counted = header + "\n%%BeginData: 11 ASCII Bytes\nXI\n%FF0080\n%%EndData\n%AI5_EndRaster\nN\n";
    auto doc = read_ok(records(layer("L", counted)));
    ASSERT_EQ(doc.images.size(), 1u);
    EXPECT_EQ(doc.images[0].channels, 3);
    EXPECT_EQ(doc.images[0].samples, std::string("\xff\x00\x80", 3));
}

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

TEST(AiNativeReader, GreyPictureColouredBlackIsTheGreyPicture)
{
    // Illustrator 29's save of a grey picture from a version 7 file (2026-10-21): coloured with
    // black, `0 /DeviceGray XC`, its fields as Illustrator writes them.
    auto art = [](std::string const &colour, int type, int count, std::string const &data) {
        return "%AI5_BeginRaster\n() 1 XG\n" + colour + " XC\n[ 1.5 0 0 1.5 105 165 ] 20 30 2 Xh\n"
               "[ 1.5 0 0 1.5 105 165 ] 0 0 3 1 3 1 8 " + std::to_string(type) + " 0 0 1 2 4 4 0 0\n"
               "%%BeginData: " + std::to_string(count) + "\nXI\n" + data + "\n%%EndData\nXH\n%AI5_EndRaster\nF\n";
    };
    auto doc = read_ok(records(layer("L", art("0 /DeviceGray", 1, 7, "\x10\x80\xf0"))));
    ASSERT_EQ(doc.images.size(), 1u);
    EXPECT_EQ(doc.images[0].channels, 1);
    EXPECT_EQ(doc.images[0].samples, "\x10\x80\xf0"s);
    // Another paint changes what it prints: named, not drawn as grey.
    for (auto const &colour : {std::string("1 /DeviceGray"), std::string("0 0 0 1 /DeviceCMYK"),
                               std::string("1 [/Separation (PROOF Spot) /DeviceCMYK]")}) {
        SCOPED_TRACE(colour);
        EXPECT_NE(read_error(records(layer("L", art(colour, 1, 7, "\x10\x80\xf0")))).find("pictures coloured with a paint"),
                  std::string::npos);
    }
    // Black on a picture that isn't grey.
    EXPECT_NE(read_error(records(layer("L", art("0 /DeviceGray", 3, 13, "\x10\x80\xf0\x10\x80\xf0\x10\x80\xf0")))).find(
                  "pictures coloured with a paint"),
              std::string::npos);
}

TEST(AiNativeReader, ImageOnHiddenLinesIsTheObjectBehindItsLook)
{
    // A picture with an effect: what it draws, then the picture itself on %_ lines. Its
    // samples used to be read as operators.
    auto raster = [](std::string const &p) {
        return p + "%AI5_BeginRaster\n" + p + "() 1 XG\n" + p + "/DeviceRGB XN\n" + p + "[ 1 0 0 1 10 20 ] 0 0 1 1 1 1 8 3 0 0 1 0 0\n" +
               p + "%%BeginData: 7\nXI\n{(}\n%%EndData\n" + p + "XH\n" + p + "%AI5_EndRaster\n" + p + "N\n";
    };
    std::string art = "u\n" + raster("") + "U\n" + raster("%_") + "%_/ArtDictionary :\n%_(Photo) /UnicodeString (AIArtName) ,\n%_;\n"
                      "1 (Shadow) XW\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_TRUE(kids[0].drawn_look);
    EXPECT_EQ(kids[0].name, "Photo");
    ASSERT_EQ(kids[0].children.size(), 1u);
    EXPECT_EQ(kids[0].children[0].kind, Node::Kind::Image);
    ASSERT_EQ(doc.images.size(), 2u);
    EXPECT_EQ(doc.images[1].samples, "{(}"s);

    // Without a look that replaces it, the hidden picture is the object.
    auto alone = read_ok(records(layer("L", raster("%_") + "0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    ASSERT_EQ(alone.layers[0].children.size(), 2u);
    EXPECT_EQ(alone.layers[0].children[0].kind, Node::Kind::Image);
    EXPECT_TRUE(alone.layers[0].children[0].commented);
    EXPECT_EQ(alone.commented_kept, 1u);
}

// ---- the reader: placed files ------------------------------------------------------------

namespace {

/// A linked picture as Illustrator 2025 writes it; `p` prefixes every line.
std::string placed(std::string const &path, std::string const &folder = {}, std::string const &p = {})
{
    return p + "%AI5_BeginPlace\n" + p + "%%FileType: 1\n" + p + "[2 0 0 -2 -100 300] 50 60 90 80 4 4 0\n" + p + "(" + path + ")`\n" +
           p + "%%IncludeFile:" + path + "\n" + p + "%AI17_Begin_Content_if_version_gt:24 12\n\n" +
           p + "%AI28_BeginPlacedRelativePath\n" + p + "(" + folder + ")\n" + p + "%AI28_EndPlacedRelativePath\n" +
           p + "%AI17_Alternate_Content\n" + p + "%AI17_End_Versioned_Content\n\n" + p + "~\n" + p + "%AI5_EndPlace\n";
}

} // namespace

TEST(AiNativeReader, PlacedPictureKeepsItsLinkAndPlace)
{
    std::string art = "1 A\n0 0.5 0 0 0 Xy\n" + placed("C:\\\\Work\\\\Caf\\351 logo.png", "..\\\\Links\\\\") +
                      "%_/ArtDictionary :\n%_(Logo) /UnicodeString (AIArtName) ,\n%_;\n9 () XW\n0 A\n0 g\n" + rect(0, 0, 1, 1) + "f\n";
    auto doc = read_ok(records(layer("L", art)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 2u);
    auto const &n = kids[0];
    EXPECT_EQ(n.kind, Node::Kind::Placed);
    EXPECT_EQ(n.name, "Logo");
    EXPECT_TRUE(n.locked);
    EXPECT_EQ(n.transparency.opacity, 0.5);
    EXPECT_FALSE(n.commented);
    ASSERT_EQ(doc.placed.size(), 1u);
    ASSERT_EQ(n.placed, 0);
    auto const &p = doc.placed[0];
    EXPECT_EQ(p.matrix, Geom::Affine(2, 0, 0, -2, -100, 300));
    EXPECT_EQ(p.box, Geom::Rect(Geom::Point(50, 60), Geom::Point(90, 80)));
    // The name's bytes as the file has them: here one of Windows' own.
    EXPECT_EQ(p.path, "C:\\Work\\Caf\xe9 logo.png"s);
    EXPECT_EQ(p.folder, "..\\Links\\"s);
    EXPECT_EQ(p.file_type, 1);
    EXPECT_EQ(kids[1].kind, Node::Kind::Path);
    // The folder's string is not left for the path that follows.
    EXPECT_EQ(kids[1].path.size(), 1u);
}

namespace {

/// The picture a document keeps of a placed file, as Illustrator 2022 and later write it.
std::string preview(int w, int h, int row_bytes, std::string const &rows, int bits = 32, int kind = 17)
{
    return "%AI17_Begin_Content_if_version_gt:24 5\r\n\r\n%AI26_BeginPlacedObjectPreview\r\n" + std::to_string(w) + " " +
           std::to_string(h) + " " + std::to_string(row_bytes) + " " + std::to_string(bits) + " " + std::to_string(kind) +
           " " + rows + "\r\n%AI26_EndPlacedObjectPreview\r\n%AI17_End_Versioned_Content\r\n";
}

/// A placed file with what follows its placement.
std::string placed_with(std::string const &more)
{
    return "%AI5_BeginPlace\n%%FileType: 1\n[2 0 0 -2 -100 300] 50 60 90 80 4 4 0\n(a.png)`\n%%IncludeFile:a.png\n\n" + more +
           "~\n%AI5_EndPlace\n";
}

} // namespace

TEST(AiNativeReader, PlacedPictureKeepsThePictureTheDocumentHasOfIt)
{
    // Two pixels by two, in rows of twelve bytes: alpha, red, green, blue, and four bytes
    // that fill the row out.
    auto const rows = "\xff\x10\x20\x30\x80\x40\x50\x60____"s + "\x00\x00\x00\x00\xff\xff\xfe\xfd____"s;
    ASSERT_EQ(rows.size(), 24u);
    auto doc = read_ok(records(layer("L", placed_with(preview(2, 2, 12, rows)) + "0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    ASSERT_EQ(doc.placed.size(), 1u);
    auto const &kept = doc.placed[0].preview;
    EXPECT_EQ(kept.width, 2);
    EXPECT_EQ(kept.height, 2);
    // Red, green, blue, alpha.
    EXPECT_EQ(kept.pixels, "\x10\x20\x30\xff\x40\x50\x60\x80"s + "\x00\x00\x00\x00\xff\xfe\xfd\xff"s);
    // The path after it is read as before.
    ASSERT_EQ(doc.layers[0].children.size(), 2u);
    EXPECT_EQ(doc.layers[0].children[1].kind, Node::Kind::Path);

    // One written another way isn't guessed at, and one cut short isn't kept. The file is
    // placed all the same.
    for (auto const &other : {preview(2, 2, 12, rows, 24), preview(2, 2, 12, rows, 32, 3), preview(2, 3, 12, rows),
                              preview(0, 2, 12, rows), preview(2, 2, 7, rows), std::string()}) {
        auto plain = read_ok(records(layer("L", placed_with(other))));
        ASSERT_EQ(plain.placed.size(), 1u);
        EXPECT_EQ(plain.placed[0].preview.width, 0);
        EXPECT_EQ(plain.placed[0].preview.height, 0);
        EXPECT_TRUE(plain.placed[0].preview.pixels.empty());
    }
}

TEST(AiNativeReader, PlacedPictureBehindALookAndOnHiddenLayers)
{
    // What an effect draws, then the picture on %_ lines: the look replaces it.
    std::string art = "u\n0 g\n" + rect(0, 0, 9, 9) + "f\nU\n" + placed("a.png", {}, "%_") + "1 (Glow) XW\n";
    auto doc = read_ok(records(layer("L", art)));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    EXPECT_TRUE(doc.layers[0].children[0].drawn_look);
    EXPECT_EQ(doc.placed.size(), 1u);

    // A placement this doesn't know refuses the file where it shows, and only there.
    std::string old = "%AI5_BeginPlace\n(a.eps) 0 0 10 10 Unknown\n%AI5_EndPlace\n";
    EXPECT_NE(read_error(records(layer("L", old))).find("placed files"), std::string::npos);
    auto hidden = read_ok(records(layer("Old", old, "0 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb") +
                                  layer("New", "0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    EXPECT_TRUE(hidden.placed.empty());
    EXPECT_FALSE(hidden.left_out.empty());
    // The operators mean nothing outside a placed file.
    EXPECT_NE(read_error(records(layer("L", "0 g\n" + rect(0, 0, 1, 1) + "f\n~\n"))).find("`~`"), std::string::npos);
}

// ---- the reader: patterns ----------------------------------------------------------------

namespace {

/// A pattern as Illustrator CS and later write it: every line of the tile's art hidden.
std::string pattern(std::string const &name, std::string const &art, std::string const &box = "0 0 20 10")
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

TEST(AiNativeReader, PatternsAndWhatIsPaintedWithThem)
{
    std::string tile = "0 A\nu\n" + rect(0, 0, 20, 10) + "n\n/ArtDictionary :\n1 /Bool (AIPattern_Editor_Backing_Tile_Rect) ,\n;\n"
                       "0 1 1 0 k\n" + rect(0, 0, 10, 5) + "f\nU\n9 () XW\n";
    std::string art = "(Dots) 0 0 1 1 0 0 0 0 0 [2 0 0 2 30 40] p\n" + rect(0, 0, 100, 50) + "f\n"
                      "(Dots) 0 0 1 1 0 0 0 0 0 [1 0 0 1 0 0] P\n" + rect(0, 0, 100, 50) + "B\n";
    auto doc = read_ok(records(layer("L", art), pattern("Dots", tile) + pattern("Unused", "0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    ASSERT_EQ(doc.patterns.size(), 2u);
    auto const &p = doc.patterns[0];
    EXPECT_EQ(p.name, "Dots");
    EXPECT_EQ(p.tile, Geom::Rect(Geom::Point(0, 0), Geom::Point(20, 10)));
    EXPECT_TRUE(p.unread.empty());
    ASSERT_EQ(p.art.size(), 1u);
    EXPECT_EQ(p.art[0].kind, Node::Kind::Group);
    // The tile's art isn't a second copy of anything: none of it counts as commented.
    EXPECT_FALSE(p.art[0].commented);
    ASSERT_EQ(p.art[0].children.size(), 2u);
    EXPECT_FALSE(p.art[0].children[0].fill);
    EXPECT_EQ(p.art[0].children[1].fill->color, Color::cmyk(0, 1, 1, 0));
    EXPECT_FALSE(p.art[0].children[1].commented);
    EXPECT_EQ(doc.commented_kept, 0u);

    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 2u);
    ASSERT_TRUE(kids[0].fill);
    EXPECT_EQ(kids[0].fill->kind, Paint::Kind::Pattern);
    EXPECT_EQ(kids[0].fill->pattern, 0);
    EXPECT_EQ(kids[0].fill->pattern_matrix, Geom::Affine(2, 0, 0, 2, 30, 40));
    // The pattern stays the fill until another colour is set; the stroke has its own.
    ASSERT_TRUE(kids[1].fill);
    EXPECT_EQ(kids[1].fill->kind, Paint::Kind::Pattern);
    ASSERT_TRUE(kids[1].stroke);
    EXPECT_EQ(kids[1].stroke->kind, Paint::Kind::Pattern);
    EXPECT_EQ(kids[1].stroke->pattern_matrix, Geom::identity());
    // The state the tile's art set (its red) isn't the document's.
    auto after = read_ok(records(layer("L", rect(0, 0, 1, 1) + "f\n"), pattern("Dots", tile)));
    EXPECT_EQ(after.layers[0].children[0].fill->color, Color::gray(0));
    EXPECT_FALSE(after.layers[0].children[0].locked);
}

TEST(AiNativeReader, PatternsThatArentReadOnlyCostWhatUsesThem)
{
    auto const use = [](std::string const &name, std::string const &place = "0 0 1 1 0 0 0 0 0 [1 0 0 1 0 0]") {
        return "(" + name + ") " + place + " p\n" + rect(0, 0, 10, 10) + "f\n";
    };
    std::string plain = "0 g\n" + rect(0, 0, 10, 10) + "f\n";
    // Illustrator 8 and older: the tile is procedures of the printing format.
    std::string old = "%AI3_BeginPattern: (Old)\n(Old) 0 0 10 10 [\n%AI3_Tile\n(0 O 0 R 0.5 g) @\n(\n%AI6_BeginPatternLayer\n"
                      "800 Ar\n0 J 0 j 1 w 4 M []0 d\n0 0 m 5 5 L f\n%AI6_EndPatternLayer\n) &\n] E\n%AI3_EndPattern\n";
    // A tile holding what isn't read, and one whose art doesn't close.
    std::string odd = pattern("Odd", "0 g\n" + rect(0, 0, 1, 1) + "f\n7 Qz\n");
    std::string open = pattern("Open", "u\n0 g\n" + rect(0, 0, 1, 1) + "f\n");
    std::string broken = pattern("Broken", "0 g\n" + rect(0, 0, 1, 1) + "f\nU\n");
    std::string type = pattern("Type", "/AI11Text :\n0 /StoryIndex ,\n;\n");
    auto const setup = old + odd + open + broken + type + pattern("Good", plain);

    // Nothing painted with them: the document reads, and its own state is untouched.
    auto doc = read_ok(records(layer("L", plain + use("Good")), setup));
    ASSERT_EQ(doc.patterns.size(), 6u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_FALSE(doc.patterns[i].unread.empty()) << doc.patterns[i].name;
        EXPECT_TRUE(doc.patterns[i].art.empty());
    }
    EXPECT_NE(doc.patterns[1].unread.find("`Qz`"), std::string::npos) << doc.patterns[1].unread;
    EXPECT_TRUE(doc.patterns[5].unread.empty());
    ASSERT_EQ(doc.layers[0].children.size(), 2u);
    EXPECT_EQ(doc.layers[0].children[1].fill->pattern, 5);

    for (auto const *name : {"Old", "Odd", "Open", "Broken", "Type", "Missing"}) {
        auto const e = read_error(records(layer("L", use(name)), setup));
        EXPECT_NE(e.find("pattern fills"), std::string::npos) << name << ": " << e;
    }
    // Moved, scaled or turned the way Illustrator 8 and older wrote it; a matrix that flattens.
    EXPECT_NE(read_error(records(layer("L", use("Good", "5 0 1 1 0 0 0 0 0 [1 0 0 1 0 0]")), setup)).find("pattern fills"), std::string::npos);
    EXPECT_NE(read_error(records(layer("L", use("Good", "0 0 1 1 30 0 0 0 0 [1 0 0 1 0 0]")), setup)).find("pattern fills"), std::string::npos);
    EXPECT_NE(read_error(records(layer("L", use("Good", "0 0 1 1 0 0 0 0 0 [1 0 2 0 0 0]")), setup)).find("pattern fills"), std::string::npos);
    // On a hidden layer it is left out: the object paints nothing.
    auto hidden = read_ok(records(layer("Old", use("Odd"), "0 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb") + layer("New", plain), setup));
    EXPECT_FALSE(hidden.left_out.empty());
    EXPECT_FALSE(hidden.layers[0].children[0].fill);
    // The Swatches panel lists patterns with the same operator: nothing is painted there.
    read_ok(records(layer("L", plain), setup + "%AI5_BeginPalette\n0 0 Pb\n(Old) 0 0 1 1 0 0 0 0 0 [1 0 0 1 0 0] p\n(Old) Pc\nPB\n%AI5_EndPalette\n"));
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

TEST(AiNativeReader, TypeAnAppearanceDrewTakesItsObjectsStories)
{
    auto text = [](int story, bool drawn, std::string const &p = {}) {
        return p + "/AI11Text :\n" + p + (drawn ? "1" : "0") + " /FreeUndo ,\n" + p + std::to_string(story) + " /StoryIndex ,\n" + p + ";\n";
    };
    // A group with an appearance: what it draws is groups of drawn type, whose story
    // numbers count among the drawn stories, then the group itself on %_ lines. Nothing
    // but its type is drawn, so the group stands as it is. A marker without a style name
    // says the same.
    for (auto const *name : {"Anon", ""}) {
        auto same = read_ok(records(layer("L", "u\nu\n" + text(36, true) + "U\nU\n%_u\n" + text(83, false, "%_") + "%_U\n1 (" +
                                                   std::string(name) + ") XW\n")));
        ASSERT_EQ(same.layers[0].children.size(), 1u) << name;
        EXPECT_EQ(same.layers[0].children[0].children.at(0).story, 83u) << name;
    }
    auto doc = read_ok(records(layer("L", "u\nu\n" + text(36, true) + "U\nU\n%_u\n" + text(83, false, "%_") + "%_U\n1 (Anon) XW\n")));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    auto const *group = &doc.layers[0].children[0];
    EXPECT_EQ(group->kind, Node::Kind::Group);
    EXPECT_FALSE(group->drawn_look);
    EXPECT_FALSE(group->commented);
    ASSERT_EQ(group->children.size(), 1u);
    EXPECT_EQ(group->children[0].kind, Node::Kind::Text);
    EXPECT_EQ(group->children[0].story, 83u);
    EXPECT_FALSE(group->children[0].text_proxy);
    EXPECT_FALSE(group->children[0].commented);
    EXPECT_EQ(doc.drawn_looks, 0u);
    EXPECT_EQ(doc.commented_kept, 0u);

    // Several, in order.
    doc = read_ok(records(layer("L", "u\n" + text(5, true) + text(9, true) + "U\n%_u\n" + text(82, false, "%_") +
                                         text(83, false, "%_") + "%_U\n1 (Anon) XW\n")));
    group = &doc.layers[0].children.at(0);
    ASSERT_EQ(group->children.size(), 2u);
    EXPECT_EQ(group->children[0].story, 82u);
    EXPECT_EQ(group->children[1].story, 83u);

    // A look that adds something of its own (here half the opacity) is kept. These records
    // have no stories of drawn type, so its type takes the object's story: the groups
    // around it then draw nothing that type doesn't, and aren't a look.
    doc = read_ok(records(layer("L", "u\nu\n" + text(36, true) + "U\n0 0.5 0 0 0 Xy\nU\n%_u\n" + text(83, false, "%_") +
                                         "%_U\n1 (Anon) XW\n")));
    group = &doc.layers[0].children.at(0);
    EXPECT_FALSE(group->drawn_look);
    ASSERT_EQ(group->children.size(), 1u);
    EXPECT_EQ(group->children[0].transparency.opacity, 0.5);
    ASSERT_EQ(group->children[0].children.size(), 1u);
    EXPECT_EQ(group->children[0].children[0].story, 83u);
    EXPECT_FALSE(group->children[0].children[0].text_proxy);

    // Type with an appearance inside a group with one: the inner look, the type and the
    // marker between them are all on %_ lines, and are settled there first.
    auto inner = [&](int stand_in, int story) {
        return "%_u\n" + text(stand_in, true, "%_") + "%_U\n" + text(story, false, "%_") + "%_1 (Inner) XW\n";
    };
    doc = read_ok(records(layer("L", "u\nu\n" + text(1, true) + "U\nu\n" + text(2, true) + "U\nU\n%_u\n" + inner(9, 5) +
                                         inner(8, 6) + "%_U\n1 (Outer) XW\n")));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    group = &doc.layers[0].children[0];
    ASSERT_EQ(group->children.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(group->children[i].kind, Node::Kind::Text);
        EXPECT_EQ(group->children[i].story, 5u + i);
        EXPECT_FALSE(group->children[i].text_proxy);
        EXPECT_FALSE(group->children[i].commented);
    }
    EXPECT_EQ(doc.drawn_looks, 0u);
    EXPECT_EQ(doc.commented_kept, 0u);

    // Stand-ins that can't be told apart, or with no object behind them, refuse the file
    // where they show; where they don't, they are left without a story.
    auto const unmatched = "u\n" + text(5, true) + text(9, true) + "U\n%_u\n" + text(81, false, "%_") + text(82, false, "%_") +
                           text(83, false, "%_") + "%_U\n1 (Anon) XW\n";
    EXPECT_NE(read_error(records(layer("L", unmatched))).find("type drawn by an appearance"), std::string::npos);
    EXPECT_NE(read_error(records(layer("L", text(4, true)))).find("type drawn by an appearance"), std::string::npos);
    doc = read_ok(records(layer("Old", text(4, true), "0 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb") +
                          layer("New", "0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    EXPECT_FALSE(doc.layers[0].children[0].story);
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
    EXPECT_NE(read_error(records(layer("L", "0 g\n/Mesh X!\n%_2 /Version X#\n/End X!\n"))).find("gradient meshes"), std::string::npos);
    // An opacity mask: its art is in the masked object's dictionary.
    std::string mask = "u\n0 g\n" + rect(0, 0, 9, 9) + "f\nU\n%_/ArtDictionary :\n%_/Mask :\n%_1 /Bool (Clipping) ,\n"
                       "%_0 /Bool (Inverted) ,\n%_0 /Bool (Disabled) ,\n%_X=\n%_0 g\n%_" + rect(0, 0, 5, 5) + "%_f\n%_;\n%_;\n";
    EXPECT_NE(read_error(records(layer("L", mask))).find("opacity masks"), std::string::npos);
    // A disabled mask changes nothing.
    auto disabled = mask;
    disabled.replace(disabled.find("%_0 /Bool (Disabled)"), 20, "%_1 /Bool (Disabled)");
    read_ok(records(layer("L", disabled)));
}

TEST(AiNativeReader, ClippedVectorOpacityMasksKeepTheirArtworkAndState)
{
    auto mask = [](std::string const &art, int clipping = 1, int inverted = 0, int disabled = 0) {
        return "/ArtDictionary :\n/Mask :\n" + std::to_string(clipping) + " /Bool (Clipping) ,\n" +
            std::to_string(inverted) + " /Bool (Inverted) ,\n" + std::to_string(disabled) +
            " /Bool (Disabled) ,\nX=\n" + art + "X+\n(Art) ,\n; (AI9_Mask) ,\n;\n";
    };
    auto const shape = "0 g\n" + rect(0, 0, 40, 40) + "f\n";
    auto const coverage = "u\n0.5 g\n" + rect(0, 0, 20, 40) + "f\nU\n";
    auto doc = read_ok(records(layer("L", shape + mask(coverage) + shape)));
    auto const &kids = doc.layers[0].children;
    ASSERT_EQ(kids.size(), 2u);
    ASSERT_TRUE(kids[0].opacity_mask);
    ASSERT_EQ(kids[0].opacity_mask->size(), 1u);
    EXPECT_EQ(kids[0].opacity_mask->at(0).children[0].fill->color, Color::gray(0.5));
    EXPECT_EQ(kids[1].fill->color, Color::gray(0));
    EXPECT_FALSE(kids[1].opacity_mask);
    // Empty active masks hide everything; disabled masks change nothing.
    doc = read_ok(records(layer("L", shape + mask(""))));
    ASSERT_TRUE(doc.layers[0].children[0].opacity_mask);
    EXPECT_TRUE(doc.layers[0].children[0].opacity_mask->empty());
    doc = read_ok(records(layer("L", shape + mask(coverage, 1, 0, 1))));
    EXPECT_FALSE(doc.layers[0].children[0].opacity_mask);
    for (auto const &unsupported : {mask(coverage, 1, 1),
            mask("0 1 0 0 k\n" + rect(0, 0, 20, 40) + "f\n"),
            mask("0 1 0 0 K 2 w\n" + rect(0, 0, 20, 40) + "S\n"),
            mask("/AI11Text : 0 /StoryIndex , ;\n"),
            mask("/Mask :\n1 /Bool (Clipping) , 0 /Bool (Inverted) , 0 /Bool (Disabled) , ;\n")}) {
        SCOPED_TRACE(unsupported);
        EXPECT_NE(read_error(records(layer("L", shape + unsupported))).find("opacity masks"), std::string::npos);
    }
    // A mask's art is painted as the art before it where it sets nothing else: white
    // here, which lets the object show, where a state begun afresh would be black and hide it.
    auto white = read_ok(records(layer("L", "0 0 0 0 k\n" + rect(0, 0, 40, 40) + "f\n"
                                            + hidden("/ArtDictionary :\n/Mask :\n1 /Bool (Linked) ,\nX=\nu\n10 M\n" +
                                                     rect(0, 0, 10, 10) + "f\nU\n9 () XW\nX+\n (Art) ,\n"
                                                     "1 /Bool (Clipping) ,\n0 /Bool (Inverted) ,\n0 /Bool (Disabled) ,\n"
                                                     "; (AI9_Mask) ,\n;\n"))));
    ASSERT_EQ(white.layers[0].children.size(), 1u);
    ASSERT_TRUE(white.layers[0].children[0].opacity_mask);
    ASSERT_EQ(white.layers[0].children[0].opacity_mask->size(), 1u);
    auto const &covering = white.layers[0].children[0].opacity_mask->front();
    ASSERT_EQ(covering.children.size(), 1u);
    ASSERT_TRUE(covering.children[0].fill);
    EXPECT_EQ(covering.children[0].fill->color, Color::cmyk(0, 0, 0, 0));
    // Masks on component paths cannot be flattened into a compound path's geometry.
    EXPECT_NE(read_error(records(layer("L", "*u\n" + shape + mask(coverage) + "*U\n"))).find("opacity masks"), std::string::npos);
}

TEST(AiNativeReader, UnclippedGradientOpacityMasksKeepNestedArtwork)
{
    auto mask = [](std::string const &art, int clip) {
        return "/ArtDictionary :\n/Mask :\n" + std::to_string(clip) +
            " /Bool (Clipping) ,\n0 /Bool (Inverted) ,\n0 /Bool (Disabled) ,\n1 /Bool (Linked) ,\n"
            "X=\n" + art + "X+\n(Art) ,\n; (AI9_Mask) ,\n;\n";
    };
    std::string const setup = "%AI5_BeginGradient: (G)\n(G) 0 2 Bd\n[\n1 0 50 100 Bs\n0 0 50 0 Bs\nBD\n%AI5_EndGradient\n";
    auto const gradient = "0 g\n" + rect(0, 0, 20, 40) +
        "Bb\n0 (G) 0 0 0 20 1 0 0 1 0 0 Bg\nf\n0 BB\n";
    auto doc = read_ok(records(layer("L", "0 g\n" + rect(0, 0, 40, 40) + "f\n" +
                                          mask(gradient + mask("", 0), 0)), setup));
    auto const &n = doc.layers[0].children[0];
    ASSERT_TRUE(n.opacity_mask);
    EXPECT_FALSE(n.opacity_mask_clipped);
    ASSERT_EQ(n.opacity_mask->size(), 1u);
    auto const &g = n.opacity_mask->front();
    ASSERT_TRUE(g.fill);
    EXPECT_EQ(g.fill->kind, Paint::Kind::Gradient);
    ASSERT_TRUE(g.opacity_mask);
    EXPECT_TRUE(g.opacity_mask->empty());
    EXPECT_FALSE(g.opacity_mask_clipped);
    auto stroke = read_ok(records(layer("L", "0 g\n" + rect(0, 0, 40, 40) + "f\n" +
                                              mask("1 G 4 w\n" + rect(0, 0, 20, 40) + "S\n", 1))));
    ASSERT_TRUE(stroke.layers[0].children[0].opacity_mask->front().stroke);
    auto coloured = setup;
    coloured.replace(coloured.find("1 0 50 100"), 10, "1 0 0 0 1 50 100");
    EXPECT_NE(read_error(records(layer("L", "0 g\n" + rect(0, 0, 40, 40) + "f\n" +
                                          mask(gradient, 0)), coloured)).find("opacity masks"), std::string::npos);
}

TEST(AiNativeReader, OpacityMaskFlagsAndAttachmentsRemainBounded)
{
    auto const shape = "0 g\n" + rect(0, 0, 40, 40) + "f\n";
    for (auto const &flags : {"0.5 /Bool (Clipping) ,", "1 /Bool (Inverted) ,", "0 /Bool (Linked) ,"}) {
        std::string settings = "1 /Bool (Clipping) ,\n0 /Bool (Inverted) ,\n0 /Bool (Disabled) ,\n1 /Bool (Linked) ,\n";
        auto const key = std::string(flags).substr(std::string(flags).find(" /Bool"));
        auto const end = settings.find(key);
        ASSERT_NE(end, settings.npos);
        auto const start = settings.rfind('\n', end);
        settings.replace(start == settings.npos ? 0 : start + 1,
                         end + key.size() - (start == settings.npos ? 0 : start + 1), flags);
        EXPECT_NE(read_error(records(layer("L", shape + "/ArtDictionary :\n/Mask :\n" + settings +
            "X=\n1 g\n" + rect(0, 0, 20, 40) + "f\nX+\n(Art) ,\n; (AI9_Mask) ,\n;\n"))).find("opacity masks"), std::string::npos);
    }
}

TEST(AiNativeReader, AGroupsOwnStateAfterItsDictionaryIsStillTheGroups)
{
    auto const shape = "0 g\n" + rect(0, 0, 40, 40) + "f\n";
    // A mask's art ends with a style marker of its own (`9 () XW`), as Illustrator writes it: the
    // transparency after the dictionary is still the group's, not lost to that marker.
    auto const mask = "/ArtDictionary :\n/Mask :\n1 /Bool (Linked) ,\n0 /Bool (Inverted) ,\n1 /Bool (Clipping) ,\n"
                      "0 /Bool (Disabled) ,\nX=\nu\n1 g\n" + rect(0, 0, 20, 40) + "f\nU\n9 () XW\nX+\n (Art) ,\n"
                      "; (AI9_Mask) ,\n;\n";
    auto doc = read_ok(records(layer("L", "u\n" + shape + "U\n" + hidden(mask) + "0 0.6 0 0 0 Xy\n0 0 Xd\n6 () XW\n" +
                                          "0 1 0 0 0 Xy\n" + shape)));
    ASSERT_EQ(doc.layers[0].children.size(), 2u);
    auto const &group = doc.layers[0].children[0];
    EXPECT_EQ(group.kind, Node::Kind::Group);
    EXPECT_TRUE(group.opacity_mask);
    EXPECT_DOUBLE_EQ(group.transparency.opacity, 0.6);
    EXPECT_DOUBLE_EQ(doc.layers[0].children[1].transparency.opacity, 1.0);
    // A group as transparent as its last member sets no Xy of its own: `0 0 Xd` after its name
    // says it is the group's.
    auto named = read_ok(records(layer("L", "u\n0 0.6 0 0 0 Xy\n" + shape + "U\n" +
                                            hidden("/ArtDictionary :\n(G) /String (AIArtName) ,\n;\n") +
                                            "0 0 Xd\n6 () XW\n0 1 0 0 0 Xy\n" + shape)));
    ASSERT_EQ(named.layers[0].children.size(), 2u);
    EXPECT_EQ(named.layers[0].children[0].name, "G");
    EXPECT_DOUBLE_EQ(named.layers[0].children[0].transparency.opacity, 0.6);
}

namespace {
// A square with independent, exact cubic sides; the four corners have different greys.
std::string square_mesh(std::string const &first = "0.2", std::string const &space = {})
{
    return "/Mesh X!\n2 /Version X#\n/Cartesian /Type X#\n[1 1] /Size X#\n/Data X#\n[0 0] /P X#\n" + space +
           "[" + first + " 0.5 10 20 10 40 1 30 20 1] /N X#\n"
           "[0.4 0.5 10 80 30 80 1 10 60 1] /N X#\n"
           "[0.6 0.5 70 80 70 60 1 50 80 1] /N X#\n"
           "[0.8 0.5 70 20 50 20 1 70 40 1] /N X#\n/E X#\n/End X!\n";
}

std::string changed(std::string s, std::string const &from, std::string const &to)
{
    auto const at = s.find(from);
    EXPECT_NE(at, s.npos);
    if (at != s.npos) s.replace(at, from.size(), to);
    return s;
}
}

TEST(AiNativeReader, MeshCornersKeepGeometryColorAndOpacity)
{
    auto doc = read_ok(records(layer("L", square_mesh())));
    auto const &node = doc.layers[0].children[0];
    EXPECT_EQ(node.kind, Node::Kind::Mesh);
    ASSERT_EQ(node.mesh.patches.size(), 1u);
    auto const &p = node.mesh.patches[0];
    EXPECT_FALSE(p.outlined);
    EXPECT_EQ(p.corners[0].color, Color::gray(0.2));
    EXPECT_EQ(p.corners[0].opacity, 0.5);
    EXPECT_EQ(p.corners[0].next, Geom::Point(10, 40));
    EXPECT_EQ(p.corners[0].previous, Geom::Point(30, 20));
    ASSERT_EQ(p.boundary.size(), 1u);
    EXPECT_TRUE(p.boundary[0].closed());
    EXPECT_EQ(p.boundary[0].initialPoint(), Geom::Point(10, 20));
    auto cmyk = square_mesh("0.1 0.2 0.3 0.4", "/DeviceCMYK /CS X#\n");
    for (auto const &grey : {"0.4 0.5", "0.6 0.5", "0.8 0.5"})
        cmyk = changed(cmyk, "[" + std::string(grey), "[0.1 0.2 0.3 0.4 0.5");
    auto ink = read_ok(records(layer("L", cmyk)));
    EXPECT_EQ(ink.layers[0].children[0].mesh.patches[0].corners[0].color, Color::cmyk(0.1, 0.2, 0.3, 0.4));
}

TEST(AiNativeReader, MeshExtraBoundaryNodesOutlineAUniformPatchAndCutAChangingOne)
{
    auto mesh = square_mesh();
    auto const extra = "[4 50 4 40 4 60 1] /S X#\n[6 70 6 60 6 75 1] /A X#\n";
    mesh = changed(mesh, "[0.4", std::string(extra) + "[0.4");
    auto changing = read_ok(records(layer("L", mesh)));
    auto const &cut = changing.layers[0].children[0].mesh.patches[0];
    EXPECT_FALSE(cut.outlined);
    EXPECT_EQ(cut.edges[0].size(), 3u);
    EXPECT_EQ(cut.edges[1].size(), 1u);
    for (auto const &v : {"0.4", "0.6", "0.8"}) mesh = changed(mesh, "[" + std::string(v), "[0.2");
    auto doc = read_ok(records(layer("L", mesh)));
    auto const &p = doc.layers[0].children[0].mesh.patches[0];
    EXPECT_TRUE(p.outlined);
    ASSERT_EQ(p.boundary[0].size(), 6u);
    auto const &side = dynamic_cast<Geom::CubicBezier const &>(p.boundary[0][1]);
    EXPECT_EQ(side.controlPoint(0), Geom::Point(4, 50));
    EXPECT_EQ(side.controlPoint(1), Geom::Point(4, 60));
    EXPECT_EQ(side.controlPoint(2), Geom::Point(6, 60));
}

namespace {
/// A 1 x 1 mesh laid out as Illustrator 29.2.1 saves one with an anchor bent out of each edge
/// and a process ink on each corner (mesh-1x1-all, a control made for this, 2026-10-17).
std::string control_mesh()
{
    auto ink = [](char const *cmyk, char const *name) {
        return std::string("[ [") + cmyk + " /DeviceCMYK (" + name + ") /Separation]\n1 /NChannel] /CS X#\n";
    };
    return "/Mesh X!\n2 /Version X#\n/Cartesian /Type X#\n[1 1] /Size X#\n/Data X#\n[0 0] /P X#\n[0] /R X#\n"
           "[20 90 20 73.3333333333 20 106.6666666667 1] /A X#\n" +
           ink("0 0 0 1", "Black") + "[1 1 40 40 56.6666666667 40 1 40 56.6666666667 1] /N X#\n"
           "[90 19.875 73.3333333333 19.875 106.6666666667 19.875 1] /A X#\n" +
           ink("0 0 1 0", "Yellow") + "[1 1 140 40 140 56.6666666667 1 123.3333333333 40 1] /N X#\n"
           "[159.75 90 159.75 73.3333333333 159.75 106.6666666667 1] /A X#\n" +
           ink("0 1 0 0", "Magenta") + "[1 1 140 140 123.3333333333 140 1 140 123.3333333333 1] /N X#\n"
           "[90 159.875 73.3333333333 159.875 106.6666666667 159.875 1] /A X#\n" +
           ink("1 0 0 0", "Cyan") + "[1 1 40 140 40 123.3333333333 1 56.6666666667 140 1] /N X#\n/E X#\n/End X!\n";
}
}

TEST(AiNativeReader, MeshAnchorsAreListedAlongTheGrid)
{
    // An edge's anchors are listed along the mesh's grid, the first handle towards the one
    // before: with the patch's turn on its first two edges, against it on the other two.
    auto doc = read_ok(records(layer("L", control_mesh())));
    auto const &p = doc.layers[0].children[0].mesh.patches[0];
    EXPECT_FALSE(p.outlined);
    EXPECT_EQ(p.corners[0].color, Color::cmyk(0, 0, 0, 1));
    EXPECT_EQ(p.corners[2].color, Color::cmyk(0, 1, 0, 0));
    auto same = [](Geom::Point a, Geom::Point b) { return Geom::distance(a, b) < 1e-6; };
    for (auto const &edge : p.edges) ASSERT_EQ(edge.size(), 2u);
    // The bottom edge, along: from the first corner, the anchor's first handle comes first.
    EXPECT_TRUE(same(p.edges[0][0][2], {73.3333333333, 19.875}));
    // The top edge, against: from the third corner, the anchor's second handle comes first.
    EXPECT_TRUE(same(p.edges[2][0][0], {140, 140}));
    EXPECT_TRUE(same(p.edges[2][0][2], {106.6666666667, 159.875}));
    EXPECT_TRUE(same(p.edges[2][1][1], {73.3333333333, 159.875}));
    // The left edge, listed before the first corner, against: from the fourth corner down.
    EXPECT_TRUE(same(p.edges[3][0][0], {40, 140}));
    EXPECT_TRUE(same(p.edges[3][0][2], {20, 106.6666666667}));
    EXPECT_TRUE(same(p.edges[3][1][3], {40, 40}));
    ASSERT_EQ(p.boundary[0].size(), 8u);

    // An anchor turned into a corner ends in 0, not 1 (mesh-s-corner).
    auto corner = read_ok(records(layer("L", changed(control_mesh(), "20 106.6666666667 1]", "35 116.625 0]"))));
    EXPECT_TRUE(same(corner.layers[0].children[0].mesh.patches[0].edges[3][0][2], {35, 116.625}));

    // A named process colour gives each point a tint of it, blending towards paper in RGB.
    auto swatch = control_mesh();
    for (auto const *ink : {"0 0 0 1 /DeviceCMYK (Black)", "0 0 1 0 /DeviceCMYK (Yellow)", "0 1 0 0 /DeviceCMYK (Magenta)",
                            "1 0 0 0 /DeviceCMYK (Cyan)"}) {
        swatch = changed(swatch, std::string("[ [") + ink + " /Separation]\n1 /NChannel]",
                         "[0.2 0.4 0.6 /DeviceRGB (R=51 G=102 B=153) /Process]");
    }
    auto named = read_ok(records(layer("L", swatch)));
    auto const full = named.layers[0].children[0].mesh.patches[0].corners[0].color;
    EXPECT_EQ(full.model, Color::Model::Rgb);
    EXPECT_NEAR(full.v[2], 0.6, 1e-9);
    auto half = read_ok(records(layer("L", changed(swatch, "[1 1 40 40", "[0.5 1 40 40"))));
    EXPECT_NEAR(half.layers[0].children[0].mesh.patches[0].corners[0].color.v[0], 0.6, 1e-9);
    EXPECT_FALSE(half.layers[0].children[0].mesh.patches[0].outlined);

    // Spot inks in a mesh aren't read.
    EXPECT_NE(read_error(records(layer("L", changed(control_mesh(), "(Black)", "(PANTONE 300 C)")))).find("gradient meshes"),
              std::string::npos);
}

TEST(AiNativeReader, MeshMalformedAndUnsupportedRecordsFailClosed)
{
    auto mesh = square_mesh();
    EXPECT_NE(read_error(records(layer("L", "*u\n" + mesh + "*U\n"))).find("gradient meshes"), std::string::npos);
    for (auto const &bad : {
             changed(mesh, "2 /Version", "3 /Version"),
             changed(mesh, "/Cartesian", "/Polar"),
             changed(mesh, "[1 1] /Size", "[2 2] /Size"),
             changed(mesh, "[0 0] /P", "[1 0] /P"),
             changed(mesh, "[0.2", "[-0.2"),
             changed(mesh, "0.5 10 20", "1.5 10 20"),
             changed(mesh, "10 40 1", "10 40 0.5"),
             changed(mesh, "10 40 1", "1000001 40 1"),
             changed(mesh, "[0.2", "[(bad) 0.2"),
             changed(mesh, "/Data X#", "/Data X#\n/Unknown X#"),
             changed(mesh, "/E X#", "[0 0] /P X#\n/E X#"),
             changed(mesh, "/End X!", "")}) {
        EXPECT_NE(read_error(records(layer("L", bad))).find("gradient meshes"), std::string::npos);
    }
}

TEST(AiNativeReader, MeshLimitsAreCheckedBeforeAllocatingPatches)
{
    auto mesh = square_mesh();
    EXPECT_NE(read_error(records(layer("L", changed(mesh, "[1 1] /Size", "[1000000000 1000000000] /Size")))).find("gradient meshes"), std::string::npos);
    Limits limits;
    limits.max_mesh_patches = 0;
    std::string error;
    EXPECT_FALSE(read(records(layer("L", mesh)), error, limits));
    limits.max_mesh_patches = 1;
    limits.max_points = 3;
    EXPECT_FALSE(read(records(layer("L", mesh)), error, limits));
}

TEST(AiNativeReader, MeshGridMustBeCompleteAndHaveUniquePatchPositions)
{
    auto mesh = changed(square_mesh(), "[1 1] /Size", "[2 1] /Size");
    auto const begin = mesh.find("[0 0] /P");
    auto const end = mesh.find("/End X!");
    auto patch = changed(mesh.substr(begin, end - begin), "[0 0] /P", "[1 0] /P");
    mesh.insert(end, patch);
    auto doc = read_ok(records(layer("L", mesh)));
    ASSERT_EQ(doc.layers[0].children[0].mesh.patches.size(), 2u);
    auto duplicate = changed(mesh, "[1 0] /P", "[0 0] /P");
    EXPECT_NE(read_error(records(layer("L", duplicate))).find("gradient meshes"), std::string::npos);
    EXPECT_NE(read_error(records(layer("L", "") + square_mesh())).find("gradient meshes"), std::string::npos);
}

TEST(AiNativeReader, AnEnvelopeIsWhatItDrewAndAnUnreadMeshIsRefused)
{
    auto refused_for_mesh = [](std::string const &art) {
        auto const e = read_error(records(layer("L", art)));
        EXPECT_NE(e.find("gradient meshes"), std::string::npos) << e;
        // The operators of a mesh are known ones now.
        EXPECT_EQ(e.find("X#"), std::string::npos) << e;
    };
    // A gradient mesh is art that isn't read, wherever it is written, ended or not.
    std::string const mesh = "/Mesh X!\n2 /Version X#\n/Cartesian /Type X#\n[3 1] /Size X#\n/E X#\n/End X!\n";
    std::string hidden_mesh = "%_/Mesh X!\n%_2 /Version X#\n%_/Cartesian /Type X#\n%_[3 1] /Size X#\n%_/E X#\n%_/End X!\n";
    refused_for_mesh("0 g\n" + mesh);
    refused_for_mesh("%_0 g\n" + hidden_mesh);
    refused_for_mesh("u\n0 g\n" + mesh + "U\n");
    refused_for_mesh("0 g\n/Mesh X!\n2 /Version X#\n");
    // An `X!` that begins no mesh is an operator nobody knows.
    EXPECT_NE(read_error(records(layer("L", "0 g\n/Other X!\n"))).find("`X!`"), std::string::npos);
    // On a layer that doesn't show it is left out, and said to be.
    auto old = read_ok(records(layer("Old", "0 g\n" + mesh, "0 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb") +
                               layer("New", "0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    ASSERT_EQ(old.layers.size(), 2u);
    EXPECT_TRUE(old.layers[0].children.empty());
    ASSERT_EQ(old.left_out.size(), 1u);
    EXPECT_NE(old.left_out[0].find("gradient meshes"), std::string::npos);

    // An envelope as Illustrator 2025 writes one. On %_ lines: a group with the mesh it
    // distorts with, and in that group's dictionary the type it distorts. Then, plainly,
    // what it drew, and the plug-in's name.
    auto envelope = [&](std::string const &plugin) {
        return "0 0 Xd\n6 () XW\n%_0 Ae\n%_u\n%_0 O\n%_0 g\n" + hidden_mesh + "%_\n%_0 0 Xd\n%_6 () XW\n%_U\n"
               "%_/ArtDictionary :\n%_(Warp: Arc) /UnicodeString (DisplayString) ,\n%_X=\n%_0 Ae\n%_u\n"
               "%_/AI11Text :\n%_0 /FreeUndo ,\n%_0 /FrameIndex ,\n%_7 /StoryIndex ,\n%_;\n%_\n%_0 0 Xd\n%_6 () XW\n%_U\n"
               "%_9 () XW\n%_X+\n%_ (AIDeformArtKey) ,\n%_1 /Int (AIDeformStyle) ,\n%_;\n%_\n%_9 () XW\n"
               "0 Ap\n0 A\n0 Xw\n0 Ae\nu\n0 Ae\nu\n0 O\n0 g\n" + rect(10, 10, 30, 20) + "f\nU\n9 () XW\nU\n9 () XW\n0 Ae\n(" +
               plugin + ") 1 0 0 XP\n9 () XW\n";
    };
    auto doc = read_ok(records(layer("L", "0 g\n" + rect(0, 0, 5, 5) + "f\n" + envelope("Adobe Deform Plugin") +
                                          "0 g\n" + rect(50, 0, 55, 5) + "f\n")));
    ASSERT_EQ(doc.layers.size(), 1u);
    auto const &kids = doc.layers[0].children;
    // The path before it, what the envelope drew, the path after it: no mesh, and no type.
    ASSERT_EQ(kids.size(), 3u);
    EXPECT_EQ(kids[0].kind, Node::Kind::Path);
    EXPECT_EQ(kids[2].kind, Node::Kind::Path);
    EXPECT_EQ(kids[1].kind, Node::Kind::Group);
    EXPECT_TRUE(kids[1].envelope);
    EXPECT_TRUE(kids[1].drawn_look);
    EXPECT_FALSE(kids[1].commented);
    ASSERT_EQ(kids[1].children.size(), 1u);
    ASSERT_EQ(kids[1].children[0].children.size(), 1u);
    EXPECT_EQ(kids[1].children[0].children[0].kind, Node::Kind::Path);
    EXPECT_EQ(doc.envelopes, 1u);
    EXPECT_EQ(doc.drawn_looks, 0u);
    EXPECT_FALSE(doc.texts);
    // The same written by a plug-in this doesn't know: its mesh may be art.
    refused_for_mesh(envelope("Some Other Plugin"));
}

TEST(AiNativeReader, ObjectsAPlugInKeepsLiveAreWhatItDrew)
{
    // What an object was made from, on %_ lines, then what the plug-in drew of it, then the
    // plug-in's name: a brush stroke as Illustrator writes one, in the group that is the
    // brushed path.
    std::string const spine = hidden("0 Ae\nu\n0 R\n0 G\n1 w\n0 0 m\n40 0 L\nS\nU\n9 () XW\n");
    std::string const drawn = "0 Ap\n0 A\n0 Xw\n0 Ae\nu\n0 O\n0 g\n" + rect(0, -2, 40, 2) + "f\nU\n9 () XW\n0 Ae\n";
    auto stroke = [&](std::string const &plugin) {
        return "0 Ae\nu\n" + spine + drawn + "(" + plugin + ") 1 0 34 XP\n%040302010000803F\n"
               "%_/ArtDictionary :\n%_ /String (Adobe_Brushed_Path) ,\n%_;\n%_\n9 () XW\nU\n0 0 Xd\n6 () XW\n";
    };
    auto doc = read_ok(records(layer("L", stroke("Adobe ArtOnPath Brush Tool") + "0 g\n" + rect(50, 0, 55, 5) + "f\n")));
    ASSERT_EQ(doc.layers.size(), 1u);
    auto const *kids = &doc.layers[0].children;
    ASSERT_EQ(kids->size(), 2u);
    // The brushed path holds what the brush drew and nothing else: no path under its stroke.
    ASSERT_EQ((*kids)[0].children.size(), 1u);
    auto const &made = (*kids)[0].children[0];
    EXPECT_EQ(made.plugin, "Adobe ArtOnPath Brush Tool");
    EXPECT_FALSE(made.commented);
    EXPECT_FALSE(made.drawn_look);
    ASSERT_EQ(made.children.size(), 1u);
    EXPECT_TRUE(made.children[0].fill);
    EXPECT_FALSE(made.children[0].stroke);
    EXPECT_EQ(doc.commented_kept, 0u);
    ASSERT_EQ(doc.plugin_objects.size(), 1u);
    EXPECT_EQ(doc.plugin_objects.at("Adobe ArtOnPath Brush Tool"), 1u);

    // A plug-in that wasn't met in a file: left as it was read.
    doc = read_ok(records(layer("L", stroke("Some Other Plugin"))));
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    EXPECT_EQ(doc.layers[0].children[0].children.size(), 2u);
    EXPECT_EQ(doc.commented_kept, 2u);
    EXPECT_TRUE(doc.plugin_objects.empty());

    // A blend: the transparency after the marker is the blend's own.
    std::string const blend = hidden("u\n0 g\n" + rect(0, 0, 10, 10) + "f\n" + rect(30, 0, 40, 10) + "f\nU\n9 () XW\n") +
                              "u\n0 g\n" + rect(0, 0, 10, 10) + "f\n" + rect(15, 0, 25, 10) + "f\n" + rect(30, 0, 40, 10) +
                              "f\nU\n9 () XW\n(Adobe Path Blends) 1 0 92 XP\n%4443424101\n0 0.5 0 1 0 Xy\n10 M\n0 0 Xd\n6 () XW\n";
    doc = read_ok(records(layer("L", blend + "0 1 0 0 0 Xy\n0 g\n" + rect(50, 0, 55, 5) + "f\n")));
    kids = &doc.layers[0].children;
    ASSERT_EQ(kids->size(), 2u);
    EXPECT_EQ((*kids)[0].plugin, "Adobe Path Blends");
    EXPECT_EQ((*kids)[0].children.size(), 3u);
    EXPECT_DOUBLE_EQ((*kids)[0].transparency.opacity, 0.5);
    EXPECT_EQ((*kids)[0].transparency.knockout, 1);
    EXPECT_DOUBLE_EQ((*kids)[1].transparency.opacity, 1.0);
    EXPECT_EQ(doc.plugin_objects.at("Adobe Path Blends"), 1u);

    // A compound shape with an appearance: its drawn look, then all of it on %_ lines (its
    // paths in their own colours, the shape they add up to, the marker), then the paint its
    // style gives it and the style's marker. The look stands; neither group under it does.
    std::string const shape =
        "u\nu\n1 g\n" + rect(0, 0, 20, 10) + "f\nU\n9 () XW\nu\n0 G\n" + rect(0, 0, 20, 10) + "s\nU\n9 () XW\nU\n0 0 Xd\n6 () XW\n" +
        hidden("u\n0 O\n0.3 g\n0 R\n0 G\n" + rect(0, 0, 10, 10) + "b\n" + rect(10, 0, 20, 10) + "b\nU\n9 () XW\n"
               "0 Ap\n0 A\n0 Xw\nu\n0 1 0 0 0 Xy\n" + rect(0, 0, 20, 10) + "n\n"
               "/ArtDictionary :\n0 /Int (AI10 compound shape mode) ,\n;\n\nU\n9 () XW\n"
               "(Pathfinder Suite) 1 0 0 XP\n") +
        "0 O\n1 g\n0 R\n0 G\n0 1 0 0 0 Xy\n0 J 0 j 2 w 4 M []0 d\n0 XR\n1 1 Xd\n1 () XW\n";
    doc = read_ok(records(layer("L", shape + "0 g\n" + rect(50, 0, 55, 5) + "f\n")));
    kids = &doc.layers[0].children;
    ASSERT_EQ(kids->size(), 2u);
    EXPECT_TRUE((*kids)[0].drawn_look);
    EXPECT_EQ((*kids)[0].plugin, "Pathfinder Suite");
    EXPECT_FALSE((*kids)[0].commented);
    ASSERT_EQ((*kids)[0].children.size(), 2u);
    EXPECT_EQ((*kids)[1].kind, Node::Kind::Path);
    EXPECT_EQ(doc.commented_kept, 0u);
    EXPECT_EQ(doc.drawn_looks, 1u);
    // It counts as the look it is.
    EXPECT_TRUE(doc.plugin_objects.empty());
}

TEST(AiNativeReader, ForeignPdfBytesAndFullPlacementAreRetained)
{
    // ASCII85 for %PDF-1.5\n; the parser validates the actual PDF later.
    std::string const foreign = "/ForeignObject :\n1 /Version ,\n0 2 -3 0 100 200 /RTransform ,\n"
        "20 30 /Origin ,\n5 7 15 27 /Bounds ,\n/Data ,\n%,u@!!/MSk8$3~>\n;\n";
    auto doc = read_ok(records(layer("L", foreign + "0 g\n" + rect(1, 2, 3, 4) + "f\n")));
    ASSERT_EQ(doc.foreign.size(), 1u);
    EXPECT_EQ(doc.foreign[0].pdf, "%PDF-1.5\n");
    ASSERT_EQ(doc.layers[0].children.size(), 2u);
    EXPECT_EQ(doc.layers[0].children[0].kind, Node::Kind::Foreign);
    EXPECT_EQ(doc.layers[0].children[1].kind, Node::Kind::Path);
    // PDF (0,0) maps to source (5,7), then Origin flips y, then RTransform.
    EXPECT_EQ(Geom::Point(0, 0) * doc.foreign[0].to_art, Geom::Point(31, 250));
    EXPECT_EQ(Geom::Point(10, 20) * doc.foreign[0].to_art, Geom::Point(91, 270));
    for (auto const &bad : {
        changed(foreign, "1 /Version", "2 /Version"),
        changed(foreign, "0 2 -3 0", "0 0 0 0"),
        changed(foreign, "5 7 15 27", "5 7 5 27"),
        changed(foreign, "20 30 /Origin", "20 (bad) 30 /Origin"),
        changed(foreign, "/Data ,", "/Extra ,\n/Data ,"),
        changed(foreign, "1 /Version ,", "1 /Version ,\n1 /Version ,"),
        "*u\n" + foreign + "*U\n"}) {
        EXPECT_NE(read_error(records(layer("L", bad))).find("foreign objects"), std::string::npos);
    }
    EXPECT_FALSE(read_error(records(layer("L", changed(foreign, "~>", "")))).empty());
    Limits limits;
    limits.max_foreign_bytes = 8;
    std::string error;
    EXPECT_FALSE(read(records(layer("L", foreign)), error, limits));
    limits.max_foreign_bytes = 17;
    EXPECT_FALSE(read(records(layer("L", foreign + foreign)), error, limits));
}

TEST(AiNativeReader, ForeignAscii85ChecksTerminatorsPaddingAndBounds)
{
    EXPECT_EQ(ascii85_decode("%9jqo^BlbD-BleB1DJ+*+F(f,q~>", 20), "Man is distinguished");
    EXPECT_EQ(ascii85_decode("%z~>", 4), std::string(4, '\0'));
    EXPECT_EQ(ascii85_decode("%!!~>", 1), std::string(1, '\0'));
    // Only the first % is a comment marker; '_' and '%' at the start of data survive.
    EXPECT_TRUE(ascii85_decode("%_!!!!\n%%!!!!~>", 8));
    for (auto s : {"!~>", "uuuuu~>", "!z~>", "!!!!!", "!!!!!{~>"}) EXPECT_FALSE(ascii85_decode(s, 100));
    EXPECT_FALSE(ascii85_decode("z~>", 3));
    EXPECT_FALSE(ascii85_decode("!!!!~>", 2));
}

TEST(AiNativeReader, UnsupportedForeignObjectsAreRefusedNotDropped)
{
    // Art Illustrator keeps in the format it came in: a blob after /Data, in lines that start
    // with a percent sign. One of them starts "%_(" by chance, which read as a hidden line
    // opened a string that swallowed the rest of the layer.
    std::string const foreign = "/ForeignObject :\n2 /Version ,\n1 0 0 -1 0 0  /RTransform ,\n0 0  /Origin ,\n0 0 10 10  /Bounds ,\n"
                                "/Data ,\n%,u@!!/MSk8%41#ocdN=10d\n%_(29#!0n[=#e7]Vg0n\n%68^JT77&[t2Ddlh~>\n;\n";
    auto const after = "0 g\n" + rect(0, 0, 1, 1) + "f\n";
    auto const e = read_error(records(layer("L", foreign + after)));
    EXPECT_NE(e.find("foreign objects"), std::string::npos) << e;
    // Where it doesn't show it is left out, and what follows it is read.
    auto doc = read_ok(records(layer("Old", foreign + after, "0 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb") + layer("New", after)));
    ASSERT_EQ(doc.layers.size(), 2u);
    EXPECT_EQ(doc.layers[0].children.size(), 1u);
    ASSERT_EQ(doc.left_out.size(), 1u);
    EXPECT_NE(doc.left_out[0].find("foreign objects"), std::string::npos);
}

TEST(AiNativeReader, BrokenStructure)
{
    EXPECT_FALSE(read_error(records(layer("L", "0 g\n" + rect(0, 0, 1, 1) + "f\nQ\n"))).empty());
    EXPECT_FALSE(read_error("%AI5_BeginLayer\n1 1 1 1 0 0 0 0 0 0 0 0 50 0 Lb\n(L) Ln\n0 g\n10 10 m 20 20 l f\n").empty());
    EXPECT_FALSE(read_error("%AI5_BeginPattern\nnever ends").empty());
}

TEST(AiNativeReader, RecordsWithoutLayersAreOneLayer)
{
    // Illustrator 3, and Photoshop's paths exported to Illustrator: no layers; each path a
    // compound path of its own, closed and unpainted, its lines ending in a bare CR.
    std::string const body = "%%Note:\r%Adobe_Photoshop_Path_Begin:<< /defaultFill false >>\r*u\r"
                             "%AI3_Note:<< /operation /xor /defaultFill false >>\r1 XR\r10 10 m\r"
                             "10 40 20 50 40 50 C\r60 50 60 10 10 10 C\rn\r*U\r%Adobe_Photoshop_Path_End\r";
    auto doc = read_ok(records("0 0 0 1 k\r0 i 0 J 0 j 1 w 4 M []0 d\r" + body));
    ASSERT_EQ(doc.layers.size(), 1u);
    EXPECT_EQ(doc.layers[0].name, "Layer 1");
    EXPECT_TRUE(doc.layers[0].visible);
    ASSERT_EQ(doc.layers[0].children.size(), 1u);
    auto const *path = &doc.layers[0].children[0];
    if (path->kind == Node::Kind::Compound) path = &path->children.at(0);
    ASSERT_EQ(path->kind, Node::Kind::Path);
    ASSERT_EQ(path->path.size(), 1u);
    EXPECT_TRUE(path->path[0].closed());
    EXPECT_FALSE(doc.layers[0].children[0].fill || doc.layers[0].children[0].stroke);

    // A file of nothing is one empty layer, as Illustrator opens it.
    auto empty = read_ok(records(""));
    ASSERT_EQ(empty.layers.size(), 1u);
    EXPECT_TRUE(empty.layers[0].children.empty());

    // Records with layers still keep art outside them out.
    EXPECT_FALSE(read_error(records(layer("L", "") + "0 g\n" + rect(0, 0, 1, 1) + "f\n")).empty());
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

TEST(AiNativeReader, ArraysNestedTooDeeplyAreRefused)
{
    // Arrays are built without recursion, but a chain of them is freed by recursion: half a
    // million brackets, well within the values allowed on the stack, would overflow it.
    std::size_t const n = 500'000;
    std::string const deep = std::string(n, '[') + std::string(n, ']') + "\n0 g\n";
    std::string error;
    EXPECT_FALSE(read(records(layer("L", deep)), error));
    EXPECT_NE(error.find("arrays are nested too deeply"), std::string::npos) << error;

    // As deep as the records nest (a mesh entry, a dash in a dictionary) reads.
    auto doc = read_ok(records(layer("L", "[[[[1 2] 3]]]\n0 g\n" + rect(0, 0, 1, 1) + "f\n")));
    ASSERT_EQ(doc.layers.size(), 1u);
    EXPECT_EQ(doc.layers[0].children.size(), 1u);
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

TEST(AiNativeReader, OldFileFormatNamesUseWindows1252WithoutGuessingUtf8)
{
    auto const art = layer("\\303\\251", "0 g\n" + rect(0, 0, 1, 1) + "f\n");
    for (auto const header : {"%AI5_FileFormat 3\n", "%AI5_FileFormat: 3\n"}) {
        auto input = records(art);
        auto const start = input.find("%AI5_FileFormat 14.0\n");
        input.replace(start, std::string("%AI5_FileFormat 14.0\n").size(), header);
        auto const doc = read_ok(input);
        ASSERT_EQ(doc.layers.size(), 1u);
        EXPECT_EQ(doc.layers[0].name, "\xc3\x83\xc2\xa9");
    }
    auto const modern = read_ok("%AI5_FileFormat 14.0\n" + records(art));
    ASSERT_EQ(modern.layers.size(), 1u);
    EXPECT_EQ(modern.layers[0].name, "\xc3\xa9");
    EXPECT_EQ(text_of("\xc3\xa9", true), "\xc3\x83\xc2\xa9");
    EXPECT_EQ(text_of("\xfe\xff", true), "\xc3\xbe\xc3\xbf");
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
