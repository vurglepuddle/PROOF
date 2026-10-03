// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for tracking icc profiles in a document.
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/document-cms.h"

#include <gtest/gtest.h>

#include "colors/color.h"
#include "colors/manager.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "inkscape.h"
#include "object/color-profile.h"
#include "object/sp-stop.h"

static std::string icc_dir = INKSCAPE_TESTS_DIR "/data/colors/";
static std::string svg_objs_file = INKSCAPE_TESTS_DIR "/data/colors/cms-in-objs.svg";
static std::string svg_defs_file = INKSCAPE_TESTS_DIR "/data/colors/cms-in-defs.svg";
static std::string grb_profile = INKSCAPE_TESTS_DIR "/data/colors/SwappedRedAndGreen.icc";

using namespace Inkscape;
using namespace Inkscape::Colors;

namespace {

class ColorDocumentCMSObjsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Setup inkscape dependency
        Inkscape::Application::create(false);

        // Allow lookup by ID and name with test icc profiles
        auto &cms = Inkscape::Colors::CMS::System::get();
        cms.clearDirectoryPaths();
        cms.addDirectoryPath(icc_dir, false);
        cms.refreshProfiles();

        // Load the test svg file with a bunch of icc profiles
        doc = SPDocument::createNewDoc(getDocFilename().c_str());
    }
    std::unique_ptr<SPDocument> doc;

    virtual std::string const &getDocFilename() const
    {
        return svg_objs_file;
    }
};

TEST_F(ColorDocumentCMSObjsTest, loadDocument)
{
    auto &cm = Manager::get();
    auto &tr = doc->getDocumentCMS();

    ASSERT_FALSE(tr.getSpace("nonsense"));

    // Internal spaces
    ASSERT_TRUE(cm.find(Space::Type::CSSNAME));
    ASSERT_TRUE(cm.find(Space::Type::RGB));
    ASSERT_TRUE(cm.find(Space::Type::HSL));

    // Document icc profiles
    ASSERT_TRUE(tr.getSpace("grb"));
    ASSERT_TRUE(tr.getSpace("cmyk-rcm"));
    ASSERT_TRUE(tr.getSpace("cmyk-acm"));

    ASSERT_TRUE(cm.find(Space::Type::CMYK));
    ASSERT_EQ(cm.find(Space::Type::CMYK)->getName(), "DeviceCMYK");
    ASSERT_EQ(cm.find(Space::Type::RGB)->getName(), "RGB");
}

TEST_F(ColorDocumentCMSObjsTest, updateIntent)
{
    auto &tr = doc->getDocumentCMS();
    auto space = tr.getSpace("grb");
    ASSERT_TRUE(space);

    auto cp = tr.getColorProfileForSpace(space);
    ASSERT_TRUE(cp);

    ASSERT_EQ(space->getIntent(), RenderingIntent::PERCEPTUAL);
    ASSERT_EQ(cp->getRenderingIntent(), RenderingIntent::UNKNOWN);
    ASSERT_EQ(cp->getAttribute("rendering-intent"), nullptr);
    tr.setRenderingIntent("grb", RenderingIntent::PERCEPTUAL);
    ASSERT_EQ(space->getIntent(), RenderingIntent::PERCEPTUAL);
    ASSERT_EQ(cp->getRenderingIntent(), RenderingIntent::PERCEPTUAL);
    ASSERT_EQ(std::string(cp->getAttribute("rendering-intent")), "perceptual");

    space = tr.getSpace("cmyk-acm");
    ASSERT_EQ(space->getIntent(), RenderingIntent::ABSOLUTE_COLORIMETRIC);

    space = tr.getSpace("cmyk-rcm");
    ASSERT_EQ(space->getIntent(), RenderingIntent::RELATIVE_COLORIMETRIC);
}

TEST_F(ColorDocumentCMSObjsTest, checkProfileName)
{
    auto &tr = doc->getDocumentCMS();
    auto space = tr.getSpace("grb");
    auto profile = space->getProfile();
    {
        auto [name, exists] = tr.checkProfileName(*profile, RenderingIntent::PERCEPTUAL, "grb");
        EXPECT_TRUE(exists);
        EXPECT_EQ(name, "grb");
    }
    {
        auto [name, exists] = tr.checkProfileName(*profile, RenderingIntent::PERCEPTUAL);
        EXPECT_FALSE(exists);
        EXPECT_EQ(name, "Swapped-Red-and-Green");
    }
    {
        auto [name, exists] = tr.checkProfileName(*profile, RenderingIntent::AUTO, "grb");
        EXPECT_FALSE(exists);
        EXPECT_EQ(name, "Swapped-Red-and-Green");
    }
    auto old = tr.attachProfileToDoc(*profile, ColorProfileStorage::LOCAL_ID, RenderingIntent::PERCEPTUAL);
    ASSERT_TRUE(tr.getSpace(old));
    {
        auto [name, exists] = tr.checkProfileName(*profile, RenderingIntent::PERCEPTUAL);
        EXPECT_TRUE(exists);
        EXPECT_EQ(name, old);
    }
    {
        auto [name, exists] = tr.checkProfileName(*profile, RenderingIntent::PERCEPTUAL, old);
        EXPECT_TRUE(exists);
        EXPECT_EQ(name, old);
    }
    {
        auto [name, exists] = tr.checkProfileName(*profile, RenderingIntent::AUTO);
        EXPECT_FALSE(exists);
        EXPECT_EQ(name, "Swapped-Red-and-Green-auto");
    }
    {
        auto [name, exists] = tr.checkProfileName(*profile, RenderingIntent::AUTO, old);
        EXPECT_FALSE(exists);
        EXPECT_EQ(name, "Swapped-Red-and-Green-auto");
    }
}

