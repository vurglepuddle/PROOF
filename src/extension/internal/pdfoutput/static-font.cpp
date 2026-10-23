// SPDX-License-Identifier: GPL-2.0-or-later
#include "static-font.h"

#include <cmath>
#include <stdexcept>
#include <vector>
#include <glib.h>
#include <glib/gstdio.h>
#include <hb-ot.h>
#include <hb-subset.h>

#include "libnrtype/font-instance.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

StaticFont::~StaticFont()
{
    g_unlink(path.c_str());
}

std::unique_ptr<StaticFont> static_font(FontInstance const &font)
{
    auto *loaded = pango_font_get_hb_font(font.get_font());
    auto *face = loaded ? hb_font_get_face(loaded) : nullptr;
    unsigned count = face ? hb_ot_var_get_axis_count(face) : 0, set = 0;
    if (!count || count > 64) throw std::runtime_error("Invalid variable font axis count");
    std::vector<hb_ot_var_axis_info_t> axes(count);
    hb_ot_var_get_axis_infos(face, 0, &count, axes.data());
    auto const *coords = hb_font_get_var_coords_design(loaded, &set);
    if (set && (set != count || !coords)) throw std::runtime_error("Incomplete loaded font coordinates");

    auto input = std::unique_ptr<hb_subset_input_t, decltype(&hb_subset_input_destroy)>(
        hb_subset_input_create_or_fail(), hb_subset_input_destroy);
    if (!input) throw std::runtime_error("Cannot create static font input");
    hb_subset_input_set_flags(input.get(), HB_SUBSET_FLAGS_RETAIN_GIDS | HB_SUBSET_FLAGS_NOTDEF_OUTLINE |
                                          HB_SUBSET_FLAGS_GLYPH_NAMES | HB_SUBSET_FLAGS_DOWNGRADE_CFF2);
    // Shaping has already chosen glyph IDs. Keep every ID, including unencoded ligatures.
    hb_set_invert(hb_subset_input_glyph_set(input.get()));
    hb_set_invert(hb_subset_input_unicode_set(input.get()));
    for (unsigned i = 0; i < count; ++i) {
        float const value = set ? coords[i] : axes[i].default_value;
        if (!std::isfinite(value) || !hb_subset_input_pin_axis_location(input.get(), face, axes[i].tag, value)) {
            throw std::runtime_error("Cannot freeze a font variation axis");
        }
    }
    auto frozen = std::unique_ptr<hb_face_t, decltype(&hb_face_destroy)>(
        hb_subset_or_fail(face, input.get()), hb_face_destroy);
    if (!frozen || hb_ot_var_has_data(frozen.get()) ||
        hb_face_get_glyph_count(frozen.get()) != hb_face_get_glyph_count(face)) {
        throw std::runtime_error("Static font conversion failed or changed glyph IDs");
    }
    auto cff2 = std::unique_ptr<hb_blob_t, decltype(&hb_blob_destroy)>(
        hb_face_reference_table(frozen.get(), HB_TAG('C', 'F', 'F', '2')), hb_blob_destroy);
    if (hb_blob_get_length(cff2.get())) throw std::runtime_error("Static font still contains CFF2");
    auto blob = std::unique_ptr<hb_blob_t, decltype(&hb_blob_destroy)>(
        hb_face_reference_blob(frozen.get()), hb_blob_destroy);
    unsigned length = 0;
    auto const *bytes = hb_blob_get_data(blob.get(), &length);
    if (!bytes || !length) throw std::runtime_error("Static font has no data");
    char *filename = nullptr;
    GError *error = nullptr;
    int fd = g_file_open_tmp("proof-font-XXXXXX", &filename, &error);
    if (fd < 0) {
        std::string message = error ? error->message : "Cannot create static font file";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
    auto result = std::make_unique<StaticFont>(filename);
    g_free(filename);
    if (!g_close(fd, &error) || !g_file_set_contents(result->path.c_str(), bytes, length, &error)) {
        std::string message = error ? error->message : "Cannot write static font file";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
    return result;
}
} // namespace Inkscape::Extension::Internal::PdfBuilder
