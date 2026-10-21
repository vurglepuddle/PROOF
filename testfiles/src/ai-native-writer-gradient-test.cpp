// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic SVG/records only. Actual Illustrator and PDF qualification are separate.
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
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
#include "colors/color.h"
#include "colors/document-colors.h"
#include "colors/manager.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "object/sp-gradient.h"
#include "object/sp-shape.h"
#include "spot-ink.h"

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
                    "0 g\n20 30 m 180 30 L 180 110 L 20 110 L\n" + gradient.begin + "f\n" + AN::gradient_end('f') +
                    "LB\n%AI5_EndLayer--\n%%PageTrailer\n%%EOF\n", error);
}

double scalar(AN::GradientPlacement const &placement, Geom::Point const &point)
{
    if (placement.xm) return (point * placement.xm->inverse()).x();
    double const angle = placement.angle * std::acos(-1.0) / 180;
    auto const delta = point - placement.origin;
    return (delta.x() * std::cos(angle) + delta.y() * std::sin(angle)) / placement.length;
}

void expect_refusal(AN::WrittenGradient const &result, std::string const &reason)
{
    EXPECT_TRUE(result.definition.empty());
    EXPECT_TRUE(result.begin.empty());
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
            EXPECT_EQ(result.begin.rfind("Bb\n", 0), 0u); // A fill's instance opens with Bb alone.
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
        {geometry, R"x(<stop offset="0" stop-color="red"/><stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/>)x", "", "linearGradient", "mixed gradient colour spaces"},
        {geometry, R"(<stop offset="0" stop-color="red" proof:ink="Ink"/><stop offset="1" stop-color="blue"/>)", "", "linearGradient", "spot ink stops without a definition"},
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

TEST_F(AiNativeWriterGradientTest, GradientObjectsKeepTheirOpacityInTheModernFormOnly)
{
    // The object's opacity, and the fill's own when the fill paints alone, are the object's
    // transparency in the modern form; Illustrator 7 has none.
    for (auto const attributes : {R"(fill-opacity=".5")", R"(opacity=".5")"}) {
        SCOPED_TRACE(attributes);
        auto doc = document(geometry, rgb_stops, "", attributes);
        ASSERT_TRUE(doc);
        expect_refusal(AN::write_gradient(shape(doc.get()), Geom::identity(), "Refused", true), "gradient transparency");
        auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
        EXPECT_NE(std::find(legacy.refused.begin(), legacy.refused.end(), "gradient transparency"), legacy.refused.end());
        auto const written = AN::write_records(doc.get());
        ASSERT_TRUE(written.refused.empty()) << written.refused.front();
        std::string error;
        auto ai = AN::read(written.records, error);
        ASSERT_TRUE(ai) << error;
        std::vector<AN::Node const *> shapes;
        gradient_shapes(ai->layers.at(0).children, shapes);
        ASSERT_EQ(shapes.size(), 1u);
        EXPECT_EQ(shapes[0]->fill->kind, AN::Paint::Kind::Gradient);
        EXPECT_DOUBLE_EQ(shapes[0]->transparency.opacity, 0.5);
        EXPECT_EQ(shapes[0]->transparency.blend, 0);
    }
}

TEST_F(AiNativeWriterGradientTest, StopsKeepTheirOpacityInTheNewerFormOnly)
{
    // A stop's own opacity takes Illustrator's newer stop, `… style opacity 6 midpoint ramp`;
    // every stop of that gradient is written so. Opaque gradients keep the older form, and
    // Illustrator 7, which has no other, refuses it.
    std::string const clear_stops = R"(<stop offset="0" stop-color="red"/><stop offset=".5" stop-color="green" )"
                                    R"(stop-opacity=".4"/><stop offset="1" stop-color="blue"/>)";
    std::string const spot_stops =
        R"x(<stop offset="0" style="stop-color:device-cmyk(0 0.6 1 0)" proof:ink="Clear Ink" )x"
        R"x(proof:ink-alternate="device-cmyk(0 0.6 1 0)"/><stop offset="1" style="stop-color:device-cmyk(0 0.3 0.5 0);stop-opacity:0.25" )x"
        R"x(proof:ink="Clear Ink" proof:ink-alternate="device-cmyk(0 0.6 1 0)" proof:ink-tint="0.5"/>)x";
    for (bool const spot : {false, true}) {
        auto const &stops = spot ? spot_stops : clear_stops;
        SCOPED_TRACE(spot ? "spot ink stops" : "RGB stops");
        auto doc = document(geometry, stops);
        ASSERT_TRUE(doc);
        expect_refusal(AN::write_gradient(shape(doc.get()), Geom::identity(), "Clear", true), "gradient transparency");
        auto const gradient = AN::write_gradient(shape(doc.get()), Geom::identity(), "Clear");
        ASSERT_TRUE(gradient.refused.empty()) << gradient.refused.front();
        if (spot) {
            EXPECT_NE(gradient.definition.find("(Clear Ink) 0.5 3 0.25 6 50 100 %_Bs\n"), std::string::npos) << gradient.definition;
            EXPECT_NE(gradient.definition.find("(Clear Ink) 0 3 1 6 50 0 %_Bs\n"), std::string::npos) << gradient.definition;
        } else {
            EXPECT_NE(gradient.definition.find(" 2 0.4 6 50 50 %_Bs\n"), std::string::npos) << gradient.definition;
            EXPECT_NE(gradient.definition.find(" 2 1 6 50 100 %_Bs\n"), std::string::npos) << gradient.definition;
        }
        std::string error;
        auto ai = read(gradient, error);
        ASSERT_TRUE(ai) << error;
        ASSERT_EQ(ai->gradients.size(), 1u);
        auto const &read_stops = ai->gradients[0].stops;
        ASSERT_EQ(read_stops.size(), spot ? 2u : 3u);
        EXPECT_DOUBLE_EQ(read_stops.front().opacity, 1);
        EXPECT_NEAR(read_stops[1].opacity, spot ? 0.25 : 0.4, 1e-6);
        auto const written = AN::write_records(doc.get());
        EXPECT_TRUE(written.refused.empty());
        auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
        EXPECT_NE(std::find(legacy.refused.begin(), legacy.refused.end(), "gradient transparency"), legacy.refused.end());
    }
    // Opaque stops: the older form, as before.
    auto doc = document();
    ASSERT_TRUE(doc);
    auto const opaque = AN::write_gradient(shape(doc.get()), Geom::identity(), "Opaque");
    EXPECT_EQ(opaque.definition.find(" 6 50 "), std::string::npos) << opaque.definition;
    EXPECT_NE(opaque.definition.find(" 2 50 100 %_Bs\n"), std::string::npos) << opaque.definition;
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
                       std::string(extra).find("proof:ink") != std::string::npos ? "spot ink swatches used as gradients" :
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

namespace {

std::unique_ptr<SPDocument> stroked(std::string const &shapes)
{
    auto out = SPDocument::createNewDocFromMem(
        R"x(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="180" viewBox="0 0 320 180"><defs>)x"
        R"x(<linearGradient id="l" gradientUnits="userSpaceOnUse" x1="10" y1="0" x2="150" y2="0">)x" + rgb_stops +
        R"x(</linearGradient><radialGradient id="r" gradientUnits="userSpaceOnUse" cx="230" cy="90" r="50">)x" +
        rgb_stops + "</radialGradient></defs>" + shapes + "</svg>");
    if (out) out->ensureUpToDate();
    return out;
}

} // namespace

TEST_F(AiNativeWriterGradientTest, InstanceEndsSayWhetherTheOperatorStrokes)
{
    // Adobe's 1998 specification, table 10, and Illustrator's own files: f and F 0, S and B 1,
    // s and b 2.
    EXPECT_EQ(AN::gradient_end('f'), "0 BB\n");
    EXPECT_EQ(AN::gradient_end('F'), "0 BB\n");
    EXPECT_EQ(AN::gradient_end('S'), "1 BB\n");
    EXPECT_EQ(AN::gradient_end('B'), "1 BB\n");
    EXPECT_EQ(AN::gradient_end('s'), "2 BB\n");
    EXPECT_EQ(AN::gradient_end('b'), "2 BB\n");
}

TEST_F(AiNativeWriterGradientTest, GradientStrokesAndStrokedGradientFillsReadBack)
{
    auto doc = stroked(R"x(<path d="M10 20 L150 40" fill="none" stroke="url(#l)" stroke-width="8"/>)x"
                       R"x(<rect x="190" y="50" width="80" height="80" fill="none" stroke="url(#r)" stroke-width="6"/>)x"
                       R"x(<rect x="10" y="80" width="100" height="60" fill="url(#l)" stroke="#000000" stroke-width="2"/>)x"
                       R"x(<path d="M10 160 L100 170 L60 175" fill="url(#l)" stroke="#000000" stroke-width="2"/>)x");
    ASSERT_TRUE(doc);
    auto const result = AN::write_records(doc.get(), AN::RecordsVersion::Modern);
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    // As Illustrator writes them: a stroke's instance opens with `1 Bb` about S or s, a fill's
    // with Bb about b or B; the end says whether the operator strokes, and closes first.
    EXPECT_EQ(occurrences(result.records, "\n1 Bb\n"), 2u);
    EXPECT_EQ(occurrences(result.records, "\nBb\n"), 2u);
    EXPECT_EQ(occurrences(result.records, "\nS\n1 BB\n"), 1u);
    EXPECT_EQ(occurrences(result.records, "\ns\n2 BB\n"), 1u);
    EXPECT_EQ(occurrences(result.records, "\nb\n2 BB\n"), 1u);
    EXPECT_EQ(occurrences(result.records, "\nB\n1 BB\n"), 1u);
    EXPECT_EQ(occurrences(result.records, " BB\n"), 4u);

    std::string error;
    auto ai = AN::read(result.records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->layers.size(), 1u);
    auto const &nodes = ai->layers[0].children;
    ASSERT_EQ(nodes.size(), 4u);
    for (std::size_t i : {0u, 1u}) {
        SCOPED_TRACE(i);
        EXPECT_FALSE(nodes[i].fill);
        ASSERT_TRUE(nodes[i].stroke);
        EXPECT_EQ(nodes[i].stroke->kind, AN::Paint::Kind::Gradient);
    }
    EXPECT_NEAR(nodes[0].stroke_style.width, 6, 1e-6);
    EXPECT_NEAR(nodes[1].stroke_style.width, 4.5, 1e-6);
    EXPECT_FALSE(nodes[0].path[0].closed());
    EXPECT_TRUE(nodes[1].path[0].closed());
    // The stroke's ramp is where the SVG has it: halfway at x 80, in art space (x .75, y −.75).
    EXPECT_NEAR(scalar(nodes[0].stroke->gradient, {60, -22.5}), .5, 2e-6);
    EXPECT_NEAR(scalar(nodes[0].stroke->gradient, {7.5, -15}), 0, 2e-6);
    auto const radial = read_radial(nodes[1].stroke->gradient);
    expect_same_ellipse(radial.unit, Geom::Affine(37.5, 0, 0, 37.5, 172.5, -67.5));
    ASSERT_EQ(ai->gradients.size(), 4u); // One definition per painted shape.
    EXPECT_FALSE(ai->gradients.at(nodes[0].stroke->gradient.gradient).radial);
    EXPECT_TRUE(ai->gradients.at(nodes[1].stroke->gradient.gradient).radial);
    for (std::size_t i : {2u, 3u}) {
        SCOPED_TRACE(i);
        ASSERT_TRUE(nodes[i].fill);
        EXPECT_EQ(nodes[i].fill->kind, AN::Paint::Kind::Gradient);
        ASSERT_TRUE(nodes[i].stroke);
        EXPECT_EQ(nodes[i].stroke->kind, AN::Paint::Kind::Solid);
        EXPECT_EQ(nodes[i].stroke->color, AN::Color::rgb(0, 0, 0));
        EXPECT_NEAR(nodes[i].stroke_style.width, 1.5, 1e-6);
    }
    EXPECT_TRUE(nodes[2].path[0].closed());
    EXPECT_FALSE(nodes[3].path[0].closed());

    // Illustrator 7 has gradient fills with plain strokes, and no gradient strokes.
    auto fills = stroked(R"x(<rect x="10" y="80" width="100" height="60" fill="url(#l)" stroke="#000000" stroke-width="2"/>)x");
    ASSERT_TRUE(fills);
    auto const legacy = AN::write_records(fills.get(), AN::RecordsVersion::Illustrator7);
    ASSERT_TRUE(legacy.refused.empty()) << legacy.refused.front();
    EXPECT_EQ(occurrences(legacy.records, "\nb\n2 BB\n"), 1u);
    auto const strokes = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(strokes.records.empty());
    EXPECT_EQ(strokes.refused, std::vector<std::string>{"gradients on strokes (Illustrator 7)"});
}

TEST_F(AiNativeWriterGradientTest, RadialStrokeFocalPointsAreRefusedWithoutChangingFillSupport)
{
    // Illustrator 29 re-saved a noncentral stroke highlight as `0 0 0 0 Bh` and drew it
    // centred. Exercise both axes, an outside focus and inherited stops; fills still work.
    for (auto const &focus : {std::string{}, std::string(R"(fx="230" fy="90")"),
                             std::string(R"(fx="240")"), std::string(R"(fy="80")"),
                             std::string(R"(fx="330" fy="90")")}) {
        SCOPED_TRACE(focus);
        auto doc = SPDocument::createNewDocFromMem(
            R"x(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
                width="320" height="180"><defs>
                <radialGradient id="base">)x" + rgb_stops + R"x(</radialGradient>
                <radialGradient id="g" xlink:href="#base" gradientUnits="userSpaceOnUse"
                    cx="230" cy="90" r="50" )x" + focus + R"x(/></defs>
                <rect id="shape" x="190" y="50" width="80" height="80" fill="url(#g)" stroke="url(#g)"/>
                </svg>)x");
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        auto const *item = shape(doc.get());
        ASSERT_TRUE(item);
        auto const fill = AN::write_gradient(item, Geom::identity(), "Fill");
        EXPECT_TRUE(fill.refused.empty());
        EXPECT_FALSE(fill.begin.empty());
        auto const stroke = AN::write_gradient(item, Geom::identity(), "Stroke", false, {}, true);
        if (focus.empty() || focus == R"(fx="230" fy="90")") {
            EXPECT_TRUE(stroke.refused.empty());
            EXPECT_NE(stroke.begin.find("0 0 0 0 Bh\n"), std::string::npos);
        } else {
            expect_refusal(stroke, "radial gradient strokes with an offset focal point");
        }
    }
}

TEST_F(AiNativeWriterGradientTest, RadialStrokesCompensateIllustratorsStrokedBoundsExpansion)
{
    auto doc = SPDocument::createNewDocFromMem(
        R"x(<svg xmlns="http://www.w3.org/2000/svg" width="430" height="400"><defs>
            <radialGradient id="g" gradientUnits="userSpaceOnUse" cx="310" cy="70" r="90">)x" +
        rgb_stops + R"x(</radialGradient>
            <radialGradient id="off" gradientUnits="userSpaceOnUse" cx="330" cy="80" r="90">)x" +
        rgb_stops + R"x(</radialGradient>
            <radialGradient id="turned" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="60"
                gradientTransform="translate(110 290) rotate(15) scale(1 0.4)">)x" + rgb_stops +
        R"x(</radialGradient></defs>
            <rect x="230" y="20" width="160" height="100" fill="none" stroke="url(#g)" stroke-width="16"/>
            <rect x="230" y="20" width="160" height="100" fill="none" stroke="url(#off)" stroke-width="16"/>
            <path d="M20 290L200 290" fill="none" stroke="url(#turned)" stroke-width="20"/>
            </svg>)x");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const written = AN::write_records(doc.get());
    ASSERT_TRUE(written.refused.empty()) << written.refused.front();
    std::string error;
    auto ai = AN::read(written.records, error);
    ASSERT_TRUE(ai) << error;
    auto const &nodes = ai->layers[0].children;
    ASSERT_EQ(nodes.size(), 3u);
    for (auto const &node : nodes) ASSERT_TRUE(node.stroke);
    auto const &centred = nodes[0].stroke->gradient;
    auto const &offset = nodes[1].stroke->gradient;
    auto const &turned = nodes[2].stroke->gradient;
    // These Bg values were independently checked against Illustrator's exported pictures.
    // Bm remains the full imaging geometry; applying the correction there would double it.
    EXPECT_NEAR(centred.length, 60.4200, 1e-4);
    EXPECT_NEAR(offset.length, 60.4200, 1e-4);
    EXPECT_NEAR(offset.origin.x(), 246.1364, 1e-4);
    EXPECT_NEAR(offset.origin.y(), -58.9655, 1e-4);
    EXPECT_NEAR(turned.length, 42.4555, 1e-4);
    expect_same_ellipse(read_radial(centred).unit, Geom::Affine(67.5, 0, 0, 67.5, 232.5, -52.5));
    expect_same_ellipse(read_radial(offset).unit, Geom::Affine(67.5, 0, 0, 67.5, 247.5, -60));
    expect_same_ellipse(read_radial(turned).unit,
                        Geom::Affine(43.4667, -11.6469, -4.6587, -17.3867, 82.5, -217.5));
}

TEST_F(AiNativeWriterGradientTest, OffCentreRotatedRadialStrokesAreRefusedButFillsRemainSupported)
{
    for (auto const *path : {"M20 290L200 290", "M20 250H200V330H20Z M40 270V310H180V270Z"}) {
        SCOPED_TRACE(path);
        auto doc = SPDocument::createNewDocFromMem(
            R"x(<svg xmlns="http://www.w3.org/2000/svg" width="430" height="400"><defs>
                <radialGradient id="g" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="60"
                    gradientTransform="translate(125 280) rotate(15) scale(1 0.4)">)x" + rgb_stops +
            R"x(</radialGradient></defs><path id="shape" d=")x" + path +
            R"x(" fill="url(#g)" stroke="url(#g)" stroke-width="20"/></svg>)x");
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        auto const *item = shape(doc.get());
        ASSERT_TRUE(item);
        auto const to_art = item->i2doc_affine() * Geom::Affine(.75, 0, 0, -.75, 0, 0);
        EXPECT_TRUE(AN::write_gradient(item, to_art, "Fill").refused.empty());
        expect_refusal(AN::write_gradient(item, to_art, "Stroke", false, {}, true),
                       "off-centre rotated radial gradient strokes");
    }
}

TEST_F(AiNativeWriterGradientTest, CompoundPathStrokeGradientIsOnItsFinalContour)
{
    auto doc = stroked(R"x(<path d="M10 10H90V90H10Z M30 30V70H70V30Z" fill="none" stroke="url(#l)" stroke-width="4"/>)x");
    ASSERT_TRUE(doc);
    auto const result = AN::write_records(doc.get(), AN::RecordsVersion::Modern);
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    EXPECT_EQ(occurrences(result.records, "\n1 Bb\n"), 1u);
    EXPECT_NE(result.records.find("0 G\n"), std::string::npos); // The early contour's plain stroke.
    EXPECT_LT(result.records.find("\n1 Bb\n"), result.records.find("\ns\n2 BB\n*U\n"));
    std::string error;
    auto ai = AN::read(result.records, error);
    ASSERT_TRUE(ai) << error;
    auto const &compound = ai->layers[0].children.at(0);
    EXPECT_EQ(compound.kind, AN::Node::Kind::Compound);
    ASSERT_EQ(compound.children.size(), 2u);
    ASSERT_TRUE(compound.children[1].stroke);
    EXPECT_EQ(compound.children[1].stroke->kind, AN::Paint::Kind::Gradient);
}

TEST_F(AiNativeWriterGradientTest, FilledAndDualGradientStrokesKeepIndependentPaintsOnOnePath)
{
    auto doc = stroked(R"x(<rect x="10" y="10" width="80" height="40" fill="#336699"
                            stroke="url(#l)" stroke-width="4" stroke-dasharray="8 4"/>
        <path d="M10 80L90 80L50 120" fill="#00ff00" stroke="url(#l)" stroke-width="6"/>
        <rect x="190" y="50" width="80" height="80" fill="url(#l)" stroke="url(#r)" stroke-width="6"/>
        <path d="M190 20L270 20L230 40" fill="url(#r)" stroke="url(#l)" stroke-width="4"/>
        <rect x="10" y="140" width="80" height="20" fill="#000000"/>)x");
    ASSERT_TRUE(doc);
    auto const result = AN::write_records(doc.get());
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    EXPECT_EQ(occurrences(result.records, "\nBb\n"), 2u);
    EXPECT_EQ(occurrences(result.records, "\n1 Bb\n"), 4u);
    EXPECT_EQ(occurrences(result.records, "\nb\n2 BB\n"), 2u);
    EXPECT_EQ(occurrences(result.records, "\nB\n1 BB\n"), 2u);
    EXPECT_EQ(occurrences(result.records, " BB\n"), 6u);
    EXPECT_EQ(occurrences(result.records, "\n2 BB\n1 Bb\n"), 1u);
    EXPECT_EQ(occurrences(result.records, "\n1 BB\n1 Bb\n"), 1u);
    std::string error;
    auto ai = AN::read(result.records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->gradients.size(), 6u);
    auto const &nodes = ai->layers.at(0).children;
    ASSERT_EQ(nodes.size(), 5u); // Both paints belong to one path, with no duplicate art.
    for (unsigned i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        ASSERT_TRUE(nodes[i].fill);
        ASSERT_TRUE(nodes[i].stroke);
        EXPECT_EQ(nodes[i].fill->kind, i < 2 ? AN::Paint::Kind::Solid : AN::Paint::Kind::Gradient);
        EXPECT_EQ(nodes[i].stroke->kind, AN::Paint::Kind::Gradient);
        EXPECT_EQ(nodes[i].path.at(0).closed(), i % 2 == 0);
    }
    EXPECT_EQ(nodes[0].fill->color, AN::Color::rgb(.2, .4, .6));
    EXPECT_EQ(nodes[0].stroke_style.dash, (std::vector<double>{6, 3}));
    EXPECT_NEAR(scalar(nodes[2].fill->gradient, {60, -60}), .5, 2e-6);
    auto const radial_stroke = read_radial(nodes[2].stroke->gradient);
    expect_same_ellipse(radial_stroke.unit, Geom::Affine(37.5, 0, 0, 37.5, 172.5, -67.5));
    EXPECT_NE(nodes[2].fill->gradient.gradient, nodes[2].stroke->gradient.gradient);
    ASSERT_TRUE(nodes[4].fill);
    EXPECT_EQ(nodes[4].fill->kind, AN::Paint::Kind::Solid);
    EXPECT_EQ(nodes[4].fill->color, AN::Color::rgb(0, 0, 0));
    EXPECT_FALSE(nodes[4].stroke);
    auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(legacy.records.empty());
    EXPECT_EQ(legacy.refused, std::vector<std::string>{"gradients on strokes (Illustrator 7)"});
}

TEST_F(AiNativeWriterGradientTest, CompoundDualGradientUsesItsFinalContourAndKeepsTheHole)
{
    auto doc = stroked(R"x(<path d="M10 10H90V90H10Z M30 30V70H70V30Z" fill="url(#r)"
                          fill-rule="evenodd" stroke="url(#l)" stroke-width="4"/>)x");
    ASSERT_TRUE(doc);
    auto const result = AN::write_records(doc.get());
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    EXPECT_EQ(occurrences(result.records, "\nBb\n"), 1u);
    EXPECT_EQ(occurrences(result.records, "\n1 Bb\n"), 1u);
    EXPECT_EQ(occurrences(result.records, "\n2 BB\n1 Bb\n"), 1u);
    std::string error;
    auto ai = AN::read(result.records, error);
    ASSERT_TRUE(ai) << error;
    auto const &compound = ai->layers.at(0).children.at(0);
    EXPECT_EQ(compound.kind, AN::Node::Kind::Compound);
    ASSERT_EQ(compound.children.size(), 2u);
    EXPECT_TRUE(compound.evenodd);
    ASSERT_TRUE(compound.fill);
    ASSERT_TRUE(compound.stroke);
    EXPECT_EQ(compound.fill->kind, AN::Paint::Kind::Gradient);
    EXPECT_EQ(compound.stroke->kind, AN::Paint::Kind::Gradient);
    EXPECT_TRUE(ai->gradients.at(compound.fill->gradient.gradient).radial);
    EXPECT_FALSE(ai->gradients.at(compound.stroke->gradient.gradient).radial);
}

TEST_F(AiNativeWriterGradientTest, GradientStrokeLimitsAreRefusedByName)
{
    struct Case { std::string shape, reason; };
    std::vector<Case> const cases{
        {R"x(<rect x="10" y="10" width="80" height="40" fill="#ff0000" stroke="url(#l)" stroke-width="4"
                  stroke-opacity=".5"/>)x", "separate fill and stroke transparency"},
        {R"x(<rect x="10" y="10" width="80" height="40" fill="url(#l)" stroke="url(#r)"
                  stroke-width="4" paint-order="stroke fill"/>)x", "strokes painted under their fill"},
        {R"x(<rect x="10" y="10" width="80" height="40" fill="#ff0000" stroke="url(#l)"
                  stroke-width="4" transform="scale(2 1)"/>)x", "strokes on stretched or skewed objects"},
        {R"x(<path d="M10 10V90" fill="none" stroke="url(#l)" stroke-width="4"/>)x",
         "linear gradient strokes without bounds along their ramp"},
    };
    for (auto const &test : cases) {
        SCOPED_TRACE(test.reason);
        auto doc = stroked(test.shape);
        ASSERT_TRUE(doc);
        auto const written = AN::write_records(doc.get(), AN::RecordsVersion::Modern);
        EXPECT_TRUE(written.records.empty());
        EXPECT_NE(std::find(written.refused.begin(), written.refused.end(), test.reason), written.refused.end());
    }
}

TEST_F(AiNativeWriterGradientTest, LinearStrokeBoundsDoNotMoveTheNativeScalarField)
{
    for (bool compound : {false, true}) {
        SCOPED_TRACE(compound);
        auto doc = stroked(compound
            ? R"x(<path d="M10 10H90V50H10Z M30 20V40H70V20Z" fill="#336699" stroke="url(#l)" stroke-width="4"/>)x"
            : R"x(<rect x="10" y="10" width="80" height="40" fill="#336699" stroke="url(#l)" stroke-width="4"/>)x");
        ASSERT_TRUE(doc);
        auto const result = AN::write_records(doc.get());
        ASSERT_TRUE(result.refused.empty()) << result.refused.front();
        std::string error;
        auto ai = AN::read(result.records, error);
        ASSERT_TRUE(ai) << error;
        auto const &node = ai->layers.at(0).children.at(0);
        ASSERT_TRUE(node.stroke);
        auto const &placement = node.stroke->gradient;
        ASSERT_TRUE(placement.xm);
        EXPECT_NEAR((*placement.xm)[0], 105, 1e-6);
        EXPECT_NEAR((*placement.xm)[4], 7.5, 1e-6);
        EXPECT_NEAR(scalar(placement, {60, -30}), .5, 1e-6);
        // A simple path expands by 3 art units; Illustrator leaves a compound's ramp alone.
        EXPECT_NEAR(placement.length, compound ? 105 : 105 * 60. / 63., 1e-4);
        EXPECT_NEAR(placement.origin.x(), compound ? 7.5 : 7.5 + 1.5 * 60. / 63., 1e-4);
    }
}

TEST_F(AiNativeWriterGradientTest, DualRadialKeepsTheFillFocusAndCentresTheStroke)
{
    auto doc = stroked(R"x(<defs><radialGradient id="focus" gradientUnits="userSpaceOnUse"
        cx="230" cy="90" r="50" fx="240" fy="80">)x" + rgb_stops +
        R"x(</radialGradient></defs><rect x="190" y="50" width="80" height="80"
        fill="url(#focus)" stroke="url(#r)" stroke-width="6"/>)x");
    ASSERT_TRUE(doc);
    auto const result = AN::write_records(doc.get());
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    // Illustrator shares Bh across the pair. A second zero Bh would erase the fill focus.
    EXPECT_EQ(occurrences(result.records, " Bh\n"), 1u);
    std::string error;
    auto ai = AN::read(result.records, error);
    ASSERT_TRUE(ai) << error;
    auto const &node = ai->layers.at(0).children.at(0);
    ASSERT_TRUE(node.fill && node.stroke);
    EXPECT_NEAR(node.fill->gradient.hilight_length, std::sqrt(.08), 1e-4);
    EXPECT_DOUBLE_EQ(node.stroke->gradient.hilight_length, 0);
}

namespace {

std::string const brand = R"x(proof:ink="Brand Red" proof:ink-alternate="device-cmyk(0 1 0.8 0.1)")x";

/// Spot stops as the .ai import writes them: the ink, its full-strength alternate and the tint
/// on the <stop>, its colour the tint shown. `swatch` is the alternate of a solid swatch of the
/// same ink.
std::unique_ptr<SPDocument> spotted(std::string const &stops, std::string const &swatch = "device-cmyk(0 1 0.8 0.1)")
{
    auto out = SPDocument::createNewDocFromMem(
        R"x(<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" )x"
        R"x(xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="320" height="180" viewBox="0 0 320 180">)x"
        R"x(<defs><linearGradient id="s" gradientUnits="userSpaceOnUse" x1="10" y1="0" x2="150" y2="0">)x" + stops +
        R"x(</linearGradient><linearGradient id="sw" inkscape:swatch="solid" proof:ink="Brand Red" proof:ink-alternate=")x" +
        swatch + R"x("><stop style="stop-color:)x" + swatch + R"x("/></linearGradient></defs>)x"
        R"x(<rect x="10" y="10" width="140" height="60" fill="url(#s)"/>)x"
        R"x(<rect x="170" y="10" width="60" height="60" fill="url(#sw)"/></svg>)x");
    if (out) out->ensureUpToDate();
    return out;
}

std::string const spot_stops =
    R"x(<stop offset="0" style="stop-color:device-cmyk(0 1 0.8 0.1)" )x" + brand + "/>" +
    R"x(<stop offset="0.5" style="stop-color:device-cmyk(0 0.3 0.24 0.03)" proof:ink-tint="0.3" )x" + brand + "/>" +
    R"x(<stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/>)x";

} // namespace

TEST_F(AiNativeWriterGradientTest, SpotStopsAreWrittenAsTintsOfTheirInk)
{
    auto doc = spotted(spot_stops);
    ASSERT_TRUE(doc);
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        SCOPED_TRACE(version == AN::RecordsVersion::Modern ? "modern" : "AI7");
        auto const result = AN::write_records(doc.get(), version);
        ASSERT_TRUE(result.refused.empty()) << result.refused.front();
        // The ink is declared once, for the solid swatch and the gradient alike.
        EXPECT_EQ(occurrences(result.records, "%%DocumentCustomColors: (Brand Red)\n"), 1u);
        EXPECT_EQ(occurrences(result.records, "%%CMYKCustomColor: 0 1 0.8 0.1 (Brand Red)\n"), 1u);
        EXPECT_EQ(occurrences(result.records, "(Brand Red)"), 5u);
        // Full-strength CMYK, the name, the tint taken from 1, colour style 3.
        EXPECT_NE(result.records.find("0 1 0.8 0.1 (Brand Red) 0 3 50 0 %_Bs\n"), std::string::npos);
        EXPECT_NE(result.records.find("0 1 0.8 0.1 (Brand Red) 0.7 3 50 50 %_Bs\n"), std::string::npos);
        EXPECT_NE(result.records.find("1 0 0 0 1 50 100 %_Bs\n"), std::string::npos);

        std::string error;
        auto ai = AN::read(result.records, error);
        ASSERT_TRUE(ai) << error;
        ASSERT_EQ(ai->gradients.size(), 1u);
        auto const &stops = ai->gradients[0].stops;
        ASSERT_EQ(stops.size(), 3u);
        ASSERT_GE(stops[0].ink, 0);
        EXPECT_EQ(stops[1].ink, stops[0].ink);
        EXPECT_EQ(stops[2].ink, -1);
        EXPECT_NEAR(stops[0].tint, 1, 1e-6);
        EXPECT_NEAR(stops[1].tint, 0.3, 1e-6);
        EXPECT_EQ(stops[2].color, AN::Color::cmyk(1, 0, 0, 0));
        auto const &ink = ai->inks.at(stops[0].ink);
        EXPECT_EQ(ink.name, "Brand Red");
        EXPECT_EQ(ink.kind, AN::Ink::Kind::Spot);
        EXPECT_EQ(ink.color, AN::Color::cmyk(0, 1, 0.8, 0.1));
    }
}

namespace {

std::string fixed4(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

/// A colour's CMYK in `space`, by the working rendering intent, as the writer converts an ink's.
std::string cmyk_text(Inkscape::Colors::Color const &c, std::shared_ptr<Inkscape::Colors::Space::AnySpace> const &space)
{
    auto values = c.getValues();
    if (!c.getSpace()->convert(values, space, Inkscape::Colors::DocumentColors::workingIntent())) return "none";
    std::string out;
    for (unsigned i = 0; i < 4; ++i) out += (i ? " " : "") + fixed4(std::clamp(values[i], 0.0, 1.0));
    return out;
}

} // namespace

TEST_F(AiNativeWriterGradientTest, InkStopsDefinedInLabOrRgbAreWrittenAsIllustratorWritesThem)
{
    using Type = Inkscape::Colors::Space::Type;
    std::string const lab = R"x(proof:ink="Book Red" proof:ink-alternate="lab(50 60 30)")x";
    std::string const rgb = R"x(proof:ink="Screen Orange" proof:ink-alternate="#ff8000")x";
    auto doc = SPDocument::createNewDocFromMem(
        R"x(<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" )x"
        R"x(xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="320" height="180" viewBox="0 0 320 180"><defs>)x"
        R"x(<linearGradient id="a" gradientUnits="userSpaceOnUse" x1="10" y1="0" x2="150" y2="0">)x"
        R"x(<stop offset="0" style="stop-color:lab(50 60 30)" )x" + lab + "/>" +
        R"x(<stop offset="0.5" style="stop-color:lab(80 24 12)" proof:ink-tint="0.4" )x" + lab + "/>" +
        R"x(<stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/></linearGradient>)x"
        R"x(<linearGradient id="b" gradientUnits="userSpaceOnUse" x1="170" y1="0" x2="310" y2="0">)x"
        R"x(<stop offset="0" style="stop-color:#ff8000" )x" + rgb + "/>" +
        R"x(<stop offset="1" style="stop-color:#ffffff" proof:ink-tint="0" )x" + rgb + "/></linearGradient>" +
        R"x(<linearGradient id="sa" inkscape:swatch="solid" )x" + lab + R"x(><stop style="stop-color:lab(50 60 30)"/></linearGradient>)x" +
        R"x(<linearGradient id="sb" inkscape:swatch="solid" )x" + rgb + R"x(><stop style="stop-color:#ff8000"/></linearGradient>)x" +
        R"x(</defs><rect x="10" y="10" width="140" height="60" fill="url(#a)"/>)x"
        R"x(<rect x="170" y="10" width="140" height="60" fill="url(#b)"/>)x"
        R"x(<rect x="10" y="100" width="60" height="60" fill="url(#sa)"/>)x"
        R"x(<rect x="170" y="100" width="60" height="60" fill="url(#sb)"/></svg>)x");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const result = AN::write_records(doc.get());
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    auto const &records = result.records;
    // Declared once for the swatch and the stops alike, by their RGB.
    EXPECT_EQ(occurrences(records, "%%DocumentCustomColors: (Book Red)\n%%+ (Screen Orange)\n"), 1u);
    EXPECT_EQ(records.find("%%CMYKCustomColor:"), std::string::npos);
    EXPECT_EQ(occurrences(records, "%%+ 1 0.502 0 (Screen Orange)\n"), 1u);

    auto const definition = Inkscape::Colors::Color(Type::LAB, {0.5, (60.0 + 128) / 255, (30.0 + 128) / 255});
    auto const device = Inkscape::Colors::Manager::get().find(Type::CMYK);
    auto const full = cmyk_text(definition, device);
    auto const tinted = cmyk_text(Inkscape::SpotInk::display_color(definition, 0.4), device);
    ASSERT_NE(full, tinted);
    // Each named stop twice: the stand-in, the full-strength CMYK in colour style 3, then the
    // exact stop, the CMYK at its tint and the Lab, type 2 and style 4; every stop in the newer form.
    EXPECT_EQ(occurrences(records, full + " (Book Red) 0 3 1 6 50 0 %_BS\n%_" + full +
                                       " 50 60 30 (Book Red) 0 2 4 1 6 50 0 Bs\n"), 1u);
    EXPECT_EQ(occurrences(records, full + " (Book Red) 0.6 3 1 6 50 50 %_BS\n%_" + tinted +
                                       " 50 60 30 (Book Red) 0.6 2 4 1 6 50 50 Bs\n"), 1u);
    EXPECT_EQ(occurrences(records, "\n1 0 0 0 1 1 6 50 100 %_Bs\n"), 1u);
    // RGB: type 1, and style 4 as Illustrator 29 writes it.
    EXPECT_EQ(occurrences(records, " 1 0.502 0 (Screen Orange) 0 1 4 1 6 50 0 Bs\n"), 1u);
    EXPECT_EQ(occurrences(records, " 1 0.502 0 (Screen Orange) 1 1 4 1 6 50 100 Bs\n"), 1u);
    EXPECT_EQ(occurrences(records, " 50 60 30 (Book Red) 0 2 Xx\n"), 1u);
    EXPECT_EQ(occurrences(records, " 1 0.502 0 (Screen Orange) 0 1 Xx\n"), 1u);

    std::string error;
    auto ai = AN::read(records, error);
    ASSERT_TRUE(ai) << error;
    ASSERT_EQ(ai->inks.size(), 2u);
    ASSERT_EQ(ai->gradients.size(), 2u);
    auto const ink_named = [&](std::string const &name) {
        for (std::size_t i = 0; i < ai->inks.size(); ++i) {
            if (ai->inks[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    auto const book = ink_named("Book Red"), orange = ink_named("Screen Orange");
    ASSERT_GE(book, 0);
    ASSERT_GE(orange, 0);
    EXPECT_EQ(ai->inks[book].color, AN::Color::lab(50, 60, 30));
    EXPECT_EQ(ai->inks[orange].color, AN::Color::rgb(1, 0.502, 0));
    for (auto const &gradient : ai->gradients) {
        auto const &stops = gradient.stops;
        if (stops.size() == 3) {
            EXPECT_EQ(stops[0].ink, book);
            EXPECT_EQ(stops[1].ink, book);
            EXPECT_NEAR(stops[0].tint, 1, 1e-6);
            EXPECT_NEAR(stops[1].tint, 0.4, 1e-6);
            EXPECT_EQ(stops[1].color, AN::Color::lab(50, 60, 30));
            EXPECT_EQ(stops[2].ink, -1);
        } else {
            ASSERT_EQ(stops.size(), 2u);
            EXPECT_EQ(stops[0].ink, orange);
            EXPECT_EQ(stops[1].ink, orange);
            EXPECT_NEAR(stops[1].tint, 0, 1e-6);
        }
    }

    auto const legacy = AN::write_records(doc.get(), AN::RecordsVersion::Illustrator7);
    EXPECT_TRUE(legacy.records.empty());
    EXPECT_NE(std::find(legacy.refused.begin(), legacy.refused.end(), "spot inks defined in RGB or Lab (Illustrator 7)"),
              legacy.refused.end());

    // One name, one definition: a Lab stop of an ink whose swatch is CMYK.
    auto clash = spotted(R"x(<stop offset="0" style="stop-color:lab(50 60 30)" proof:ink="Brand Red" )x"
                         R"x(proof:ink-alternate="lab(50 60 30)"/><stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/>)x");
    ASSERT_TRUE(clash);
    auto const twice = AN::write_records(clash.get());
    EXPECT_TRUE(twice.records.empty());
    EXPECT_NE(std::find(twice.refused.begin(), twice.refused.end(), "one ink name with two definitions"),
              twice.refused.end());
}

TEST_F(AiNativeWriterGradientTest, TheCmykBesideALabInkComesThroughTheDocumentsProfile)
{
    using Type = Inkscape::Colors::Space::Type;
    auto const path = std::string(INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc");
    std::string const lab = R"x(proof:ink="Book Red" proof:ink-alternate="lab(50 60 30)")x";
    auto doc = SPDocument::createNewDocFromMem(
        R"x(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )x"
        R"x(xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" )x"
        R"x(xmlns:proof="urn:x-proof:document:1" proof:color-mode="CMYK" proof:color-profile="Press" )x"
        R"x(width="320" height="180" viewBox="0 0 320 180"><defs>)x"
        R"x(<color-profile id="p" name="Press" xlink:href="file:///)x" + path + R"x("/>)x"
        R"x(<linearGradient id="sa" inkscape:swatch="solid" )x" + lab + R"x(><stop style="stop-color:lab(50 60 30)"/></linearGradient>)x"
        R"x(</defs><rect x="10" y="10" width="60" height="60" fill="url(#sa)"/></svg>)x");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const space = Inkscape::Colors::DocumentColors::assignedSpace(doc.get());
    ASSERT_TRUE(space);
    auto const definition = Inkscape::Colors::Color(Type::LAB, {0.5, (60.0 + 128) / 255, (30.0 + 128) / 255});
    auto const press = cmyk_text(definition, space);
    ASSERT_NE(press, cmyk_text(definition, Inkscape::Colors::Manager::get().find(Type::CMYK)));
    auto const result = AN::write_records(doc.get());
    ASSERT_TRUE(result.refused.empty()) << result.refused.front();
    EXPECT_EQ(occurrences(result.records, "\n" + press + " 50 60 30 (Book Red) 0 2 Xx\n"), 1u);
}

TEST_F(AiNativeWriterGradientTest, LegacyGradientAndStopNamesUseStrictWindows1252)
{
    std::string const name = "Ramp \xc3\x83\xc2\xa9 \xe2\x82\xac (A) \\ B";
    std::string const ink_name = "Ink \xc3\x83\xc2\xa9";
    auto const stops = R"x(<stop offset="0" style="stop-color:device-cmyk(0 1 0.8 0.1)" proof:ink=")x" + ink_name +
                       R"x(" proof:ink-alternate="device-cmyk(0 1 0.8 0.1)"/>)x" +
                       R"x(<stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/>)x";
    auto doc = document(geometry, stops);
    ASSERT_TRUE(doc);
    for (bool legacy : {false, true}) {
        auto const result = AN::write_gradient(shape(doc.get()), Geom::identity(), name, legacy);
        ASSERT_TRUE(result.refused.empty()) << result.refused.front();
        ASSERT_EQ(result.inks.size(), 1u);
        EXPECT_EQ(result.inks[0].first, ink_name);
        if (legacy) {
            EXPECT_NE(result.definition.find("(Ramp \\303\\251 \\200 \\(A\\) \\\\ B)"), std::string::npos);
            EXPECT_NE(result.begin.find("(Ramp \\303\\251 \\200 \\(A\\) \\\\ B)"), std::string::npos);
            EXPECT_NE(result.definition.find("(Ink \\303\\251)"), std::string::npos);
        }
        std::string error;
        auto parsed = AN::read(std::string(legacy ? "%AI5_FileFormat 3\n" : "%AI5_FileFormat 14.0\n") +
                               "%%BoundingBox: 0 0 300 200\n1 Bn\n" + result.definition +
                               "%AI5_BeginLayer\n1 1 1 1 0 0 0 79 128 255 Lb\n(Art) Ln\n0 g\n"
                               "20 30 m 180 30 L 180 110 L 20 110 L\n" + result.begin + "f\n" +
                               AN::gradient_end('f') + "LB\n%AI5_EndLayer--\n%%EOF\n", error);
        ASSERT_TRUE(parsed) << error;
        ASSERT_EQ(parsed->gradients.size(), 1u);
        EXPECT_EQ(parsed->gradients[0].name, name);
        ASSERT_EQ(parsed->inks.size(), 1u);
        EXPECT_EQ(parsed->inks[0].name, ink_name);
        EXPECT_EQ(parsed->layers.at(0).children.at(0).fill->gradient.gradient, 0);
    }
    expect_refusal(AN::write_gradient(shape(doc.get()), Geom::identity(), "Ramp \xe4\xb8\xad", true),
                   "gradient names outside Windows-1252 (Illustrator 7)");
}

TEST_F(AiNativeWriterGradientTest, SpotStopsThatCannotBeWrittenAreRefusedByName)
{
    struct Case { std::string stops, swatch, reason; bool legacy_only = false; };
    auto const ink = [](std::string const &attrs, std::string const &offset) {
        return R"x(<stop offset=")x" + offset + R"x(" style="stop-color:device-cmyk(0 1 0.8 0.1)" )x" + attrs + "/>";
    };
    std::string const process_end = R"x(<stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/>)x";
    std::string const cmyk_swatch = "device-cmyk(0 1 0.8 0.1)";
    std::vector<Case> const cases{
        {ink(brand, "0") + R"(<stop offset="1" stop-color="#00ff00"/>)", cmyk_swatch, "mixed gradient colour spaces"},
        {ink(R"(proof:ink="Web Red" proof:ink-alternate="#ff0000")", "0") + process_end, cmyk_swatch,
         "spot inks defined in RGB or Lab (Illustrator 7)", true},
        {ink(R"x(proof:ink="Deep Ink" proof:ink-alternate="xyz(0.2 0.1 0.3)")x", "0") + process_end, cmyk_swatch,
         "spot inks defined in other colour spaces"},
        {ink(R"x(proof:ink="All" proof:ink-alternate="device-cmyk(1 1 1 1)")x", "0") + process_end, cmyk_swatch,
         "Registration in gradients"},
        {ink(brand, "0") + process_end, "device-cmyk(0 1 1 0)", "one ink name with two definitions"},
        {ink(R"x(proof:ink="Фирменный" proof:ink-alternate="device-cmyk(0 1 0.8 0.1)")x", "0") + process_end,
         cmyk_swatch, "ink names outside Windows-1252 (Illustrator 7)", true},
    };
    for (auto const &test : cases) {
        SCOPED_TRACE(test.reason);
        auto doc = spotted(test.stops, test.swatch);
        ASSERT_TRUE(doc);
        for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
            auto const written = AN::write_records(doc.get(), version);
            bool const refused = !test.legacy_only || version == AN::RecordsVersion::Illustrator7;
            EXPECT_EQ(written.records.empty(), refused);
            EXPECT_EQ(std::find(written.refused.begin(), written.refused.end(), test.reason) != written.refused.end(),
                      refused);
        }
    }
}

TEST_F(AiNativeWriterGradientTest, IccCmykStopsAreWrittenAsTheirNumbers)
{
    // As the import leaves a CMYK file with its own profile: every process paint in it, stops too.
    auto const path = std::string(INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc");
    auto doc = SPDocument::createNewDocFromMem(
        R"x(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )x"
        R"x(xmlns:proof="urn:x-proof:document:1" proof:color-mode="CMYK" proof:color-profile="Press" )x"
        R"x(width="320" height="180" viewBox="0 0 320 180"><defs>)x"
        R"x(<color-profile id="p" name="Press" xlink:href="file:///)x" + path + R"x("/>)x"
        R"x(<linearGradient id="g" gradientUnits="userSpaceOnUse" x1="10" y1="0" x2="150" y2="0">)x"
        R"x(<stop offset="0" style="stop-color:#cd2468 icc-color(Press, 0.05, 1, 0.45, 0.22)"/>)x"
        R"x(<stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/></linearGradient></defs>)x"
        R"x(<rect x="10" y="10" width="140" height="60" fill="url(#g)"/>)x"
        R"x(<rect x="170" y="10" width="60" height="60" style="fill:#cd2468 icc-color(Press, 0.05, 1, 0.45, 0.22)"/></svg>)x");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const *g = cast<SPGradient>(doc->getObjectById("g"));
    ASSERT_TRUE(g && g->getGradientVector()->stops.front().color);
    auto const space = g->getGradientVector()->stops.front().color->getSpace();
    ASSERT_TRUE(space);
    ASSERT_EQ(space->getType(), Inkscape::Colors::Space::Type::CMS); // Really in the profile.
    for (auto const version : {AN::RecordsVersion::Modern, AN::RecordsVersion::Illustrator7}) {
        SCOPED_TRACE(version == AN::RecordsVersion::Modern ? "modern" : "AI7");
        auto const result = AN::write_records(doc.get(), version);
        ASSERT_TRUE(result.refused.empty()) << result.refused.front();
        EXPECT_NE(result.records.find("0.05 1 0.45 0.22 1 50 0 %_Bs\n"), std::string::npos);
        EXPECT_NE(result.records.find("1 0 0 0 1 50 100 %_Bs\n"), std::string::npos);
        EXPECT_NE(result.records.find("0.05 1 0.45 0.22 k\n"), std::string::npos); // The solid, as before.
        std::string error;
        auto ai = AN::read(result.records, error);
        ASSERT_TRUE(ai) << error;
        ASSERT_EQ(ai->gradients.size(), 1u);
        ASSERT_EQ(ai->gradients[0].stops.size(), 2u);
        EXPECT_EQ(ai->gradients[0].stops[0].color, AN::Color::cmyk(0.05, 1, 0.45, 0.22));
        EXPECT_EQ(ai->gradients[0].stops[1].color, AN::Color::cmyk(1, 0, 0, 0));
    }
}
