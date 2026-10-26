// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Combination style and object testing for cascading and flags.
 *//*
 *
 * Authors:
 *   Martin Owens
 *
 * Copyright (C) 2018 Authors
 *
 * Released under GNU GPL version 2 or later, read the file 'COPYING' for more information
 */

#include <gtest/gtest.h>
#include <doc-per-case-test.h>

#include <src/desktop-style.h>
#include <src/style.h>
#include <src/object/sp-root.h>
#include <src/object/sp-rect.h>

using namespace Inkscape;
using namespace Inkscape::XML;
using namespace std::literals;

class ObjectTest: public DocPerCaseTest {
public:
    ObjectTest() {
        constexpr auto docString = R"A(
<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>
<style>
rect { fill: #808080; opacity:0.5; }
.extra { opacity:1.0; }
.overload { fill: #d0d0d0 !important; stroke: #c0c0c0 !important; }
.font { font: italic bold 12px/30px Georgia, serif; }
.exsize { stroke-width: 1ex; }
.fosize { font-size: 15px; }
</style>
<g style='fill:blue; stroke-width:2px;font-size: 14px;'>
  <rect id='one' style='fill:red; stroke:green;'/>
  <rect id='two' style='stroke:green; stroke-width:4px;'/>
  <rect id='three' class='extra' style='fill: #cccccc;'/>
  <rect id='four' class='overload' style='fill:green;stroke:red !important;'/>
  <rect id='five' class='font' style='font: 15px arial, sans-serif;'/>/
  <rect id='six' style='stroke-width:1em;'/>
  <rect id='seven' class='exsize'/>
  <rect id='eight' class='fosize' style='stroke-width: 50%;'/>
</g>
</svg>)A"sv;
        doc = SPDocument::createNewDocFromMem(docString);
        doc->ensureUpToDate();
    }

    std::unique_ptr<SPDocument> doc;
};

/*
 * Test basic cascade values, that they are set correctly as we'd want to see them.
 */
TEST_F(ObjectTest, Styles) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto one = cast<SPRect>(doc->getObjectById("one"));
    ASSERT_TRUE(one != nullptr);

    EXPECT_EQ(one->style->fill.get_value(), Glib::ustring("red"));
    EXPECT_EQ(one->style->stroke.get_value(), Glib::ustring("green"));
    EXPECT_EQ(one->style->opacity.get_value(), Glib::ustring("0.5"));
    EXPECT_EQ(one->style->stroke_width.get_value(), Glib::ustring("2px"));

    auto two = cast<SPRect>(doc->getObjectById("two"));
    ASSERT_TRUE(two != nullptr);

    EXPECT_EQ(two->style->fill.get_value(), Glib::ustring("#808080"));
    EXPECT_EQ(two->style->stroke.get_value(), Glib::ustring("green"));
    EXPECT_EQ(two->style->opacity.get_value(), Glib::ustring("0.5"));
    EXPECT_EQ(two->style->stroke_width.get_value(), Glib::ustring("4px"));

    auto three = cast<SPRect>(doc->getObjectById("three"));
    ASSERT_TRUE(three != nullptr);

    EXPECT_EQ(three->style->fill.get_value(), Glib::ustring("#cccccc"));
    EXPECT_EQ(three->style->stroke.get_value(), Glib::ustring(""));
    EXPECT_EQ(three->style->opacity.get_value(), Glib::ustring("1"));
    EXPECT_EQ(three->style->stroke_width.get_value(), Glib::ustring("2px"));

    auto four = cast<SPRect>(doc->getObjectById("four"));
    ASSERT_TRUE(four != nullptr);

    EXPECT_EQ(four->style->fill.get_value(), Glib::ustring("#d0d0d0"));
    EXPECT_EQ(four->style->stroke.get_value(), Glib::ustring("red"));
    EXPECT_EQ(four->style->opacity.get_value(), Glib::ustring("0.5"));
    EXPECT_EQ(four->style->stroke_width.get_value(), Glib::ustring("2px"));
}

/*
 * Test the origin flag for each of the values, should indicate where it came from.
 */
