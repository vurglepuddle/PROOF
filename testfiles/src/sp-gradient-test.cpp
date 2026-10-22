// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests migrated from cxxtest
 *
 * Authors:
 *   Adrian Boguszewski
 *
 * Copyright (C) 2018 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <glibmm/i18n.h>
#include <doc-per-case-test.h>
#include <src/object/sp-gradient.h>
#include <src/attributes.h>
#include <2geom/transforms.h>
#include <src/xml/node.h>
#include <src/xml/simple-document.h>
#include <src/svg/svg.h>
#include "document-undo.h"
#include "gradient-chemistry.h"
#include "inkscape.h"
#include "colors/color.h"
#include "colors/spaces/base.h"
#include "object/gradient-perceptual.h"
#include "object/gradient-rendering.h"
#include "object/mesh-object.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-path.h"
#include "object/sp-defs.h"
#include "object/sp-star.h"
#include "preferences.h"
#include "style.h"

using namespace Inkscape;
using namespace Inkscape::XML;

class SPGradientTest: public DocPerCaseTest {
public:
    SPGradientTest() {
        DocPerCaseTest::SetUpTestCase();
        gr = new SPGradient();
    }

    ~SPGradientTest() override {
        delete gr;
        DocPerCaseTest::TearDownTestCase();
    }

    SPGradient *gr;
};

TEST_F(SPGradientTest, Init) {
    ASSERT_TRUE(gr != nullptr);
    EXPECT_TRUE(gr->gradientTransform.isIdentity());
    EXPECT_TRUE(Geom::are_near(Geom::identity(), gr->gradientTransform));
}

TEST_F(SPGradientTest, SetGradientTransform) {
    gr->document = _doc.get();

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "translate(5, 8)");
    EXPECT_TRUE(Geom::are_near(Geom::Affine(Geom::Translate(5.0, 8.0)), gr->gradientTransform));

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "");
    EXPECT_TRUE(Geom::are_near(Geom::identity(), gr->gradientTransform));

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "rotate(90)");
    EXPECT_TRUE(Geom::are_near(Geom::Affine(Geom::Rotate::from_degrees(90.0)), gr->gradientTransform));
}

TEST_F(SPGradientTest, Write) {
    gr->document = _doc.get();

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "matrix(0, 1, -1, 0, 0, 0)");
    Document *xml_doc = _doc->getReprDoc();

    ASSERT_TRUE(xml_doc != nullptr);

    Node *repr = xml_doc->createElement("svg:radialGradient");
    gr->updateRepr(xml_doc, repr, SP_OBJECT_WRITE_ALL);

    gchar const *tr = repr->attribute("gradientTransform");
    Geom::Affine svd;
    bool const valid = sp_svg_transform_read(tr, &svd);

    EXPECT_TRUE(valid);
    EXPECT_TRUE(Geom::are_near(Geom::Affine(Geom::Rotate::from_degrees(90.0)), svd));
}

TEST_F(SPGradientTest, GetG2dGetGs2dSetGs2) {
    gr->document = _doc.get();

    Geom::Affine grXform(2, 1,
                         1, 3,
                         4, 6);
    gr->gradientTransform = grXform;

    Geom::Rect unit_rect(Geom::Point(0, 0), Geom::Point(1, 1));
    {
        Geom::Affine g2d(gr->get_g2d_matrix(Geom::identity(), unit_rect));
        Geom::Affine gs2d(gr->get_gs2d_matrix(Geom::identity(), unit_rect));
        EXPECT_TRUE(Geom::are_near(Geom::identity(), g2d));
        EXPECT_TRUE(Geom::are_near(gs2d, gr->gradientTransform * g2d, 1e-12));

        gr->set_gs2d_matrix(Geom::identity(), unit_rect, gs2d);
        EXPECT_TRUE(Geom::are_near(gr->gradientTransform, grXform, 1e-12));
    }

    gr->gradientTransform = grXform;
    Geom::Affine funny(2, 3,
                       4, 5,
                       6, 7);
    {
        Geom::Affine g2d(gr->get_g2d_matrix(funny, unit_rect));
        Geom::Affine gs2d(gr->get_gs2d_matrix(funny, unit_rect));
        EXPECT_TRUE(Geom::are_near(funny, g2d));
        EXPECT_TRUE(Geom::are_near(gs2d, gr->gradientTransform * g2d, 1e-12));

        gr->set_gs2d_matrix(funny, unit_rect, gs2d);
        EXPECT_TRUE(Geom::are_near(gr->gradientTransform, grXform, 1e-12));
    }

    gr->gradientTransform = grXform;
    Geom::Rect larger_rect(Geom::Point(5, 6), Geom::Point(8, 10));
    {
        Geom::Affine g2d(gr->get_g2d_matrix(funny, larger_rect));
        Geom::Affine gs2d(gr->get_gs2d_matrix(funny, larger_rect));
        EXPECT_TRUE(Geom::are_near(Geom::Affine(3, 0,
                                                0, 4,
                                                5, 6) * funny, g2d ));
        EXPECT_TRUE(Geom::are_near(gs2d, gr->gradientTransform * g2d, 1e-12));

        gr->set_gs2d_matrix(funny, larger_rect, gs2d);
        EXPECT_TRUE(Geom::are_near(gr->gradientTransform, grXform, 1e-12));

        gr->setKeyValue( SPAttr::GRADIENTUNITS, "userSpaceOnUse");
        Geom::Affine user_g2d(gr->get_g2d_matrix(funny, larger_rect));
        Geom::Affine user_gs2d(gr->get_gs2d_matrix(funny, larger_rect));
        EXPECT_TRUE(Geom::are_near(funny, user_g2d));
        EXPECT_TRUE(Geom::are_near(user_gs2d, gr->gradientTransform * user_g2d, 1e-12));
    }
}

class MeshObjectTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Application::create(false);
        auto prefs = Preferences::get();
        rows = prefs->getInt("/tools/mesh/mesh_rows", 1);
        columns = prefs->getInt("/tools/mesh/mesh_cols", 1);
        geometry = prefs->getInt("/tools/mesh/mesh_geometry", 0);
        prefs->setInt("/tools/mesh/mesh_rows", 2);
        prefs->setInt("/tools/mesh/mesh_cols", 2);
        prefs->setInt("/tools/mesh/mesh_geometry", 0);
        constexpr std::string_view svg = R"svg(
            <svg xmlns="http://www.w3.org/2000/svg"
                 xmlns:proof="urn:x-proof:document:1"
                 xmlns:xlink="http://www.w3.org/1999/xlink"
                 xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
                 xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
                 width="400" height="300" viewBox="0 0 400 300">
              <defs><meshgradient id="mesh"/>
                <meshgradient id="inherited" xlink:href="#mesh" gradientUnits="userSpaceOnUse"
                              gradientTransform="translate(300,0)"/>
              </defs>
              <rect id="shape" x="20" y="40" width="120" height="90" fill="#cc2244"/>
              <path id="ordinary" d="M20,40 H140 V130 H20 Z" fill="url(#mesh)"/>
              <path id="inherited-shape" proof:mesh-object="true"
                    d="M320,40 H440 V130 H320 Z" fill="url(#inherited)"/>
              <path id="stroke" proof:mesh-object="true" d="M20,40 H140 V130 H20 Z"
                    fill="none" stroke="url(#mesh)"/>
              <path id="triangle" d="M200,50 L280,110 L180,140 Z" fill="#cc2244"/>
              <rect id="rounded" x="190" y="170" width="130" height="85" rx="20" fill="#cc2244"/>
              <path id="star" sodipodi:type="star" sodipodi:sides="5"
                    sodipodi:cx="220" sodipodi:cy="160" sodipodi:r1="60" sodipodi:r2="27"
                    sodipodi:arg1="0" sodipodi:arg2="0.628318530718"
                    inkscape:flatsided="false" inkscape:rounded="0" inkscape:randomized="0"
                    fill="#cc2244"/>
            </svg>)svg";
        doc = SPDocument::createNewDocFromMem({svg.data(), svg.size()}, "");
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        mesh = cast<SPMeshGradient>(doc->getObjectById("mesh"));
        auto shape = cast<SPItem>(doc->getObjectById("shape"));
        ASSERT_TRUE(mesh);
        mesh->array.create(mesh, shape, shape->geometricBounds());
        sp_style_set_property_url(shape, "fill", mesh, false);
        path = MeshObject::create(shape, mesh);
        ASSERT_TRUE(path);
        doc->ensureUpToDate();
        DocumentUndo::done(doc.get(), RC_("Undo", "Create mesh object"), "");
    }

    void TearDown() override
    {
        auto prefs = Preferences::get();
        prefs->setInt("/tools/mesh/mesh_rows", rows);
        prefs->setInt("/tools/mesh/mesh_cols", columns);
        prefs->setInt("/tools/mesh/mesh_geometry", geometry);
    }

    std::string outline() const { return path->getRepr()->attribute("d"); }
    SPPath *make(char const *id)
    {
        auto repr = doc->getReprDoc()->createElement("svg:meshgradient");
        doc->getDefs()->getRepr()->appendChild(repr);
        auto gradient = cast<SPMeshGradient>(doc->getObjectByRepr(repr));
        auto item = cast<SPItem>(doc->getObjectById(id));
        gradient->array.create(gradient, item, item->geometricBounds());
        sp_style_set_property_url(item, "fill", gradient, false);
        auto result = MeshObject::create(item, gradient);
        GC::release(repr);
        return result;
    }
    std::unique_ptr<SPDocument> doc;
    SPMeshGradient *mesh = nullptr;
    SPPath *path = nullptr;
    int rows = 1, columns = 1, geometry = 0;
};

