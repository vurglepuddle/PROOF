// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a TIFF's own pixels (display/tiff-pixels.h), for the screen and for print.
 */

#include "display/tiff-pixels.h"

#include <cstdint>
#include <cstring>
#include <glib.h>
#include <glib/gstdio.h>
#include <memory>
#include <gtest/gtest.h>
#include <string>
#include <tiffio.h>
#include <vector>

#include "colors/cms/profile.h"
#include "display/cairo-utils.h"

using namespace Inkscape;

namespace {

struct Shape
{
    std::uint32_t w = 4, h = 2;
    std::uint16_t photometric = PHOTOMETRIC_SEPARATED;
    std::uint16_t bits = 8;
    std::uint16_t spp = 4;
    std::vector<std::uint16_t> extras;
    bool tiled = false;
    bool lzw = true; // with the horizontal predictor, as Photoshop writes
    std::uint16_t planar = PLANARCONFIG_CONTIG;
    std::string icc;
};

/// A TIFF of `samples` (every sample of a pixel together; 16 bits in native order), as bytes.
std::string make_tiff(Shape const &s, std::vector<std::uint8_t> const &samples)
{
    gchar *path = nullptr;
    int fd = g_file_open_tmp("proof-tiff-XXXXXX.tif", &path, nullptr);
    EXPECT_GE(fd, 0);
    g_close(fd, nullptr);
    TIFF *t = TIFFOpen(path, "w");
    EXPECT_NE(t, nullptr);
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, s.w);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, s.h);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, s.bits);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, s.spp);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, s.photometric);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, s.planar);
    if (s.photometric == PHOTOMETRIC_SEPARATED) TIFFSetField(t, TIFFTAG_INKSET, INKSET_CMYK);
    if (!s.extras.empty()) TIFFSetField(t, TIFFTAG_EXTRASAMPLES, std::uint16_t(s.extras.size()), s.extras.data());
    if (s.lzw) {
        TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
        TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    }
    if (!s.icc.empty()) TIFFSetField(t, TIFFTAG_ICCPROFILE, std::uint32_t(s.icc.size()), s.icc.data());
    std::size_t const pixel = std::size_t(s.spp) * s.bits / 8;
    if (s.tiled) {
        TIFFSetField(t, TIFFTAG_TILEWIDTH, 16);
        TIFFSetField(t, TIFFTAG_TILELENGTH, 16);
        std::vector<std::uint8_t> tile(16 * 16 * pixel, 0);
        for (std::uint32_t y = 0; y < s.h; y += 16) {
            for (std::uint32_t x = 0; x < s.w; x += 16) {
                std::fill(tile.begin(), tile.end(), 0);
                for (std::uint32_t r = 0; r < 16 && y + r < s.h; ++r) {
                    for (std::uint32_t c = 0; c < 16 && x + c < s.w; ++c) {
                        std::memcpy(tile.data() + (r * 16 + c) * pixel,
                                    samples.data() + ((y + r) * s.w + x + c) * pixel, pixel);
                    }
                }
                TIFFWriteTile(t, tile.data(), x, y, 0, 0);
            }
        }
    } else {
        TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, 1);
        std::vector<std::uint8_t> row(s.w * pixel);
        for (std::uint32_t y = 0; y < s.h; ++y) {
            std::memcpy(row.data(), samples.data() + y * s.w * pixel, row.size());
            TIFFWriteScanline(t, row.data(), y, 0);
        }
    }
    TIFFClose(t);
    gchar *data = nullptr;
    gsize len = 0;
    EXPECT_TRUE(g_file_get_contents(path, &data, &len, nullptr));
    std::string out(data, len);
    g_free(data);
    g_unlink(path);
    g_free(path);
    return out;
}

std::vector<std::uint8_t> bytes16(std::vector<std::uint16_t> const &v)
{
    std::vector<std::uint8_t> out(v.size() * 2);
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}

std::uint16_t at16(std::vector<std::uint8_t> const &v, std::size_t i)
{
    std::uint16_t x;
    std::memcpy(&x, v.data() + 2 * i, 2);
    return x;
}

