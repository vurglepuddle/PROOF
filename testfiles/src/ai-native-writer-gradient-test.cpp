// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic SVG/records only. Actual Illustrator and PDF qualification are separate.
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include "doc-per-case-test.h"
#include "document.h"
#include "extension/internal/ai/ai-native-reader.h"
#include "extension/internal/ai/ai-native-writer.h"
#include "extension/internal/ai/ai-native-writer-gradient.h"
#include "object/sp-shape.h"

namespace AN = Inkscape::Extension::Internal::AiNative;

namespace {

std::string const geometry = R"(gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="100" y2="0")";
std::string const rgb_stops = R"(<stop offset="0" stop-color="#ff0000"/>)"
                              R"(<stop offset="0.4" stop-color="#00ff00"/>)"
                              R"(<stop offset="1" stop-color="#0000ff"/>)";
std::string const cmyk_stops = R"x(<stop offset="0" style="stop-color:device-cmyk(0.1 0.2 0.3 0.4)"/>)x"
                               R"x(<stop offset="1" style="stop-color:device-cmyk(0.9 0.8 0.7 0.6)"/>)x";

std::unique_ptr<SPDocument> document(std::string const &attributes = geometry,
                                    std::string const &stops = rgb_stops,
                                    std::string const &extra_defs = {}, std::string const &shape_attributes = {},
                                    std::string const &tag = "linearGradient")
{
    auto const svg = std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )") +
                     R"(xmlns:proof="urn:x-proof:document:1" width="300" height="200" viewBox="0 0 300 200"><defs>)" +
                     extra_defs + "<" + tag + " id=\"g\" " + attributes + ">" + stops + "</" + tag + "></defs>" +
                     R"x(<g transform="translate(7 11) rotate(25)"><rect id="shape" x="20" y="30" width="160" height="80" )x" +
                     R"x(style="fill:url(#g)" )x" + shape_attributes + "/></g></svg>";
    auto out = SPDocument::createNewDocFromMem(svg);
    if (out) out->ensureUpToDate();
    return out;
}

SPShape const *shape(SPDocument *doc)
{
    return dynamic_cast<SPShape *>(doc->getObjectById("shape"));
}

std::optional<AN::Document> read(AN::WrittenGradient const &gradient, std::string &error)
{
    return AN::read("%!PS-Adobe-3.0\n%%BoundingBox: 0 0 300 200\n%AI3_Cropmarks: 0 0 300 200\n"
                    "%%EndComments\n%%BeginSetup\n1 Bn\n" + gradient.definition +
                    "%%EndSetup\n%AI5_BeginLayer\n1 1 1 1 0 0 0 79 128 255 Lb\n(Art) Ln\n"
                    "0 g\n20 30 m 180 30 L 180 110 L 20 110 L\n" + gradient.begin + "f\n" + gradient.end +
                    "LB\n%AI5_EndLayer--\n%%PageTrailer\n%%EOF\n", error);
}

double scalar(AN::GradientPlacement const &placement, Geom::Point const &point)
{
    double const angle = placement.angle * std::acos(-1.0) / 180;
    auto const delta = point - placement.origin;
    return (delta.x() * std::cos(angle) + delta.y() * std::sin(angle)) / placement.length;
}

void expect_refusal(AN::WrittenGradient const &result, std::string const &reason)
{
    EXPECT_TRUE(result.definition.empty());
    EXPECT_TRUE(result.begin.empty());
    EXPECT_TRUE(result.end.empty());
    ASSERT_FALSE(result.refused.empty());
    EXPECT_NE(std::find(result.refused.begin(), result.refused.end(), reason), result.refused.end());
}

std::size_t occurrences(std::string const &text, std::string const &needle)
{
    std::size_t count = 0, at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        ++count;
        at += needle.size();
    }
    return count;
}

void gradient_shapes(std::vector<AN::Node> const &nodes, std::vector<AN::Node const *> &out)
{
    for (auto const &node : nodes) {
        if (node.fill && node.fill->kind == AN::Paint::Kind::Gradient) out.push_back(&node);
        // A compound's child contours are one shape, with the compound's final paint.
        if (node.kind != AN::Node::Kind::Compound) gradient_shapes(node.children, out);
    }
}

} // namespace

