// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a placed picture's own pixels, shown. See raster-pixels.h.
 */

#include "raster-pixels.h"

#include <lcms2.h>

#include "colors/cms/profile.h"
#include "colors/cms/transform.h"
#include "colors/document-colors.h"

namespace Inkscape {

cairo_surface_t *raster_surface(RasterPixels const &pixels, std::shared_ptr<Colors::CMS::Profile> profile,
                                Colors::RenderingIntent intent)
{
    bool const cmyk = pixels.model == RasterPixels::Model::CMYK;
    cmsHTRANSFORM transform = nullptr;
    if (cmyk) {
        // Preserve Numbers: the document's profile first; the picture's own, and the working
        // profile, only where the document has none.
        if (!profile && !pixels.profile.empty()) {
            try {
                profile = Colors::CMS::Profile::create_from_data(pixels.profile);
            } catch (...) {
                profile.reset();
            }
            if (profile && cmsGetColorSpace(profile->getHandle()) != cmsSigCmykData) profile.reset();
        }
        if (!profile) profile = Colors::DocumentColors::workingProfile(Colors::Space::Type::CMYK);
        if (!profile || !profile->getHandle()) return nullptr;
        auto srgb = Colors::CMS::Profile::create_srgb();
        transform = cmsCreateTransform(profile->getHandle(), pixels.bits == 16 ? TYPE_CMYK_16 : TYPE_CMYK_8,
                                       srgb->getHandle(), TYPE_RGB_8, Colors::CMS::Transform::lcms_intent(intent),
                                       Colors::CMS::Transform::lcms_bpc(intent));
        if (!transform) return nullptr;
    }
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(pixels.width), int(pixels.height));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (transform) cmsDeleteTransform(transform);
        cairo_surface_destroy(surface);
        return nullptr;
    }
    cairo_surface_flush(surface);
    auto *data = cairo_image_surface_get_data(surface);
    int const stride = cairo_image_surface_get_stride(surface);
    unsigned const channels = pixels.channels();
    std::size_t const row_bytes = std::size_t(pixels.width) * channels * pixels.bits / 8;
    // A sample at 8 bits.
    auto sample = [&](std::uint8_t const *row, std::size_t i) -> std::uint32_t {
        return pixels.bits == 16 ? reinterpret_cast<std::uint16_t const *>(row)[i] >> 8 : row[i];
    };
    std::vector<std::uint8_t> rgb(std::size_t(pixels.width) * 3);
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        auto const *in = pixels.colour.data() + y * row_bytes;
        if (transform) {
            cmsDoTransform(transform, in, rgb.data(), pixels.width);
        } else {
            for (std::uint32_t x = 0; x < pixels.width; ++x) {
                for (unsigned k = 0; k < 3; ++k) {
                    rgb[x * 3 + k] = std::uint8_t(sample(in, std::size_t(x) * channels + (channels == 1 ? 0 : k)));
                }
            }
        }
        auto *row = reinterpret_cast<std::uint32_t *>(data + std::size_t(y) * stride);
        for (std::uint32_t x = 0; x < pixels.width; ++x) {
            std::uint32_t a = 255;
            if (!pixels.alpha.empty()) {
                std::size_t const i = std::size_t(y) * pixels.width + x;
                a = pixels.bits == 16 ? reinterpret_cast<std::uint16_t const *>(pixels.alpha.data())[i] >> 8
                                      : pixels.alpha[i];
            }
            auto premultiply = [a](std::uint32_t v) { return (v * a + 127) / 255; };
            row[x] = (a << 24) | (premultiply(rgb[x * 3]) << 16) | (premultiply(rgb[x * 3 + 1]) << 8) |
                     premultiply(rgb[x * 3 + 2]);
        }
    }
    if (transform) cmsDeleteTransform(transform);
    cairo_surface_mark_dirty(surface);
    return surface;
}

} // namespace Inkscape
