// SPDX-License-Identifier: GPL-2.0-or-later
/** @file Selection baseline geometry without a desktop or canvas. */

#include <cmath>
#include <string>

#include <gtest/gtest.h>
#include <2geom/transforms.h>
#include <glibmm/i18n.h>

#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "object/sp-text.h"
#include "text-editing.h"
#include "xml/node.h"

using namespace Inkscape;

namespace {

constexpr double EPSILON = 1e-4;

class TextBaselines : public ::testing::Test
{
protected:
    std::unique_ptr<SPDocument> document;

    void SetUp() override { Application::create(false); }

    void load(std::string const &body, std::string const &attributes = "")
    {
        std::string const svg =
            "<svg xmlns='http://www.w3.org/2000/svg' "
            "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' "
            "xmlns:xlink='http://www.w3.org/1999/xlink' width='400' height='400' " + attributes +
            "><g style='font-family:monospace;font-size:20px;line-height:1.5'>" + body + "</g></svg>";
        document = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
    }

    SPItem *item() { return cast<SPItem>(document->getObjectById("text")); }
    std::vector<Geom::LineSegment> baselines() { return sp_te_baseline_segments(item()); }

    void point(Geom::Point const &actual, Geom::Point const &expected)
    {
        EXPECT_NEAR(actual.x(), expected.x(), EPSILON);
        EXPECT_NEAR(actual.y(), expected.y(), EPSILON);
    }