class AiNativeWriterGradientTest : public DocPerCaseTest {};

TEST_F(AiNativeWriterGradientTest, ProcessStopsReadBackInBothRecordVersions)
{
    for (bool legacy : {false, true}) {
        for (bool cmyk : {false, true}) {
            SCOPED_TRACE(std::string(legacy ? "AI7 " : "modern ") + (cmyk ? "CMYK" : "RGB"));
            auto doc = document(geometry, cmyk ? cmyk_stops : rgb_stops);
            ASSERT_TRUE(doc);
            auto const result = AN::write_gradient(shape(doc.get()), Geom::identity(), "G(test)\\name", legacy);
            ASSERT_TRUE(result.refused.empty()) << result.refused.front();
            EXPECT_EQ(result.end, "0 BB\n");
            EXPECT_EQ(result.definition.find(" Bn"), std::string::npos); // Caller writes the total count once.
            // A linear's stops last first, as Illustrator writes and reads them.
            EXPECT_GT(result.definition.find(" 50 0 %_Bs\n"), result.definition.find(" 50 100 %_Bs\n"));
            std::string error;
            auto ai = read(result, error);
            ASSERT_TRUE(ai) << error;
            ASSERT_EQ(ai->gradients.size(), 1u);
            auto const &g = ai->gradients[0];
            EXPECT_EQ(g.name, "G(test)\\name");
            EXPECT_FALSE(g.radial);
            ASSERT_EQ(g.stops.size(), cmyk ? 2u : 3u);
            EXPECT_EQ(g.stops[0].offset, 0);
            EXPECT_EQ(g.stops.back().offset, 1);
            if (cmyk) {
                EXPECT_EQ(g.stops[0].color, AN::Color::cmyk(0.1, 0.2, 0.3, 0.4));
                EXPECT_EQ(g.stops[1].color, AN::Color::cmyk(0.9, 0.8, 0.7, 0.6));
            } else {
                EXPECT_EQ(g.stops[0].color, AN::Color::rgb(1, 0, 0));
                EXPECT_EQ(g.stops[1].color, AN::Color::rgb(0, 1, 0));
                EXPECT_EQ(g.stops[2].color, AN::Color::rgb(0, 0, 1));
                EXPECT_NEAR(g.stops[1].offset, 0.4, 1e-6);
            }
            for (auto const &stop : g.stops) {
                EXPECT_EQ(stop.midpoint, 0.5);
                EXPECT_EQ(stop.opacity, 1);
                EXPECT_EQ(stop.ink, -1);
            }
            ASSERT_EQ(ai->layers.size(), 1u);
            ASSERT_EQ(ai->layers[0].children.size(), 1u);
            ASSERT_TRUE(ai->layers[0].children[0].fill);
            EXPECT_EQ(ai->layers[0].children[0].fill->kind, AN::Paint::Kind::Gradient);
        }
    }
}

TEST_F(AiNativeWriterGradientTest, ShearPreservesTheScalarFieldInsteadOfTransformedEndpoints)
{
    auto doc = document();
    ASSERT_TRUE(doc);
    auto const result = AN::write_gradient(shape(doc.get()), Geom::Affine(1, 0, 1, 1, 0, 0), "Shear");
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    std::string error;
    auto ai = read(result, error);
    ASSERT_TRUE(ai) << error;
    auto const &p = ai->layers[0].children[0].fill->gradient;
    EXPECT_NEAR(p.angle, -45, 1e-4);
    EXPECT_NEAR(p.length, std::sqrt(5000.0), 1e-4);
    // q = (x+y,y), so t(q) = (q.x-q.y)/100. A transformed endpoint-only
    // vector would remain horizontal and give 0.75 for the first point.
    EXPECT_NEAR(scalar(p, {75, 50}), 0.25, 2e-6);
    EXPECT_NEAR(scalar(p, {10, -40}), 0.5, 2e-6);
    EXPECT_NEAR(scalar(p, {150, 50}), 1.0, 2e-6);
}