TEST_F(ColorDocumentCMSObjsTest, createColorProfile)
{
    auto &tr = doc->getDocumentCMS();
    ASSERT_FALSE(tr.getSpace("C-icc"));

    // PROOF: document names are CSS-safe, so the profile "C.icc" is attached as "C-icc".
    EXPECT_EQ(tr.attachProfileToDoc("C.icc", ColorProfileStorage::LOCAL_ID, RenderingIntent::AUTO), "C-icc");
    ASSERT_TRUE(tr.getSpace("C-icc"));
    auto space = tr.getSpace("C-icc");

    ASSERT_TRUE(space);
    ASSERT_EQ(space->getIntent(), RenderingIntent::AUTO);
}

TEST_F(ColorDocumentCMSObjsTest, deleteColorProfile)
{
    auto &tr = doc->getDocumentCMS();
    auto cp0 = doc->getObjectById("cp2");
    ASSERT_TRUE(cp0);

    ASSERT_TRUE(tr.getSpace("cmyk-rcm"));
    auto cp = tr.getColorProfileForSpace("cmyk-rcm");
    ASSERT_TRUE(cp);
    cp->deleteObject();
    ASSERT_FALSE(tr.getSpace("cmyk-rcm"));
}

TEST_F(ColorDocumentCMSObjsTest, cmsAddMultiple)
{
    auto &tr = doc->getDocumentCMS();
    auto space = tr.getSpace("grb");
    ASSERT_TRUE(space);
    EXPECT_EQ(space->getType(), Space::Type::CMS);
    ASSERT_EQ(space->getComponentType(), Space::Type::RGB);
    EXPECT_THROW(tr.addProfileURI(grb_profile, "grb", RenderingIntent::RELATIVE_COLORIMETRIC), ColorError);
}

TEST_F(ColorDocumentCMSObjsTest, cmsParsingRGB)
{
    auto &tr = doc->getDocumentCMS();

    auto space = tr.getSpace("grb");
    ASSERT_TRUE(space);
    EXPECT_TRUE(space->hasValidCmsProfile());
    EXPECT_EQ(space->getType(), Space::Type::CMS);
    EXPECT_EQ(space->getComponentType(), Space::Type::RGB);

    auto color = *tr.parse("#000001 icc-color(grb, 1, 0.8, 0.6)");
    EXPECT_EQ(color.toString(), "#ccff99 icc-color(grb, 1, 0.8, 0.6)");
    EXPECT_EQ(color.toRGBA(), 0xccff99ff);
    EXPECT_EQ(*color.getSpace(), *space);
}

TEST_F(ColorDocumentCMSObjsTest, fallbackColor)
{
    auto tr = DocumentCMS(nullptr);

    auto color = *tr.parse("#0080ff icc-color(missing-profile, 1, 2, 3)");
    EXPECT_EQ(color.toString(), "#0080ff icc-color(missing-profile, 1, 2, 3)");
    EXPECT_EQ(color.toRGBA(), 0x0080ffff);
    EXPECT_EQ(color.toRGBA(0.5), 0x0080ff80);
    EXPECT_EQ(color.converted(Space::Type::RGB)->toString(), "#0080ff");
    EXPECT_FALSE(color.hasOpacity());
    EXPECT_EQ(color.getOpacity(), 1.0);

    color.addOpacity(0.5);
    EXPECT_EQ(color.toString(), "#0080ff icc-color(missing-profile, 1, 2, 3)");
    EXPECT_EQ(color.toRGBA(), 0x0080ff80);
    EXPECT_EQ(color.toRGBA(0.5), 0x0080ff40);
}

TEST_F(ColorDocumentCMSObjsTest, cmsParsingCMYK1)
{
    auto &tr = doc->getDocumentCMS();

    auto space = tr.getSpace("cmyk-rcm");
    ASSERT_TRUE(space);
    EXPECT_TRUE(space->hasValidCmsProfile());
    EXPECT_EQ(space->getType(), Space::Type::CMS);
    EXPECT_EQ(space->getComponentType(), Space::Type::CMYK);

    auto color = *tr.parse("#000002 icc-color(cmyk-rcm, 0.5, 0, 0, 0)");
    EXPECT_EQ(color.toString(), "#6dcff6 icc-color(cmyk-rcm, 0.5, 0, 0, 0)");
    EXPECT_EQ(color.toRGBA(), 0x6dcff6ff);
    EXPECT_EQ(*color.getSpace(), *space);
}