std::string icc_of(char const *path)
{
    gchar *data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, nullptr)) return {};
    std::string out(data, len);
    g_free(data);
    return out;
}

constexpr char const *GRACOL = "C:/Windows/System32/spool/drivers/color/CoatedGRACoL2006.icc";
constexpr char const *FOGRA = "C:/Windows/System32/spool/drivers/color/CoatedFOGRA39.icc";

} // namespace

TEST(TiffPixels, CmykWithPremultipliedTransparencyIsReadStraight)
{
    // As Photoshop keeps a cut-out CMYK picture: the inks multiplied by the alpha.
    Shape s;
    s.spp = 5;
    s.extras.push_back(EXTRASAMPLE_ASSOCALPHA);
    std::vector<std::uint8_t> px = {
        0, 57, 35, 0, 58,     0, 250, 153, 0, 255,  0, 0, 0, 0, 0,  10, 20, 30, 40, 255,
        0, 118, 72, 0, 120,   0, 22, 13, 0, 22,     0, 0, 0, 0, 0,  255, 255, 255, 255, 255,
    };
    auto const data = make_tiff(s, px);
    EXPECT_TRUE(looks_like_tiff(data));
    EXPECT_TRUE(is_cmyk_tiff(data));
    std::string error;
    auto t = read_tiff(data, &error);
    ASSERT_TRUE(t) << error;
    EXPECT_EQ(t->model, TiffPixels::Model::CMYK);
    EXPECT_EQ(t->width, 4u);
    EXPECT_EQ(t->height, 2u);
    EXPECT_EQ(t->bits, 8u);
    EXPECT_TRUE(t->premultiplied);
    EXPECT_EQ(t->left_out, 0u);
    ASSERT_EQ(t->colour.size(), 8u * 4);
    ASSERT_EQ(t->alpha.size(), 8u);
    // 57 of 58 is 250.6 of 255.
    EXPECT_EQ(std::vector<int>(t->colour.begin(), t->colour.begin() + 4), (std::vector<int>{0, 251, 154, 0}));
    EXPECT_EQ(t->alpha[0], 58);
    // Opaque pixels keep their numbers exactly; transparent ones have no ink.
    EXPECT_EQ(std::vector<int>(t->colour.begin() + 4, t->colour.begin() + 8), (std::vector<int>{0, 250, 153, 0}));
    EXPECT_EQ(std::vector<int>(t->colour.begin() + 8, t->colour.begin() + 12), (std::vector<int>{0, 0, 0, 0}));
    EXPECT_EQ(std::vector<int>(t->colour.begin() + 12, t->colour.begin() + 16), (std::vector<int>{10, 20, 30, 40}));
    EXPECT_EQ(std::vector<int>(t->colour.begin() + 16, t->colour.begin() + 20), (std::vector<int>{0, 251, 153, 0}));
    EXPECT_EQ(t->alpha[4], 120);
}

TEST(TiffPixels, SixteenBitTiledCmykKeepsEveryNumber)
{
    Shape s;
    s.w = 20; // more than one tile across
    s.h = 3;
    s.bits = 16;
    s.spp = 5;
    s.extras.push_back(EXTRASAMPLE_UNASSALPHA);
    s.tiled = true;
    s.lzw = false;
    std::vector<std::uint16_t> px;
    for (std::uint32_t i = 0; i < s.w * s.h; ++i) {
        px.insert(px.end(), {std::uint16_t(i * 1000), std::uint16_t(65535 - i), 12345, std::uint16_t(i), 32768});
    }
    auto t = read_tiff(make_tiff(s, bytes16(px)));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->bits, 16u);
    EXPECT_FALSE(t->premultiplied);
    for (std::uint32_t i = 0; i < s.w * s.h; ++i) {
        ASSERT_EQ(at16(t->colour, i * 4), std::uint16_t(i * 1000)) << i;
        ASSERT_EQ(at16(t->colour, i * 4 + 1), std::uint16_t(65535 - i)) << i;
        ASSERT_EQ(at16(t->colour, i * 4 + 3), std::uint16_t(i)) << i;
        ASSERT_EQ(at16(t->alpha, i), 32768) << i;
    }
}

