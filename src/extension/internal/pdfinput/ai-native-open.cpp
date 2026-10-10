// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: open an .ai file from Illustrator's own copy of its art. See ai-native-open.h.
 */
#include "ai-native-open.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>
#include <tuple>
#include <utility>

#include <Catalog.h>
#include <GlobalParams.h>
#include <PDFDoc.h>
#include <Page.h>
#include <cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <goo/GooString.h>

#ifdef HAVE_POPPLER_CAIRO
#include <glib/poppler-document.h>
#include <glib/poppler-page.h>
#include <glib/poppler.h>
#endif

#include <2geom/rect.h>
#include <2geom/transforms.h>

#include "ai-private-data.h"
#include "display/drawing-context.h"
#include "display/drawing-item.h"
#include "display/drawing.h"
#include "document.h"
#include "extension/internal/ai/ai-native-import.h"
#include "extension/internal/ai/ai-native-reader.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "poppler-transition-api.h"
#include "svg/css-ostringstream.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal {
namespace {

/// The most decoded records read. They hold every embedded image uncompressed: a brochure
/// of photographs for print runs to several hundred megabytes from a file of twenty.
constexpr std::size_t RECORDS_LIMIT = std::size_t(2048) << 20;
/// The longest side, in pixels, of an artboard as it is weighed.
constexpr double LONGEST_SIDE = 600.0;
/// An artboard that differs is weighed again from pictures drawn this many times larger and
/// averaged back down.
constexpr int FINER = 4;
/// Pixels this far apart (of 255) in lightness, or in any channel, look different.
constexpr int LIGHTNESS_STEP = 48;
constexpr int COLOUR_STEP = 80;
/// Differences worth a note.
constexpr double NOTABLE = 0.002;

std::string num(double v)
{
    CSSOStringStream os;
    os << v;
    return os.str();
}

std::string percent(double share)
{
    char buf[32];
    g_snprintf(buf, sizeof buf, "%.1f%%", share * 100.0);
    return buf;
}

/// A picture as seen on white: 8-bit RGB.
struct Picture
{
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgb;
};

std::string checksum(std::string_view data)
{
    auto *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                            reinterpret_cast<guchar const *>(data.data()), data.size());
    std::string out = sum ? sum : "";
    g_free(sum);
    return out;
}

// Cache files are disposable local test artifacts. Check their size and digest before
// reading them as records or pictures; a truncated/corrupt entry must cause a fresh run.
std::optional<std::string> read_cached(std::string const &path, std::size_t limit)
{
    GStatBuf stat;
    if (g_stat(path.c_str(), &stat) || stat.st_size <= 0 || std::uint64_t(stat.st_size) > limit) return {};
    if (g_stat((path + ".sha256").c_str(), &stat) || stat.st_size != 64) return {};
    gchar *data = nullptr, *digest = nullptr;
    gsize size = 0, digest_size = 0;
    bool const ok = g_file_get_contents(path.c_str(), &data, &size, nullptr) &&
                    g_file_get_contents((path + ".sha256").c_str(), &digest, &digest_size, nullptr);
    std::optional<std::string> out;
    if (ok && digest_size == 64 && checksum({data, size}) == std::string_view(digest, digest_size)) {
        out.emplace(data, size);
    }
    g_free(data);
    g_free(digest);
    return out;
}

void write_cached(std::string const &path, std::string_view data)
{
    if (g_file_set_contents(path.c_str(), data.data(), data.size(), nullptr)) {
        auto const digest = checksum(data);
        g_file_set_contents((path + ".sha256").c_str(), digest.data(), digest.size(), nullptr);
    }
}

void write_picture(Picture const &p, std::string const &path)
{
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, p.w, p.h);
    auto *data = cairo_image_surface_get_data(surface);
    auto const stride = cairo_image_surface_get_stride(surface);
    if (data) for (int y = 0; y < p.h; ++y) {
        auto *row = reinterpret_cast<std::uint32_t *>(data + y * stride);
        for (int x = 0; x < p.w; ++x) {
            auto const *rgb = &p.rgb[(std::size_t(y) * p.w + x) * 3];
            row[x] = (std::uint32_t(rgb[0]) << 16) | (std::uint32_t(rgb[1]) << 8) | rgb[2];
        }
    }
    cairo_surface_mark_dirty(surface);
    cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);
}

