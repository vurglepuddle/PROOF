// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a placed PDF page, for the screen. See placed-page.h.
 */

#include "placed-page.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

#include <GlobalParams.h>
#include <PDFDoc.h>
#include <Page.h>
#include <glib.h>

#ifdef HAVE_POPPLER_CAIRO
#include <glib/poppler-document.h>
#include <glib/poppler-page.h>
#include <glib/poppler.h>
#endif

#include "poppler-transition-api.h"

namespace Inkscape::Extension::Internal {

bool looks_like_pdf(std::string_view data)
{
    return data.substr(0, 5) == "%PDF-";
}

std::optional<Geom::Rect> placed_page_crop(std::string const &path, std::string *why, int index)
{
    auto fail = [why](char const *reason) -> std::optional<Geom::Rect> {
        if (why) *why = reason;
        return {};
    };
    if (why) why->clear();
    if (index < 1) return fail("its page number must be at least 1");
    if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
    if (!pdf || !pdf->isOk()) return fail("it can't be read as a PDF");
    if (pdf->getNumPages() < 1) return fail("it has no page");
    if (index > pdf->getNumPages()) {
        if (why) *why = "it has no page " + std::to_string(index);
        return {};
    }
    auto *page = pdf->getPage(index);
    if (!page) return fail("its selected page can't be read");
    if (page->getRotate() % 360 != 0) return fail("its selected page is turned");
    auto const &c = page->getCropBox();
    if (!std::isfinite(c.x1) || !std::isfinite(c.y1) || !std::isfinite(c.x2) || !std::isfinite(c.y2) || c.x2 <= c.x1 ||
        c.y2 <= c.y1) {
        return fail("its selected page has no area");
    }
    return Geom::Rect(Geom::Point(c.x1, c.y1), Geom::Point(c.x2, c.y2));
}

#ifdef HAVE_POPPLER_CAIRO
namespace {

/// Pages drawn lately, the newest first, by their file's checksum and size.
struct Drawn
{
    std::string key;
    cairo_surface_t *surface;
};
constexpr std::size_t KEPT = 3;

std::list<Drawn> &drawn()
{
    static std::list<Drawn> pages;
    return pages;
}

cairo_surface_t *copy_of(cairo_surface_t *s)
{
    int const w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    auto *copy = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(copy) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(copy);
        return nullptr;
    }
    cairo_surface_flush(copy);
    int const from = cairo_image_surface_get_stride(s), to = cairo_image_surface_get_stride(copy);
    auto const *in = cairo_image_surface_get_data(s);
    auto *out = cairo_image_surface_get_data(copy);
    for (int y = 0; y < h; ++y) std::memcpy(out + std::size_t(y) * to, in + std::size_t(y) * from, std::size_t(w) * 4);
    cairo_surface_mark_dirty(copy);
    return copy;
}

} // namespace

cairo_surface_t *placed_page_surface(std::string_view data, int index)
{
    if (!looks_like_pdf(data) || index < 1) return nullptr;
    gchar *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(data.data()), data.size());
    std::string const key = std::string(sum) + ":" + std::to_string(data.size()) + ":" + std::to_string(index);
    g_free(sum);
    auto &pages = drawn();
    for (auto it = pages.begin(); it != pages.end(); ++it) {
        if (it->key == key) {
            pages.splice(pages.begin(), pages, it);
            return copy_of(pages.front().surface);
        }
    }
    cairo_surface_t *surface = nullptr;
    GBytes *bytes = g_bytes_new_static(data.data(), data.size());
    PopplerDocument *doc = poppler_document_new_from_bytes(bytes, nullptr, nullptr);
    g_bytes_unref(bytes);
    if (doc) {
        PopplerPage *page = poppler_document_get_n_pages(doc) >= index ? poppler_document_get_page(doc, index - 1) : nullptr;
        double w = 0, h = 0;
        if (page) poppler_page_get_size(page, &w, &h);
        if (page && w > 0 && h > 0 && std::isfinite(w) && std::isfinite(h)) {
            double const longest = std::min(PLACED_PAGE_PIXELS, std::max(w, h) * 300.0 / 72.0);
            double const scale = longest / std::max(w, h);
            int const pw = std::max(1, int(std::lround(w * scale))), ph = std::max(1, int(std::lround(h * scale)));
            surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
            if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
                cairo_t *cr = cairo_create(surface);
                cairo_scale(cr, pw / w, ph / h);
                // As Illustrator shows a placed page: without its annotations.
                poppler_page_render_full(page, cr, FALSE, POPPLER_RENDER_ANNOTS_NONE);
                cairo_destroy(cr);
                cairo_surface_flush(surface);
            } else {
                cairo_surface_destroy(surface);
                surface = nullptr;
            }
        }
        if (page) g_object_unref(page);
        g_object_unref(doc);
    }
    // Poppler's glib side drops Poppler's settings when its last document closes; whoever reads
    // a PDF next with Poppler's core needs them (ai-native-open.cpp).
    if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    if (!surface) return nullptr;
    pages.push_front({key, surface});
    if (pages.size() > KEPT) {
        cairo_surface_destroy(pages.back().surface);
        pages.pop_back();
    }
    return copy_of(surface);
}
#else
cairo_surface_t *placed_page_surface(std::string_view, int)
{
    return nullptr;
}
#endif

} // namespace Inkscape::Extension::Internal
