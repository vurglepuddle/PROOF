// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <2geom/bezier-curve.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include "svg/svg.h"
#include "ui/tool/live-corners.h"

using namespace Inkscape::UI::LiveCorners;
namespace {
Model rectangle()
{
    return Model::fromPath(sp_svg_read_pathv("M0,0 H160 V120 H0 Z"));
}

void expectEqual(Geom::PathVector const &a, Geom::PathVector const &b)
{
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t p = 0; p < a.size(); ++p) {
        ASSERT_EQ(a[p].size_default(), b[p].size_default());
        for (std::size_t i = 0; i < a[p].size_default(); ++i) {
            for (double t : {0., .2, .5, .8, 1.}) EXPECT_LT(Geom::distance(a[p][i].pointAt(t), b[p][i].pointAt(t)), 1e-5);
        }
    }
}
}

TEST(LiveCorners, FullRectangleGetsFourEditableQuarterRounds)
{
    auto model = rectangle();
    ASSERT_EQ(model.corners.size(), 4);
    EXPECT_DOUBLE_EQ(model.setRadius({0, 1, 2, 3}, 20), 20);
    auto path = model.render();
    ASSERT_EQ(path.size(), 1);
    unsigned curves = 0;
    for (auto const &curve : path[0]) {
        if (auto arc = dynamic_cast<Geom::CubicBezier const *>(&curve)) {
            ++curves;
            EXPECT_NEAR(Geom::distance(arc->initialPoint(), arc->finalPoint()), 20 * std::sqrt(2.), 1e-5);
            EXPECT_NEAR(Geom::distance((*arc)[0], (*arc)[1]), 20 * .5522847498307936, 1e-5);
        }
    }
    EXPECT_EQ(curves, 4);
    EXPECT_TRUE(path[0].closed());
}

TEST(LiveCorners, DisplayAngleFollowsNonuniformTransforms)
{
    auto model = Model::fromPath(sp_svg_read_pathv("M0,0 H120 L200,80 H80 Z"));
    ASSERT_EQ(model.corners.size(), 4);
    auto acute = model.displayAngle(model.corners[0], Geom::identity());
    ASSERT_TRUE(acute);
    EXPECT_NEAR(*acute, M_PI / 4, 1e-8);
    auto transform = Geom::Scale(2, .5) * Geom::Rotate(.4) * Geom::Translate(42, -17);
    auto scaled_acute = model.displayAngle(model.corners[0], transform);
    auto scaled_obtuse = model.displayAngle(model.corners[1], transform);
    ASSERT_TRUE(scaled_acute);
    ASSERT_TRUE(scaled_obtuse);
    EXPECT_NEAR(*scaled_acute, std::atan(.25), 1e-8);
    EXPECT_NEAR(*scaled_obtuse, M_PI - std::atan(.25), 1e-8);
}

TEST(LiveCorners, DisplayAngleRejectsCollapsedGeometry)
{
    auto model = rectangle();
    EXPECT_FALSE(model.displayAngle(model.corners[0], Geom::Scale(0, 0)));
    EXPECT_FALSE(model.displayAngle(Corner{100, 100}, Geom::identity()));
}

TEST(LiveCorners, SharedRadiusUsesDisplayedUnitsAndEveryObjectsLimit)
{
    auto first = rectangle();
    auto second = Model::fromPath(sp_svg_read_pathv("M0,0 H110 V50 H0 Z"));
    std::vector<RadiusGroup> groups{{&first, {0, 1, 2, 3}, 1}, {&second, {0, 1, 2, 3}, 2}};
    EXPECT_DOUBLE_EQ(setSharedRadius(groups, 24), 24);
    EXPECT_DOUBLE_EQ(first.corners[0].radius, 24);
    EXPECT_DOUBLE_EQ(second.corners[0].radius, 12);
    auto applied = setSharedRadius(groups, 100);
    EXPECT_NEAR(applied, 50, .001);
    for (auto const &corner : first.corners) EXPECT_NEAR(corner.radius, applied, 1e-8);
    for (auto const &corner : second.corners) EXPECT_NEAR(corner.radius * 2, applied, 1e-8);
}