    void same(std::vector<Geom::LineSegment> const &actual, std::vector<Geom::LineSegment> const &expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (unsigned i = 0; i < actual.size(); ++i) {
            point(actual[i].initialPoint(), expected[i].initialPoint());
            point(actual[i].finalPoint(), expected[i].finalPoint());
        }
    }
};

TEST_F(TextBaselines, PointTypeHasEachLineAndItsAdvanceExtent)
{
    load("<text id='text' x='20' y='40'><tspan sodipodi:role='line' x='20' y='40'>Alpha</tspan>"
         "<tspan sodipodi:role='line' x='20' y='70'>Hi</tspan></text>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 2);
    point(lines[0].initialPoint(), {20, 40});
    point(lines[1].initialPoint(), {20, 70});
    EXPECT_GT(lines[0].length(EPSILON), 0);
    EXPECT_NEAR(lines[1].length(EPSILON) / lines[0].length(EPSILON), 2.0 / 5, EPSILON);
    point(lines[1].finalPoint(), te_get_layout(item())->characterAnchorPoint(te_get_layout(item())->end()));
}

TEST_F(TextBaselines, AreaTypeWrapsToTextExtents)
{
    load("<text id='text' x='20' y='40' style='inline-size:70px;white-space:pre-wrap'>Alpha Beta Gamma</text>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 3);
    auto const layout = te_get_layout(item());
    std::vector<double> ends(lines.size(), 20);
    for (auto it = layout->begin(); it != layout->end(); it.nextCharacter()) {
        auto const index = layout->lineIndex(it);
        ASSERT_LT(index, lines.size());
        // The full advance of the last character belongs on the line, including at a wrap.
        ends[index] = std::max(ends[index], layout->characterBoundingBox(it).right());
    }
    for (unsigned i = 0; i < lines.size(); ++i) {
        point(lines[i].initialPoint(), {20, 40.0 + 30 * i});
        point(lines[i].finalPoint(), {ends[i], 40.0 + 30 * i});
        EXPECT_NE(lines[i].length(EPSILON), 70);
        EXPECT_GT(lines[i].length(EPSILON), 0);
    }
    // A wrapping space can hang outside the frame; its laid-out advance still counts.
    EXPECT_GT(lines.front().length(EPSILON), 70);
    EXPECT_LT(lines.back().length(EPSILON), 70);
}

TEST_F(TextBaselines, LegacyFlowedTypeWraps)
{
    load("<flowRoot id='text'><flowRegion><rect x='20' y='20' width='70' height='140'/></flowRegion>"
         "<flowPara>Alpha Beta Gamma</flowPara></flowRoot>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 3);
    for (auto const &line : lines) {
        EXPECT_NEAR(line.initialPoint().x(), 20, EPSILON);
        EXPECT_GT(line.length(EPSILON), 0);
        EXPECT_NE(line.length(EPSILON), 70);
    }
    EXPECT_NEAR(lines[1].initialPoint().y() - lines[0].initialPoint().y(), 30, EPSILON);
}

TEST_F(TextBaselines, TypeInAShapeAndReflow)
{
    load("<defs><rect id='frame' x='20' y='20' width='70' height='160'/></defs>"
         "<text id='text' style='shape-inside:url(#frame);white-space:pre-wrap'>Alpha Beta Gamma</text>");
    auto const narrow = baselines();
    ASSERT_EQ(narrow.size(), 3);
    document->getObjectById("frame")->getRepr()->setAttribute("width", "250");
    document->ensureUpToDate();
    auto const wide = baselines();
    ASSERT_EQ(wide.size(), 1);
    point(wide[0].initialPoint(), narrow[0].initialPoint());
    EXPECT_GT(wide[0].length(EPSILON), narrow[0].length(EPSILON));
    EXPECT_LT(wide[0].length(EPSILON), 250);
}

TEST_F(TextBaselines, RotationSkewAndAncestorTransform)
{
    load("<g transform='translate(50 60)'><text id='text' x='20' y='40' "
         "transform='rotate(30) skewX(20)'>Alpha</text></g>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 1);
    auto const affine = Geom::HShear(std::tan(M_PI / 9)) * Geom::Rotate(M_PI / 6) * Geom::Translate(50, 60);
    point(lines[0].initialPoint(), Geom::Point(20, 40) * affine);
    auto const end = te_get_layout(item())->characterAnchorPoint(te_get_layout(item())->end());
    point(lines[0].finalPoint(), end * affine);
    EXPECT_GT(lines[0].finalPoint().y(), lines[0].initialPoint().y());
}

TEST_F(TextBaselines, DocumentCoordinatesIncludeViewBoxScale)
{
    load("<text id='text' x='20' y='40'>Alpha</text>", "viewBox='0 0 200 200'");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 1);
    point(lines[0].initialPoint(), {40, 80});
}

TEST_F(TextBaselines, EmptyLinesHaveNoSegment)
{
    load("<text id='text' x='20' y='40' style='white-space:pre'>Alpha\n\nHi\n</text>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 2);
    point(lines[0].initialPoint(), {20, 40});
    point(lines[1].initialPoint(), {20, 100});
}

TEST_F(TextBaselines, CompletelyEmptyTypeHasNoSegment)
{
    load("<text id='text' x='20' y='40'/>");
    EXPECT_TRUE(baselines().empty());
}

TEST_F(TextBaselines, EmptyPointTypeLineHasNoSegment)
{
    load("<text id='text' x='20' y='40'><tspan sodipodi:role='line' x='20' y='40'>Alpha</tspan>"
         "<tspan sodipodi:role='line' x='20' y='70'></tspan>"
         "<tspan sodipodi:role='line' x='20' y='100'>Hi</tspan></text>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 2);
    point(lines[0].initialPoint(), {20, 40});
    point(lines[1].initialPoint(), {20, 100});
}

TEST_F(TextBaselines, SeparateShapeRunsDoNotBridgeTheGap)
{
    load("<defs><path id='frame' d='M20,20 H90 V110 H20 Z M140,20 H210 V110 H140 Z'/></defs>"
         "<text id='text' style='shape-inside:url(#frame);white-space:pre-wrap'>Alpha Beta</text>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 2);
    EXPECT_NEAR(lines[0].initialPoint().x(), 20, EPSILON);
    EXPECT_NEAR(lines[1].initialPoint().x(), 140, EPSILON);
    EXPECT_NEAR(lines[0].initialPoint().y(), lines[1].initialPoint().y(), EPSILON);
    EXPECT_LT(lines[0].finalPoint().x(), lines[1].initialPoint().x());
}

TEST_F(TextBaselines, StyledSpansShareTheLineBaseline)
{
    load("<text id='text' x='20' y='40'>A<tspan style='fill:red'>B</tspan>"
         "<tspan style='baseline-shift:super;font-size:10px'>2</tspan>C</text>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 1);
    point(lines[0].initialPoint(), {20, 40});
    EXPECT_NEAR(lines[0].finalPoint().y(), 40, EPSILON);
    point(lines[0].finalPoint(), te_get_layout(item())->characterAnchorPoint(te_get_layout(item())->end()));
}

TEST_F(TextBaselines, ExplicitTspanPositionsKeepTheirBaselines)
{
    load("<text id='text'><tspan x='20' y='40'>Alpha</tspan><tspan x='50' y='90'>Hi</tspan></text>");
    auto const lines = baselines();
    ASSERT_EQ(lines.size(), 2);
    point(lines[0].initialPoint(), {20, 40});
    point(lines[1].initialPoint(), {50, 90});
    EXPECT_NEAR(lines[1].finalPoint().y(), 90, EPSILON);
}

TEST_F(TextBaselines, CenterAndEndAnchorsKeepTheirExtents)
{
    for (auto const anchor : {"middle", "end"}) {
        load(std::string("<text id='text' x='100' y='40' text-anchor='") + anchor + "'>Alpha</text>");
        auto const lines = baselines();
        ASSERT_EQ(lines.size(), 1);
        EXPECT_NEAR(anchor == std::string("middle") ? lines[0].pointAt(0.5).x() : lines[0].finalPoint().x(),
                    100, EPSILON);
    }
}

TEST_F(TextBaselines, HiddenOverflowHasNoSegment)
{
    load("<defs><rect id='frame' x='20' y='20' width='70' height='35'/></defs>"
         "<text id='text' style='shape-inside:url(#frame);white-space:pre-wrap'>Alpha Beta Gamma</text>");
    ASSERT_EQ(baselines().size(), 1);
}

TEST_F(TextBaselines, TypeOnAPathHasNoExtraBaseline)
{
    load("<defs><path id='path' d='M20,40 Q100,5 180,80'/></defs>"
         "<text id='text'><textPath xlink:href='#path'>Alpha</textPath></text>");
    ASSERT_GT(te_get_layout(item())->glyphs().size(), 0);
    EXPECT_TRUE(baselines().empty());
}

TEST_F(TextBaselines, TypingTransformUndoAndRedoUseTheCurrentLayout)
{
    load("<text id='text' x='20' y='40'>Alpha</text>");
    auto const before = baselines();
    ASSERT_EQ(before.size(), 1);
    DocumentUndo::setUndoSensitive(document.get(), true);
    auto const layout = te_get_layout(item());
    sp_te_insert(item(), layout->end(), " Beta");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), RC_("Undo", "Type"), "");
    auto const typed = baselines();
    ASSERT_EQ(typed.size(), 1);
    EXPECT_GT(typed[0].length(EPSILON), before[0].length(EPSILON));

    item()->getRepr()->setAttribute("transform", "translate(10 15)");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), RC_("Undo", "Move"), "");
    auto const moved = baselines();
    ASSERT_EQ(moved.size(), 1);
    point(moved[0].initialPoint(), {30, 55});

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    same(baselines(), typed);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    same(baselines(), before);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    same(baselines(), typed);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    same(baselines(), moved);
}

TEST_F(TextBaselines, NonTextHasNoBaseline)
{
    load("<rect id='text' width='40' height='30'/>");
    EXPECT_TRUE(baselines().empty());
    EXPECT_TRUE(sp_te_baseline_segments(nullptr).empty());
}

} // namespace
