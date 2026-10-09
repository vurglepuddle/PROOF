// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: bounding boxes that keep the angle of rotated objects, as in Illustrator (box-frame.h).
 */
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <2geom/angle.h>
#include <2geom/transforms.h>

#include "box-frame.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "preferences.h"
#include "selection.h"
#include "util/transform-objects.h"
#include "xml/node.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

constexpr double EPSILON = 1e-6;

constexpr auto SVG = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400">
  <rect id="rect" x="10" y="10" width="100" height="50" transform="rotate(30 60 35)"/>
  <rect id="rect2" x="10" y="100" width="40" height="40" transform="rotate(120 30 120)"/>
  <path id="path" d="M 200,200 h 100 v 40 h -100 z"/>
  <path id="path2" d="M 200,300 h 60 v 20 h -60 z"/>
  <path id="triangle" d="M 200,140 h 100 v 40 z" transform="rotate(30 250 160)"/>
  <path id="stroked" d="M 20,180 h 100 v 40 z" transform="rotate(30 70 200)"
        fill="none" stroke="blue" stroke-width="20"/>
  <ellipse id="ellipse" cx="300" cy="80" rx="40" ry="20"/>
  <g id="group" transform="rotate(30)">
    <path id="g1" d="M 0,0 h 10 v 10 h -10 z"/>
    <path id="g2" d="M 20,0 h 10 v 10 h -10 z"/>
  </g>
  <g id="shapes">
    <rect id="s-rect" x="0" y="0" width="20" height="10" transform="translate(100 300)"/>
    <ellipse id="s-ellipse" cx="150" cy="350" rx="10" ry="5"/>
    <path id="s-path" d="M 0,0 h 5 v 5 z"/>
  </g>
</svg>)SVG"sv;

class BoxFrameTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Application::create(false);
        doc = SPDocument::createNewDocFromMem(SVG);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        previous_bbox_type = Preferences::get()->getInt("/tools/bounding_box");
        Preferences::get()->setInt("/tools/bounding_box", 1);
    }

    void TearDown() override
    {
        Preferences::get()->setInt("/tools/bounding_box", previous_bbox_type);
    }

    SPItem *item(char const *id) { return cast<SPItem>(doc->getObjectById(id)); }

    double degrees(std::vector<SPItem *> const &items) { return Geom::deg_from_rad(box_angle(items)); }

    /// Turn an object about a point, as the selection tool does.
    void rotate(SPItem &object, double degrees, Geom::Point const &center)
    {
        apply(object, Geom::Rotate(Geom::rad_from_deg(degrees)), center);
    }

    /// Apply a transform about a point in desktop coordinates, and write it as the editor does.
    void apply(SPItem &object, Geom::Affine const &affine, Geom::Point const &center)
    {
        object.set_i2d_affine(object.i2dt_affine() * Geom::Translate(-center) * affine * Geom::Translate(center));
        object.doWriteTransform(object.transform);
        doc->ensureUpToDate();
    }

    std::unique_ptr<SPDocument> doc;
    int previous_bbox_type = 0;
};

} // namespace

TEST_F(BoxFrameTest, ShapesFollowTheirTransform)
{
    EXPECT_NEAR(degrees({item("rect")}), 30, EPSILON);
    EXPECT_NEAR(degrees({item("ellipse")}), 0, EPSILON);
    // A box repeats every quarter turn.
    EXPECT_NEAR(degrees({item("rect2")}), 30, EPSILON);
}

TEST_F(BoxFrameTest, PathsRememberTheirAngle)
{
    auto path = item("path");
    rotate(*path, 30, {250, 220});

    // The path took the rotation into its nodes and keeps the angle instead.
    EXPECT_FALSE(path->getRepr()->attribute("transform"));
    EXPECT_NEAR(path->getRepr()->getAttributeDouble(BOX_ANGLE_ATTRIBUTE), 30, EPSILON);
    EXPECT_NEAR(degrees({path}), 30, EPSILON);

    // In the frame of its box the path is as wide and tall as it was drawn.
    auto const bounds = frame_bounds({path}, box_angle({path}), SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 100, 1e-4);
    EXPECT_NEAR(bounds->height(), 40, 1e-4);

    // Turning again adds up. The box repeats every quarter turn, but the angle is kept in full,
    // so a quarter turn reads 90 degrees, as in Illustrator.
    rotate(*path, 30, {250, 220});
    EXPECT_NEAR(degrees({path}), -30, EPSILON);
    rotate(*path, 30, {250, 220});
    EXPECT_NEAR(degrees({path}), 0, EPSILON);
    EXPECT_NEAR(path->getRepr()->getAttributeDouble(BOX_ANGLE_ATTRIBUTE), 90, EPSILON);
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({path})), 90, EPSILON);
    rotate(*path, -90, {250, 220});
    EXPECT_FALSE(path->getRepr()->attribute(BOX_ANGLE_ATTRIBUTE));
}

