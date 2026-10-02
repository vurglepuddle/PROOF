// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for testing selected colors
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>
#include <sigc++/connection.h>

#include "../test-utils.h"

#include "colors/color.h"
#include "colors/color-set.h"
#include "colors/manager.h"
#include "colors/spaces/base.h"
#include "colors/spaces/components.h"

using namespace Inkscape::Colors;

namespace {

::testing::AssertionResult signalIsCalled(sigc::signal<void()> &signal, std::function<void()> then_run)
{
    unsigned was_called = 0;
    sigc::connection ret = signal.connect([&was_called]() { was_called++; });
    then_run();
    ret.disconnect();
    if (was_called == 0)
        return ::testing::AssertionFailure() << " Signal not called.";
    if (was_called > 1)
        return ::testing::AssertionFailure() << " Signal called too many times.";
    return ::testing::AssertionSuccess();
}

TEST(ColorSetTest, setColors)
{
    auto colors = ColorSet();
    ASSERT_TRUE(colors.isEmpty());
    ASSERT_FALSE(colors.getAlphaConstraint());
    ASSERT_FALSE(colors.getSpaceConstraint());

    colors.set("i1", *Color::parse("red"));
    colors.set("i2", *Color::parse("#ff0000"));
    ASSERT_FALSE(colors.isEmpty());
    ASSERT_FALSE(colors.isSame());

    colors.set("i3", *Color::parse("#0000ffff"));
    ASSERT_FALSE(colors.isSame());

    EXPECT_EQ(colors.get("i1")->toString(), "red");
    EXPECT_EQ(colors.get("i2")->toString(), "#ff0000");
    EXPECT_EQ(colors.get("i3")->toString(), "#0000ffff");
    EXPECT_EQ(colors.size(), 3);
    EXPECT_FALSE(colors.get("i4"));

    colors.set("i1", *Color::parse("green"));
    EXPECT_EQ(colors.size(), 3);
    EXPECT_EQ(colors.get("i1")->toString(), "green");

    ASSERT_FALSE(colors.getAlphaConstraint());
    ASSERT_FALSE(colors.getSpaceConstraint());
}

TEST(ColorSetTest, setSingleColor)
{
    auto color = ColorSet();
    ASSERT_FALSE(color.get());

    color.set(*Color::parse("red"));
    EXPECT_FALSE(color.isEmpty());
    ASSERT_EQ(color.size(), 1);
    ASSERT_TRUE(color.get());
    ASSERT_EQ(color.get()->toString(), "red");
    ASSERT_EQ(color.getAverage().toString(), "#ff0000ff");

    color.set(*Color::parse("blue"));
    ASSERT_EQ(color.size(), 1);
    ASSERT_TRUE(color.get());
    ASSERT_EQ(color.get()->toString(), "blue");
    ASSERT_EQ(color.getAverage().toString(), "#0000ffff");
}

TEST(ColorSetTest, replacementPreservesIncomingCmyk)
{
    ColorSet colors;
    Color const corporate(Space::Type::CMYK, {0.05, 1.0, 0.45, 0.22});
    colors.set(Color(0x000000ff));
    colors.set(corporate);
    ASSERT_EQ(colors.get()->getSpace()->getType(), Space::Type::CMYK);
    EXPECT_TRUE(colors.get()->isClose(corporate, 1e-8));

    // Selection changes may alternate RGB objects and CMYK swatches.
    colors.set(Color(0x00ff0080));
    colors.set(corporate);
    EXPECT_TRUE(colors.get()->isClose(corporate, 1e-8));
    EXPECT_FALSE(colors.get()->hasOpacity());
}

TEST(ColorSetTest, replacementHonorsExplicitConstraints)
{
    auto space = Manager::get().find(Space::Type::CMYK);
    ColorSet colors(space, false);
    colors.set("item", Color(0xff000080));
    colors.set("item", Color(0x00ff0040));
    EXPECT_EQ(colors.get("item")->getSpace(), space);
    EXPECT_FALSE(colors.get("item")->hasOpacity());
    EXPECT_TRUE(colors.get("item")->isClose(Color(space, {1, 0, 1, 0}), 1e-8));
}

TEST(ColorSetTest, setColorsConstrained)
{
    auto space = Manager::get().find(Space::Type::RGB);
    auto colors = ColorSet(space, false);
    ASSERT_TRUE(colors.getSpaceConstraint());
    ASSERT_FALSE(*colors.getAlphaConstraint());

    colors.set("i1", *Color::parse("red"));
    colors.set("i2", *Color::parse("#ff000080"));
    ASSERT_TRUE(colors.isSame());
    colors.set("i3", *Color::parse("#0000ffff"));
    ASSERT_FALSE(colors.isSame());

    EXPECT_EQ(colors.get("i1")->toString(), "#ff0000");
    EXPECT_EQ(colors.get("i2")->toString(), "#ff0000");
    EXPECT_EQ(colors.get("i3")->toString(), "#0000ff");
}

TEST(ColorSetTest, setColorsHsl)
{
    auto space = Manager::get().find(Space::Type::HSL);
    auto colors = ColorSet(space, true);
    ASSERT_TRUE(colors.getSpaceConstraint());
    ASSERT_TRUE(colors.getAlphaConstraint());
    ASSERT_TRUE(*colors.getAlphaConstraint());

    colors.set("i1", *Color::parse("red"));
    ASSERT_EQ(colors.get("i1")->toString(), "hsla(0, 100%, 50%, 1)");
}

TEST(ColorSetTest, setAllColors)
{
    auto colors_a = ColorSet();
    colors_a.set("i1", *Color::parse("red"));
    colors_a.set("i2", *Color::parse("blue"));

    auto colors_b = ColorSet();
    colors_b.set("i1", *Color::parse("green"));
    colors_b.set("i3", *Color::parse("purple"));

    EXPECT_TRUE(signalIsCalled(colors_b.signal_changed, [&colors_a, &colors_b]() { colors_b.setAll(colors_a); }));

    // No change to other object
    EXPECT_EQ(colors_a.size(), 2);
    EXPECT_EQ(colors_a.get("i1")->toString(), "red");
    EXPECT_EQ(colors_a.get("i2")->toString(), "blue");

    // Addative changes to B
    EXPECT_EQ(colors_b.size(), 3);
    EXPECT_EQ(colors_b.get("i1")->toString(), "red");
    EXPECT_EQ(colors_b.get("i2")->toString(), "blue");
    EXPECT_EQ(colors_b.get("i3")->toString(), "purple");
}

TEST(ColorSetTest, clearColors)
{
    auto colors = ColorSet();
    colors.set("i1", *Color::parse("red"));
    colors.set("i2", *Color::parse("green"));
    ASSERT_EQ(colors.size(), 2);
    colors.clear();
    ASSERT_EQ(colors.size(), 0);
}

TEST(ColorSetTest, iterateColors)
{
    auto colors = ColorSet();
    colors.set("i1", *Color::parse("red"));

    for (auto &[id, color] : colors) {
        EXPECT_EQ(id, "i1");
        EXPECT_EQ(color.toString(), "red");
    }
}

TEST(ColorSetTest, signalGrabRelease)
{
    auto space = Manager::get().find(Space::Type::RGB);
    auto colors = ColorSet(space, false);
    ASSERT_TRUE(colors.getSpaceConstraint());
    ASSERT_TRUE(colors.getAlphaConstraint());
    ASSERT_FALSE(*colors.getAlphaConstraint());

    EXPECT_TRUE(signalIsCalled(colors.signal_grabbed, [&colors]() { colors.grab(); }));
    EXPECT_FALSE(signalIsCalled(colors.signal_grabbed, [&colors]() { colors.grab(); }));
    EXPECT_TRUE(signalIsCalled(colors.signal_released, [&colors]() { colors.release(); }));
    EXPECT_FALSE(signalIsCalled(colors.signal_released, [&colors]() { colors.release(); }));
}

TEST(ColorSetTest, signalChanged)
{
    auto space = Manager::get().find(Space::Type::RGB);
    auto colors = ColorSet(space);
    colors.set("0", Color(0xff0000ff));
    colors.set("1", Color(0x00ff00ff));

    auto comp = colors.getBestSpace()->getComponents();

    EXPECT_TRUE(signalIsCalled(colors.signal_changed, [&colors, &comp]() { ASSERT_EQ(colors.setAll(comp[0], 0.5), 2); }));
    EXPECT_FALSE(signalIsCalled(colors.signal_changed, [&colors, &comp]() { ASSERT_EQ(colors.setAll(comp[0], 0.5), 0); }));
    EXPECT_TRUE(signalIsCalled(colors.signal_changed, [&colors]() { ASSERT_TRUE(colors.set("0", *Color::parse("blue"))); }));
    EXPECT_FALSE(signalIsCalled(colors.signal_changed, [&colors]() { ASSERT_FALSE(colors.set("0", *Color::parse("blue"))); }));
    EXPECT_TRUE(signalIsCalled(colors.signal_changed, [&colors]() { ASSERT_EQ(colors.setAll(*Color::parse("blue")), 1); }));
    EXPECT_FALSE(signalIsCalled(colors.signal_changed, [&colors]() { ASSERT_EQ(colors.setAll(*Color::parse("blue")), 0); }));
    EXPECT_TRUE(signalIsCalled(colors.signal_changed, [&colors, &comp]() {
        colors.grab();
        colors.setAll(comp[0], 0.75);
        ;
    }));
}

TEST(ColorSetTest, signalModified)
{
    auto colors = ColorSet();
    EXPECT_FALSE(signalIsCalled(colors.signal_cleared, [&colors]() { colors.clear(); }));
    EXPECT_FALSE(signalIsCalled(colors.signal_cleared, [&colors]() { colors.set("new", *Color::parse("red")); }));
    EXPECT_TRUE(signalIsCalled(colors.signal_cleared, [&colors]() { colors.clear(); }));
}

TEST(ColorSetTest, colorAverages)
{
    auto space = Manager::get().find(Space::Type::RGB);
    auto colors = ColorSet(space, false);
    colors.set("0", Color(space, {0.4, 0.5, 1.0}));
    colors.set("1", Color(space, {0.5, 0.5, 0.5}));
    colors.set("2", Color(space, {0.6, 0.5, 0.0}));
    auto comp = colors.getBestSpace()->getComponents();

    // Red starts off in the middle, Green is our control
    EXPECT_NEAR(colors.getAverage(comp[0]), 0.5, 0.01);
    EXPECT_NEAR(colors.getAverage(comp[1]), 0.5, 0.01);
    EXPECT_TRUE(VectorIsNear(colors.getAll(comp[0]), {0.4, 0.5, 0.6}, 0.05));

    // Move red to 0.75, green doesn't change
    EXPECT_TRUE(signalIsCalled(colors.signal_changed, [&colors, &comp]() { colors.setAverage(comp[0], 0.75); }));

    EXPECT_NEAR(colors.getAverage(comp[0]), 0.75, 0.01);
    EXPECT_NEAR(colors.getAverage(comp[1]), 0.5, 0.01);
    EXPECT_TRUE(VectorIsNear(colors.getAll(comp[0]), {0.65, 0.75, 0.85}, 0.05));

    // Move red to 1.0 pushing some values out of bounds
    colors.setAverage(comp[0], 1.0);
    EXPECT_NEAR(colors.getAverage(comp[0]), 1.0, 0.01);
    EXPECT_TRUE(VectorIsNear(colors.getAll(comp[0]), {0.9, 1, 1}, 0.05));

    // Move red to 0.25, green doesn't change, but red remembers it's previous range
    colors.setAverage(comp[0], 0.25);
    EXPECT_NEAR(colors.getAverage(comp[0]), 0.25, 0.01);
    EXPECT_NEAR(colors.getAverage(comp[1]), 0.5, 0.01);
    EXPECT_TRUE(VectorIsNear(colors.getAll(comp[0]), {0.15, 0.25, 0.35}, 0.05));
}

TEST(ColorSetTest, getAverage)
{
    auto colors = ColorSet({}, false);
    colors.set("c1", *Color::parse("black"));
    colors.set("c2", *Color::parse("white"));
    EXPECT_EQ(colors.getBestSpace()->getName(), "CSSNAME");
    EXPECT_EQ(colors.getAverage().toString(), "gray");
    EXPECT_FALSE(colors.isSame());

    colors.set("c1", *Color::parse("red"));
    colors.set("c2", *Color::parse("red"));
    EXPECT_EQ(colors.getBestSpace()->getName(), "CSSNAME");
    EXPECT_EQ(colors.getAverage().toString(), "red");
    EXPECT_TRUE(colors.isSame());

    colors.clear();
    colors.set("c1", *Color::parse("hsl(180,100,100)"));
    colors.set("c2", *Color::parse("hsla(60,0,0, 50)"));
    EXPECT_EQ(colors.getBestSpace()->getName(), "HSL");
    EXPECT_EQ(colors.getAverage().toString(), "hsl(120, 50%, 50%)");

    colors.set("c1", *Color::parse("hsl(180,100,100)"));
    colors.set("c2", *Color::parse("hsl(0,50,100)"));
    colors.set("c3", *Color::parse("blue"));
    EXPECT_EQ(colors.getBestSpace()->getName(), "HSL");
    EXPECT_EQ(colors.getAverage().toString(), "hsl(139, 83.333%, 83.333%)");
}

TEST(ColorSetTest, getCmykAverage)
{
    auto colors = ColorSet({}, false);
    colors.set("cmyk1", *Color::parse("device-cmyk(0.5 0.5 0.0 0.2 / 0.5)"));
    colors.set("rgb1", *Color::parse("red"));
    EXPECT_EQ(colors.getBestSpace()->getName(), "DeviceCMYK");
    EXPECT_EQ(colors.getAverage().toString(), "device-cmyk(0.25 0.75 0.5 0.1)");
}

} // namespace

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
