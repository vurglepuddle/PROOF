// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a clipping path doesn't isolate what it clips. Something with a blend mode in a
 * clipped group blends with what lies under the group, as in Illustrator and in PDF.
 * See doc/PROOF-clip-blending.md.
 */
#include <array>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <cairomm/surface.h>
#include <2geom/int-rect.h>

#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "display/drawing.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-root.h"

namespace {

/// A document shown, to be drawn whole or in parts.
class Shown
{
public:
    explicit Shown(std::string const &svg)
    {
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
        doc = SPDocument::createNewDocFromMem(svg);
        if (!doc) return;
        doc->ensureUpToDate();
        key = SPItem::display_key_new(1);
        drawing.setRoot(doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
        drawing.update();
    }
    ~Shown()
    {
        if (doc) doc->getRoot()->invoke_hide(key);
    }

    Cairo::RefPtr<Cairo::ImageSurface> draw(Geom::IntRect const &rect)
    {
        auto cs = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, rect.width(), rect.height());
        auto ds = Inkscape::DrawingSurface(cs->cobj(), rect.min());
        auto dc = Inkscape::DrawingContext(ds);
        drawing.render(dc, rect);
        cs->flush();
        return cs;
    }

    std::unique_ptr<SPDocument> doc;

private:
    Inkscape::Drawing drawing;
    unsigned key = 0;
};

/// The red, green and blue of a pixel, 0-255.
std::array<int, 3> pixel(Cairo::RefPtr<Cairo::ImageSurface> const &s, int x, int y)
{
    auto const *p = s->get_data() + y * s->get_stride() + x * 4;
    return {p[2], p[1], p[0]};
}

std::string svg(std::string const &body)
{
    return "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100' viewBox='0 0 100 100'>"
           "<defs><clipPath id='c'><rect x='20' y='20' width='60' height='60'/></clipPath>"
           "<clipPath id='d'><rect x='40' y='0' width='60' height='100'/></clipPath></defs>"
           "<rect width='100' height='100' fill='#808080'/>" + body + "</svg>";
}

// A grey that covers the page, set to Multiply.
std::string const SHADE = "<rect width='100' height='100' fill='#808080' style='mix-blend-mode:multiply'/>";

constexpr int GREY = 128;   // The backdrop.
constexpr int DARK = 64;    // Grey multiplied by grey.

void expect_grey(std::array<int, 3> const &p, int level, char const *what)
{
    for (int c : p) EXPECT_NEAR(c, level, 2) << what;
}

} // namespace

TEST(ClipBlendTest, CompoundClipUsesTheInheritedClipRuleAndItsOwnOverride)
{
    for (bool const override : {false, true}) {
        auto const geometry = std::string("<path d='M10 10 H90 V90 H10 Z M30 30 H70 V70 H30 Z' ") +
                              (override ? "clip-rule='nonzero' fill-rule='evenodd'" : "fill-rule='nonzero'") + "/>";
        Shown shown("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
                    "<defs><clipPath id='c' clip-rule='evenodd'>" + geometry + "</clipPath></defs>"
                    "<rect width='100' height='100' fill='#808080'/>"
                    "<rect width='100' height='100' fill='#ffffff' clip-path='url(#c)'/></svg>");
        ASSERT_TRUE(shown.doc);
        auto const surface = shown.draw(Geom::IntRect::from_xywh(0, 0, 100, 100));
        expect_grey(pixel(surface, 20, 20), 255, "the outer contour clips the paint");
        expect_grey(pixel(surface, 50, 50), override ? 255 : GREY, "the computed clip rule determines the hole");
        expect_grey(pixel(surface, 5, 5), GREY, "outside the clip");
    }
}

TEST(ClipBlendTest, AShadeInAClippedGroupDarkensWhatIsUnderTheGroup)
{
    Shown shown(svg("<g clip-path='url(#c)'>" + SHADE + "</g>"));
    ASSERT_TRUE(shown.doc);
    auto const all = Geom::IntRect::from_xywh(0, 0, 100, 100);
    auto const s = shown.draw(all);
    // Inside the clipping path the shade multiplies with the grey under the group. Blended
    // with nothing it would be its own grey, and the page would look untouched.
    expect_grey(pixel(s, 50, 50), DARK, "inside the clipping path");
    // Outside it nothing changes.
    expect_grey(pixel(s, 5, 5), GREY, "outside the clipping path");
    expect_grey(pixel(s, 90, 50), GREY, "outside the clipping path");
}

