// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for color objects.
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include "colors/cms/profile.h"

static std::string grb_profile = INKSCAPE_TESTS_DIR "/data/colors/SwappedRedAndGreen.icc";
static std::string cmyk_profile = INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc";
static std::string not_a_profile = INKSCAPE_TESTS_DIR "/data/colors/color-cms.svg";

using namespace Inkscape::Colors;

namespace {

TEST(ColorCmsProfile, create)
{
    auto rgb_profile = cmsCreate_sRGBProfile();
    auto profile = CMS::Profile::create(rgb_profile, "", false);
    ASSERT_TRUE(profile);
    ASSERT_EQ(profile->getId(), "");
    ASSERT_EQ(profile->getName(), "sRGB built-in");
    ASSERT_EQ(profile->getPath(), "");
    ASSERT_FALSE(profile->inHome());
    ASSERT_EQ(profile->getHandle(), rgb_profile);
}

TEST(ColorCmsProfile, generatedNameIsCssIdentifier)
{
    auto handle = cmsCreate_sRGBProfile();
    auto description = cmsMLUalloc(nullptr, 1);
    ASSERT_TRUE(cmsMLUsetASCII(description, "en", "US", "Coated GRACoL 2006 (ISO 12647-2:2004).v1"));
    ASSERT_TRUE(cmsWriteTag(handle, cmsSigProfileDescriptionTag, description));
    cmsMLUfree(description);
    auto profile = CMS::Profile::create(handle, "", false);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->getName(true), "Coated-GRACoL-2006-ISO-12647-2-2004-v1");
}

TEST(ColorCmsProfile, create_from_uri)
{
    auto profile = CMS::Profile::create_from_uri(grb_profile);

    ASSERT_EQ(profile->getId(), "f9eda5a42a222a28f0adb82a938eeb0e");
    ASSERT_EQ(profile->getName(), "Swapped Red and Green");
    ASSERT_EQ(profile->getName(true), "Swapped-Red-and-Green");
    ASSERT_EQ(profile->getPath(), grb_profile);
    ASSERT_EQ(profile->getColorSpace(), cmsSigRgbData);
    ASSERT_EQ(profile->getProfileClass(), cmsSigDisplayClass);

    ASSERT_FALSE(profile->inHome());
    ASSERT_FALSE(profile->isForDisplay());
}

TEST(ColorCmsProfile, create_from_data)
{
    // Prepare some memory first
    cmsUInt32Number len = 0;
    auto rgb_profile = cmsCreate_sRGBProfile();
    ASSERT_TRUE(cmsSaveProfileToMem(rgb_profile, nullptr, &len));
    auto buf = std::vector<unsigned char>(len);
    cmsSaveProfileToMem(rgb_profile, &buf.front(), &len);
    std::string data(buf.begin(), buf.end());

    auto profile = CMS::Profile::create_from_data(data);
    ASSERT_TRUE(profile);
    cmsCloseProfile(rgb_profile);
}

TEST(ColorCmsProfile, create_srgb)
{
    auto profile = CMS::Profile::create_srgb();
    ASSERT_TRUE(profile);
}

TEST(ColorCmsProfile, equalTo)
{
    auto profile1 = CMS::Profile::create_from_uri(grb_profile);
    auto profile2 = CMS::Profile::create_from_uri(grb_profile);
    auto profile3 = CMS::Profile::create_from_uri(cmyk_profile);
    ASSERT_EQ(*profile1, *profile2);
    ASSERT_NE(*profile1, *profile3);
}

TEST(ColorCmsProfile, isIccFile)
{
    ASSERT_TRUE(CMS::Profile::isIccFile(grb_profile));
    ASSERT_FALSE(CMS::Profile::isIccFile(not_a_profile));
    ASSERT_FALSE(CMS::Profile::isIccFile("not_existing.icc"));
}

TEST(ColorCmsProfile, cmsDumpBase64)
{
    auto profile = CMS::Profile::create_from_uri(grb_profile);
    // First 100 bytes taken from the base64 of the icc profile file on the command line
    ASSERT_EQ(profile->dumpBase64().substr(0, 100),
              "AAA9aGxjbXMEMAAAbW50clJHQiBYWVogB+YAAgAWAA0AGQAuYWNzcEFQUEwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPbWAAEA");
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