TEST_F(BoxFrameTest, TheAngleIsShownInFull)
{
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({item("rect")})), 30, EPSILON);
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({item("rect2")})), 120, EPSILON);
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({item("ellipse")})), 0, EPSILON);
    // Objects sharing a box show the first one's angle; different boxes show none.
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({item("rect2"), item("rect")})), 120, EPSILON);
    EXPECT_FALSE(box_rotation({item("rect"), item("ellipse")}));

    // Flipping mirrors the angle: an upright object stays upright, one at 30 degrees reads -30.
    auto path = item("path");
    apply(*path, Geom::Scale(-1, 1), {250, 220});
    EXPECT_FALSE(path->getRepr()->attribute(BOX_ANGLE_ATTRIBUTE));
    rotate(*path, 30, {250, 220});
    apply(*path, Geom::Scale(-1, 1), {250, 220});
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({path})), -30, EPSILON);
    EXPECT_NEAR(degrees({path}), -30, EPSILON);
}

TEST_F(BoxFrameTest, BoxMiddle)
{
    // The middle of a turned rectangle's box is the rectangle's own centre.
    auto middle = box_middle({item("rect")}, SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(middle);
    EXPECT_TRUE(Geom::are_near(*middle, Geom::Point(60, 35), 1e-6));
}

TEST_F(BoxFrameTest, MovesAndScalesAlongTheBoxKeepTheAngle)
{
    auto path = item("path");
    rotate(*path, 30, {250, 220});

    apply(*path, Geom::Translate(40, -10), {0, 0});
    EXPECT_NEAR(degrees({path}), 30, EPSILON);

    // Stretching along the box, as the turned transform box does.
    Geom::Rotate const frame(Geom::rad_from_deg(30));
    apply(*path, frame.inverse() * Geom::Scale(2, 0.5) * frame, {290, 210});
    EXPECT_NEAR(degrees({path}), 30, EPSILON);
    auto const bounds = frame_bounds({path}, box_angle({path}), SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 200, 1e-4);
    EXPECT_NEAR(bounds->height(), 20, 1e-4);

    // Stretching across the box shears it: it is no longer a rectangle, so it has no angle.
    apply(*path, Geom::Scale(2, 1), {290, 210});
    EXPECT_FALSE(path->getRepr()->attribute(BOX_ANGLE_ATTRIBUTE));
    EXPECT_NEAR(degrees({path}), 0, EPSILON);
}

TEST_F(BoxFrameTest, SeveralObjectsShareABoxOnlyWithTheSameAngle)
{
    auto path = item("path");
    rotate(*path, 120, {250, 220});

    EXPECT_NEAR(degrees({item("rect"), path}), 30, EPSILON);
    EXPECT_NEAR(degrees({item("rect"), item("rect2")}), 30, EPSILON);
    EXPECT_NEAR(degrees({item("rect"), item("ellipse")}), 0, EPSILON);
    EXPECT_NEAR(degrees({item("ellipse"), item("rect")}), 0, EPSILON);
}

TEST_F(BoxFrameTest, GroupsTurnWithTheirObjects)
{
    auto group = cast<SPGroup>(item("group"));
    EXPECT_NEAR(degrees({group}), 30, EPSILON);

    // Ungrouping takes the group's rotation into the paths, which keep the angle.
    std::vector<SPItem *> children;
    sp_item_group_ungroup(group, children);
    doc->ensureUpToDate();
    ASSERT_EQ(children.size(), 2u);
    for (auto child : children) {
        EXPECT_FALSE(child->getRepr()->attribute("transform"));
        EXPECT_NEAR(degrees({child}), 30, EPSILON);
    }
    EXPECT_NEAR(degrees(children), 30, EPSILON);

    // One object turned differently and the shared box is upright.
    rotate(*children[0], 10, {0, 0});
    EXPECT_NEAR(degrees(children), 0, EPSILON);
}

TEST_F(BoxFrameTest, ResetBoundingBoxMakesItUprightWithoutMoving)
{
    auto rect = item("rect");
    auto const transform = rect->transform;
    EXPECT_TRUE(reset_box_angle(*rect));
    EXPECT_NEAR(degrees({rect}), 0, EPSILON);
    EXPECT_TRUE(Geom::are_near(rect->transform, transform, EPSILON));
    EXPECT_FALSE(reset_box_angle(*rect)); // Nothing left to reset.

    // The reset box turns with the object again, as Illustrator's does.
    rotate(*rect, 10, {60, 35});
    EXPECT_NEAR(degrees({rect}), 10, EPSILON);

    auto group = item("group");
    EXPECT_TRUE(reset_box_angle(*group));
    EXPECT_NEAR(degrees({group}), 0, EPSILON);
}

TEST_F(BoxFrameTest, ShapeCentersInGroups)
{
    auto const centers = shape_centers({item("shapes")}, 10);
    ASSERT_EQ(centers.size(), 2u); // Not the path.
    EXPECT_TRUE(Geom::are_near(centers[0], Geom::Point(110, 305), EPSILON));
    EXPECT_TRUE(Geom::are_near(centers[1], Geom::Point(150, 350), EPSILON));
    EXPECT_TRUE(shape_centers({item("shapes")}, 1).empty()); // Over the limit.
}

TEST_F(BoxFrameTest, KeyboardRotationKeepsTheTurnedBoxCentreAndUndoes)
{
    Selection selection(doc.get());
    selection.set(item("triangle"));
    auto const before = item("triangle")->getRepr()->attribute("d");
    std::string const original_path = before;
    Geom::Point const pivot(250, 160);
    ASSERT_TRUE(Geom::are_near(*box_middle(selection.items_vector(), SPItem::GEOMETRIC_BBOX), pivot, EPSILON));
    // The upright box's middle differs for a lopsided path, so the old pivot drifts.
    ASSERT_GT(Geom::L2(selection.preferredBounds()->midpoint() - pivot), 1);

    DocumentUndo::setUndoSensitive(doc.get(), true);
    for (int i = 0; i < 8; ++i) {
        selection.rotateAnchored(15);
        doc->ensureUpToDate();
        EXPECT_TRUE(Geom::are_near(*box_middle(selection.items_vector(), SPItem::GEOMETRIC_BBOX), pivot, 1e-4));
    }
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation(selection.items_vector())), 150, 1e-4);
    // Repeated keyboard turns remain one undo operation.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(item("triangle")->getRepr()->attribute("d"), original_path);
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({item("triangle")})), 30, EPSILON);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({item("triangle")})), 150, 1e-4);
    EXPECT_TRUE(Geom::are_near(*box_middle({item("triangle")}, SPItem::GEOMETRIC_BBOX), pivot, 1e-4));
}