TEST(TiffPixels, ChannelsThatArentItsTransparencyAreLeftOut)
{
    // A spot channel before the transparency, and a saved selection after it.
    Shape s;
    s.spp = 7;
    s.extras.push_back(EXTRASAMPLE_UNSPECIFIED);
    s.extras.push_back(EXTRASAMPLE_UNASSALPHA);
    s.extras.push_back(EXTRASAMPLE_UNSPECIFIED);
    std::vector<std::uint8_t> px(8 * 7, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        px[i * 7 + 1] = 100; // magenta
        px[i * 7 + 4] = 77;  // the spot channel
        px[i * 7 + 5] = 200; // transparency
        px[i * 7 + 6] = 9;   // selection
    }
    auto t = read_tiff(make_tiff(s, px));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->left_out, 2u);
    ASSERT_EQ(t->alpha.size(), 8u);
    EXPECT_EQ(t->alpha[3], 200);
    EXPECT_EQ(t->colour[3 * 4 + 1], 100);
}

TEST(TiffPixels, RgbAndGrayWithTransparency)
{
    Shape rgb;
    rgb.photometric = PHOTOMETRIC_RGB;
    rgb.spp = 4;
    rgb.extras.push_back(EXTRASAMPLE_UNASSALPHA);
    std::vector<std::uint8_t> px(8 * 4, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        px[i * 4] = 255;
        px[i * 4 + 3] = std::uint8_t(i * 30);
    }
    auto data = make_tiff(rgb, px);
    EXPECT_FALSE(is_cmyk_tiff(data));
    auto t = read_tiff(data);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->model, TiffPixels::Model::RGB);
    EXPECT_EQ(t->alpha[5], 150);
    EXPECT_EQ(t->colour[5 * 3], 255);

    Shape gray;
    gray.photometric = PHOTOMETRIC_MINISBLACK;
    gray.spp = 2;
    gray.extras.push_back(EXTRASAMPLE_ASSOCALPHA);
    std::vector<std::uint8_t> g(8 * 2);
    for (std::size_t i = 0; i < 8; ++i) {
        g[i * 2] = 50;
        g[i * 2 + 1] = 100;
    }
    t = read_tiff(make_tiff(gray, g));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->model, TiffPixels::Model::Gray);
    EXPECT_EQ(t->colour[2], 128); // 50 of 100 is half
}

TEST(TiffPixels, OtherKindsAreRefused)
{
    std::string error;
    EXPECT_FALSE(read_tiff("not a TIFF at all", &error));
    EXPECT_FALSE(error.empty());
    Shape planar;
    planar.planar = PLANARCONFIG_SEPARATE;
    planar.lzw = false;
    // Written with the samples a plane apart, which isn't read here.
    std::vector<std::uint8_t> px(8 * 4, 10);
    gchar *path = nullptr;
    int fd = g_file_open_tmp("proof-tiff-XXXXXX.tif", &path, nullptr);
    g_close(fd, nullptr);
    TIFF *t = TIFFOpen(path, "w");
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, 4);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, 2);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
    std::vector<std::uint8_t> row(4, 10);
    for (std::uint16_t plane = 0; plane < 4; ++plane) {
        for (std::uint32_t y = 0; y < 2; ++y) TIFFWriteScanline(t, row.data(), y, plane);
    }
    TIFFClose(t);
    gchar *data = nullptr;
    gsize len = 0;
    g_file_get_contents(path, &data, &len, nullptr);
    error.clear();
    EXPECT_FALSE(read_tiff(std::string_view(data, len), &error));
    EXPECT_FALSE(error.empty());
    g_free(data);
    g_unlink(path);
    g_free(path);
}