TEST_F(ObjectTest, StyleSource) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto one = cast<SPRect>(doc->getObjectById("one"));
    ASSERT_TRUE(one != nullptr);

    EXPECT_EQ(one->style->fill.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(one->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(one->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(one->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);

    auto two = cast<SPRect>(doc->getObjectById("two"));
    ASSERT_TRUE(two != nullptr);

    EXPECT_EQ(two->style->fill.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(two->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(two->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(two->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);

    auto three = cast<SPRect>(doc->getObjectById("three"));
    ASSERT_TRUE(three != nullptr);

    EXPECT_EQ(three->style->fill.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(three->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(three->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(three->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);

    auto four = cast<SPRect>(doc->getObjectById("four"));
    ASSERT_TRUE(four != nullptr);

    EXPECT_EQ(four->style->fill.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(four->style->stroke.style_src, SPStyleSrc::STYLE_PROP);
    EXPECT_EQ(four->style->opacity.style_src, SPStyleSrc::STYLE_SHEET);
    EXPECT_EQ(four->style->stroke_width.style_src, SPStyleSrc::STYLE_PROP);
}

/*
 * Test the breaking up of the font property and recreation into separate properties.
 */
TEST_F(ObjectTest, StyleFont) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto five = cast<SPRect>(doc->getObjectById("five"));
    ASSERT_TRUE(five != nullptr);

    // Font property is ALWAYS unset as it's converted into specific font css properties
    EXPECT_EQ(five->style->font.get_value(), Glib::ustring(""));
    EXPECT_EQ(five->style->font_size.get_value(), Glib::ustring("12px"));
    EXPECT_EQ(five->style->font_weight.get_value(), Glib::ustring("bold"));
    EXPECT_EQ(five->style->font_style.get_value(), Glib::ustring("italic"));
    EXPECT_EQ(five->style->font_family.get_value(), Glib::ustring("arial, sans-serif"));
}

/*
 * Test the consumption of font dependent lengths in SPILength, e.g. EM, EX and % units
 */
TEST_F(ObjectTest, StyleFontSizes) {
    ASSERT_TRUE(doc != nullptr);
    ASSERT_TRUE(doc->getRoot() != nullptr);

    SPRoot *root = doc->getRoot();
    ASSERT_TRUE(root->getRepr() != nullptr);
    ASSERT_TRUE(root->hasChildren());

    auto six = cast<SPRect>(doc->getObjectById("six"));
    ASSERT_TRUE(six != nullptr);

    EXPECT_EQ(six->style->stroke_width.get_value(), Glib::ustring("1em"));
    EXPECT_EQ(six->style->stroke_width.computed, 14);

    auto seven = cast<SPRect>(doc->getObjectById("seven"));
    ASSERT_TRUE(seven != nullptr);

    EXPECT_EQ(seven->style->stroke_width.get_value(), Glib::ustring("1ex"));
    EXPECT_EQ(seven->style->stroke_width.computed, 7);

    auto eight = cast<SPRect>(doc->getObjectById("eight"));
    ASSERT_TRUE(eight != nullptr);

    EXPECT_EQ(eight->style->stroke_width.get_value(), Glib::ustring("50%"));

    // stroke-width in percent is relative to viewport size, which is 300x150 in this example.
    // 50% is 118.59 == ((300^2 + 150^2) / 2)^0.5 * 0.5
    EXPECT_FLOAT_EQ(eight->style->stroke_width.computed, 118.58541);
}

/*
 * A text's fill and stroke are those of its characters: the paint panels query the spans
 * holding them, not the <text> element, which can say none while its spans are painted.
 */
TEST(TextPaintSources, CharactersNotTheTextElement)
{
    constexpr auto docString = R"A(
<svg xmlns='http://www.w3.org/2000/svg' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>
<text id='spans' style='fill:none;stroke:none'><tspan id='line' sodipodi:role='line'><tspan id='inner' style='fill:#ffffff;stroke:#000000'>Hi</tspan></tspan></text>
<text id='mixed' style='fill:none'>a<tspan id='red' style='fill:#ff0000'>b</tspan></text>
<text id='blank' style='fill:#00ff00'> <tspan style='fill:#ff0000'> </tspan></text>
<text id='same'><tspan id='first' style='fill:#0000ff'>a</tspan> <tspan id='second' style='fill:#0000ff'>b</tspan></text>
<rect id='rect' width='1' height='1' style='fill:#808080'/>
</svg>)A"sv;
    auto doc = SPDocument::createNewDocFromMem(docString);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto item = [&](char const *id) { return cast<SPItem>(doc->getObjectById(id)); };
    auto fill = [&](std::vector<SPItem *> const &items, int expected) {
        SPStyle query(doc.get());
        EXPECT_EQ(sp_desktop_query_style_from_list(sp_desktop_paint_sources(items), &query, QUERY_STYLE_PROPERTY_FILL), expected);
        return query.fill.isColor() ? query.fill.getColor().toRGBA() : 0u;
    };

    EXPECT_EQ(sp_desktop_paint_sources({item("spans")}), std::vector<SPItem *>{item("inner")});
    EXPECT_EQ(fill({item("spans")}, QUERY_STYLE_SINGLE), 0xffffffffu);
    SPStyle stroke(doc.get());
    EXPECT_EQ(sp_desktop_query_style_from_list(sp_desktop_paint_sources({item("spans")}), &stroke, QUERY_STYLE_PROPERTY_STROKE), QUERY_STYLE_SINGLE);
    EXPECT_EQ(stroke.stroke.getColor().toRGBA(), 0x000000ffu);

    // Characters held by the text itself and by a span: two paints, as Illustrator's "?".
    EXPECT_EQ(sp_desktop_paint_sources({item("mixed")}), (std::vector<SPItem *>{item("mixed"), item("red")}));
    fill({item("mixed")}, QUERY_STYLE_MULTIPLE_DIFFERENT);

    // Spans of one paint, the space between them left out.
    EXPECT_EQ(sp_desktop_paint_sources({item("same")}), (std::vector<SPItem *>{item("first"), item("second")}));
    EXPECT_EQ(fill({item("same")}, QUERY_STYLE_MULTIPLE_SAME), 0x0000ffffu);

    // Nothing visible: the text stands for itself; other items too.
    EXPECT_EQ(sp_desktop_paint_sources({item("blank"), item("rect")}), (std::vector<SPItem *>{item("blank"), item("rect")}));
}
