// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a Photoshop file's merged image (display/psd-pixels.h), for the screen and for print.
 */

#include "display/psd-pixels.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "colors/cms/profile.h"
#include "display/cairo-utils.h"
#include "psd-fixture.h"

using namespace Inkscape;
using namespace PsdFixture;

namespace {

constexpr char const *GRACOL = "C:/Windows/System32/spool/drivers/color/CoatedGRACoL2006.icc";

/// Four pixels of CMYK inks with their transparency, as Photoshop keeps them.
std::vector<std::string> cmyk_planes(std::vector<std::array<unsigned, 4>> const &inks, std::vector<unsigned> const &alpha)
{
    std::vector<std::vector<unsigned>> samples(5);
    for (std::size_t i = 0; i < inks.size(); ++i) {
        for (unsigned k = 0; k < 4; ++k) samples[k].push_back(stored_ink(inks[i][k], alpha[i]));
        samples[4].push_back(alpha[i]);
    }
    std::vector<std::string> planes;
    for (auto const &s : samples) planes.push_back(plane8(s));
    return planes;
}

std::uint32_t pixel(cairo_surface_t *s, int x, int y)
{
    auto const *row = cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s);
    return reinterpret_cast<std::uint32_t const *>(row)[x];
}

} // namespace

TEST(PsdPixels, CmykWithTransparencyIsUnmattedAndReadAsInks)
{
    // Two opaque, one half there, one not there at all.
    std::vector<std::array<unsigned, 4>> const inks = {{10, 20, 30, 40}, {200, 100, 50, 0}, {200, 100, 50, 0}, {0, 0, 0, 0}};
    std::vector<unsigned> const alpha = {255, 255, 128, 0};
    for (unsigned compression : {0u, 1u}) {
        Psd p;
        p.compression = compression;
        auto const data = make_psd(p, cmyk_planes(inks, alpha));
        EXPECT_TRUE(looks_like_psd(data));
        EXPECT_TRUE(psd_readable(data));
        std::string error;
        auto px = read_psd(data, &error);
        ASSERT_TRUE(px) << error;
        EXPECT_EQ(px->model, RasterPixels::Model::CMYK);
        EXPECT_EQ(px->width, 4u);
        EXPECT_EQ(px->height, 1u);
        EXPECT_EQ(px->bits, 8u);
        EXPECT_TRUE(px->premultiplied);
        EXPECT_EQ(px->left_out, 0u);
        ASSERT_EQ(px->colour.size(), 16u);
        ASSERT_EQ(px->alpha.size(), 4u);
        for (unsigned i = 0; i < 4; ++i) EXPECT_EQ(px->alpha[i], alpha[i]);
        for (unsigned k = 0; k < 4; ++k) {
            EXPECT_EQ(px->colour[k], inks[0][k]) << "opaque: its inks as they are";
            EXPECT_EQ(px->colour[4 + k], inks[1][k]);
            EXPECT_NEAR(px->colour[8 + k], inks[2][k], 1) << "half there: the matte divided back out";
            EXPECT_EQ(px->colour[12 + k], 0u);
        }
    }
}

TEST(PsdPixels, SixteenBitRgbOfALargeDocumentWithItsLayersInATaggedBlock)
{
    // A .psb at 16 bits: its layers, and the mark of the merged transparency, in "Lr16".
    std::vector<std::array<unsigned, 3>> const rgb = {{65535, 0, 0}, {0, 0, 65535}, {65535, 65535, 65535}, {0, 0, 0}};
    std::vector<unsigned> const alpha = {65535, 16384, 65535, 0};
    std::vector<std::vector<unsigned>> samples(4);
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        for (unsigned k = 0; k < 3; ++k) {
            // Matted against white.
            auto const a = std::uint64_t(alpha[i]);
            samples[k].push_back(unsigned((rgb[i][k] * a + 65535 * (65535 - a) + 32767) / 65535));
        }
        samples[3].push_back(alpha[i]);
    }
    std::vector<std::string> planes;
    for (auto const &s : samples) planes.push_back(plane16(s));
    for (unsigned compression : {0u, 1u}) {
        Psd p;
        p.version = 2;
        p.mode = 3;
        p.depth = 16;
        p.w = 2;
        p.h = 2;
        p.channels = 4;
        p.layers_tagged = true;
        p.compression = compression;
        std::string error;
        auto px = read_psd(make_psd(p, planes), &error);
        ASSERT_TRUE(px) << error;
        EXPECT_EQ(px->model, RasterPixels::Model::RGB);
        EXPECT_EQ(px->bits, 16u);
        auto const *colour = reinterpret_cast<std::uint16_t const *>(px->colour.data());
        auto const *a = reinterpret_cast<std::uint16_t const *>(px->alpha.data());
        ASSERT_EQ(px->alpha.size(), 8u);
        for (unsigned i = 0; i < 4; ++i) EXPECT_EQ(a[i], alpha[i]);
        for (unsigned k = 0; k < 3; ++k) {
            EXPECT_EQ(colour[k], rgb[0][k]);
            EXPECT_NEAR(colour[3 + k], rgb[1][k], 4) << "a quarter there: unmatted from white";
            EXPECT_EQ(colour[6 + k], rgb[2][k]);
        }
    }
}

