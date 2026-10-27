// SPDX-License-Identifier: GPL-2.0-or-later
/** @file Transform previews compared with the document's actual commit path, without a window. */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <cairomm/surface.h>
#include <gtest/gtest.h>

#include "display/drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "style.h"
#include "xml/repr.h"
#include "xml/node-observer.h"

using namespace Inkscape;

namespace {

class ReprChanges : public XML::NodeObserver
{
public:
    explicit ReprChanges(XML::Node &root) : root(root) { root.addSubtreeObserver(*this); }
    ~ReprChanges() override { root.removeSubtreeObserver(*this); }
    void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override { ++count; }
    void notifyChildAdded(XML::Node &, XML::Node &, XML::Node *) override { ++count; }
    void notifyChildRemoved(XML::Node &, XML::Node &, XML::Node *) override { ++count; }
    void notifyContentChanged(XML::Node &, Util::ptr_shared, Util::ptr_shared) override { ++count; }
    XML::Node &root;
    unsigned count = 0;
};

std::string const path = "<path id='item' d='M 30,30 H 90 V 80 H 30 Z'/>";
std::string const gradient_path =
    "<defs><linearGradient id='paint'><stop stop-color='#f00'/>"
    "<stop offset='1' stop-color='#00f'/></linearGradient></defs>"
    "<path id='item' d='M 30,30 H 90 V 80 H 30 Z' stroke='url(#paint)'/>";
std::vector<std::string> const bodies = {
    path,
    "<rect id='item' x='30' y='30' width='60' height='50'/>",
    "<g id='item'><path id='child' d='M 30,30 H 90 V 80 H 30 Z'/></g>",
    "<defs><path id='source' stroke-width='6' d='M 30,30 H 90 V 80 H 30 Z'/></defs>"
    "<use id='item' xlink:href='#source'/>",
    "<defs><path id='source' d='M 30,30 H 90 V 80 H 30 Z'/></defs><use id='item' xlink:href='#source'/>",
    "<text id='item' x='30' y='70' font-family='sans-serif' font-size='35'>Ab<tspan stroke-width='3'>c</tspan></text>",
    "<ellipse id='item' cx='60' cy='55' rx='30' ry='25'/>",
    "<line id='item' x1='30' y1='30' x2='90' y2='80'/>",
    "<g id='item'><g transform='matrix(1 .2 .1 1 0 0)'><path d='M 30,30 H 90 V 80 H 30 Z'/></g></g>",
    "<defs><clipPath id='clip'><rect width='160' height='160'/></clipPath></defs>"
    "<path id='item' clip-path='url(#clip)' d='M 30,30 H 90 V 80 H 30 Z'/>",
    "<text id='item' x='30' y='70' font-family='sans-serif' font-size='35' style='inline-size:120px'>Abc</text>"
};

std::vector<Geom::Affine> const transforms = {
    Geom::Affine(4, 0, 0, 1, 0, 0),
    Geom::Affine(1, .3, .5, 1, 0, 0),
    Geom::Affine(Geom::Rotate(.4)) * Geom::Translate(90, 0),
    Geom::Affine(Geom::Translate(20, 15)),
    Geom::Affine(Geom::Scale(2)),
    Geom::Affine(-2, 0, 0, 1, 240, 0),
    Geom::Affine(1, 0, 0, .25, 0, 40)
};

class Display
{
public:
    explicit Display(SPDocument &doc) : root(doc.getRoot()), key(SPItem::display_key_new(1))
    {
        drawing.setRoot(root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
        drawing.setExact();
        drawing.setCacheBudget(16 * 1024 * 1024);
        drawing.setCacheLimit(Geom::IntRect::from_xywh(0, 0, 640, 420));
    }
    ~Display() { root->invoke_hide(key); }

    std::vector<unsigned char> pixels(double zoom = 1.0)
    {
        drawing.update(Geom::IntRect::infinite(), Geom::Scale(zoom));
        auto area = Geom::IntRect::from_xywh(0, 0, 640, 420);
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, area.width(), area.height());
        auto ds = DrawingSurface(surface->cobj(), area.min());
        auto dc = DrawingContext(ds);
        drawing.render(dc, area);
        EXPECT_EQ(cairo_status(dc.raw()), CAIRO_STATUS_SUCCESS);
        surface->flush();
        auto data = surface->get_data();
        return {data, data + surface->get_stride() * surface->get_height()};
    }

