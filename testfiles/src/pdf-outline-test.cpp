// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include "document.h"
#include "extension/internal/pdfoutput/export-outlines.h"
#include "inkscape.h"
#include "object/sp-clippath.h"
#include "object/sp-item.h"
#include "object/sp-rect.h"
#include "object/sp-shape.h"
#include "object/sp-text.h"
#include "preferences.h"
#include "style.h"
#include "xml/repr.h"

namespace {
using namespace Inkscape;
using namespace Extension::Internal::PdfBuilder;

unsigned strokes(SPObject *object)
{
    unsigned count = is<SPShape>(object) && object->style && !object->style->stroke.isNone();
    for (auto &child : object->children) count += strokes(&child);
    return count;
}

class PdfOutlineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
    }

    std::unique_ptr<SPDocument> document(std::string const &body)
    {
        return SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' "
            "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' xmlns:proof='urn:x-proof:document:1' "
            "xmlns:xlink='http://www.w3.org/1999/xlink' width='200' height='200'>" + body + "</svg>");
    }
};

TEST_F(PdfOutlineTest, CuttingNamesUseWholeWordsAndCamelCase)
{
    for (auto name : {"Cut", "CUT_LINES", "CutContour", "Thru-cut", "Kiss cut", "Die line", "Dieline",
                      "cuts", "cutline", "cutlines", "cutter", "cutting", "thrucut", "kisscut", "dies",
                      "dielines", "diecut", "perf", "perfs", "perforation", "crease", "creases", "score",
                      "scoreline", "fold", "foldline", "vendorCutContour", "CUTContour", "Art / Fold-line"}) {
        EXPECT_TRUE(is_cut_name(name)) << name;
    }
    for (auto name : {"", "Cutout art", "Scores 2024", "shortcut", "dielectric", "performance", "creased",
                      "cutoutArt", "folding", "scored", "cuté", "écut"}) {
        EXPECT_FALSE(is_cut_name(name)) << name;
    }
}

TEST_F(PdfOutlineTest, CuttingLayersAndSpotStrokesAreExempt)
{
    auto doc = document(R"svg(<defs><linearGradient id='ink' inkscape:swatch='solid' proof:ink='CutContour'
      proof:ink-alternate='#ff00ff'><stop stop-color='#ff00ff'/></linearGradient></defs>
      <g inkscape:groupmode='layer' inkscape:label='CutContour'><g inkscape:groupmode='layer' inkscape:label='Inside'>
        <path id='cut' d='M10 10H100' style='fill:none;stroke:black;stroke-width:2'/></g></g>
      <path id='ink-cut' d='M10 30H100' style='fill:none;stroke:url(#ink);stroke-width:2'/>
      <g style='fill:none;stroke:url(#ink);stroke-width:2'><path id='inherited-cut' d='M10 35H100'/>
        <path id='override' d='M10 40H100' style='stroke:black'/></g>
      <g inkscape:label='CutContour'><path id='ordinary' d='M10 50H100' style='fill:none;stroke:black;stroke-width:2'/></g>
      <rect id='fill' x='10' y='70' width='50' height='20' fill='red'/>)svg");
    ASSERT_TRUE(doc);
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto copy = doc->copy();
    outline_for_export(copy.get(), false, true);
    ASSERT_TRUE(copy->getObjectById("cut"));
    ASSERT_TRUE(copy->getObjectById("ink-cut"));
    ASSERT_TRUE(copy->getObjectById("ordinary"));
    ASSERT_TRUE(copy->getObjectById("inherited-cut"));
    ASSERT_TRUE(copy->getObjectById("override"));
    EXPECT_EQ(strokes(copy->getObjectById("cut")), 1u);
    EXPECT_EQ(strokes(copy->getObjectById("ink-cut")), 1u);
    EXPECT_EQ(strokes(copy->getObjectById("ordinary")), 0u);
    EXPECT_EQ(strokes(copy->getObjectById("inherited-cut")), 1u);
    EXPECT_EQ(strokes(copy->getObjectById("override")), 0u);
    EXPECT_TRUE(copy->getObjectById("fill"));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
}