/// Takes the surface (premultiplied ARGB32) and frees it.
Picture seen_on_white(cairo_surface_t *s)
{
    Picture p;
    cairo_surface_flush(s);
    p.w = cairo_image_surface_get_width(s);
    p.h = cairo_image_surface_get_height(s);
    int const stride = cairo_image_surface_get_stride(s);
    auto const *data = cairo_image_surface_get_data(s);
    bool const opaque = cairo_image_surface_get_format(s) == CAIRO_FORMAT_RGB24;
    p.rgb.resize(std::size_t(p.w) * p.h * 3);
    for (int y = 0; y < p.h && data; ++y) {
        auto const *row = reinterpret_cast<std::uint32_t const *>(data + std::size_t(y) * stride);
        for (int x = 0; x < p.w; ++x) {
            std::uint32_t const px = row[x];
            int const paper = opaque ? 0 : 255 - int(px >> 24);
            auto *out = &p.rgb[(std::size_t(y) * p.w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                out[c] = std::uint8_t(std::min(255, int((px >> (16 - 8 * c)) & 0xff) + paper));
            }
        }
    }
    cairo_surface_destroy(s);
    return p;
}

/// `p` made `factor` times smaller: each pixel the mean of a factor x factor block.
Picture averaged(Picture const &p, int factor)
{
    Picture out;
    out.w = p.w / factor;
    out.h = p.h / factor;
    out.rgb.resize(std::size_t(out.w) * out.h * 3);
    int const n = factor * factor;
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            int sum[3] = {0, 0, 0};
            for (int dy = 0; dy < factor; ++dy) {
                auto const *block = &p.rgb[(std::size_t(y * factor + dy) * p.w + std::size_t(x) * factor) * 3];
                for (int i = 0; i < factor * 3; ++i) sum[i % 3] += block[i];
            }
            auto *pixel = &out.rgb[(std::size_t(y) * out.w + x) * 3];
            for (int c = 0; c < 3; ++c) pixel[c] = std::uint8_t((sum[c] + n / 2) / n);
        }
    }
    return out;
}

std::optional<Picture> read_reference(std::string const &path, int w, int h)
{
    // An RGB picture is tiny at the comparison resolution. Bound compressed input too.
    auto bytes = read_cached(path, 8u << 20);
    if (!bytes) return {};
    // Inspect the PNG IHDR before Cairo allocates the decoded image.
    if (bytes->size() < 24 || bytes->compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) return {};
    auto dimension = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = at; i < at + 4; ++i) value = (value << 8) | std::uint8_t((*bytes)[i]);
        return value;
    };
    if (dimension(16) != std::uint32_t(w) || dimension(20) != std::uint32_t(h)) return {};
    struct Input { std::string_view bytes; std::size_t at = 0; } input{*bytes};
    auto reader = [](void *closure, unsigned char *data, unsigned length) -> cairo_status_t {
        auto &in = *static_cast<Input *>(closure);
        if (length > in.bytes.size() - in.at) return CAIRO_STATUS_READ_ERROR;
        std::copy_n(in.bytes.data() + in.at, length, data);
        in.at += length;
        return CAIRO_STATUS_SUCCESS;
    };
    auto *surface = cairo_image_surface_create_from_png_stream(reader, &input);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_width(surface) != w || cairo_image_surface_get_height(surface) != h ||
        (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_RGB24 &&
         cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)) {
        cairo_surface_destroy(surface);
        return {};
    }
    return seen_on_white(surface);
}

void cache_reference(Picture const &picture, std::string const &path)
{
    write_picture(picture, path);
    gchar *bytes = nullptr;
    gsize size = 0;
    if (g_file_get_contents(path.c_str(), &bytes, &size, nullptr)) {
        auto const digest = checksum({bytes, size});
        g_file_set_contents((path + ".sha256").c_str(), digest.data(), digest.size(), nullptr);
    }
    g_free(bytes);
}