TEST(LiveCorners, SharedRadiusRetainsUnchosenCornersAndRejectsInvalidScale)
{
    auto first = rectangle();
    auto second = Model::fromPath(sp_svg_read_pathv("M0,0 H120 L200,80 H80 Z"));
    first.setRadius({1}, 18);
    second.corners[2].rounding = Rounding::Relative;
    std::vector<RadiusGroup> groups{{&first, {0}, 1}, {&second, {2}, 2}};
    EXPECT_DOUBLE_EQ(setSharedRadius(groups, 20), 20);
    EXPECT_DOUBLE_EQ(first.corners[1].radius, 18);
    EXPECT_DOUBLE_EQ(second.corners[0].radius, 0);
    EXPECT_DOUBLE_EQ(second.corners[2].radius, 10);
    // At this acute corner Relative cut length has a different geometric
    // maximum from Absolute radius; the rectangle is the shared limiter.
    EXPECT_NEAR(setSharedRadius(groups, 300), 120, .001);
    auto before_first = first.write(), before_second = second.write();
    groups[1].display_scale = 0;
    EXPECT_DOUBLE_EQ(setSharedRadius(groups, 30), 0);
    EXPECT_EQ(first.write(), before_first);
    EXPECT_EQ(second.write(), before_second);
}

TEST(LiveCorners, OneAndThreeCornerSelectionsKeepTheOtherCornersSharp)
{
    for (auto selected : {std::vector<std::size_t>{0}, {0, 1, 2}}) {
        auto model = rectangle();
        model.setRadius(selected, 15);
        auto path = model.render();
        unsigned curved = 0;
        for (auto const &curve : path[0]) curved += dynamic_cast<Geom::CubicBezier const *>(&curve) != nullptr;
        EXPECT_EQ(curved, selected.size());
    }
}

TEST(LiveCorners, SharedRadiusStopsAtTheNeighbourAndRemainsEqual)
{
    auto model = rectangle();
    auto amount = model.setRadius({0, 1, 2, 3}, 10000);
    EXPECT_NEAR(amount, 60, .001);
    for (auto const &corner : model.corners) EXPECT_DOUBLE_EQ(corner.radius, amount);
    auto single = rectangle();
    EXPECT_NEAR(single.setRadius({0}, 10000), 120, .001);
    auto shape = model.render();
    for (auto const &curve : shape[0]) {
        EXPECT_TRUE(std::isfinite(curve.initialPoint()[0]));
        EXPECT_TRUE(std::isfinite(curve.finalPoint()[1]));
    }
}

TEST(LiveCorners, ThreeStylesProduceDistinctExpandedGeometry)
{
    std::vector<Geom::PathVector> paths;
    for (auto style : {Style::Round, Style::Inverted, Style::Chamfer}) {
        auto model = rectangle();
        model.corners[0].style = style;
        model.setRadius({0}, 20);
        paths.push_back(model.render());
    }
    EXPECT_NE(sp_svg_write_path(paths[0]), sp_svg_write_path(paths[1]));
    EXPECT_NE(sp_svg_write_path(paths[0]), sp_svg_write_path(paths[2]));
    unsigned curved = 0;
    for (auto const &curve : paths[2][0]) curved += dynamic_cast<Geom::CubicBezier const *>(&curve) != nullptr;
    EXPECT_EQ(curved, 0);
}

TEST(LiveCorners, AbsoluteAndRelativeAmountsDifferAtAnAcuteCorner)
{
    auto a = Model::fromPath(sp_svg_read_pathv("M0,0 L100,0 L30,60 Z"));
    a.setRadius({0}, 10);
    auto b = a;
    b.corners[0].rounding = Rounding::Relative;
    EXPECT_NE(sp_svg_write_path(a.render()), sp_svg_write_path(b.render()));
}