TEST_F(MeshObjectTest, BoundaryMoveUpdatesOutlineInTheSameUndoStep)
{
    auto original = outline();
    mesh->array.nodes.front().back()->p += Geom::Point(50, -25);
    mesh->array.write(mesh);
    auto moved = outline();
    EXPECT_NE(original, moved);
    auto bounds = path->curve()->boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->right(), 190, 1e-6);
    EXPECT_NEAR(bounds->top(), 15, 1e-6);
    DocumentUndo::done(doc.get(), RC_("Undo", "Move mesh corner"), "");
    EXPECT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(outline(), original);
    EXPECT_TRUE(DocumentUndo::redo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(outline(), moved);
}

TEST_F(MeshObjectTest, AMeshEditRepairsAnOutlineRoundedOrMovedAwayFromItsMesh)
{
    // Saved 2026-10-18 before Direct Selection left mesh objects alone: Live
    // Corners rounded the outline, then its anchors moved; the mesh stayed.
    auto original = outline();
    EXPECT_TRUE(MeshObject::is_mesh_object(path));
    EXPECT_FALSE(MeshObject::is_mesh_object(doc->getObjectById("ordinary")));
    path->setAttribute("d", "M 30,50 C 40,30 120,30 130,50 V 120 H 30 Z");
    path->setAttribute("inkscape:live-corners", "1|M20,40 H140 V130 H20 Z|0,0,10,0,0;|1,0,0,1,0,0");
    path->setAttribute("sodipodi:nodetypes", "sssss");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), RC_("Undo", "Rounded by Direct Selection"), "");
    mesh->array.nodes.front().back()->p += Geom::Point(50, -25);
    mesh->array.write(mesh);
    EXPECT_NE(outline(), original);
    EXPECT_FALSE(path->getRepr()->attribute("inkscape:live-corners"));
    EXPECT_FALSE(path->getRepr()->attribute("sodipodi:nodetypes"));
    auto bounds = path->curve()->boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 20, 1e-6);
    EXPECT_NEAR(bounds->right(), 190, 1e-6);
    DocumentUndo::done(doc.get(), RC_("Undo", "Move mesh corner"), "");
    EXPECT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_TRUE(path->getRepr()->attribute("inkscape:live-corners"));
    EXPECT_STREQ(path->getRepr()->attribute("sodipodi:nodetypes"), "sssss");
}

TEST_F(MeshObjectTest, InnerGeometryAndColourDoNotRewriteTheBoundary)
{
    auto original = outline();
    mesh->array.nodes[3][3]->p += Geom::Point(11, 17);
    mesh->array.nodes[3][4]->p += Geom::Point(3, 8);
    mesh->array.nodes[3][3]->color = Colors::Color(0x00ff00ff);
    mesh->array.write(mesh);
    EXPECT_EQ(outline(), original);
}

TEST_F(MeshObjectTest, LegacyFillAndStrokeRemainClippedByTheirOwnPaths)
{
    auto ordinary = doc->getObjectById("ordinary")->getRepr();
    auto stroke = doc->getObjectById("stroke")->getRepr();
    std::string old_fill = ordinary->attribute("d");
    std::string old_stroke = stroke->attribute("d");
    mesh->array.nodes.front().back()->p += Geom::Point(50, -25);
    mesh->array.write(mesh);
    EXPECT_EQ(old_fill, ordinary->attribute("d"));
    EXPECT_EQ(old_stroke, stroke->attribute("d"));
}

TEST_F(MeshObjectTest, BoundaryHandleAndSubdivisionKeepTheirExactCurves)
{
    mesh->array.nodes[0][1]->p += Geom::Point(0, -45);
    mesh->array.write(mesh);
    auto curved = path->curve()->boundsExact();
    ASSERT_TRUE(curved);
    EXPECT_LT(curved->top(), 40);
    mesh->array.split_row(0, 0.5);
    mesh->array.split_column(0, 0.5);
    mesh->array.write(mesh);
    auto split = path->curve()->boundsExact();
    ASSERT_TRUE(split);
    EXPECT_NEAR(split->top(), curved->top(), 1e-6);
    EXPECT_NEAR(split->right(), curved->right(), 1e-6);
}

TEST_F(MeshObjectTest, PreviewDoesNotWriteXmlAndCommitWritesThePreview)
{
    auto original = outline();
    mesh->array.nodes.front().back()->p += Geom::Point(50, -25);
    EXPECT_TRUE(MeshObject::sync(path, mesh, false));
    EXPECT_EQ(outline(), original);
    EXPECT_NEAR(path->curve()->boundsExact()->right(), 190, 1e-6);
    mesh->array.write(mesh);
    EXPECT_NE(outline(), original);
    EXPECT_EQ(outline(), sp_svg_write_path(*path->curve()));
}