    Drawing drawing;
    SPRoot *root;
    unsigned key;
};

class TransformStrokeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
        auto prefs = Preferences::get();
        old_scale = prefs->getBool("/options/transform/stroke", true);
        old_preserve = prefs->getBool("/options/preservetransform/value", false);
        prefs->setBool("/options/preservetransform/value", false);
    }
    void TearDown() override
    {
        Preferences::get()->setBool("/options/transform/stroke", old_scale);
        Preferences::get()->setBool("/options/preservetransform/value", old_preserve);
    }

    std::unique_ptr<SPDocument> load(std::string const &body, std::string const &style = "",
                                    std::string const &parent_transform = "")
    {
        return SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
            "width='640' height='420'><g transform='" + parent_transform +
            "' style='fill:none;stroke:#000;stroke-width:6;stroke-linejoin:round;" + style + "'>" + body + "</g></svg>");
    }

    void same(std::vector<unsigned char> const &a, std::vector<unsigned char> const &b, bool alpha_only = false)
    {
        ASSERT_EQ(a.size(), b.size());
        double error = 0, ink = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            error += std::abs(int(a[i]) - int(b[i]));
            ink += std::max(a[i], b[i]);
        }
        ASSERT_GT(ink, 0);
        if (alpha_only) {
            max_color_error = std::max(max_color_error, error / ink);
            error = ink = 0;
            for (size_t i = 0; i < a.size(); i += 4) {
                uint32_t pa, pb;
                std::memcpy(&pa, &a[i], 4);
                std::memcpy(&pb, &b[i], 4);
                auto aa = int(pa >> 24), ab = int(pb >> 24);
                error += std::abs(aa - ab);
                ink += std::max(aa, ab);
            }
            ASSERT_GT(ink, 0);
        }
        EXPECT_LT(error / ink, .015);
    }

    void compare(std::string const &body, Geom::Affine const &affine, bool scale,
                 std::string const &style = "", std::string const &parent = "", double zoom = 1,
                 bool alpha_only = false)
    {
        SCOPED_TRACE(body);
        SCOPED_TRACE(scale ? "stroke scaling on" : "stroke scaling off");
        Preferences::get()->setBool("/options/transform/stroke", scale);
        auto doc = load(body, style, parent);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        auto item = cast<SPItem>(doc->getObjectById("item"));
        ASSERT_TRUE(item);
        Display display(*doc);
        display.pixels(zoom);
        ObjectSet selection(doc.get());
        selection.set(item);
        auto also = cast<SPItem>(doc->getObjectById("also"));
        Geom::Affine also_original;
        if (also) {
            selection.setList(std::vector<SPItem*>{item, also});
            also_original = also->i2dt_affine();
        }
        auto xml = sp_repr_save_buf(doc->getReprDoc());
        auto serial = doc->serial();
        auto modified = doc->isModifiedSinceSave();
        ReprChanges changes(*doc->getReprRoot());
        auto original = item->i2dt_affine();
        auto width = item->style->stroke_width.computed;
        item->set_i2d_affine(original * affine);
        if (also) also->set_i2d_affine(also_original * affine);
        auto preview = item->getStrokeTransform(item->transform);
        auto view = item->get_arenaitem(display.key);
        ASSERT_TRUE(view);
        selection.setTransformPreview(affine, display.key);
        doc->ensureUpToDate();
        auto before = display.pixels(zoom);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
        EXPECT_EQ(doc->serial(), serial);
        EXPECT_EQ(doc->isModifiedSinceSave(), modified);
        EXPECT_EQ(changes.count, 0u);
        item->setTransformPreview(display.key, std::nullopt);
        selection.applyAffine(affine, false);
        doc->ensureUpToDate();
        if (also) also->setTransformPreview(display.key, std::nullopt);
        if (!also) {
            for (int i = 0; i < 6; ++i) EXPECT_NEAR(preview.transform[i], item->transform[i], 1e-5);
            if (item->style->stroke_width.inherit) {
                // adjust_stroke can change computed without writing an inherited width.
                auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
                ASSERT_TRUE(reopened);
                reopened->ensureUpToDate();
                auto saved = cast<SPItem>(reopened->getObjectById("item"));
                ASSERT_TRUE(saved);
                EXPECT_NEAR(saved->style->stroke_width.computed, width * preview.width_expansion, 1e-5);
            } else {
                EXPECT_NEAR(item->style->stroke_width.computed, width * preview.width_expansion, 1e-5);
            }
        }
        same(before, display.pixels(zoom), alpha_only);
    }

    bool old_scale, old_preserve;
    double max_color_error = 0;
};

using Case = std::tuple<unsigned, unsigned, bool>;
class TransformStrokeReleaseTest : public TransformStrokeTest, public ::testing::WithParamInterface<Case> {};

TEST_P(TransformStrokeReleaseTest, PreviewEqualsActualRelease)
{
    auto [body, transform, scale] = GetParam();
    compare(bodies[body], transforms[transform], scale);
}

INSTANTIATE_TEST_SUITE_P(ObjectsAndAffines, TransformStrokeReleaseTest,
                        ::testing::Combine(::testing::Range(0u, unsigned(bodies.size())),
                                           ::testing::Range(0u, unsigned(transforms.size())), ::testing::Bool()));