TEST_F(AiNativeWriterGradientTest, ObjectBoundsGradientTransformItemTransformAndArtOriginComposeInOrder)
{
    auto doc = document(R"x(x1="20%" y1="10%" x2="80%" y2="90%" gradientTransform="matrix(1 .25 .4 1 .1 -.2)")x");
    ASSERT_TRUE(doc);
    auto const *s = shape(doc.get());
    ASSERT_TRUE(s);
    auto const to_art = s->i2doc_affine() * Geom::Affine(0.75, 0, 0, -0.75, -8, 23);
    auto const result = AN::write_gradient(s, to_art, "BBox");
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    std::string error;
    auto ai = read(result, error);
    ASSERT_TRUE(ai) << error;
    auto const &p = ai->layers[0].children[0].fill->gradient;
    auto const expected_transform = Geom::Affine(1, .25, .4, 1, .1, -.2) *
                                    Geom::Affine(160, 0, 0, 80, 20, 30) * to_art;
    for (auto const point : std::array<Geom::Point, 4>{{{.2, .1}, {.8, .9}, {.3, .8}, {1.2, -.3}}}) {
        double const expected = ((point.x() - .2) * .6 + (point.y() - .1) * .8); // |P1-P0| = 1.
        EXPECT_NEAR(scalar(p, point * expected_transform), expected, 1e-5);
    }
    EXPECT_FALSE(p.xm);
    EXPECT_FALSE(p.bm_given);
    EXPECT_TRUE(p.bg.isIdentity());
}