TEST(ClipBlendTest, DrawnInPartsItIsTheSamePicture)
{
    // The canvas draws in tiles, each with what is under the group in its own surface.
    Shown shown(svg("<g clip-path='url(#c)'>" + SHADE + "</g>"));
    ASSERT_TRUE(shown.doc);
    auto const whole = shown.draw(Geom::IntRect::from_xywh(0, 0, 100, 100));
    for (auto const &part : {Geom::IntRect::from_xywh(10, 10, 30, 30), Geom::IntRect::from_xywh(45, 15, 40, 25),
                             Geom::IntRect::from_xywh(70, 60, 30, 40)}) {
        auto const s = shown.draw(part);
        for (int y = 0; y < part.height(); y += 3) {
            for (int x = 0; x < part.width(); x += 3) {
                auto const a = pixel(s, x, y);
                auto const b = pixel(whole, part.left() + x, part.top() + y);
                for (int c = 0; c < 3; ++c) ASSERT_NEAR(a[c], b[c], 2) << "at " << part.left() + x << "," << part.top() + y;
            }
        }
    }
}

TEST(ClipBlendTest, OneClippedGroupInAnother)
{
    // Clipped twice: the shade shows where both clipping paths let it through.
    Shown shown(svg("<g clip-path='url(#c)'><g clip-path='url(#d)'>" + SHADE + "</g></g>"));
    ASSERT_TRUE(shown.doc);
    auto const s = shown.draw(Geom::IntRect::from_xywh(0, 0, 100, 100));
    expect_grey(pixel(s, 60, 50), DARK, "inside both");
    expect_grey(pixel(s, 30, 50), GREY, "inside the outer one only");
    expect_grey(pixel(s, 90, 50), GREY, "inside the inner one only");
}

TEST(ClipBlendTest, WhatIsUnderItInTheSameGroupCounts)
{
    // The shade lies over a white shape of its own group: there it multiplies with white
    // and is its own grey, and beside the shape with the page's grey.
    Shown shown(svg("<g clip-path='url(#c)'><rect x='20' y='20' width='30' height='60' fill='#ffffff'/>" + SHADE + "</g>"));
    ASSERT_TRUE(shown.doc);
    auto const s = shown.draw(Geom::IntRect::from_xywh(0, 0, 100, 100));
    expect_grey(pixel(s, 35, 50), GREY, "over the white shape");
    expect_grey(pixel(s, 65, 50), DARK, "beside it");
}

TEST(ClipBlendTest, AGroupThatIsIsolatedStaysSo)
{
    // Opacity makes a group of its own, clipped or not: what is in it blends inside it.
    // Half of the shade's own grey over the page's grey is that grey.
    Shown faded(svg("<g clip-path='url(#c)' style='opacity:0.5'>" + SHADE + "</g>"));
    ASSERT_TRUE(faded.doc);
    expect_grey(pixel(faded.draw(Geom::IntRect::from_xywh(0, 0, 100, 100)), 50, 50), GREY, "a clipped group at half opacity");
    Shown isolated(svg("<g clip-path='url(#c)' style='isolation:isolate'>" + SHADE + "</g>"));
    ASSERT_TRUE(isolated.doc);
    expect_grey(pixel(isolated.draw(Geom::IntRect::from_xywh(0, 0, 100, 100)), 50, 50), GREY, "an isolated clipped group");
    // And a clipped group with nothing blending in it is drawn as it always was.
    Shown plain(svg("<g clip-path='url(#c)'><rect width='100' height='100' fill='#404040'/></g>"));
    ASSERT_TRUE(plain.doc);
    auto const s = plain.draw(Geom::IntRect::from_xywh(0, 0, 100, 100));
    expect_grey(pixel(s, 50, 50), DARK, "a plain shape, clipped");
    expect_grey(pixel(s, 5, 5), GREY, "outside its clipping path");
}