TEST_F(BoxFrameTest, KeyboardRotationHonoursAnExplicitCentre)
{
    auto rect = item("rect");
    Geom::Point const pivot(20, 25);
    rect->setCenter(pivot);
    rect->updateRepr();
    auto const before = shape_centers({rect}, 1).front();
    Selection selection(doc.get());
    selection.set(rect);
    selection.rotateAnchored(15);
    doc->ensureUpToDate();

    auto const expected = before * Geom::Translate(-pivot) * Geom::Rotate::from_degrees(15) * Geom::Translate(pivot);
    EXPECT_TRUE(Geom::are_near(shape_centers({rect}, 1).front(), expected, EPSILON));
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({rect})), 45, EPSILON);
    EXPECT_TRUE(Geom::are_near(rect->getCenter(), pivot, EPSILON));
}

TEST_F(BoxFrameTest, KeyboardRotationKeepsItsAnchor)
{
    auto rect = item("rect");
    Selection selection(doc.get());
    selection.set(rect);
    selection.setAnchor(0, 0);
    auto const pivot = selection.visualBounds()->min();
    auto const before = shape_centers({rect}, 1).front();
    DocumentUndo::setUndoSensitive(doc.get(), true);
    selection.rotateAnchored(15);
    selection.rotateAnchored(15);
    doc->ensureUpToDate();

    auto const expected = before * Geom::Translate(-pivot) * Geom::Rotate::from_degrees(30) * Geom::Translate(pivot);
    EXPECT_TRUE(Geom::are_near(shape_centers({rect}, 1).front(), expected, EPSILON));
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({rect})), 60, EPSILON);
}