TEST_F(MeshObjectTest, GradientTransformIsAppliedInItemCoordinates)
{
    path->setAttribute("transform", "translate(30,40)");
    mesh->getRepr()->setAttribute("gradientTransform", "matrix(2,0,0,3,5,7)");
    mesh->array.write(mesh);
    auto bounds = path->curve()->boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 45, 1e-6);
    EXPECT_NEAR(bounds->top(), 127, 1e-6);
    EXPECT_NEAR(bounds->right(), 285, 1e-6);
    EXPECT_STREQ(path->getRepr()->attribute("transform"), "translate(30,40)");
}

TEST_F(MeshObjectTest, StarKeepsItsTenCornersWithoutAStrokedRadialSeam)
{
    auto star = cast<SPStar>(doc->getObjectById("star"));
    ASSERT_TRUE(star);
    auto original = *star->curve();
    auto converted = make("star");
    ASSERT_TRUE(converted);
    ASSERT_EQ(converted->curve()->size(), 1u);
    EXPECT_EQ(converted->curve()->front().size(), 10u);
    auto const &outline = converted->curve()->front();
    for (unsigned i = 0; i < original.front().size(); ++i) {
        EXPECT_TRUE(Geom::are_near(outline[i].initialPoint(), original.front()[i].initialPoint(), 1e-6));
    }
}

TEST_F(MeshObjectTest, GenericTriangleKeepsItsContourInsteadOfItsBox)
{
    auto converted = make("triangle");
    ASSERT_TRUE(converted);
    auto const &outline = converted->curve()->front();
    ASSERT_EQ(outline.size(), 3u);
    EXPECT_TRUE(Geom::are_near(outline[0].initialPoint(), Geom::Point(200, 50)));
    EXPECT_TRUE(Geom::are_near(outline[1].initialPoint(), Geom::Point(280, 110)));
    EXPECT_TRUE(Geom::are_near(outline[2].initialPoint(), Geom::Point(180, 140)));
}

TEST_F(MeshObjectTest, RadialInteriorChangesKeepTheOuterRingBoundary)
{
    auto converted = make("star");
    ASSERT_TRUE(converted);
    EXPECT_STREQ(converted->getRepr()->attribute(MeshObject::BOUNDARY_ATTRIBUTE), "outer-ring");
    std::string original = converted->getRepr()->attribute("d");
    auto gradient = cast<SPMeshGradient>(converted->style->getFillPaintServer());
    ASSERT_TRUE(gradient);
    // The coincident inner ring can separate during interior editing. That
    // must not turn the radial seam or inner handles into the outer contour.
    gradient->array.nodes.back()[1]->p += Geom::Point(4, 7);
    gradient->array.nodes.back()[3]->p += Geom::Point(9, 5);
    gradient->array.write(gradient);
    EXPECT_EQ(original, converted->getRepr()->attribute("d"));

    auto copied = doc->copy();
    copied->ensureUpToDate();
    auto reopened = cast<SPPath>(copied->getObjectById("star"));
    ASSERT_TRUE(reopened);
    EXPECT_STREQ(reopened->getRepr()->attribute(MeshObject::BOUNDARY_ATTRIBUTE), "outer-ring");
    auto reopened_mesh = cast<SPMeshGradient>(reopened->style->getFillPaintServer());
    ASSERT_TRUE(reopened_mesh);
    reopened_mesh->ensureArray();
    reopened_mesh->array.nodes.back()[1]->p += Geom::Point(3, 2);
    reopened_mesh->array.write(reopened_mesh);
    EXPECT_EQ(original, reopened->getRepr()->attribute("d"));
    reopened_mesh->array.nodes.front()[3]->p += Geom::Point(10, -10);
    reopened_mesh->array.write(reopened_mesh);
    EXPECT_NE(original, reopened->getRepr()->attribute("d"));
}

TEST_F(MeshObjectTest, RoundedRectangleKeepsItsCurvedBoundary)
{
    auto converted = make("rounded");
    ASSERT_TRUE(converted);
    auto const &outline = converted->curve()->front();
    EXPECT_GE(outline.size(), 8u);
    bool curved = false;
    for (auto const &segment : outline) {
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&segment)) {
            curved = curved || !Geom::are_near((*cubic)[1], cubic->initialPoint() +
                (cubic->finalPoint() - cubic->initialPoint()) / 3, 1e-6);
        }
    }
    EXPECT_TRUE(curved);
}

