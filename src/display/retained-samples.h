// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_DISPLAY_RETAINED_SAMPLES_H
#define INKSCAPE_DISPLAY_RETAINED_SAMPLES_H

#include <glib.h>
#include <glibmm/i18n.h>
#include "display/tiff-pixels.h"
#include "util/uri.h"

namespace Inkscape::RetainedSamples {

// Hash the image-file bytes, not its URI spelling, pathname, decoded RGB or profile.
// The importer hashes the PNG it made; exporters hash the same immutable input bytes.
inline std::string fingerprint(std::string_view bytes)
{
    auto *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<guchar const *>(bytes.data()), bytes.size());
    std::string result(hash, 16);
    g_free(hash);
    return result;
}

enum class State { Absent, Valid, Stale, Unreadable };
struct Checked {
    State state = State::Absent;
    std::string tiff;
    std::optional<RasterPixels> pixels;
    char const *reason() const
    {
        return state == State::Stale ? _("Retained samples are stale.") : _("Retained samples are unreadable.");
    }
};

inline Checked read(char const *samples, char const *binding, std::string_view display, int w, int h)
{
    if (!samples) return {};
    // Old documents are deliberately untrusted. Dimensions cannot establish identity.
    if (!binding || display.empty() || fingerprint(display) != binding) return {State::Stale, {}, {}};
    Checked result{State::Unreadable, {}, {}};
    auto [base64, type] = extract_uri_data(samples);
    if (type != Base64Data::RASTER) return result;
    gsize len = 0;
    auto *bytes = g_base64_decode(base64, &len);
    result.tiff.assign(reinterpret_cast<char const *>(bytes), len);
    g_free(bytes);
    try {
        result.pixels = read_tiff(result.tiff);
        if (result.pixels && int(result.pixels->width) == w && int(result.pixels->height) == h) {
            result.state = State::Valid;
        }
    } catch (...) { /* A malformed retained image must not prevent the display-image fallback. */ }
    return result;
}

} // namespace Inkscape::RetainedSamples
#endif