TEST(LiveCorners, OpenEndpointsAndSmoothJunctionsHaveNoWidgets)
{
    auto open = Model::fromPath(sp_svg_read_pathv("M0,0 L100,0 L100,100"));
    ASSERT_EQ(open.corners.size(), 1);
    EXPECT_EQ(open.corners[0].node, 1);
    auto smooth = Model::fromPath(sp_svg_read_pathv("M0,0 C20,0 80,0 100,0 C120,0 140,10 160,10"));
    EXPECT_TRUE(smooth.corners.empty());
}

TEST(LiveCorners, ACornerBetweenCurvesRetainsTheUntrimmedCurvePortions)
{
    auto model = Model::fromPath(sp_svg_read_pathv("M0,100 C20,80 80,20 100,0 C100,40 130,60 150,100"));
    ASSERT_EQ(model.corners.size(), 1);
    model.setRadius({0}, 10);
    auto path = model.render();
    EXPECT_TRUE(Geom::are_near(path[0].initialPoint(), Geom::Point(0, 100)));
    EXPECT_TRUE(Geom::are_near(path[0].finalPoint(), Geom::Point(150, 100)));
    EXPECT_GE(path[0].size_open(), 3);
}

TEST(LiveCorners, SavingReloadingAndObjectTranslationKeepLiveParameters)
{
    auto model = rectangle();
    model.setRadius({0, 2}, 18.125);
    model.corners[2].style = Style::Inverted;
    auto loaded = Model::read(model.write().c_str());
    ASSERT_TRUE(loaded);
    expectEqual(model.render(), loaded->render());
    auto moved = loaded->render();
    moved *= Geom::Translate(30, -40);
    ASSERT_TRUE(loaded->reconcile(moved));
    expectEqual(moved, loaded->render());
    EXPECT_DOUBLE_EQ(loaded->corners[0].radius, 18.125);
    EXPECT_EQ(loaded->corners[2].style, Style::Inverted);
}

TEST(LiveCorners, EditingOneRoundBakesItAndKeepsTheOtherThreeLive)
{
    auto model = rectangle();
    model.setRadius({0, 1, 2, 3}, 20);
    auto rendered = model.render();
    Geom::Path edited(rendered[0].initialPoint());
    bool changed = false;
    for (auto const &curve : rendered[0]) {
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&curve); cubic && !changed) {
            edited.appendNew<Geom::CubicBezier>((*cubic)[1] + Geom::Point(7, 3), (*cubic)[2], (*cubic)[3]);
            changed = true;
        } else edited.append(curve);
    }
    edited.close();
    ASSERT_TRUE(model.reconcile({edited}));
    expectEqual(model.render(), {edited});
    EXPECT_EQ(std::count_if(model.corners.begin(), model.corners.end(), [](auto const &c) { return c.radius > 0; }), 3);
}

TEST(LiveCorners, BakedRotationScaleReflectionAndSkewKeepAllLiveCorners)
{
    for (auto transform : {Geom::Affine(2, 0, 0, 2, 40, -20), Geom::Affine(2, 0, 0, .5, 0, 0),
                           Geom::Affine(0, 1, -1, 0, 20, 10), Geom::Affine(-1, 0, .3, 1, 0, 0)}) {
        auto model = rectangle();
        model.setRadius({0, 1, 2, 3}, 20);
        auto transformed = model.render();
        transformed *= transform;
        ASSERT_TRUE(model.reconcile(transformed));
        expectEqual(transformed, model.render());
        auto loaded = Model::read(model.write().c_str());
        ASSERT_TRUE(loaded);
        expectEqual(transformed, loaded->render());
        EXPECT_EQ(std::count_if(model.corners.begin(), model.corners.end(), [](auto const &c) { return c.radius > 0; }), 4);
    }
}