TEST_F(MeshObjectTest, SvgCopyRetainsTheMarkerAndRemainsEditable)
{
    auto copied = doc->copy();
    copied->ensureUpToDate();
    auto copy_path = cast<SPPath>(copied->getObjectById("shape"));
    auto copy_mesh = cast<SPMeshGradient>(copied->getObjectById("mesh"));
    ASSERT_TRUE(copy_path);
    ASSERT_TRUE(copy_mesh);
    EXPECT_STREQ(copy_path->getRepr()->attribute(MeshObject::ATTRIBUTE), "true");
    copy_mesh->ensureArray();
    copy_mesh->array.nodes.front().back()->p += Geom::Point(50, -25);
    copy_mesh->array.write(copy_mesh);
    EXPECT_NEAR(copy_path->curve()->boundsExact()->right(), 190, 1e-6);
    EXPECT_NEAR(path->curve()->boundsExact()->right(), 140, 1e-6);
}

TEST_F(MeshObjectTest, ReferencedMeshUsesItsOwnTransformAndFreshSourceGeometry)
{
    auto inherited = cast<SPPath>(doc->getObjectById("inherited-shape"));
    ASSERT_TRUE(inherited);
    mesh->array.nodes.front().back()->p += Geom::Point(50, -25);
    mesh->array.write(mesh);
    EXPECT_NEAR(inherited->curve()->boundsExact()->right(), 490, 1e-6);
    EXPECT_NEAR(inherited->curve()->boundsExact()->top(), 15, 1e-6);
}