TEST(TiffPixels, CmykShowsThroughTheDocumentsProfileNotItsOwn)
{
    auto gracol = Colors::CMS::Profile::create_from_uri(GRACOL);
    auto const fogra_icc = icc_of(FOGRA);
    if (!gracol || fogra_icc.empty()) GTEST_SKIP() << "CoatedGRACoL2006.icc and CoatedFOGRA39.icc are needed";
    Shape s;
    s.spp = 5;
    s.extras.push_back(EXTRASAMPLE_ASSOCALPHA);
    // No ink, opaque; a grey-green mid-tone, opaque; nothing at all; cyan, a quarter there.
    std::vector<std::uint8_t> px = {
        0, 0, 0, 0, 255,  128, 60, 110, 40, 255,  0, 0, 0, 0, 0,  64, 0, 0, 0, 64,
        0, 0, 0, 255, 255, 0, 0, 0, 0, 255,  0, 0, 0, 0, 0,  0, 0, 0, 0, 255,
    };
    auto plain = read_tiff(make_tiff(s, px));
    s.icc = fogra_icc;
    auto tagged = read_tiff(make_tiff(s, px));
    ASSERT_TRUE(plain && tagged);
    EXPECT_EQ(tagged->profile, fogra_icc);
    auto *a = cmyk_tiff_surface(*plain, gracol);
    auto *b = cmyk_tiff_surface(*tagged, gracol);
    auto *own = cmyk_tiff_surface(*tagged, nullptr);
    ASSERT_TRUE(a && b && own);
    auto pixel = [](cairo_surface_t *s, int x, int y) {
        auto const *row = cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s);
        return reinterpret_cast<std::uint32_t const *>(row)[x];
    };
    // Preserve Numbers: the given profile, whatever the picture carries.
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) EXPECT_EQ(pixel(a, x, y), pixel(b, x, y)) << x << "," << y;
    }
    // With none given, its own: FOGRA39 shows a mid-tone otherwise than GRACoL.
    EXPECT_NE(pixel(own, 1, 0), pixel(a, 1, 0));
    // No ink is near white, full black is dark, nothing at all is transparent.
    auto const white = pixel(a, 0, 0);
    EXPECT_EQ(white >> 24, 255u);
    EXPECT_GT((white >> 16) & 0xff, 235u);
    EXPECT_GT(white & 0xff, 235u);
    EXPECT_LT((pixel(a, 0, 1) >> 16) & 0xff, 70u);
    EXPECT_EQ(pixel(a, 2, 0), 0u);
    // A quarter there: premultiplied, so no channel above the alpha, and cyan's red low.
    auto const quarter = pixel(a, 3, 0);
    EXPECT_EQ(quarter >> 24, 64u);
    EXPECT_LE((quarter >> 16) & 0xff, 64u);
    EXPECT_LT((quarter >> 16) & 0xff, quarter & 0xff);
    cairo_surface_destroy(a);
    cairo_surface_destroy(b);
    cairo_surface_destroy(own);
}

TEST(TiffPixels, APixbufOfACmykTiffKeepsItsTransparency)
{
    auto gracol = Colors::CMS::Profile::create_from_uri(GRACOL);
    if (!gracol) GTEST_SKIP() << "CoatedGRACoL2006.icc not installed";
    Shape s;
    s.spp = 5;
    s.extras.push_back(EXTRASAMPLE_ASSOCALPHA);
    std::vector<std::uint8_t> px(8 * 5, 0);
    px[4] = 255; // the first pixel opaque, no ink; the rest transparent
    auto const data = make_tiff(s, px);
    std::unique_ptr<Pixbuf> pb(Pixbuf::create_from_buffer(data, 0, "picture.tif", gracol));
    ASSERT_TRUE(pb);
    EXPECT_EQ(pb->width(), 4);
    EXPECT_EQ(pb->height(), 2);
    auto *surface = pb->getSurfaceRaw();
    auto const *row = reinterpret_cast<std::uint32_t const *>(cairo_image_surface_get_data(surface));
    EXPECT_EQ(row[0] >> 24, 255u);
    EXPECT_EQ(row[1], 0u); // transparent, not a white box
}
