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
#include "object/sp-shape.h"
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

TEST_F(BoxFrameTest, ShapesThatPaintNothingAreOutlined)
{
    auto const invisible = SPDocument::createNewDocFromMem(R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
      xmlns:xlink="http://www.w3.org/1999/xlink" width="400" height="400">
  <defs><marker id="dot"><circle r="2"/></marker></defs>
  <path id="none" d="M 0,0 h 10 v 10 z" fill="none"/>
  <path id="filled" d="M 0,0 h 10 v 10 z"/>
  <path id="stroked" d="M 0,0 h 10 v 10 z" fill="none" stroke="blue"/>
  <path id="clear" d="M 0,0 h 10 v 10 z" fill="red" fill-opacity="0" stroke="blue" stroke-opacity="0"/>
  <path id="thin" d="M 0,0 h 10 v 10 z" fill="none" stroke="blue" stroke-width="0"/>
  <path id="marked" d="M 0,0 h 10 v 10 z" fill="none" marker-start="url(#dot)"/>
  <g id="faded" opacity="0"><rect id="inside" x="0" y="0" width="5" height="5" fill="red"/></g>
  <g id="unseen" visibility="hidden">
    <rect id="shown" x="0" y="0" width="5" height="5" fill="red" visibility="visible"/>
    <rect id="hidden" x="0" y="0" width="5" height="5" fill="red"/>
  </g>
  <g id="mixed" transform="translate(100 0)">
    <rect id="m-fill" x="0" y="0" width="20" height="20" fill="red"/>
    <rect id="m-none" x="0" y="0" width="10" height="10" fill="none"/>
  </g>
  <use id="clone" xlink:href="#none" x="50" y="0"/>
</svg>)SVG"sv);
    ASSERT_TRUE(invisible);
    invisible->ensureUpToDate();
    auto object = [&](char const *id) { return cast<SPItem>(invisible->getObjectById(id)); };

    for (auto id : {"none", "clear", "thin", "faded", "inside", "hidden", "m-none"}) {
        EXPECT_TRUE(paints_nothing(*object(id))) << id;
    }
    for (auto id : {"filled", "stroked", "marked", "shown", "m-fill", "mixed"}) {
        EXPECT_FALSE(paints_nothing(*object(id))) << id;
    }

    // A group's shapes that paint nothing, where they are on the desktop, and clones of them.
    auto const outlines = invisible_outlines({object("mixed"), object("clone")}, 10);
    ASSERT_EQ(outlines.size(), 2u);
    auto const first = object("m-none")->desktopGeometricBounds();
    auto const second = object("none")->desktopGeometricBounds();
    ASSERT_TRUE(first && second);
    EXPECT_TRUE(Geom::are_near(outlines[0].boundsExact()->min(), first->min(), EPSILON));
    EXPECT_TRUE(Geom::are_near(outlines[0].boundsExact()->max(), first->max(), EPSILON));
    EXPECT_TRUE(Geom::are_near(outlines[1].boundsExact()->min(), second->min() + Geom::Point(50, 0), EPSILON));
    EXPECT_TRUE(invisible_outlines({object("filled"), object("stroked")}, 10).empty());
    EXPECT_TRUE(invisible_outlines({object("mixed"), object("clone")}, 1).empty()); // Over the limit.
}