// PROOF: a CMYK document's gradients are drawn blended in CMYK, as Illustrator blends them
// (object/gradient-rendering.h).
namespace {

std::unique_ptr<SPDocument> blend_document(char const *mode, char const *stops, char const *extra = "")
{
    auto const svg = std::string(R"S(<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" )S"
                                 R"S(width="100" height="10" proof:color-mode=")S") + mode + R"S(">)S" +
                     R"S(<defs><linearGradient id="g" x1="0" x2="100" y1="0" y2="0" gradientUnits="userSpaceOnUse" )S" +
                     extra + ">" + stops + R"S(</linearGradient></defs>)S" +
                     R"S(<rect width="100" height="10" fill="url(#g)"/></svg>)S";
    auto document = SPDocument::createNewDocFromMem(svg);
    if (document) document->ensureUpToDate();
    return document;
}

SPGradient *blend_gradient(SPDocument &document)
{
    return cast<SPGradient>(document.getObjectById("g"));
}

constexpr auto PROCESS_STOPS = R"S(<stop offset="0" style="stop-color:device-cmyk(1 0 0 0);stop-opacity:1"/>)S"
                               R"S(<stop offset="1" style="stop-color:device-cmyk(0 0 1 0.2);stop-opacity:0.5"/>)S";

} // namespace

class GradientRenderingTest : public DocPerCaseTest {};

TEST_F(GradientRenderingTest, AnRgbDocumentsGradientIsDrawnFromItsOwnStops)
{
    auto document = blend_document("RGB", PROCESS_STOPS);
    ASSERT_TRUE(document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(blend_gradient(*document));
    ASSERT_EQ(vector.stops.size(), 2u);
    EXPECT_EQ(vector.stops[0].color->getSpace()->getComponentType(), Colors::Space::Type::CMYK);
}

TEST_F(GradientRenderingTest, ACmykDocumentsGradientIsBlendedInCmyk)
{
    auto document = blend_document("CMYK", PROCESS_STOPS);
    ASSERT_TRUE(document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(blend_gradient(*document));
    // A whole channel's step gets the most samples.
    ASSERT_EQ(vector.stops.size(), 65u);
    double last = -1;
    for (auto const &stop : vector.stops) {
        ASSERT_TRUE(stop.color);
        EXPECT_EQ(stop.color->getSpace()->getComponentType(), Colors::Space::Type::CMYK);
        EXPECT_GT(stop.offset, last);
        last = stop.offset;
    }
    // The ends keep their inks; between them every channel and the opacity move in step.
    auto const &first = vector.stops.front().color->getValues();
    EXPECT_NEAR(first[0], 1, 1e-9);
    EXPECT_NEAR(first[3], 0, 1e-9);
    EXPECT_NEAR(vector.stops.back().color->getOpacity(), 0.5, 1e-9);
    auto const &middle = vector.stops[32];
    EXPECT_NEAR(middle.offset, 0.5, 1e-9);
    auto const &m = middle.color->getValues();
    EXPECT_NEAR(m[0], 0.5, 1e-9);
    EXPECT_NEAR(m[1], 0, 1e-9);
    EXPECT_NEAR(m[2], 0.5, 1e-9);
    EXPECT_NEAR(m[3], 0.1, 1e-9);
    EXPECT_NEAR(middle.color->getOpacity(), 0.75, 1e-9);
}

TEST_F(GradientRenderingTest, ASmallStepGetsFewerSamplesAndAHardEdgeNone)
{
    auto document = blend_document("CMYK",
        R"S(<stop offset="0" style="stop-color:device-cmyk(0 0.5 0 0)"/>)S"
        R"S(<stop offset="0.5" style="stop-color:device-cmyk(0 0.6 0 0)"/>)S"
        R"S(<stop offset="0.5" style="stop-color:device-cmyk(1 0 0 0)"/>)S"
        R"S(<stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/>)S");
    ASSERT_TRUE(document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(blend_gradient(*document));
    // 0.1 of magenta: seven steps, six samples; none at the edge; none between equal stops.
    EXPECT_EQ(vector.stops.size(), 4u + 6u);
    EXPECT_NEAR(vector.stops[7].offset, 0.5, 1e-9);
    EXPECT_NEAR(vector.stops[8].offset, 0.5, 1e-9);
}

TEST_F(GradientRenderingTest, AnRgbStopInACmykDocumentIsBlendedAsTheDocumentConvertsIt)
{
    auto document = blend_document("CMYK",
        R"S(<stop offset="0" style="stop-color:#c8503c"/>)S"
        R"S(<stop offset="1" style="stop-color:device-cmyk(0 0 0 1)"/>)S");
    ASSERT_TRUE(document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(blend_gradient(*document));
    ASSERT_GT(vector.stops.size(), 2u);
    auto const expected = Colors::Color::parse("#c8503c")->converted(Colors::Space::Type::CMYK);
    ASSERT_TRUE(expected);
    auto const &first = vector.stops.front().color;
    EXPECT_EQ(first->getSpace()->getComponentType(), Colors::Space::Type::CMYK);
    for (unsigned k = 0; k < 4; ++k) {
        EXPECT_NEAR(first->getValues()[k], expected->getValues()[k], 1e-6);
    }
}

TEST_F(GradientRenderingTest, APerceptualGradientKeepsItsOklabSamples)
{
    auto document = blend_document("CMYK", PROCESS_STOPS, R"S(proof:gradient-method="oklab")S");
    ASSERT_TRUE(document);
    auto const gradient = blend_gradient(*document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(gradient);
    auto const oklab = Inkscape::PerceptualGradient::rendering_vector(gradient);
    ASSERT_EQ(vector.stops.size(), oklab.stops.size());
    for (std::size_t i = 0; i < vector.stops.size(); ++i) {
        EXPECT_EQ(vector.stops[i].offset, oklab.stops[i].offset);
    }
}

namespace {

/// One patch from (10, 20), 80 wide and 40 high, its corners cyan, magenta, yellow and black, or
/// with a curved top side.
std::unique_ptr<SPDocument> mesh_document(char const *mode, bool curved = false)
{
    auto const top = curved ? "C 20,-15 60,15 80,0" : "l 80,0";
    auto const svg = std::string(R"S(<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" )S"
                                 R"S(width="100" height="100" proof:color-mode=")S") + mode + R"S("><defs>)S" +
                     R"S(<meshgradient id="m" x="10" y="20" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>)S" +
                     R"S(<stop path=")S" + top + R"S(" style="stop-color:device-cmyk(1 0 0 0)"/>)S" +
                     R"S(<stop path="l 0,40" style="stop-color:device-cmyk(0 1 0 0)"/>)S" +
                     R"S(<stop path="l -80,0" style="stop-color:device-cmyk(0 0 1 0);stop-opacity:0.5"/>)S" +
                     R"S(<stop path="l 0,-40" style="stop-color:device-cmyk(0 0 0 1)"/>)S" +
                     R"S(</meshpatch></meshrow></meshgradient></defs>)S" +
                     R"S(<rect x="10" y="20" width="80" height="40" fill="url(#m)"/></svg>)S";
    auto document = SPDocument::createNewDocFromMem(svg);
    if (document) document->ensureUpToDate();
    return document;
}

SPGradientMesh const &mesh_of(SPDocument &document)
{
    return *cast<SPGradient>(document.getObjectById("m"))->getGradientMesh();
}

std::vector<SPGradientPatch> all_patches(SPGradientMesh const &mesh)
{
    std::vector<SPGradientPatch> all;
    for (auto const &row : mesh.patches) all.insert(all.end(), row.begin(), row.end());
    return all;
}

} // namespace

TEST_F(GradientRenderingTest, AnRgbDocumentsMeshKeepsItsPatches)
{
    auto document = mesh_document("RGB");
    ASSERT_TRUE(document);
    auto const &mesh = mesh_of(*document);
    auto const drawn = Inkscape::GradientRendering::canvas_mesh(document.get(), mesh);
    EXPECT_EQ(all_patches(drawn).size(), all_patches(mesh).size());
}

TEST_F(GradientRenderingTest, ACmykDocumentsMeshPatchIsCutAndBlendedInCmyk)
{
    auto document = mesh_document("CMYK");
    ASSERT_TRUE(document);
    auto const &mesh = mesh_of(*document);
    ASSERT_EQ(all_patches(mesh).size(), 1u);
    auto const patches = all_patches(Inkscape::GradientRendering::canvas_mesh(document.get(), mesh));
    // Corners a whole channel apart: eight cuts a side.
    ASSERT_EQ(patches.size(), 64u);
    // Row by row from C0's side; each piece's corners lie on the patch, its inner points those
    // of a flat patch (a third of the way in), and every corner is a CMYK colour.
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned j = 0; j < 8; ++j) {
            auto const &piece = patches[i * 8 + j];
            Geom::Point const c0(10 + 10.0 * j, 20 + 5.0 * i);
            EXPECT_TRUE(Geom::are_near(piece.points[0][0], c0, 1e-9)) << i << " " << j;
            EXPECT_TRUE(Geom::are_near(piece.points[1][0], c0 + Geom::Point(10, 0), 1e-9));
            EXPECT_TRUE(Geom::are_near(piece.points[2][0], c0 + Geom::Point(10, 5), 1e-9));
            EXPECT_TRUE(Geom::are_near(piece.points[3][0], c0 + Geom::Point(0, 5), 1e-9));
            EXPECT_TRUE(Geom::are_near(piece.tensorpoints[0], c0 + Geom::Point(10.0 / 3, 5.0 / 3), 1e-9));
            for (auto const &color : piece.color) {
                ASSERT_TRUE(color);
                EXPECT_EQ(color->getSpace()->getComponentType(), Colors::Space::Type::CMYK);
            }
        }
    }
    // The middle of the patch is the four inks blended evenly, the opacity too.
    auto const &middle = patches[4 * 8 + 4].color[0];
    for (unsigned c = 0; c < 4; ++c) {
        EXPECT_NEAR(middle->getValues()[c], 0.25, 1e-9);
    }
    EXPECT_NEAR(middle->getOpacity(), 0.875, 1e-9);
    // C2 (yellow, half opaque) is the far corner of the last piece.
    auto const &far = patches.back().color[2];
    EXPECT_NEAR(far->getValues()[2], 1, 1e-9);
    EXPECT_NEAR(far->getOpacity(), 0.5, 1e-9);
}

TEST_F(GradientRenderingTest, ACutMeshKeepsItsCurvedSides)
{
    auto document = mesh_document("CMYK", true);
    ASSERT_TRUE(document);
    auto const &mesh = mesh_of(*document);
    auto const original = all_patches(mesh).front();
    auto const patches = all_patches(Inkscape::GradientRendering::canvas_mesh(document.get(), mesh));
    ASSERT_EQ(patches.size(), 64u);
    // The top side, cut in eight, traces the original curve: the pieces' top sides join at the
    // curve's points at eighths, and the first piece's ends where the original's eighth is.
    auto const &side = original.points[0];
    auto const at = [&side](double t) {
        double const u = 1 - t;
        return side[0] * (u * u * u) + side[1] * (3 * u * u * t) + side[2] * (3 * u * t * t) + side[3] * (t * t * t);
    };
    for (unsigned j = 0; j < 8; ++j) {
        EXPECT_TRUE(Geom::are_near(patches[j].points[0][0], at(j / 8.0), 1e-9)) << j;
        EXPECT_TRUE(Geom::are_near(patches[j].points[0][3], at((j + 1) / 8.0), 1e-9)) << j;
    }
}

namespace {

std::string ink_stop(char const *offset, char const *name, char const *alternate, char const *tint = nullptr)
{
    return std::string(R"S(<stop offset=")S") + offset + R"S(" style="stop-color:)S" + alternate +
           R"S(" proof:ink=")S" + name + R"S(" proof:ink-alternate=")S" + alternate + R"S(")S" +
           (tint ? std::string(R"S( proof:ink-tint=")S") + tint + R"S(")S" : std::string()) + "/>";
}

/// The sample of the vector at this offset.
Colors::Color const &at(SPGradientVector const &vector, double offset)
{
    for (auto const &stop : vector.stops) {
        if (std::abs(stop.offset - offset) < 1e-6) return *stop.color; // Stop offsets are floats.
    }
    throw std::runtime_error("no sample at that offset");
}

} // namespace

TEST_F(GradientRenderingTest, AnInkBesideProcessOverprintsItInACmykDocument)
{
    auto const stops = ink_stop("0", "Leaf", "device-cmyk(0.306 0 0.6 0)") +
                       R"S(<stop offset="1" style="stop-color:device-cmyk(0 0.666 0.743 0.321)"/>)S";
    auto document = blend_document("CMYK", stops.c_str());
    ASSERT_TRUE(document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(blend_gradient(*document));
    ASSERT_EQ(vector.stops.size(), 65u);
    // Halfway: the ink at half its tint over half the process colour, each channel
    // 1 - (1 - 0.5 x ink)(1 - 0.5 x process), as Illustrator's DeviceN tint transform has it.
    auto const &middle = at(vector, 0.5);
    EXPECT_EQ(middle.getSpace()->getComponentType(), Colors::Space::Type::CMYK);
    auto const &v = middle.getValues();
    EXPECT_NEAR(v[0], 0.153, 1e-6);
    EXPECT_NEAR(v[1], 0.333, 1e-6);
    EXPECT_NEAR(v[2], 1 - (1 - 0.3) * (1 - 0.3715), 1e-6); // 0.560, where a blend has 0.672.
    EXPECT_NEAR(v[3], 0.1605, 1e-6);
    // The ends are the ink at its tint and the process colour.
    EXPECT_NEAR(at(vector, 0).getValues()[2], 0.6, 1e-6);
    EXPECT_NEAR(at(vector, 1).getValues()[1], 0.666, 1e-6);
}

TEST_F(GradientRenderingTest, AnInkBesideProcessMultipliesOverItInAnRgbDocument)
{
    auto const stops = ink_stop("0", "Green", "#33cc66") +
                       R"S(<stop offset="1" style="stop-color:#808080;stop-opacity:0.5"/>)S";
    auto document = blend_document("RGB", stops.c_str());
    ASSERT_TRUE(document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(blend_gradient(*document));
    ASSERT_EQ(vector.stops.size(), 65u);
    // The ink's tint ramp multiplied over the process ramp, each against white.
    auto const &middle = at(vector, 0.5);
    double const grey = 128 / 255.0;
    double const expected[3] = {0.2, 0.8, 0.4};
    for (unsigned j = 0; j < 3; ++j) {
        double const ink = 1 - 0.5 * (1 - expected[j]), process = 1 - 0.5 * (1 - grey);
        EXPECT_NEAR(middle.getValues()[j], ink * process, 1e-6) << j;
    }
    EXPECT_NEAR(middle.getOpacity(), 0.75, 1e-9);
}

TEST_F(GradientRenderingTest, OneInksTintsAreBlendedAsBefore)
{
    // No process colour and no second ink: nothing overprints.
    auto const stops = ink_stop("0", "Leaf", "device-cmyk(0.306 0 0.6 0)") +
                       ink_stop("1", "Leaf", "device-cmyk(0.153 0 0.3 0)", "0.5");
    auto rgb = blend_document("RGB", stops.c_str());
    ASSERT_TRUE(rgb);
    EXPECT_EQ(Inkscape::GradientRendering::canvas_vector(blend_gradient(*rgb)).stops.size(), 2u);
}

TEST_F(GradientRenderingTest, InksAreFoundWhenTheStopsDoNotReachTheEnds)
{
    // The vector adds a copy of the first stop at 0 and of the last at 1.
    auto const stops = ink_stop("0.2", "Leaf", "device-cmyk(0.306 0 0.6 0)") +
                       R"S(<stop offset="0.8" style="stop-color:device-cmyk(0 1 0 0)"/>)S";
    auto document = blend_document("CMYK", stops.c_str());
    ASSERT_TRUE(document);
    auto const vector = Inkscape::GradientRendering::canvas_vector(blend_gradient(*document));
    // Overprinted only between the two stops: 0 to 0.2 and 0.8 to 1 are flat.
    auto const &middle = at(vector, 0.5).getValues();
    EXPECT_NEAR(middle[1], 0.5, 1e-6);
    EXPECT_NEAR(middle[2], 0.3, 1e-6);
    EXPECT_NEAR(at(vector, 0).getValues()[2], 0.6, 1e-6);
    EXPECT_NEAR(at(vector, 1).getValues()[1], 1, 1e-6);
}

TEST_F(GradientRenderingTest, PdfOutputDrawsAnInkBesideProcessAsTheCanvasDoes)
{
    // An ink beside process colour: the same overprinted samples, so the page looks as the
    // canvas does (in process colour; the ink's own plate is the separations work).
    auto const stops = ink_stop("0", "Leaf", "device-cmyk(0.306 0 0.6 0)") +
                       R"S(<stop offset="1" style="stop-color:device-cmyk(0 0.666 0.743 0.321)"/>)S";
    auto document = blend_document("CMYK", stops.c_str());
    ASSERT_TRUE(document);
    auto const gradient = blend_gradient(*document);
    auto const pdf = Inkscape::GradientRendering::pdf_vector(gradient);
    auto const canvas = Inkscape::GradientRendering::canvas_vector(gradient);
    ASSERT_EQ(pdf.stops.size(), canvas.stops.size());
    EXPECT_NEAR(at(pdf, 0.5).getValues()[2], at(canvas, 0.5).getValues()[2], 1e-12);

    // Process stops alone: the stops themselves; a CMYK shading blends in CMYK by itself.
    auto plain = blend_document("CMYK", PROCESS_STOPS);
    ASSERT_TRUE(plain);
    EXPECT_EQ(Inkscape::GradientRendering::pdf_vector(blend_gradient(*plain)).stops.size(), 2u);
}