TEST_F(TransformStrokeTest, PreservedTransformsMatchRelease)
{
    Preferences::get()->setBool("/options/preservetransform/value", true);
    for (auto scale : {false, true}) for (auto const &body : bodies) compare(body, transforms[0], scale);
}

TEST_F(TransformStrokeTest, ExistingTransformsAndZoomMatchRelease)
{
    for (auto scale : {false, true}) for (auto const &body : bodies) {
        compare(body, transforms[1], scale, "", "matrix(1.2 .1 .3 .9 10 5)", 1.5);
    }
}

TEST_F(TransformStrokeTest, DashesMatchRelease)
{
    for (auto scale : {false, true}) for (auto const &body : bodies) {
        compare(body, transforms[0], scale, "stroke-dasharray:8,3;stroke-dashoffset:2");
    }
}

TEST_F(TransformStrokeTest, ExplicitAndInheritedStrokePropertiesMatchRelease)
{
    std::vector<std::string> const cases = {
        "<path id='item' d='M 30,30 H 90 V 80 H 30 Z' style='stroke-dasharray:8,3;stroke-dashoffset:2'/>",
        "<g id='item' style='stroke-dasharray:8,3;stroke-dashoffset:2'><path d='M 30,30 H 90 V 80 H 30 Z'/></g>",
        "<g id='item'><path d='M 30,30 H 90 V 80 H 30 Z' style='stroke-dasharray:8,3;stroke-dashoffset:2'/></g>",
        "<path id='item' d='M 30,30 H 90 V 80 H 30 Z' style='stroke-width:inherit'/>",
        "<g id='item'><path d='M 30,30 H 90 V 80 H 30 Z' style='stroke-width:inherit'/></g>"
    };
    for (auto scale : {false, true}) for (auto const &body : cases) for (auto const &affine : transforms) {
        compare(body, affine, scale);
    }
}

TEST_F(TransformStrokeTest, NonScalingStrokesMatchRelease)
{
    for (auto scale : {false, true}) for (auto const &body : bodies) for (auto const &affine : transforms) {
        compare(body, affine, scale, "vector-effect:non-scaling-stroke");
    }
}

TEST_F(TransformStrokeTest, MarkersMatchRelease)
{
    std::string const markers =
        "<defs><marker id='arrow' orient='auto' markerWidth='4' markerHeight='4' refX='1' refY='2'>"
        "<path d='M 0,0 4,2 0,4 Z' fill='#000' stroke='none'/></marker></defs>"
        "<path id='item' d='M 30,30 L 90,80' marker-start='url(#arrow)' marker-end='url(#arrow)'/>";
    for (auto scale : {false, true}) for (auto const &affine : transforms) compare(markers, affine, scale);
}

TEST_F(TransformStrokeTest, GradientStrokeGeometryMatchesRelease)
{
    for (auto scale : {false, true}) for (auto const &affine : transforms) {
        compare(gradient_path, affine, scale, "", "", 1, true);
    }
    RecordProperty("gradient_rgba_error_max", std::to_string(max_color_error));
}

TEST_F(TransformStrokeTest, GradientTranslationGeometryWithoutPreviewOverride)
{
    auto doc = load(gradient_path);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    Display display(*doc);
    auto item = cast<SPItem>(doc->getObjectById("item"));
    ASSERT_TRUE(item);
    ObjectSet selection(doc.get());
    selection.set(item);
    item->set_i2d_affine(item->i2dt_affine() * transforms[3]);
    doc->ensureUpToDate();
    auto before = display.pixels();
    selection.applyAffine(transforms[3], false);
    doc->ensureUpToDate();
    same(before, display.pixels(), true);
    RecordProperty("gradient_rgba_error_without_preview", std::to_string(max_color_error));
}

TEST_F(TransformStrokeTest, CloneAndOriginalTogetherMatchRelease)
{
    std::string const body = "<path id='also' stroke-width='6' d='M 30,30 H 90 V 80 H 30 Z'/>"
                             "<use id='item' xlink:href='#also' transform='translate(100)'/>";
    for (auto scale : {false, true}) for (auto const &affine : transforms) compare(body, affine, scale);
}

TEST_F(TransformStrokeTest, TextDecorationsMatchRelease)
{
    for (auto scale : {false, true}) for (auto const &affine : transforms) {
        compare(bodies[5], affine, scale, "text-decoration:underline line-through");
    }
}

TEST_F(TransformStrokeTest, CloneAndTransformedOriginalMoveTogether)
{
    std::string const body =
        "<path id='also' transform='matrix(1.2 .1 .3 .9 0 0)' stroke-width='6' d='M 30,30 H 90 V 80 H 30 Z'/>"
        "<use id='item' xlink:href='#also' transform='translate(100)'/>";
    for (auto scale : {false, true}) compare(body, transforms[3], scale);
}