int lightness(std::uint8_t const *p)
{
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
}

/// Whether pixel (x, y) of `a` looks like some pixel of `b` within one pixel of it.
template <typename Same>
bool matched(Picture const &a, Picture const &b, int x, int y, Same const &same)
{
    auto const *pa = &a.rgb[(std::size_t(y) * a.w + x) * 3];
    for (int dy = -1; dy <= 1; ++dy) {
        int const yy = y + dy;
        if (yy < 0 || yy >= b.h) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            int const xx = x + dx;
            if (xx < 0 || xx >= b.w) continue;
            if (same(pa, &b.rgb[(std::size_t(yy) * b.w + xx) * 3])) return true;
        }
    }
    return false;
}

AiPageDifference weigh(Picture const &a, Picture const &b)
{
    if (std::abs(a.w - b.w) > 2 || std::abs(a.h - b.h) > 2) return {1.0, 1.0};
    int const w = std::min(a.w, b.w), h = std::min(a.h, b.h);
    auto light = [](std::uint8_t const *p, std::uint8_t const *q) {
        return std::abs(lightness(p) - lightness(q)) <= LIGHTNESS_STEP;
    };
    auto colour = [](std::uint8_t const *p, std::uint8_t const *q) {
        return std::abs(p[0] - q[0]) <= COLOUR_STEP && std::abs(p[1] - q[1]) <= COLOUR_STEP &&
               std::abs(p[2] - q[2]) <= COLOUR_STEP;
    };
    std::size_t by_light = 0, by_colour = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Art missing from either side counts: each picture is matched against the other.
            if (!matched(a, b, x, y, light) || !matched(b, a, x, y, light)) ++by_light;
            if (!matched(a, b, x, y, colour) || !matched(b, a, x, y, colour)) ++by_colour;
        }
    }
    double const n = std::max(1.0, double(w) * h);
    return {by_light / n, by_colour / n};
}

double difference_score(AiPageDifference const &d)
{
    return std::max(d.lightness / AI_NATIVE_MAX_LIGHTNESS, d.colour / AI_NATIVE_MAX_COLOUR);
}

/// The built document, shown once and drawn artboard by artboard.
class NativeView
{
public:
    explicit NativeView(SPDocument *doc)
        : _doc(doc)
        , _key(SPItem::display_key_new(1))
    {
        doc->ensureUpToDate();
        _drawing.setRoot(doc->getRoot()->invoke_show(_drawing, _key, SP_ITEM_SHOW_DISPLAY));
        _drawing.setExact();
    }
    ~NativeView() { _doc->getRoot()->invoke_hide(_key); }
    NativeView(NativeView const &) = delete;
    NativeView &operator=(NativeView const &) = delete;

    /// `board` (user units) at `scale` pixels a point, into a w x h picture. With
    /// `thin_lines_shown`, a line thinner than a pixel is drawn a pixel wide, as Poppler
    /// draws it on a screen.
    Picture draw(Geom::Rect const &board, double scale, int w, int h, bool thin_lines_shown = false)
    {
        _doc->ensureUpToDate();
        // User units are points; the drawing is in the document's pixels.
        auto const px = _doc->getDocumentScale();
        auto const area = board * px;
        _drawing.root()->setTransform(Geom::Translate(-area.min()) * Geom::Scale(scale / px[Geom::X], scale / px[Geom::Y]));
        auto const box = Geom::IntRect::from_xywh(0, 0, w, h);
        _drawing.update(box);
        auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        {
            Inkscape::DrawingContext dc(surface, Geom::Point(0, 0));
            _drawing.render(dc, box, Inkscape::DrawingItem::RENDER_BYPASS_CACHE |
                                         (thin_lines_shown ? Inkscape::DrawingItem::RENDER_VISIBLE_HAIRLINES : 0));
        }
        return seen_on_white(surface);
    }

private:
    SPDocument *_doc;
    unsigned _key;
    Inkscape::Drawing _drawing;
};