TEST_F(PdfOutlineTest, TextOnCuttingLayersAndInCuttingInksStaysText)
{
    auto doc = document(R"svg(<defs><linearGradient id='ink' inkscape:swatch='solid' proof:ink='CutContour'
      proof:ink-alternate='#ff00ff'><stop stop-color='#ff00ff'/></linearGradient></defs>
      <g inkscape:groupmode='layer' inkscape:label='Die line'><g>
        <text id='cut' x='10' y='30'>CUT</text></g></g>
      <text id='ink-cut' x='10' y='60' style='stroke:url(#ink)'>INK</text>
      <text id='span-cut' x='10' y='90'>A<tspan style='stroke:url(#ink)'>B</tspan></text>
      <text id='ordinary' x='10' y='120'>OUTLINE</text>)svg");
    ASSERT_TRUE(doc);
    outline_for_export(doc.get(), true, true);
    EXPECT_TRUE(is<SPText>(doc->getObjectById("cut")));
    EXPECT_TRUE(is<SPText>(doc->getObjectById("ink-cut")));
    EXPECT_TRUE(is<SPText>(doc->getObjectById("span-cut")));
    ASSERT_TRUE(doc->getObjectById("ordinary"));
    EXPECT_FALSE(is<SPText>(doc->getObjectById("ordinary")));
}

TEST_F(PdfOutlineTest, CuttingCloneKeepsItsStrokeWhenItsSourceIsOutlined)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    prefs->setBool("/options/transform/stroke", false);
    auto doc = document(R"svg(<path id='source' d='M10 10H80' style='fill:none;stroke:black;stroke-width:2'/>
      <use id='ordinary' xlink:href='#source' y='30'/>
      <g inkscape:groupmode='layer' inkscape:label='Cut'><use id='cut' xlink:href='#source' y='60'/></g>)svg");
    ASSERT_TRUE(doc);
    outline_for_export(doc.get(), false, true);
    ASSERT_TRUE(doc->getObjectById("source"));
    ASSERT_TRUE(doc->getObjectById("ordinary"));
    ASSERT_TRUE(doc->getObjectById("cut"));
    EXPECT_EQ(strokes(doc->getObjectById("source")), 0u);
    EXPECT_EQ(strokes(doc->getObjectById("ordinary")), 0u);
    EXPECT_EQ(strokes(doc->getObjectById("cut")), 1u);
    EXPECT_FALSE(prefs->getBool("/options/transform/stroke"));
}

TEST_F(PdfOutlineTest, StrokeOptionLeavesUnstrokedTextAlone)
{
    auto doc = document("<text id='text' x='10' y='30'>LIVE</text>");
    ASSERT_TRUE(doc);
    outline_for_export(doc.get(), false, true);
    EXPECT_TRUE(is<SPText>(doc->getObjectById("text")));
}

TEST_F(PdfOutlineTest, WideStrokesInsideClipPathsKeepTheirGeometry)
{
    auto doc = document(R"svg(<defs><clipPath id='clip'><g>
      <rect id='clip-rect' x='30' y='40' width='80' height='50' style='stroke:black;stroke-width:30'/>
      </g></clipPath><mask id='mask'><rect id='mask-rect' width='200' height='200'
      style='fill:white;stroke:black;stroke-width:10'/></mask></defs>
      <rect width='200' height='200' fill='blue' clip-path='url(#clip)'/>)svg");
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto clip = cast<SPClipPath>(doc->getObjectById("clip"));
    ASSERT_TRUE(clip);
    auto const bounds = clip->geometricBounds(Geom::Affine());
    ASSERT_TRUE(bounds);
    outline_for_export(doc.get(), false, true);
    auto rect = cast<SPRect>(doc->getObjectById("clip-rect"));
    ASSERT_TRUE(rect);
    EXPECT_EQ(strokes(rect), 1u);
    EXPECT_DOUBLE_EQ(rect->style->stroke_width.computed, 30);
    EXPECT_EQ(clip->geometricBounds(Geom::Affine()), bounds);
    ASSERT_TRUE(doc->getObjectById("mask-rect"));
    EXPECT_EQ(strokes(doc->getObjectById("mask-rect")), 0u);
}

TEST_F(PdfOutlineTest, TextInsideClipPathsCanStillBecomeCurves)
{
    auto doc = document(R"svg(<defs><clipPath id='clip'><text id='clip-text' x='10' y='30'
      style='font-family:Arial;font-size:20px;stroke:black;stroke-width:10'>CLIP</text></clipPath></defs>
      <rect width='200' height='200' fill='blue' clip-path='url(#clip)'/>)svg");
    ASSERT_TRUE(doc);
    outline_for_export(doc.get(), true, true);
    auto text = doc->getObjectById("clip-text");
    ASSERT_TRUE(text);
    EXPECT_FALSE(is<SPText>(text));
    EXPECT_GT(strokes(text), 0u);
}

} // namespace