TEST_F(BoxFrameTest, ClippingPathsArePickedByTheirOutline)
{
    auto const clips = SPDocument::createNewDocFromMem(R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
      width="400" height="400">
  <defs>
    <clipPath id="clip" clipPathUnits="userSpaceOnUse">
      <rect id="clip-rect" x="10" y="10" width="40" height="20" transform="translate(5 0)"/>
    </clipPath>
    <clipPath id="clip-group" clipPathUnits="userSpaceOnUse">
      <g transform="translate(0 100)"><path id="clip-path" d="M 0,0 h 30 v 30 z"/></g>
    </clipPath>
    <clipPath id="clip-box" clipPathUnits="objectBoundingBox">
      <rect x="0" y="0" width="0.5" height="1"/>
    </clipPath>
  </defs>
  <g id="clipped" clip-path="url(#clip)" transform="translate(100 0)">
    <rect x="0" y="0" width="100" height="100" fill="red"/>
  </g>
  <g id="twin" clip-path="url(#clip)" transform="translate(100 0)">
    <rect x="0" y="0" width="100" height="100" fill="blue"/>
  </g>
  <path id="clipped-path" clip-path="url(#clip-group)" d="M 0,100 h 50 v 50 h -50 z"/>
  <rect id="boxed" clip-path="url(#clip-box)" x="300" y="300" width="50" height="50"/>
  <rect id="plain" x="0" y="0" width="5" height="5"/>
</svg>)SVG"sv);
    ASSERT_TRUE(clips);
    clips->ensureUpToDate();
    auto object = [&](char const *id) { return cast<SPItem>(clips->getObjectById(id)); };
    auto shape = [&](char const *id) { return cast<SPShape>(clips->getObjectById(id)); };
    auto const near_box = [](Geom::PathVector const &path, Geom::Rect const &box) {
        auto const bounds = path.boundsExact();
        return bounds && Geom::are_near(bounds->min(), box.min(), EPSILON) &&
               Geom::are_near(bounds->max(), box.max(), EPSILON);
    };

    // The clipping path is where the object's own children are, with its shapes' transforms.
    auto const clipped = object("clipped");
    auto outlines = clip_outlines(*clipped);
    ASSERT_EQ(outlines.size(), 1u);
    EXPECT_EQ(outlines[0].clipped, clipped);
    EXPECT_EQ(outlines[0].shape, shape("clip-rect"));
    EXPECT_TRUE(near_box(outlines[0].path, Geom::Rect(15, 10, 55, 30) * clipped->i2dt_affine()));

    // A shape in a group of the clipping path, under the group's transform.
    outlines = clip_outlines(*object("clipped-path"));
    ASSERT_EQ(outlines.size(), 1u);
    EXPECT_EQ(outlines[0].shape, shape("clip-path"));
    EXPECT_TRUE(near_box(outlines[0].path, Geom::Rect(0, 100, 30, 130) * object("clipped-path")->i2dt_affine()));

    // Clipping paths in bounding box units are not edited, and an object without one has none.
    EXPECT_TRUE(clip_outlines(*object("boxed")).empty());
    EXPECT_TRUE(clip_outlines(*object("plain")).empty());

    // Near the rectangle's top edge, within the tolerance, it is found; inside it, too far, not.
    std::vector<SPItem *> const items = {object("plain"), object("clipped-path"), clipped, object("boxed")};
    auto const on_top = Geom::Point(35, 12) * clipped->i2dt_affine();
    auto found = clip_outline_at(items, on_top, 3);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->clipped, clipped);
    EXPECT_EQ(found->shape, shape("clip-rect"));
    EXPECT_FALSE(clip_outline_at(items, on_top, 1));
    EXPECT_FALSE(clip_outline_at(items, Geom::Point(35, 20) * clipped->i2dt_affine(), 3));

    // Of two outlines within the tolerance, the nearer wins (the triangle's corner is at
    // (30, 100) and the rectangle's at (115, 30) on the desktop).
    auto const dt = object("clipped-path")->i2dt_affine();
    found = clip_outline_at(items, Geom::Point(31, 65) * dt, 120);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->clipped, object("clipped-path"));
    found = clip_outline_at(items, Geom::Point(114, 31) * dt, 120);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->clipped, clipped);

    // Of two objects sharing a clipping path, the first passed, the topmost, wins.
    found = clip_outline_at({object("twin"), clipped}, on_top, 3);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->clipped, object("twin"));
    found = clip_outline_at({clipped, object("twin")}, on_top, 3);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->clipped, clipped);
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
