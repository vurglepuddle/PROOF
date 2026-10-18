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