TEST_F(TransformStrokeTest, HairlineDashesMatchRelease)
{
    for (auto scale : {false, true}) for (auto const &affine : transforms) {
        compare("<path id='item' style='-inkscape-stroke:hairline;stroke-dasharray:8,3' d='M 30,30 H 90 V 80 H 30 Z'/>",
                affine, scale);
    }
}

TEST_F(TransformStrokeTest, PatternStrokeMatchesRelease)
{
    std::string const pattern =
        "<defs><pattern id='paint' patternUnits='userSpaceOnUse' width='10' height='10'>"
        "<rect width='5' height='10' fill='#08f' stroke='none'/></pattern></defs>"
        "<path id='item' d='M 30,30 H 90 V 80 H 30 Z' stroke='url(#paint)'/>";
    for (auto scale : {false, true}) for (auto const &affine : transforms) {
        compare(pattern, affine, scale);
    }
}

TEST_F(TransformStrokeTest, LargeCachedShapeMatchesRelease)
{
    std::string const large = "<path id='item' d='M 30,30 H 420 V 240 H 30 Z'/>";
    for (auto scale : {false, true}) compare(large, Geom::Affine(1.1, 0, 0, .8, 0, 0), scale);
}

TEST_F(TransformStrokeTest, ClearingPreviewRestoresOriginalRenderingWithoutWriting)
{
    auto doc = load(bodies[0]);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    Display display(*doc);
    auto original = display.pixels();
    auto item = cast<SPItem>(doc->getObjectById("item"));
    auto affine = item->i2dt_affine();
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    auto view = item->get_arenaitem(display.key);
    for (auto const &transform : transforms) {
        item->set_i2d_affine(affine * transform);
        item->setTransformPreview(display.key, item->getStrokeTransform(item->transform));
        doc->ensureUpToDate();
        display.pixels();
        item->setTransformPreview(display.key, std::nullopt);
        item->set_i2d_affine(affine);
        doc->ensureUpToDate();
        EXPECT_EQ(display.pixels(), original);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    }
}

TEST_F(TransformStrokeTest, SnapshotDefersPreviewAndRestorationWithoutWriting)
{
    auto doc = load(path);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    Display display(*doc);
    auto original_pixels = display.pixels();
    auto item = cast<SPItem>(doc->getObjectById("item"));
    auto original = item->i2dt_affine();
    ReprChanges changes(*doc->getReprRoot());
    display.drawing.snapshot();
    item->set_i2d_affine(original * transforms[0]);
    item->setTransformPreview(display.key, item->getStrokeTransform(item->transform));
    doc->ensureUpToDate();
    item->setTransformPreview(display.key, std::nullopt);
    item->set_i2d_affine(original);
    doc->ensureUpToDate();
    display.drawing.unsnapshot();
    EXPECT_EQ(display.pixels(), original_pixels);
    EXPECT_EQ(changes.count, 0u);
}

TEST_F(TransformStrokeTest, ManySelectedItemsPreviewWithoutWriting)
{
    Preferences::get()->setBool("/options/transform/stroke", false);
    std::string body;
    constexpr unsigned count = 1000;
    for (unsigned i = 0; i < count; ++i) {
        body += "<path id='p" + std::to_string(i) + "' transform='translate(" +
                std::to_string(5 * (i % 100)) + "," + std::to_string(8 * (i / 100)) +
                ")' d='M 0,0 h 3 v 5 h -3 Z' stroke-width='1'/>";
    }
    auto doc = load(body);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    Display display(*doc);
    display.pixels();
    std::vector<SPItem*> items;
    std::vector<Geom::Affine> originals;
    for (unsigned i = 0; i < count; ++i) {
        auto item = cast<SPItem>(doc->getObjectById("p" + std::to_string(i)));
        ASSERT_TRUE(item);
        items.push_back(item);
        originals.push_back(item->i2dt_affine());
    }
    ObjectSet selection(doc.get());
    selection.setList(items);
    ReprChanges changes(*doc->getReprRoot());
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    std::chrono::nanoseconds planning{};
    for (auto const &affine : transforms) {
        auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < count; ++i) items[i]->set_i2d_affine(originals[i] * affine);
        selection.setTransformPreview(affine, display.key);
        planning += std::chrono::steady_clock::now() - start;
        doc->ensureUpToDate();
        display.pixels();
    }
    for (unsigned i = 0; i < count; ++i) {
        items[i]->setTransformPreview(display.key, std::nullopt);
        items[i]->set_i2d_affine(originals[i]);
    }
    doc->ensureUpToDate();
    EXPECT_EQ(changes.count, 0u);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    RecordProperty("planning_us_per_1000_items", std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(planning).count() / transforms.size()));
}

} // namespace