TEST_F(AiNativeWriterGradientTest, StopOnlyHrefAndTerminalPaddingUseEffectiveStops)
{
    std::string const vector = R"(<linearGradient id="v" gradientUnits="userSpaceOnUse" spreadMethod="pad">)"
                               R"(<stop offset=".2" stop-color="#ff0000"/>)"
                               R"(<stop offset=".8" stop-color="#0000ff"/></linearGradient>)";
    auto doc = document(R"(xlink:href="#v" x1="10" y1="20" x2="110" y2="20")", "", vector);
    ASSERT_TRUE(doc);
    auto const result = AN::write_gradient(shape(doc.get()), Geom::identity(), "Padded");
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    std::string error;
    auto ai = read(result, error);
    ASSERT_TRUE(ai) << error;
    auto const &stops = ai->gradients[0].stops;
    ASSERT_EQ(stops.size(), 4u);
    EXPECT_EQ(stops[0].offset, 0);
    EXPECT_NEAR(stops[1].offset, .2, 1e-6);
    EXPECT_NEAR(stops[2].offset, .8, 1e-6);
    EXPECT_EQ(stops[3].offset, 1);
    EXPECT_EQ(stops[0].color, stops[1].color);
    EXPECT_EQ(stops[2].color, stops[3].color);
}

TEST_F(AiNativeWriterGradientTest, FullWriterPlacesSharedObjectBoundsCompoundHoleAndClippedGradient)
{
    auto doc = SPDocument::createNewDocFromMem(
        R"x(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="180" viewBox="0 0 320 180">)x"
        R"x(<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">)x" + rgb_stops +
        R"x(</linearGradient><clipPath id="c"><rect x="200" y="120" width="60" height="40"/></clipPath></defs>)x"
        R"x(<rect x="10" y="10" width="80" height="40" fill="url(#g)"/>)x"
        R"x(<rect x="120" y="20" width="160" height="80" fill="url(#g)"/>)x"
        R"x(<path d="M10 80H90V160H10Z M30 100V140H70V100Z" fill="url(#g)" fill-rule="evenodd"/>)x"
        R"x(<g clip-path="url(#c)"><rect x="190" y="110" width="100" height="60" fill="url(#g)"/></g>)x"
        R"x(<rect x="285" y="10" width="10" height="10" fill="#ff0000"/></svg>)x");
    ASSERT_TRUE(doc);
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        SCOPED_TRACE(version == AN::RecordsVersion::Modern ? "modern" : "AI7");
        auto const result = AN::write_records(doc.get(), version);
        ASSERT_TRUE(result.refused.empty()) << result.refused.front();
        std::string error;
        auto ai = AN::read(result.records, error);
        ASSERT_TRUE(ai) << error;
        EXPECT_EQ(occurrences(result.records, " Bn\n"), 1u);
        EXPECT_NE(result.records.find(std::to_string(ai->gradients.size()) + " Bn\n"), std::string::npos);
        EXPECT_LT(result.records.find("%AI5_BeginGradient:"), result.records.find("%%EndSetup"));
        EXPECT_EQ(occurrences(result.records, "\nBb\n"), 4u);
        EXPECT_EQ(occurrences(result.records, " Bg\n"), 4u);
        EXPECT_EQ(occurrences(result.records, "\n0 BB\n"), 4u);
        ASSERT_EQ(ai->layers.size(), 1u);
        std::vector<AN::Node const *> gradients;
        gradient_shapes(ai->layers[0].children, gradients);
        ASSERT_EQ(gradients.size(), 4u);
        auto const &a = gradients[0]->fill->gradient;
        auto const &b = gradients[1]->fill->gradient;
        double const art_y = version == AN::RecordsVersion::Illustrator7 ? 135 : 0;
        EXPECT_NEAR(a.origin.x(), 7.5, 1e-4);
        EXPECT_NEAR(a.origin.y(), art_y - 7.5, 1e-4);
        EXPECT_NEAR(b.origin.x(), 90, 1e-4);
        EXPECT_NEAR(b.origin.y(), art_y - 15, 1e-4);
        EXPECT_NEAR(b.length, 2 * a.length, 2e-4);
        EXPECT_NEAR(scalar(a, {37.5, art_y - 22.5}), .5, 2e-6);
        EXPECT_NEAR(scalar(b, {150, art_y - 45}), .5, 2e-6);
        EXPECT_EQ(gradients[2]->kind, AN::Node::Kind::Compound);
        // The reader retains the two source contours as children. The importer
        // combines their paths and applies the compound's final gradient paint.
        ASSERT_EQ(gradients[2]->children.size(), 2u);
        for (auto const &contour : gradients[2]->children) {
            EXPECT_EQ(contour.kind, AN::Node::Kind::Path);
            ASSERT_EQ(contour.path.size(), 1u);
            EXPECT_TRUE(contour.path[0].closed());
        }
        ASSERT_TRUE(gradients[2]->children[0].fill);
        EXPECT_EQ(gradients[2]->children[0].fill->kind, AN::Paint::Kind::Solid);
        ASSERT_TRUE(gradients[2]->children[1].fill);
        EXPECT_EQ(gradients[2]->children[1].fill->kind, AN::Paint::Kind::Gradient);
        EXPECT_TRUE(gradients[2]->evenodd);
        auto const &last = ai->layers[0].children.back();
        ASSERT_TRUE(last.fill);
        EXPECT_EQ(last.fill->kind, AN::Paint::Kind::Solid);
        EXPECT_EQ(last.fill->color, AN::Color::rgb(1, 0, 0));
        bool clipped = false;
        for (auto const &node : ai->layers[0].children) {
            if (node.kind != AN::Node::Kind::Group || !node.clipped) continue;
            std::vector<AN::Node const *> inside;
            gradient_shapes(node.children, inside);
            clipped = !inside.empty();
        }
        EXPECT_TRUE(clipped);
    }
}

TEST_F(AiNativeWriterGradientTest, UnsupportedPaintAndOpacityAreRefusedWithoutRecords)
{
    struct Case { std::string attributes, stops, shape_attributes, tag, reason; };
    std::vector<Case> const cases{
        {geometry + R"( spreadMethod="repeat")", rgb_stops, "", "linearGradient", "repeating or reflected gradients"},
        {geometry + R"( spreadMethod="reflect")", rgb_stops, "", "linearGradient", "repeating or reflected gradients"},
        {geometry + R"( color-interpolation="linearRGB")", rgb_stops, "", "linearGradient", "gradient interpolation other than RGB or CMYK"},
        {geometry, rgb_stops, R"(stroke="#000000" stroke-width="0")", "linearGradient", "strokes on gradient-filled objects"},
        {geometry, rgb_stops, R"(fill-opacity=".5")", "linearGradient", "gradient transparency"},
        {geometry, rgb_stops, R"(opacity=".5")", "linearGradient", "gradient transparency"},
        {geometry, R"(<stop offset="0" stop-color="red"/><stop offset=".5" stop-color="green" stop-opacity=".4"/><stop offset="1" stop-color="blue"/>)", "", "linearGradient", "gradient transparency"},
        {geometry, R"x(<stop offset="0" stop-color="red"/><stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/>)x", "", "linearGradient", "mixed gradient colour spaces"},
        {geometry, R"(<stop offset="0" stop-color="red" proof:ink="Ink"/><stop offset="1" stop-color="blue"/>)", "", "linearGradient", "spot inks in gradients"},
        {geometry, "", "", "linearGradient", "gradients without stops"},
        {R"(gradientUnits="userSpaceOnUse" cx="50" cy="50" r="40" fr="5")", rgb_stops, "", "radialGradient", "radial gradients with a focal radius"},
        {R"(gradientUnits="userSpaceOnUse" cx="50" cy="50" r="0")", rgb_stops, "", "radialGradient", "zero-radius or nonfinite radial gradients"},
        {R"(gradientUnits="userSpaceOnUse" cx="50%" cy="50" r="40")", rgb_stops, "", "radialGradient", "viewport-relative gradient lengths"},
        {R"(gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="100%" y2="0")", rgb_stops, "", "linearGradient", "viewport-relative gradient lengths"},
        {R"(x1="0" y1="0" x2="10px" y2="0")", rgb_stops, "", "linearGradient", "non-normalized object-bounding-box gradient lengths"},
        {R"(gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="0")", rgb_stops, "", "linearGradient", "zero-length or nonfinite linear gradients"},
    };
    for (auto const &test : cases) {
        SCOPED_TRACE(test.reason);
        auto doc = document(test.attributes, test.stops, "", test.shape_attributes, test.tag);
        ASSERT_TRUE(doc);
        expect_refusal(AN::write_gradient(shape(doc.get()), Geom::identity(), "Refused"), test.reason);
        for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
            auto const written = AN::write_records(doc.get(), version);
            EXPECT_TRUE(written.records.empty());
            EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), test.reason), written.refused.end());
        }
    }
}

TEST_F(AiNativeWriterGradientTest, CoincidentDecreasingAndRoundedOffsetsFailClosed)
{
    for (auto const offset : {".5", ".4", ".50000001"}) {
        auto doc = document(geometry, std::string(R"(<stop offset="0" stop-color="red"/><stop offset=".5" stop-color="green"/><stop offset=")") +
                            offset + R"(" stop-color="blue"/><stop offset="1" stop-color="black"/>)");
        ASSERT_TRUE(doc);
        expect_refusal(AN::write_gradient(shape(doc.get()), Geom::identity(), "Hard"),
                       "gradient hard stops or coincident offsets");
    }
}

TEST_F(AiNativeWriterGradientTest, ReferencesCannotHideUnsupportedPlacementSpotsOrCycles)
{
    for (auto const extra : {R"(x2="70")", R"x(gradientTransform="translate(10)")x", R"(proof:ink="Brand")"}) {
        auto const vector = std::string("<linearGradient id=\"v\" ") + extra + ">" + rgb_stops + "</linearGradient>";
        auto doc = document(R"(xlink:href="#v" gradientUnits="userSpaceOnUse")", "", vector);
        ASSERT_TRUE(doc);
        expect_refusal(AN::write_gradient(shape(doc.get()), Geom::identity(), "Inherited"),
                       std::string(extra).find("proof:ink") != std::string::npos ? "spot inks in gradients" :
                                                                                "inherited gradient geometry or transforms");
    }
    auto missing = document(geometry + R"( xlink:href="#missing")");
    ASSERT_TRUE(missing);
    expect_refusal(AN::write_gradient(shape(missing.get()), Geom::identity(), "Missing"), "unresolved gradient references");
    auto cyclic = document(geometry + R"( xlink:href="#g")");
    ASSERT_TRUE(cyclic);
    // URIReference rejects the self-reference before the writer sees it.
    expect_refusal(AN::write_gradient(shape(cyclic.get()), Geom::identity(), "Cycle"), "unresolved gradient references");
    std::string chain;
    for (int i = 0; i < 33; ++i) {
        chain += "<linearGradient id=\"v" + std::to_string(i) + "\"";
        if (i != 32) chain += " xlink:href=\"#v" + std::to_string(i + 1) + "\"";
        chain += ">" + (i == 32 ? rgb_stops : "") + "</linearGradient>";
    }
    auto excessive = document(geometry + R"( xlink:href="#v0")", "", chain);
    ASSERT_TRUE(excessive);
    expect_refusal(AN::write_gradient(shape(excessive.get()), Geom::identity(), "Chain"), "cyclic or excessive gradient references");
}

namespace {

/// The reader's unit circle onto the art for a radial placement, and the focal point's offset
/// from the centre in art space as Bh's angle and share give it through that circle.
struct ReadRadial
{
    Geom::Affine unit;
    Geom::Point focus;
};

ReadRadial read_radial(AN::GradientPlacement const &p)
{
    if (p.bm_given) {
        double const a = p.hilight_angle * std::acos(-1.0) / 180;
        auto const in_unit = Geom::Point(std::cos(a), -std::sin(a)) * p.hilight_length;
        return {p.bm, in_unit * p.bm.withoutTranslation()};
    }
    // Without Bm: Bg's circle about its origin, then Bg's matrix; Bh's x y is the offset.
    return {Geom::Affine(p.length, 0, 0, p.length, p.origin.x(), p.origin.y()) * p.bg, p.hilight};
}

/// Does the reader's circle draw the same ellipse as `expected` (both carry the unit circle)?
void expect_same_ellipse(Geom::Affine const &read, Geom::Affine const &expected)
{
    auto const back = expected.inverse();
    for (int k = 0; k < 12; ++k) {
        double const t = k * std::acos(-1.0) / 6;
        auto const on = Geom::Point(std::cos(t), std::sin(t)) * read * back;
        EXPECT_NEAR(Geom::L2(on), 1.0, 2e-4) << "angle " << k * 30;
    }
    EXPECT_NEAR(read[4], expected[4], 1e-4);
    EXPECT_NEAR(read[5], expected[5], 1e-4);
}

std::string const radial_circle =
    R"(gradientUnits="userSpaceOnUse" cx="100" cy="70" r="50" fx="120" fy="55")";

} // namespace

TEST_F(AiNativeWriterGradientTest, RadialCircleAndFocalPointReadBackInBothRecordVersions)
{
    for (bool legacy : {false, true}) {
        SCOPED_TRACE(legacy ? "AI7" : "modern");
        auto doc = document(radial_circle, rgb_stops, "", "", "radialGradient");
        ASSERT_TRUE(doc);
        auto const *s = shape(doc.get());
        ASSERT_TRUE(s);
        auto const to_art = s->i2doc_affine() * Geom::Affine(0.75, 0, 0, -0.75, -8, 23);
        auto const result = AN::write_gradient(s, to_art, "Radial", legacy);
        ASSERT_TRUE(result.refused.empty()) << result.refused.front();
        // As Illustrator writes every radial instance: Bb, Bh, Bg, then (modern) Bm.
        auto const bh = result.begin.find(" Bh\n"), bg = result.begin.find(" Bg\n"), bm = result.begin.find(" Bm\n");
        ASSERT_NE(bh, std::string::npos);
        ASSERT_NE(bg, std::string::npos);
        EXPECT_EQ(result.begin.rfind("Bb\n", 0), 0u);
        EXPECT_LT(bh, bg);
        EXPECT_EQ(bm == std::string::npos, legacy);
        if (!legacy) EXPECT_LT(bg, bm);
        EXPECT_NE(result.definition.find(" 1 3 Bd\n"), std::string::npos); // Radial, three stops.
        // Illustrator takes a radial's stops in the order listed: first first, as it writes them.
        EXPECT_LT(result.definition.find(" 50 0 %_Bs\n"), result.definition.find(" 50 40 %_Bs\n"));
        EXPECT_LT(result.definition.find(" 50 40 %_Bs\n"), result.definition.find(" 50 100 %_Bs\n"));
        std::string error;
        auto ai = read(result, error);
        ASSERT_TRUE(ai) << error;
        ASSERT_EQ(ai->gradients.size(), 1u);
        EXPECT_TRUE(ai->gradients[0].radial);
        ASSERT_EQ(ai->gradients[0].stops.size(), 3u);
        EXPECT_EQ(ai->gradients[0].stops[0].color, AN::Color::rgb(1, 0, 0));
        EXPECT_EQ(ai->gradients[0].stops[2].color, AN::Color::rgb(0, 0, 1));
        auto const &p = ai->layers[0].children[0].fill->gradient;
        EXPECT_EQ(p.bm_given, !legacy);
        EXPECT_FALSE(p.xm);
        EXPECT_TRUE(p.bg.isIdentity());
        auto const expected = Geom::Affine(50, 0, 0, 50, 100, 70) * to_art;
        auto const got = read_radial(p);
        expect_same_ellipse(got.unit, expected);
        auto const focus = Geom::Point(0.4, -0.3) * expected.withoutTranslation();
        EXPECT_NEAR(got.focus.x(), focus.x(), 1e-3);
        EXPECT_NEAR(got.focus.y(), focus.y(), 1e-3);
        // Bh's x y is the same offset in both forms.
        EXPECT_NEAR(p.hilight.x(), focus.x(), 1e-3);
        EXPECT_NEAR(p.hilight.y(), focus.y(), 1e-3);
        // The share of the radius: 0.5, at most 1.
        EXPECT_NEAR(p.hilight_length, 0.5, 1e-4);
        EXPECT_NEAR(p.origin.x(), expected[4], 1e-4);
        EXPECT_NEAR(p.origin.y(), expected[5], 1e-4);
        EXPECT_NEAR(p.length, 50 * 0.75, 1e-4);
    }
}

TEST_F(AiNativeWriterGradientTest, RadialEllipseIsWholeInBmAndRefusedInIllustrator7)
{
    auto doc = document(radial_circle + R"x( gradientTransform="matrix(1.5 .3 -.2 .6 10 5)")x", rgb_stops, "", "",
                        "radialGradient");
    ASSERT_TRUE(doc);
    auto const *s = shape(doc.get());
    ASSERT_TRUE(s);
    auto const to_art = s->i2doc_affine() * Geom::Affine(1, 0, 0, -1, 0, 200);
    Geom::Point const canvas(4158.25, 4199.5);
    auto const result = AN::write_gradient(s, to_art, "Ellipse", false, canvas);
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    std::string error;
    auto ai = read(result, error);
    ASSERT_TRUE(ai) << error;
    auto const &p = ai->layers[0].children[0].fill->gradient;
    ASSERT_TRUE(p.bm_given);
    auto const expected = Geom::Affine(50, 0, 0, 50, 100, 70) * Geom::Affine(1.5, .3, -.2, .6, 10, 5) * to_art;
    auto const got = read_radial(p);
    expect_same_ellipse(got.unit, expected);
    auto const focus = Geom::Point(0.4, -0.3) * expected.withoutTranslation();
    EXPECT_NEAR(got.focus.x(), focus.x(), 1e-3);
    EXPECT_NEAR(got.focus.y(), focus.y(), 1e-3);
    // Bm's axes are the ellipse's, at right angles. Bg has the ellipse as Illustrator reads it
    // from a file another application wrote: lying along x, its x radius Bg's length, the
    // eleventh number y's over x's, Bg's angle turning nothing; then Bg's matrix applied on
    // Illustrator's y-down canvas, where the art origin is `canvas`.
    auto const x = Geom::Point(p.bm[0], p.bm[1]), y = Geom::Point(p.bm[2], p.bm[3]);
    EXPECT_NEAR(Geom::dot(x, y), 0, 1e-2);
    EXPECT_EQ(p.angle, 0);
    EXPECT_NEAR(Geom::L2(x), p.length, 1e-3);
    auto const bg = result.begin.substr(0, result.begin.find(" Bg\n"));
    auto const aspect = std::stod(bg.substr(bg.rfind(' ') + 1));
    EXPECT_NEAR(aspect, Geom::L2(y) / Geom::L2(x), 2e-4);
    EXPECT_FALSE(p.bg.isIdentity(1e-3)); // This one is turned.
    auto const lying = Geom::Affine(p.length, 0, 0, aspect * p.length, p.origin.x(), p.origin.y());
    auto const flip = Geom::Scale(1, -1);
    auto const on_canvas = flip * Geom::Translate(canvas) * p.bg * Geom::Translate(-canvas) * flip;
    expect_same_ellipse(lying * on_canvas, expected);

    expect_refusal(AN::write_gradient(s, to_art, "Ellipse", true), "elliptical radial gradients (Illustrator 7)");
}

TEST_F(AiNativeWriterGradientTest, RadialEllipseAlongYNeedsNoTurn)
{
    auto doc = document(R"x(gradientUnits="userSpaceOnUse" cx="0" cy="0" r="20" gradientTransform="translate(100 70) scale(.5 1)")x",
                        rgb_stops, "", "", "radialGradient");
    ASSERT_TRUE(doc);
    auto const result = AN::write_gradient(shape(doc.get()), Geom::Affine(1, 0, 0, -1, 0, 200), "Tall", false, {4000, 4000});
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    std::string error;
    auto ai = read(result, error);
    ASSERT_TRUE(ai) << error;
    auto const &p = ai->layers[0].children[0].fill->gradient;
    EXPECT_TRUE(p.bg.isIdentity());
    EXPECT_EQ(p.angle, 0);
    EXPECT_NEAR(p.length, 10, 1e-4); // The x radius, the smaller.
    EXPECT_NE(result.begin.find(" 0 10 1 0 0 1 0 0 2 Bg\n"), std::string::npos) << result.begin;
    EXPECT_NE(result.begin.find("10 0 0 -20 100 130 Bm\n"), std::string::npos) << result.begin;
}

TEST_F(AiNativeWriterGradientTest, RadialFocalPointOutsideTheCircleIsWrittenOnIt)
{
    auto doc = document(R"(gradientUnits="userSpaceOnUse" cx="100" cy="70" r="20" fx="160" fy="70")", rgb_stops, "", "",
                        "radialGradient");
    ASSERT_TRUE(doc);
    for (bool legacy : {false, true}) {
        auto const result = AN::write_gradient(shape(doc.get()), Geom::identity(), "Outside", legacy);
        ASSERT_TRUE(result.refused.empty()) << result.refused.front();
        std::string error;
        auto ai = read(result, error);
        ASSERT_TRUE(ai) << error;
        auto const &p = ai->layers[0].children[0].fill->gradient;
        EXPECT_NEAR(p.hilight_length, 1, 1e-6);
        EXPECT_NEAR(Geom::L2(p.hilight), 20, 1e-4);
    }
    auto centred = document(R"(gradientUnits="userSpaceOnUse" cx="100" cy="70" r="20")", rgb_stops, "", "",
                            "radialGradient");
    ASSERT_TRUE(centred);
    auto const result = AN::write_gradient(shape(centred.get()), Geom::identity(), "Centred");
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    EXPECT_NE(result.begin.find("Bb\n0 0 0 0 Bh\n"), std::string::npos);
}

TEST_F(AiNativeWriterGradientTest, SingularNonfiniteAndExcessiveInputsFailClosed)
{
    auto doc = document();
    ASSERT_TRUE(doc);
    for (auto const matrix : {Geom::Affine(1, 0, 0, 0, 0, 0),
                              Geom::Affine(1, 0, 0, 1, std::numeric_limits<double>::infinity(), 0)}) {
        expect_refusal(AN::write_gradient(shape(doc.get()), matrix, "BadMatrix"), "singular or nonfinite gradient transforms");
    }
    std::string stops;
    for (int i = 0; i <= 256; ++i) {
        stops += "<stop offset=\"" + std::to_string(i / 256.0) + "\" stop-color=\"red\"/>";
    }
    auto excessive = document(geometry, stops);
    ASSERT_TRUE(excessive);
    expect_refusal(AN::write_gradient(shape(excessive.get()), Geom::identity(), "TooMany"), "excessive or missing gradient stops");
}