TEST(LiveCorners, EditingOneScaledRoundStillRetainsTheOtherThree)
{
    auto model = rectangle();
    model.setRadius({0, 1, 2, 3}, 20);
    auto transform = Geom::Affine(2, 0, 0, .5, 20, 10);
    auto transformed = model.render();
    transformed *= transform;
    ASSERT_TRUE(model.reconcile(transformed));
    Geom::Path edited(transformed[0].initialPoint());
    bool changed = false;
    for (auto const &curve : transformed[0]) {
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&curve); cubic && !changed) {
            edited.appendNew<Geom::CubicBezier>((*cubic)[1] + Geom::Point(4, 2), (*cubic)[2], (*cubic)[3]);
            changed = true;
        } else edited.append(curve);
    }
    edited.close();
    ASSERT_TRUE(model.reconcile({edited}));
    expectEqual(model.render(), {edited});
    EXPECT_EQ(std::count_if(model.corners.begin(), model.corners.end(), [](auto const &c) { return c.radius > 0; }), 3);
}

TEST(LiveCorners, MalformedMetadataAndNonFiniteAmountsAreRejected)
{
    for (auto text : {"", "2|M0,0 H20 V20 Z|0,0,10,0,0;", "1|M0,0 H20 V20 Z|99,99,10,0,0;",
                      "1|M0,0 H20 V20 Z|0,0,nan,0,0;", "1|M0,0 H20 V20 Z|0,0,10,3,0;",
                      "1|M0,0 H20 V20 Z|0x,0,10,0,0;", "1|M0,0 H20 V20 Z|0,0,10,0x,0;",
                      "1|M0,0 H20 V20 Z|0,0,10,0,0;0,0,12,0,0;"}) EXPECT_FALSE(Model::read(text));
    auto model = rectangle();
    EXPECT_DOUBLE_EQ(model.setRadius({0}, std::numeric_limits<double>::infinity()), 0);
    EXPECT_FALSE(model.hasRounding());
}

TEST(LiveCorners, NativeRectangleApproximationSurvivesAnOrdinaryAnchorEdit)
{
    auto model = Model::fromPath(sp_svg_read_pathv("M320,270 H460 V370 H320 Z"));
    model.setRadius({0, 1, 2, 3}, 20);
    for (auto &corner : model.corners) corner.handle_factor = .554 / (4. / 3. * std::tan(M_PI / 8));
    auto edited = sp_svg_read_pathv("M340,270 454.379085,259.5425036 C465.4130636,258.5336846 460,278.92 460,290 v60 c0,11.08 -8.92,20 -20,20 H340 c-11.08,0 -20,-8.92 -20,-20 v-60 c0,-11.08 8.9660214,-18.991181 20,-20 z");
    ASSERT_TRUE(model.reconcile(edited));
    expectEqual(model.render(), edited);
    EXPECT_EQ(std::count_if(model.corners.begin(), model.corners.end(), [](auto const &c) { return c.radius > 0; }), 2);
    auto loaded = Model::read(model.write().c_str());
    ASSERT_TRUE(loaded);
    expectEqual(loaded->render(), edited);
}

TEST(LiveCorners, SplittingASideAndAddingASubpathKeepUnrelatedCornersLive)
{
    auto model = rectangle();
    model.setRadius({0, 1, 2, 3}, 20);
    auto rendered = model.render();
    Geom::Path split_path(rendered[0].initialPoint());
    bool split = false;
    for (auto const &curve : rendered[0]) {
        if (dynamic_cast<Geom::LineSegment const *>(&curve) && !split) {
            std::unique_ptr<Geom::Curve> a(curve.portion(0, .5)), b(curve.portion(.5, 1));
            split_path.append(*a);
            split_path.append(*b);
            split = true;
        } else split_path.append(curve);
    }
    split_path.close();
    auto extra = sp_svg_read_pathv("M300,0 H330 V30 Z");
    Geom::PathVector edited{split_path};
    edited.push_back(extra[0]);
    ASSERT_TRUE(model.reconcile(edited));
    expectEqual(model.render(), edited);
    EXPECT_EQ(std::count_if(model.corners.begin(), model.corners.end(), [](auto const &c) { return c.radius > 0; }), 4);
    auto loaded = Model::read(model.write().c_str());
    ASSERT_TRUE(loaded);
    expectEqual(loaded->render(), edited);
}

