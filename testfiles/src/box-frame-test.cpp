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
#include "document.h"
#include "inkscape.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
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

    // Turning again adds up; the box repeats every quarter turn.
    rotate(*path, 30, {250, 220});
    EXPECT_NEAR(degrees({path}), -30, EPSILON);
    rotate(*path, 30, {250, 220});
    EXPECT_NEAR(degrees({path}), 0, EPSILON);
    EXPECT_FALSE(path->getRepr()->attribute(BOX_ANGLE_ATTRIBUTE));
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