TEST_F(BoxFrameTest, TransformRotationKeepsTheSharedTurnedBoxCentre)
{
    Selection selection(doc.get());
    selection.set(item("triangle"));
    selection.add(item("rect"));
    auto const pivot = box_middle(selection.items_vector(), SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(pivot);
    transform_rotate(&selection, 15, false);
    doc->ensureUpToDate();

    EXPECT_TRUE(Geom::are_near(*box_middle(selection.items_vector(), SPItem::GEOMETRIC_BBOX), *pivot, 1e-4));
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation(selection.items_vector())), 45, EPSILON);
}

TEST_F(BoxFrameTest, SeparateTransformRotationKeepsEachObjectsCentre)
{
    Selection selection(doc.get());
    auto triangle = item("triangle");
    auto rect = item("rect2");
    selection.set(triangle);
    selection.add(rect);
    Geom::Point const triangle_pivot(250, 160);
    Geom::Point const rect_pivot(30, 120);
    transform_rotate(&selection, 15, true);
    doc->ensureUpToDate();

    EXPECT_TRUE(Geom::are_near(*box_middle({triangle}, SPItem::GEOMETRIC_BBOX), triangle_pivot, 1e-4));
    EXPECT_TRUE(Geom::are_near(*box_middle({rect}, SPItem::GEOMETRIC_BBOX), rect_pivot, EPSILON));
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({triangle})), 45, EPSILON);
    EXPECT_NEAR(Geom::deg_from_rad(*box_rotation({rect})), 135, EPSILON);
    EXPECT_EQ(selection.size(), 2u);
}

TEST_F(BoxFrameTest, TransformRotationHonoursExplicitCentresTogetherAndSeparately)
{
    Selection selection(doc.get());
    auto rect = item("rect");
    Geom::Point const pivot(20, 25);
    rect->setCenter(pivot);
    rect->updateRepr();
    selection.set(item("triangle"));
    selection.add(rect); // The pivot object matches ObjectSet::center() and the canvas box.
    ASSERT_TRUE(Geom::are_near(*selection.center(), pivot, EPSILON));
    auto const before = shape_centers({rect}, 1).front();
    transform_rotate(&selection, 15, false);
    doc->ensureUpToDate();
    auto const expected = before * Geom::Translate(-pivot) * Geom::Rotate::from_degrees(15) * Geom::Translate(pivot);
    EXPECT_TRUE(Geom::are_near(shape_centers({rect}, 1).front(), expected, EPSILON));
    EXPECT_TRUE(Geom::are_near(rect->getCenter(), pivot, EPSILON));

    transform_rotate(&selection, -15, true);
    doc->ensureUpToDate();
    EXPECT_TRUE(Geom::are_near(shape_centers({rect}, 1).front(), before, EPSILON));
    EXPECT_TRUE(Geom::are_near(rect->getCenter(), pivot, EPSILON));
}

TEST_F(BoxFrameTest, MixedAnglesUseTheUprightBoxCentre)
{
    Selection selection(doc.get());
    selection.set(item("triangle"));
    selection.add(item("ellipse"));
    ASSERT_NEAR(box_angle(selection.items_vector()), 0, EPSILON);
    auto const pivot = box_rotation_center(selection.items_vector(), SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(pivot);
    EXPECT_TRUE(Geom::are_near(*pivot, selection.preferredBounds()->midpoint(), EPSILON));
}

TEST_F(BoxFrameTest, RotationFollowsThePreferredBoundsAndHandlesEmptySelections)
{
    Selection selection(doc.get());
    EXPECT_FALSE(box_rotation_center(selection.items_vector(), SPItem::GEOMETRIC_BBOX));
    selection.rotateAnchored(15);
    transform_rotate(&selection, 15, true);
    transform_rotate(&selection, 15, false);
    selection.set(item("stroked"));
    auto const geometric = box_middle(selection.items_vector(), SPItem::GEOMETRIC_BBOX);
    auto const visual = box_middle(selection.items_vector(), SPItem::VISUAL_BBOX);
    ASSERT_TRUE(geometric);
    ASSERT_TRUE(visual);
    ASSERT_GT(Geom::L2(*visual - *geometric), 1);
    Preferences::get()->setInt("/tools/bounding_box", 0);
    transform_rotate(&selection, 15, false);
    doc->ensureUpToDate();
    EXPECT_TRUE(Geom::are_near(*box_middle(selection.items_vector(), SPItem::VISUAL_BBOX), *visual, 1e-4));
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
