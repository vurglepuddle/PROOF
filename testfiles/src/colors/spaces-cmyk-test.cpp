// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the device-cmyk css color space
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/spaces/cmyk.h"
#include "colors/spaces/cms.h"
#include "spaces-testbase.h"

namespace {

using Space::Type::CMYK;
using Space::Type::RGB;

class ColorsSpacesCmykProtected
    : public Inkscape::Colors::Space::DeviceCMYK
    , public testing::Test
{
}; // Access protected

// clang-format off
INSTANTIATE_TEST_SUITE_P(ColorsSpacesCmyk, fromString, testing::Values(
    // Taken from the w3c device-cmyk example chart
    _P(in, "device-cmyk(0 0.2 0.2 0.2)",        { 0,   0.2, 0.2, 0.2      }, 0xcca3a3ff),
    _P(in, "device-cmyk(30% 0.2 0.2 0.0)",      { 0.3, 0.2, 0.2, 0        }, 0xb3ccccff),
    _P(in, "device-cmyk(0 0.4 0.4 0.3)",        { 0,   0.4, 0.4, 0.3      }, 0xb36b6bff),
    _P(in, "device-cmyk(0 0.6 60% 0.5)",        { 0,   0.6, 0.6, 0.5      }, 0x803333ff),
    _P(in, "device-cmyk(0.3 60% 0.6 10%)",      { 0.3, 0.6, 0.6, 0.1      }, 0xa15c5cff),
    _P(in, "   device-cmyk(90% 0.6 0.6 0)   ",  { 0.9, 0.6, 0.6, 0        }, 0x196666ff),
    _P(in, "device-cmyk(0 0.8 0.8 0.2)",        { 0.0, 0.8, 0.8, 0.2      }, 0xcc2929ff),
    _P(in, "device-cmyk(0 1.0 1.0 0.1 / 0.5)",  { 0.0, 1.0, 1.0, 0.1, 0.5 }, 0xe6000080)
));

INSTANTIATE_TEST_SUITE_P(ColorsSpacesCmyk, badColorString, testing::Values(
    "device-cmyk", "device-cmyk(", "device-cmyk(10%,",
    "device-cmyk(1.0, 1.0, 1.0)"
));

INSTANTIATE_TEST_SUITE_P(ColorsSpacesCmyk, toString, testing::Values(
    _P(out, CMYK, { 0.1, 0.2, 0.8, 0.1      }, "device-cmyk(0.1 0.2 0.8 0.1)"),
    _P(out, CMYK, { 0.2, 0.1, 0.2, 0.1      }, "device-cmyk(0.2 0.1 0.2 0.1)"),
    _P(out, CMYK, { 0.3, 0.3, 0.0, 0.5      }, "device-cmyk(0.3 0.3 0 0.5)"),
    _P(out, CMYK, { 0.9, 0.0, 0.2, 0.6, 0.8 }, "device-cmyk(0.9 0 0.2 0.6 / 80%)"),
    _P(out, CMYK, { 0.9, 0.0, 0.2, 0.6, 0.8 }, "device-cmyk(0.9 0 0.2 0.6)", false)
));

INSTANTIATE_TEST_SUITE_P(ColorsSpacesCmyk, convertColorSpace, testing::Values(
    _P(inb, CMYK, {1.000, 0.000, 0.000, 0.000}, RGB, {0.000, 1.000, 1.000}),
    _P(inb, CMYK, {0.000, 1.000, 0.000, 0.000}, RGB, {1.000, 0.000, 1.000}),
    _P(inb, CMYK, {0.000, 0.000, 1.000, 0.000}, RGB, {1.000, 1.000, 0.000}),
    _P(inb, CMYK, {0.000, 0.000, 0.000, 1.000}, RGB, {0.000, 0.000, 0.000}),
    _P(inb, CMYK, {1.000, 1.000, 0.000, 0.000}, RGB, {0.000, 0.000, 1.000}),
    _P(inb, CMYK, {0.000, 1.000, 1.000, 0.000}, RGB, {1.000, 0.000, 0.000}),
    _P(inb, CMYK, {1.000, 0.000, 1.000, 0.000}, RGB, {0.000, 1.000, 0.000}),

    // No conversion
    _P(inb, CMYK, {1.000, 0.400, 0.200, 0.300}, CMYK, {1.000, 0.400, 0.200, 0.300}, false)
));

INSTANTIATE_TEST_SUITE_P(ColorsSpacesCmyk, normalize, testing::Values(
    _P(inb, CMYK, { 0.5,  0.5,  0.5,  0.5,  0.5 }, CMYK, { 0.5, 0.5, 0.5, 0.5, 0.5 }),
    _P(inb, CMYK, { 1.2,  1.2,  1.2,  1.2,  1.2 }, CMYK, { 1.0, 1.0, 1.0, 1.0, 1.0 }),
    _P(inb, CMYK, {-0.2, -0.2, -0.2, -0.2, -0.2 }, CMYK, { 0.0, 0.0, 0.0, 0.0, 0.0 }),
    _P(inb, CMYK, { 0.0,  0.0,  0.0,  0.0,  0.0 }, CMYK, { 0.0, 0.0, 0.0, 0.0, 0.0 }),
    _P(inb, CMYK, { 1.0,  1.0,  1.0,  1.0,  1.0 }, CMYK, { 1.0, 1.0, 1.0, 1.0, 1.0 })
));
// clang-format on

TEST(ColorsSpacesCmyk, components)
{
    auto c = Manager::get().find(CMYK)->getComponents();
    ASSERT_EQ(c.size(), 4);
    ASSERT_EQ(c[0].id, "c");
    ASSERT_EQ(c[1].id, "m");
    ASSERT_EQ(c[2].id, "y");
    ASSERT_EQ(c[3].id, "k");
}

TEST_F(ColorsSpacesCmykProtected, overInk)
{
    ASSERT_TRUE(overInk({1.0, 1.0, 1.0, 0.21}));
    ASSERT_FALSE(overInk({0.0, 1.0, 1.0, 0.19}));
}

// PROOF: DeviceCMYK shown through a CMYK profile, as Illustrator shows CMYK paint in an RGB
// document through its working CMYK space. The numbers and what is written stay; the colour is
// that of the same numbers in an icc-color() of the profile; without a profile, the formula.
TEST(ColorsSpacesCmyk, shownThroughProfile)
{
    auto profile = CMS::Profile::create_from_uri(INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc");
    ASSERT_TRUE(profile);
    // The library's space: Manager::get() is inline, and a test program linked to the library
    // as a DLL has a manager of its own.
    auto device = std::dynamic_pointer_cast<Space::DeviceCMYK>(Color(CMYK, {0, 0, 0, 0}).getSpace());
    ASSERT_TRUE(device);
    auto const managed = std::make_shared<Space::CMS>(profile);
    managed->setIntent(RenderingIntent::RELATIVE_COLORIMETRIC);
    device->setProfile(profile, RenderingIntent::RELATIVE_COLORIMETRIC);
    // The space is shared by every test in this program.
    struct Formula
    {
        std::shared_ptr<Space::DeviceCMYK> device;
        ~Formula() { device->setProfile({}, RenderingIntent::UNKNOWN); }
    } formula{device};

    for (auto const &values : std::vector<std::vector<double>>{
             {1, 0, 0, 0}, {0, 1, 1, 0}, {0.3, 0.6, 0.6, 0.1}, {0, 0, 0, 1}, {0, 0, 0, 0}}) {
        Color const shown(CMYK, values);
        EXPECT_EQ(Hex(shown.toRGBA()), Hex(Color(managed, values).toRGBA())) << shown.toString();
    }
    Color const cyan(CMYK, {1, 0, 0, 0});
    EXPECT_NE(Hex(cyan.toRGBA()), Hex(0x00ffffff));
    EXPECT_EQ(cyan.toString(), "device-cmyk(1 0 0 0)");
    EXPECT_EQ(cyan.getSpace(), device);
    EXPECT_EQ(Hex(Color(CMYK, {1, 0, 0, 0, 0.5}).toRGBA()), Hex((cyan.toRGBA() & 0xffffff00) | 0x80));

    // Into it, the profile's separation: the same colour, a step or two apart at most.
    auto const rgb = Color(CMYK, {0.3, 0.6, 0.6, 0.1}).converted(RGB);
    ASSERT_TRUE(rgb);
    auto const back = rgb->converted(CMYK);
    ASSERT_TRUE(back);
    ASSERT_EQ(back->size(), 4u);
    auto const again = back->converted(RGB);
    ASSERT_TRUE(again);
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_NEAR((*again)[i], (*rgb)[i], 2.5 / 255) << i;
    }

    device->setProfile({}, RenderingIntent::UNKNOWN);
    EXPECT_EQ(Hex(cyan.toRGBA()), Hex(0x00ffffff));
    // An RGB profile isn't taken.
    device->setProfile(CMS::Profile::create_srgb(), RenderingIntent::RELATIVE_COLORIMETRIC);
    EXPECT_EQ(Hex(cyan.toRGBA()), Hex(0x00ffffff));
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