/// The layers that don't print, hidden while the view draws.
class NonPrintingHidden
{
public:
    explicit NonPrintingHidden(SPDocument *doc)
    {
        sp_repr_visit_descendants(doc->getReprRoot(), [&](XML::Node *n) {
            auto const print = n->attribute(AiNative::LAYER_PRINT_ATTRIBUTE);
            if (print && std::string_view(print) == "false") {
                auto const style = n->attribute("style");
                _saved.emplace_back(n, style ? std::optional<std::string>(style) : std::nullopt);
                n->setAttribute("style", std::string(style ? style : "") + ";display:none");
                return false;
            }
            return true;
        });
    }
    ~NonPrintingHidden()
    {
        for (auto const &[n, style] : _saved) n->setAttribute("style", style ? style->c_str() : nullptr);
    }
    bool any() const { return !_saved.empty(); }

private:
    std::vector<std::pair<XML::Node *, std::optional<std::string>>> _saved;
};

bool any_non_printing(SPDocument *doc)
{
    bool found = false;
    sp_repr_visit_descendants(doc->getReprRoot(), [&](XML::Node *n) {
        auto const print = n->attribute(AiNative::LAYER_PRINT_ATTRIBUTE);
        found = found || (print && std::string_view(print) == "false");
        return !found;
    });
    return found;
}

#ifdef HAVE_POPPLER_CAIRO
/// The page's trim box at `scale` pixels a point, into a w x h picture.
Picture draw_page(PopplerPage *page, PDFRectangle const &crop, PDFRectangle const &trim, double scale, int w, int h)
{
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    auto *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    // Poppler draws the crop box with its top-left corner at 0,0.
    cairo_translate(cr, -(trim.x1 - crop.x1), -(crop.y2 - trim.y2));
    poppler_page_render_full(page, cr, false, POPPLER_RENDER_ANNOTS_NONE);
    cairo_destroy(cr);
    return seen_on_white(surface);
}

struct Unref
{
    void operator()(gpointer p) const { g_object_unref(p); }
};
#endif

std::optional<AiNativeOpen> open_checked(std::string const &path, std::string &reason, bool keep_differing,
                                         std::string const &diagnostic_directory, AiNativeCache *cache);

} // namespace

std::optional<AiNativeOpen> open_ai_native(std::string const &path, std::string &reason, bool keep_differing,
                                         std::string const &diagnostic_directory, AiNativeCache *cache)
{
    // The records hold every image uncompressed. A machine that runs out of memory over
    // them still has the page to open.
    try {
        return open_checked(path, reason, keep_differing, diagnostic_directory, cache);
    } catch (std::bad_alloc const &) {
        reason = "there isn't enough memory to read its own art";
        return {};
    }
}