TEST(PsdPixels, ItsProfileIsKeptAndChannelsThatArentItsTransparencyLeftOut)
{
    std::vector<std::array<unsigned, 4>> const inks = {{10, 20, 30, 40}, {0, 0, 0, 255}, {0, 0, 0, 0}, {255, 0, 0, 0}};
    auto planes = cmyk_planes(inks, {255, 255, 255, 255});
    planes.push_back(plane8({0, 255, 0, 255})); // a spot channel
    Psd p;
    p.channels = 6;
    p.icc = "seventeen bytes!!"; // an odd length: its resource is padded
    auto const data = make_psd(p, planes);
    auto px = read_psd(data);
    ASSERT_TRUE(px);
    EXPECT_EQ(px->profile, p.icc);
    EXPECT_EQ(cmyk_psd_profile(data), p.icc);
    EXPECT_EQ(px->left_out, 1u);
    EXPECT_EQ(px->alpha.size(), 4u);
    // Unmarked, the extra channels are no transparency (saved selections, spot channels).
    p.layer_count = 0;
    px = read_psd(make_psd(p, planes));
    ASSERT_TRUE(px);
    EXPECT_TRUE(px->alpha.empty());
    EXPECT_FALSE(px->premultiplied);
    EXPECT_EQ(px->left_out, 2u);
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned k = 0; k < 4; ++k) EXPECT_EQ(px->colour[i * 4 + k], inks[i][k]);
    }
}

TEST(PsdPixels, KindsNotReadSayWhy)
{
    auto const planes = cmyk_planes({{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, {255, 255, 255, 255});
    auto refused = [&](Psd const &p, char const *why) {
        auto const data = make_psd(p, planes);
        std::string error;
        EXPECT_FALSE(psd_readable(data, &error));
        EXPECT_NE(error.find(why), std::string::npos) << error;
        error.clear();
        EXPECT_FALSE(read_psd(data, &error));
        EXPECT_NE(error.find(why), std::string::npos) << error;
    };
    Psd p;
    p.merged = false;
    refused(p, "Maximize Compatibility");
    p = Psd();
    p.mode = 9;
    refused(p, "Lab");
    p = Psd();
    p.depth = 32;
    refused(p, "8 or 16 bits");
    p = Psd();
    p.compression = 2;
    refused(p, "ZIP");
    // Cut short, and damaged.
    auto data = make_psd(Psd(), planes);
    std::string error;
    EXPECT_FALSE(read_psd(data.substr(0, data.size() - 3), &error));
    EXPECT_FALSE(error.empty());
    data[data.size() - 2] = char(0x80 - 1); // a literal run longer than its row
    error.clear();
    EXPECT_FALSE(read_psd(data, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(looks_like_psd(std::string("II*\0 not one", 12)));
    error.clear();
    EXPECT_FALSE(read_psd(std::string(40, 'x'), &error));
    EXPECT_NE(error.find("isn't a Photoshop file"), std::string::npos);
    Psd rgb;
    rgb.mode = 3;
    rgb.channels = 3;
    rgb.layer_count = 0;
    EXPECT_FALSE(cmyk_psd_profile(make_psd(rgb, {plane8({0, 0, 0, 0}), plane8({0, 0, 0, 0}), plane8({0, 0, 0, 0})})));
}

TEST(PsdPixels, APixbufShowsCmykThroughTheDocumentsProfileWithItsTransparency)
{
    auto gracol = Colors::CMS::Profile::create_from_uri(GRACOL);
    if (!gracol) GTEST_SKIP() << "CoatedGRACoL2006.icc not installed";
    // No ink; full black; nothing at all; cyan, half there.
    auto const data = make_psd(Psd(), cmyk_planes({{0, 0, 0, 0}, {0, 0, 0, 255}, {0, 0, 0, 0}, {255, 0, 0, 0}},
                                                  {255, 255, 0, 128}));
    std::unique_ptr<Pixbuf> pb(Pixbuf::create_from_buffer(data, 0, "picture.psd", gracol));
    ASSERT_TRUE(pb);
    EXPECT_EQ(pb->width(), 4);
    EXPECT_EQ(pb->height(), 1);
    auto *surface = pb->getSurfaceRaw();
    auto const white = pixel(surface, 0, 0);
    EXPECT_EQ(white >> 24, 255u);
    EXPECT_GT((white >> 16) & 0xff, 235u);
    EXPECT_LT((pixel(surface, 1, 0) >> 16) & 0xff, 70u);
    EXPECT_EQ(pixel(surface, 2, 0), 0u) << "transparent, not a white box";
    auto const half = pixel(surface, 3, 0);
    EXPECT_EQ(half >> 24, 128u);
    EXPECT_LT((half >> 16) & 0xff, half & 0xff) << "cyan: less red than blue";
}

TEST(PsdPixels, RgbAndGrayShowTheirValues)
{
    Psd p;
    p.mode = 3;
    p.channels = 3;
    p.w = 2;
    p.layer_count = 0;
    auto const rgb = make_psd(p, {plane8({255, 10}), plane8({0, 20}), plane8({0, 30})});
    std::unique_ptr<Pixbuf> pb(Pixbuf::create_from_buffer(rgb, 0, "picture.psd"));
    ASSERT_TRUE(pb);
    EXPECT_EQ(pixel(pb->getSurfaceRaw(), 0, 0), 0xffff0000u);
    EXPECT_EQ(pixel(pb->getSurfaceRaw(), 1, 0), 0xff0a141eu);
    p.mode = 1;
    p.channels = 1;
    std::unique_ptr<Pixbuf> gray(Pixbuf::create_from_buffer(make_psd(p, {plane8({0, 200})}), 0, "picture.psd"));
    ASSERT_TRUE(gray);
    EXPECT_EQ(pixel(gray->getSurfaceRaw(), 1, 0), 0xffc8c8c8u);
}