TEST_F(ColorDocumentCMSObjsTest, cmsParsingCMYK2)
{
    auto &tr = doc->getDocumentCMS();

    auto space = tr.getSpace("cmyk-acm");
    ASSERT_TRUE(space);
    EXPECT_TRUE(space->hasValidCmsProfile());
    EXPECT_EQ(space->getType(), Space::Type::CMS);
    EXPECT_EQ(space->getComponentType(), Space::Type::CMYK);

    auto color = *tr.parse("#000003 icc-color(cmyk-acm, 0.5, 0, 0, 0)");
    EXPECT_EQ(color.toString(), "#69b6d1 icc-color(cmyk-acm, 0.5, 0, 0, 0)");
    EXPECT_EQ(color.toRGBA(), 0x69b6d1ff);
    EXPECT_EQ(*color.getSpace(), *space);
}

TEST_F(ColorDocumentCMSObjsTest, applyConversion)
{
    auto &tr = doc->getDocumentCMS();
    auto grb = tr.getSpace("grb");
    auto rcm = tr.getSpace("cmyk-rcm");
    auto acm = tr.getSpace("cmyk-acm");
    ASSERT_TRUE(grb);
    ASSERT_TRUE(rcm);
    ASSERT_TRUE(acm);

    auto color = *Color::parse("red");
    ASSERT_EQ(color.toString(), "red");

    // Converting an anonymous color fails
    auto other = tr.parse("icc-color(bad, 1.0, 0.8, 0.6)");
    color.convert(other->getSpace());
    ASSERT_EQ(color.toString(), "red");
    ASSERT_FALSE(color.converted(other->getSpace()));

    // Specifying the space properly works
    EXPECT_EQ(color.converted(grb)->toString(), "#ff0000 icc-color(grb, 0, 1, 0)");

    // Double conversion does nothing
    color.convert(grb);
    color.convert(grb);
    EXPECT_EQ(color.toString(), "#ff0000 icc-color(grb, 0, 1, 0)");

    // Converting from one icc profile to another is possible
    EXPECT_EQ(color.converted(rcm)->toString(), "#ed1d24 icc-color(cmyk-rcm, 0, 0.998, 1, 0)");
    // Same icc profile should keep the same cmyk values, but
    // because the render intent is different the RGB changes
    EXPECT_EQ(color.converted(acm)->toString(), "#cf2c2d icc-color(cmyk-acm, 0, 1, 1, 0)");
}

TEST_F(ColorDocumentCMSObjsTest, findSvgAttribute)
{
    auto &tr = doc->getDocumentCMS();
    // ICC color profile spaces
    EXPECT_TRUE(tr.findSvgColorSpace("grb"));
    EXPECT_TRUE(tr.findSvgColorSpace("cmyk-rcm"));
    EXPECT_FALSE(tr.findSvgColorSpace("cmyk-nope"));
    // Non ICC profile color spaces
    EXPECT_TRUE(tr.findSvgColorSpace("sRGB"));
    EXPECT_TRUE(tr.findSvgColorSpace("linearRGB"));
}

class ColorDocumentCMSDefsTest : public ColorDocumentCMSObjsTest
{
    std::string const &getDocFilename() const override
    {
        return svg_defs_file;
    }
};

TEST_F(ColorDocumentCMSDefsTest, loadDocument)
{
    auto &tracker = doc->getDocumentCMS();
    auto spaces = tracker.getSpaces();

    ASSERT_EQ(spaces.size(), 1);
    ASSERT_EQ(spaces[0]->getName(), "Artifex-CMYK-SWOP-Profile");
    ASSERT_TRUE(spaces[0]->hasValidCmsProfile());

    auto objects = tracker.getObjects();
    ASSERT_EQ(objects.size(), 1);
    ASSERT_EQ(std::string(objects[0]->getId()), "artefact-cmyk");

    auto stop = cast<SPStop>(doc->getObjectById("stop2212"));
    ASSERT_TRUE(stop);
    auto color = stop->getColor();

    // Test the expected values actually return
    std::string expected = "#2c292a icc-color(Artifex-CMYK-SWOP-Profile, 0, 0, 0, 1)";
    EXPECT_TRUE(tracker.parse(expected)->getSpace()->hasValidCmsProfile());
    EXPECT_EQ(tracker.parse(expected)->toString(), expected);
    EXPECT_EQ(color.toString(), expected);

    auto space = dynamic_pointer_cast<Space::CMS>(color.getSpace());
    EXPECT_TRUE(space->hasValidCmsProfile());
    EXPECT_EQ(space, spaces[0]);
    EXPECT_NEAR(color[0], 0, 0.01);
    EXPECT_NEAR(color[1], 0, 0.01);
    EXPECT_NEAR(color[2], 0, 0.01);
    EXPECT_NEAR(color[3], 1, 0.01);
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