TEST(LiveCorners, DraggingAHandleUnderAStaleNoisyRecordNeverThrows)
{
    // A saved drawing (2026-10-18): four rounds on a mesh object's outline, then
    // its anchors dragged while the mesh stayed. The record's frame carries
    // floating noise. Dragging a Bezier handle of the visible path then threw
    // "Non-contiguous path" from reconcileTopology inside the drag, and PROOF aborted.
    char const *record =
        "1|M 50.06378131,58.85876475 C 25.37014911,21.31093392 328.7972665,28.41457857 "
        "264.8644647,53.78473286 338.9453303,91.33256712 164.736902,113.6583057 164.736902,141.0580762 "
        "H 50.06378131 Z|0,0,27.213632721051866,0,0;0,1,27.213632721051866,0,0;"
        "0,2,27.213632721051866,0,0;0,3,27.213632721051866,0,0;|0.99999999976117582,"
        "4.6829207178693707e-13,-7.8301809480761464e-11,0.99999999984046595,"
        "-82.199285648178318,47.695899776351141";
    auto const saved = sp_svg_read_pathv(
        "m -32.4976892,98.92449096 c 11.67904089,-17.9955191 116.13464404,-21.66682564 "
        "178.2093715,-16.14148757 34.9793856,3.11355263 63.4645259,43.30233451 31.8793696,58.65256321 "
        "-21.9987809,10.6912981 -54.5496498,20.4110583 -75.2772179,30.2341594 "
        "-15.24658376,7.2255817 -30.11776256,17.08425 -46.98985036,17.08425 H -4.921871629 "
        "c -15.029674341,0 -27.213632721,-12.1839584 -27.213632721,-27.2136327 v -46.8389935 "
        "c 0,-5.3708142 -3.28603923,-11.2716706 -0.36218485,-15.77685884 z");
    // Direct Selection holds anchors in canvas space; the round trip leaves noise.
    double const px = 96 / 25.4;
    auto const canvas = Geom::Affine(Geom::Scale(px * .7821689113)) * Geom::Translate(277.4, 181.2);
    unsigned tried = 0;
    for (std::size_t i = 0; i < saved[0].size_default(); ++i) {
        if (!dynamic_cast<Geom::CubicBezier const *>(&saved[0][i])) continue;
        for (unsigned handle : {1u, 2u}) {
            for (auto delta : {Geom::Point(.6, -.4), Geom::Point(-2.5, 1.5), Geom::Point(9, 4)}) {
                auto model = Model::read(record);
                ASSERT_TRUE(model);
                Geom::Path dragged(saved[0].initialPoint());
                for (std::size_t j = 0; j < saved[0].size_default(); ++j) {
                    std::unique_ptr<Geom::Curve> curve(saved[0][j].duplicate());
                    if (j == i) {
                        auto cubic = static_cast<Geom::CubicBezier *>(curve.get());
                        cubic->setPoint(handle, (*cubic)[handle] + delta);
                    }
                    curve->setInitial(dragged.finalPoint());
                    dragged.append(*curve);
                }
                dragged.close();
                Geom::PathVector edited{dragged};
                edited *= canvas;
                edited *= canvas.inverse();
                bool kept = false;
                EXPECT_NO_THROW(kept = model->reconcile(edited)) << "curve " << i << " handle " << handle;
                if (kept) expectEqual(model->render(), edited);
                ++tried;
            }
        }
    }
    EXPECT_GT(tried, 0u);
}