namespace {

std::optional<AiNativeOpen> open_checked(std::string const &path, std::string &reason, bool keep_differing,
                                         std::string const &diagnostic_directory, AiNativeCache *cache)
{
    reason.clear();
    if (cache) cache->records_reused = cache->references_reused = 0;
#ifndef HAVE_POPPLER_CAIRO
    reason = "this build can't draw the PDF page to check the art against";
    return {};
#else
    std::string error;
    std::vector<std::pair<std::string, double>> timings;
    auto clock = std::chrono::steady_clock::now();
    auto timed = [&](char const *part) {
        auto const now = std::chrono::steady_clock::now();
        timings.emplace_back(part, std::chrono::duration<double>(now - clock).count());
        clock = now;
    };
    std::optional<AiNativeRecords> records;
    if (cache && !cache->records_directory.empty()) {
        if (auto text = read_cached(cache->records_directory + "/records.txt", RECORDS_LIMIT)) {
            records.emplace();
            records->text = std::move(*text);
            ++cache->records_reused;
        }
    }
    if (!records) {
        records = read_ai_native_records(path, "", RECORDS_LIMIT, error);
        if (records && cache && !cache->records_directory.empty()) {
            g_mkdir_with_parents(cache->records_directory.c_str(), 0700);
            write_cached(cache->records_directory + "/records.txt", records->text);
        }
    }
    if (!records) {
        reason = error.empty() ? "it has no data of Illustrator's own" : error;
        return {};
    }
    if (records->text.find("%AI5_BeginLayer") == std::string::npos) {
        reason = "its own data has no layers";
        return {};
    }
    timed("records");
    auto ai = AiNative::read(records->text, error);
    records.reset();
    if (!ai) {
        reason = error;
        return {};
    }
    timed("read");
    AiNative::Source source;
    {
        gchar *folder = g_path_get_dirname(path.c_str());
        source.folder = folder;
        g_free(folder);
    }
    auto built = AiNative::build(*ai, error, source);
    ai.reset();
    if (!built) {
        reason = error;
        return {};
    }
    timed("build");
    auto *doc = built->document.get();
    auto const &boards = built->artboards;

    // The page: Poppler's core for its boxes, its glib side to draw it.
    if (!globalParams) {
        globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    }
    auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
    if (!pdf || !pdf->isOk()) {
        reason = "its PDF part can't be read to check the art against";
        return {};
    }
    GError *gerror = nullptr;
    gchar *absolute = g_canonicalize_filename(path.c_str(), nullptr);
    gchar *uri = g_filename_to_uri(absolute, nullptr, &gerror);
    g_free(absolute);
    std::unique_ptr<PopplerDocument, Unref> pages;
    if (uri) {
        pages.reset(poppler_document_new_from_file(uri, nullptr, &gerror));
        g_free(uri);
    }
    if (gerror) {
        g_error_free(gerror);
    }
    if (!pages) {
        reason = "its PDF part can't be drawn to check the art against";
        return {};
    }
    int const page_count = std::min(pdf->getNumPages(), poppler_document_get_n_pages(pages.get()));
    if (boards.empty() || page_count < 1) {
        reason = "it has no artboard to check against its page";
        return {};
    }
    if (boards.size() != std::size_t(page_count)) {
        reason = "its artboard count doesn't match its PDF page count";
        return {};
    }
    std::size_t const compared = boards.size();

    AiNativeOpen result;
    auto const doc_pages = doc->getPageManager().getPages();
    bool const non_printing = any_non_printing(doc);
    std::optional<NativeView> view;
    view.emplace(doc);
    std::size_t worst = 0;
    if (!diagnostic_directory.empty()) g_mkdir_with_parents(diagnostic_directory.c_str(), 0700);
    for (std::size_t i = 0; i < compared; ++i) {
        auto *page = pdf->getCatalog()->getPage(int(i) + 1);
        if (!page || page->getRotate() % 360 != 0) {
            reason = "its page " + std::to_string(i + 1) + " is turned or missing";
            return {};
        }
        auto const &crop = page->getCropBox();
        auto const &trim = page->getTrimBox();
        auto const &bleed = page->getBleedBox();
        double const tw = trim.x2 - trim.x1, th = trim.y2 - trim.y1;
        auto const &board = boards[i];
        if (!std::isfinite(tw) || !std::isfinite(th) || tw <= 0 || th <= 0 ||
            std::abs(tw - board.width()) > 1.0 || std::abs(th - board.height()) > 1.0) {
            reason = "its page " + std::to_string(i + 1) + " isn't the size of its artboard";
            return {};
        }
        // The page carries the bleed the artboard prints with.
        double const top = bleed.y2 - trim.y2, right = bleed.x2 - trim.x2;
        double const bottom = trim.y1 - bleed.y1, left = trim.x1 - bleed.x1;
        if (i < doc_pages.size() && std::max({top, right, bottom, left}) > 0.01) {
            doc_pages[i]->getRepr()->setAttribute("inkscape:bleed", num(std::max(0.0, top)) + " " +
                                                                        num(std::max(0.0, right)) + " " +
                                                                        num(std::max(0.0, bottom)) + " " +
                                                                        num(std::max(0.0, left)));
        }

        double const scale = std::min(4.0, LONGEST_SIDE / std::max(tw, th));
        int const w = std::max(1, int(std::ceil(tw * scale - 1e-6)));
        int const h = std::max(1, int(std::ceil(th * scale - 1e-6)));
        std::unique_ptr<PopplerPage, Unref> drawn(poppler_document_get_page(pages.get(), int(i)));
        if (!drawn) {
            reason = "its page " + std::to_string(i + 1) + " can't be drawn";
            return {};
        }
        std::optional<Picture> reference;
        std::string reference_path;
        if (cache && !cache->reference_directory.empty()) {
            g_mkdir_with_parents(cache->reference_directory.c_str(), 0700);
            reference_path = cache->reference_directory + "/page-" + std::to_string(i + 1) + ".png";
            reference = read_reference(reference_path, w, h);
            if (reference) ++cache->references_reused;
        }
        if (!reference) {
            reference = draw_page(drawn.get(), crop, trim, scale, w, h);
            if (!reference_path.empty()) cache_reference(*reference, reference_path);
        }
        auto page_picture = std::move(*reference);
        Picture native_picture;
        AiPageDifference d;
        // The art against `page`, each drawn by `draw`; also without the layers that don't
        // print, which a page may leave out.
        auto weigh_art = [&](Picture const &page, auto const &draw) {
            auto picture = draw();
            auto difference = weigh(page, picture);
            if (difference_score(difference) > 1.0 && non_printing) {
                NonPrintingHidden hidden(doc);
                auto hidden_picture = draw();
                auto e = weigh(page, hidden_picture);
                if (difference_score(e) < difference_score(difference)) {
                    difference = e;
                    picture = std::move(hidden_picture);
                }
            }
            return std::pair(difference, std::move(picture));
        };
        std::tie(d, native_picture) = weigh_art(page_picture, [&] { return view->draw(board, scale, w, h); });
        if (difference_score(d) > 1.0) {
            // Lines thinner than a pixel are where the two renderers part: Poppler draws them
            // a whole pixel wide and on the pixel grid, the drawing as thin as they are. A
            // page of hairlines then seems to differ everywhere. Drawn FINER times larger and
            // averaged down, with the drawing keeping thin lines a pixel wide as well, both
            // show the same ink for the same line; art that really differs still does.
            auto fine_page = averaged(draw_page(drawn.get(), crop, trim, scale * FINER, w * FINER, h * FINER), FINER);
            auto [e, fine_native] = weigh_art(fine_page, [&] {
                return averaged(view->draw(board, scale * FINER, w * FINER, h * FINER, true), FINER);
            });
            if (difference_score(e) < difference_score(d)) {
                d = e;
                page_picture = std::move(fine_page);
                native_picture = std::move(fine_native);
            }
        }
        if (!diagnostic_directory.empty()) {
            auto stem = diagnostic_directory + "/page-" + std::to_string(i + 1);
            write_picture(page_picture, stem + "-pdf.png");
            write_picture(native_picture, stem + "-native.png");
        }
        result.differences.push_back(d);
        if (difference_score(d) > difference_score(result.differences[worst])) worst = i;
        // A differing artboard settles it: the remaining ones needn't be drawn.
        if (difference_score(d) > 1.0 && !keep_differing) break;
    }
    view.reset();

    auto const &d = result.differences[worst];
    std::string const where = boards.size() > 1 ? " on artboard " + std::to_string(worst + 1) : "";
    if (d.lightness > AI_NATIVE_MAX_LIGHTNESS || d.colour > AI_NATIVE_MAX_COLOUR) {
        reason = "its own art draws differently from its page" + where + " (" + percent(d.lightness) +
                 " by lightness, " + percent(d.colour) + " by colour)";
        if (!keep_differing) return {};
    }
    if (std::max(d.lightness, d.colour) > NOTABLE) {
        result.notes.push_back("its own art differs from its page" + where + " in about " +
                               percent(std::max(d.lightness, d.colour)) + " of it");
    }
    for (auto &n : built->notes) result.notes.push_back(std::move(n));
    result.document = std::move(built->document);
    timed("compare");
    result.timings = std::move(timings);
    return result;
#endif
}

} // namespace

} // namespace Inkscape::Extension::Internal
